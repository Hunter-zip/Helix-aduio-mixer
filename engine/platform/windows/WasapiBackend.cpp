// Implementacja backendu WASAPI (spec §2, §5, §20, §23).
//
// Tryb współdzielony pracuje na formacie miksu urządzenia (float32) i jest
// sterowany zdarzeniami; tryb wyłączny próbuje formatu żądanego przez silnik.
// Konwersję częstotliwości między zegarem urządzenia a zegarem silnika robi
// DeviceManager, dlatego backend nie musi niczego przepróbkowywać.
#include "helix/platform/WasapiBackend.h"

#include "WasapiCommon.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <avrt.h>

#include "helix/Log.h"

namespace helix::core {

using namespace helix::platform;

namespace {

constexpr const char* kLog = "WASAPI";

/// 100-nanosekundowe jednostki REFERENCE_TIME w jednej sekundzie.
constexpr double kRefTimesPerSecond = 10000000.0;

REFERENCE_TIME framesToReferenceTime(int frames, double sampleRate) {
    return static_cast<REFERENCE_TIME>(kRefTimesPerSecond * frames / sampleRate + 0.5);
}

/// Buduje WAVEFORMATEXTENSIBLE dla float32.
WAVEFORMATEXTENSIBLE makeFloatFormat(double sampleRate, int channels) {
    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag      = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels       = static_cast<WORD>(channels);
    format.Format.nSamplesPerSec  = static_cast<DWORD>(sampleRate);
    format.Format.wBitsPerSample  = 32;
    format.Format.nBlockAlign     = static_cast<WORD>(channels * 4);
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize          = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = (channels == 1) ? kSpeakerFrontCenter
                                           : (kSpeakerFrontLeft | kSpeakerFrontRight);
    format.SubFormat = kSubFormatIeeeFloat;
    return format;
}

} // namespace

// ── Klient powiadomień o urządzeniach ───────────────────────────────────────

// Interfejsy COM z założenia nie mają wirtualnych destruktorów — czasem życia
// zarządza licznik referencji, a nie `delete` przez wskaźnik do bazy.
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif

class WasapiNotificationClient final : public IMMNotificationClient {
public:
    using Handler = std::function<void(const DeviceChangeEvent&)>;

    explicit WasapiNotificationClient(Handler handler) : handler_(std::move(handler)) {}

    // IUnknown
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(references_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const long remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** target) override {
        if (target == nullptr) return E_POINTER;
        if (IsEqualIID(id, __uuidof(IUnknown)) || IsEqualIID(id, __uuidof(IMMNotificationClient))) {
            *target = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *target = nullptr;
        return E_NOINTERFACE;
    }

    // IMMNotificationClient
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR deviceId, DWORD state) override {
        DeviceChangeEvent event;
        event.deviceId = toUtf8(deviceId);
        event.type = (state == DEVICE_STATE_ACTIVE) ? DeviceChangeEvent::Type::Added
                                                    : DeviceChangeEvent::Type::Removed;
        notify(event);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR deviceId) override {
        DeviceChangeEvent event;
        event.deviceId = toUtf8(deviceId);
        event.type = DeviceChangeEvent::Type::Added;
        notify(event);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR deviceId) override {
        DeviceChangeEvent event;
        event.deviceId = toUtf8(deviceId);
        event.type = DeviceChangeEvent::Type::Removed;
        notify(event);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role,
                                                     LPCWSTR deviceId) override {
        if (role != eConsole && role != eMultimedia) return S_OK;
        DeviceChangeEvent event;
        event.deviceId = toUtf8(deviceId);
        event.type = DeviceChangeEvent::Type::DefaultChanged;
        event.direction = (flow == eCapture) ? DeviceDirection::Capture : DeviceDirection::Render;
        notify(event);
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR deviceId, const PROPERTYKEY key) override {
        // Zmiana formatu urządzenia wymaga ponownego otwarcia strumienia (spec §20).
        if (IsEqualGUID(key.fmtid, kPropertyAudioEngineDeviceFormat.fmtid) &&
            key.pid == kPropertyAudioEngineDeviceFormat.pid) {
            DeviceChangeEvent event;
            event.deviceId = toUtf8(deviceId);
            event.type = DeviceChangeEvent::Type::FormatChanged;
            notify(event);
        }
        return S_OK;
    }

private:
    void notify(const DeviceChangeEvent& event) {
        if (handler_) handler_(event);
    }

    std::atomic<long> references_{1};
    Handler handler_;
};

// ── Strumień ────────────────────────────────────────────────────────────────

class WasapiStream final : public AudioStream {
public:
    WasapiStream(ComPtr<IAudioClient> client, StreamConfig config, bool isRender,
                 RenderCallback* render, CaptureCallback* capture,
                 SampleFormat format, int deviceChannels, double deviceSampleRate,
                 int bufferFrames, double latencyMs)
        : client_(std::move(client)),
          config_(std::move(config)),
          isRender_(isRender),
          render_(render),
          capture_(capture),
          format_(format),
          deviceChannels_(deviceChannels),
          deviceSampleRate_(deviceSampleRate),
          bufferFrames_(bufferFrames),
          latencyMs_(latencyMs) {
        scratch_.assign(static_cast<std::size_t>(bufferFrames_) *
                            static_cast<std::size_t>(deviceChannels_), 0.0f);
    }

    ~WasapiStream() override { stop(); }

    Status start() override {
        if (running_.load(std::memory_order_acquire)) return Status::success();

        if (!event_.create()) return Status::error("Nie można utworzyć zdarzenia strumienia");

        HRESULT result = client_->SetEventHandle(event_.get());
        if (FAILED(result))
            return Status::error("SetEventHandle: " + describeHresult(result));

        if (isRender_) {
            result = client_->GetService(__uuidof(IAudioRenderClient), renderClient_.putVoid());
            if (FAILED(result))
                return Status::error("IAudioRenderClient: " + describeHresult(result));
        } else {
            result = client_->GetService(__uuidof(IAudioCaptureClient), captureClient_.putVoid());
            if (FAILED(result))
                return Status::error("IAudioCaptureClient: " + describeHresult(result));
        }

        result = client_->Start();
        if (FAILED(result)) return Status::error("Start: " + describeHresult(result));

        running_.store(true, std::memory_order_release);
        thread_ = std::make_unique<std::thread>([this] { run(); });
        return Status::success();
    }

    void stop() override {
        if (!running_.exchange(false, std::memory_order_acq_rel)) return;
        if (event_) SetEvent(event_.get());
        if (thread_ && thread_->joinable()) thread_->join();
        thread_.reset();

        if (client_) client_->Stop();
        renderClient_.reset();
        captureClient_.reset();
        event_.reset();
    }

    [[nodiscard]] bool isRunning() const override {
        return running_.load(std::memory_order_acquire) && !failed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] double sampleRate() const override { return deviceSampleRate_; }
    [[nodiscard]] int channels() const override { return deviceChannels_; }
    [[nodiscard]] int bufferFrames() const override { return bufferFrames_; }
    [[nodiscard]] double latencyMs() const override { return latencyMs_; }

    [[nodiscard]] std::uint64_t glitchCount() const override {
        return glitches_.load(std::memory_order_relaxed);
    }

private:
    void run() {
        ComApartment apartment;

        // Podniesienie priorytetu wątku — bez tego planista Windows potrafi
        // zgubić bufor przy krótkich okresach (spec §22).
        DWORD taskIndex = 0;
        HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);

        if (isRender_) runRender();
        else           runCapture();

        if (task != nullptr) AvRevertMmThreadCharacteristics(task);
    }

    void runRender() {
        // Pierwsze wypełnienie bufora ciszą — inaczej usłyszelibyśmy śmieci.
        UINT32 totalFrames = 0;
        if (SUCCEEDED(client_->GetBufferSize(&totalFrames)) && totalFrames > 0) {
            BYTE* data = nullptr;
            if (SUCCEEDED(renderClient_->GetBuffer(totalFrames, &data)))
                renderClient_->ReleaseBuffer(totalFrames, AUDCLNT_BUFFERFLAGS_SILENT);
        }

        while (running_.load(std::memory_order_acquire)) {
            const DWORD waited = WaitForSingleObject(event_.get(), 2000);
            if (!running_.load(std::memory_order_acquire)) break;
            if (waited != WAIT_OBJECT_0) {
                glitches_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            UINT32 padding = 0;
            HRESULT result = client_->GetCurrentPadding(&padding);
            if (FAILED(result)) { handleFailure(result); break; }

            UINT32 available = totalFrames - padding;
            if (config_.exclusive) available = totalFrames;   // tryb wyłączny wydaje cały bufor
            if (available == 0) continue;

            BYTE* data = nullptr;
            result = renderClient_->GetBuffer(available, &data);
            if (FAILED(result)) { handleFailure(result); break; }

            const int frames = std::min(static_cast<int>(available), bufferFrames_);

            std::memset(scratch_.data(), 0,
                        static_cast<std::size_t>(frames) * static_cast<std::size_t>(deviceChannels_)
                            * sizeof(float));
            if (render_ != nullptr) render_->renderBlock(scratch_.data(), frames, deviceChannels_);

            convertFromFloat(scratch_.data(), data, frames, deviceChannels_, format_);
            if (static_cast<int>(available) > frames) {
                // Zabezpieczenie: reszta bufora zostaje wyciszona.
                const std::size_t written = static_cast<std::size_t>(frames) *
                                            static_cast<std::size_t>(deviceChannels_) *
                                            bytesPerSample();
                std::memset(data + written, 0,
                            (static_cast<std::size_t>(available) - static_cast<std::size_t>(frames)) *
                                static_cast<std::size_t>(deviceChannels_) * bytesPerSample());
            }

            result = renderClient_->ReleaseBuffer(available, 0);
            if (FAILED(result)) { handleFailure(result); break; }
        }
    }

    void runCapture() {
        while (running_.load(std::memory_order_acquire)) {
            const DWORD waited = WaitForSingleObject(event_.get(), 2000);
            if (!running_.load(std::memory_order_acquire)) break;
            if (waited != WAIT_OBJECT_0) {
                glitches_.fetch_add(1, std::memory_order_relaxed);
                continue;
            }

            UINT32 packet = 0;
            HRESULT result = captureClient_->GetNextPacketSize(&packet);
            if (FAILED(result)) { handleFailure(result); break; }

            while (packet > 0 && running_.load(std::memory_order_acquire)) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;

                result = captureClient_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (result == AUDCLNT_S_BUFFER_EMPTY) break;
                if (FAILED(result)) { handleFailure(result); return; }

                const int useFrames = std::min(static_cast<int>(frames), bufferFrames_);

                if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY)
                    glitches_.fetch_add(1, std::memory_order_relaxed);

                if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                    std::memset(scratch_.data(), 0,
                                static_cast<std::size_t>(useFrames) *
                                    static_cast<std::size_t>(deviceChannels_) * sizeof(float));
                } else {
                    convertToFloat(data, scratch_.data(), useFrames, deviceChannels_, format_);
                }

                if (capture_ != nullptr)
                    capture_->captureBlock(scratch_.data(), useFrames, deviceChannels_);

                result = captureClient_->ReleaseBuffer(frames);
                if (FAILED(result)) { handleFailure(result); return; }

                result = captureClient_->GetNextPacketSize(&packet);
                if (FAILED(result)) { handleFailure(result); return; }
            }
        }
    }

    [[nodiscard]] std::size_t bytesPerSample() const noexcept {
        switch (format_) {
            case SampleFormat::Float32: return 4;
            case SampleFormat::Int16:   return 2;
            case SampleFormat::Int24:   return 3;
            case SampleFormat::Int32:   return 4;
            case SampleFormat::Unsupported: return 4;
        }
        return 4;
    }

    void handleFailure(HRESULT result) {
        failed_.store(true, std::memory_order_release);
        // Urządzenie zniknęło — DeviceManager podniesie strumień, gdy wróci (spec §23).
        Log::warn(kLog, "Strumień przerwany: " + describeHresult(result));
    }

    ComPtr<IAudioClient>        client_;
    ComPtr<IAudioRenderClient>  renderClient_;
    ComPtr<IAudioCaptureClient> captureClient_;
    StreamConfig                config_;
    bool                        isRender_;
    RenderCallback*             render_ = nullptr;
    CaptureCallback*            capture_ = nullptr;
    SampleFormat                format_;
    int                         deviceChannels_;
    double                      deviceSampleRate_;
    int                         bufferFrames_;
    double                      latencyMs_;

    std::vector<float>          scratch_;
    EventHandle                 event_;
    std::unique_ptr<std::thread> thread_;
    std::atomic<bool>           running_{false};
    std::atomic<bool>           failed_{false};
    std::atomic<std::uint64_t>  glitches_{0};
};

// ── Obsługa aktywacji per proces ────────────────────────────────────────────

/// Handler dla ActivateAudioInterfaceAsync — potrzebny przy przechwytywaniu
/// audio konkretnej aplikacji (spec §5).
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler {
public:
    ActivationHandler() { done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivationHandler() { if (done_) CloseHandle(done_); }

    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(references_.fetch_add(1, std::memory_order_relaxed) + 1);
    }

    ULONG STDMETHODCALLTYPE Release() override {
        const long remaining = references_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remaining == 0) delete this;
        return static_cast<ULONG>(remaining);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** target) override {
        if (target == nullptr) return E_POINTER;
        if (IsEqualIID(id, __uuidof(IUnknown)) ||
            IsEqualIID(id, __uuidof(IActivateAudioInterfaceCompletionHandler))) {
            *target = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
            AddRef();
            return S_OK;
        }
        *target = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(
        IActivateAudioInterfaceAsyncOperation* operation) override {
        IUnknown* activated = nullptr;
        operation->GetActivateResult(&activationResult_, &activated);
        activated_ = activated;
        if (done_) SetEvent(done_);
        return S_OK;
    }

    /// Czeka na wynik aktywacji. Zwraca przejęty wskaźnik (wywołujący zwalnia).
    [[nodiscard]] IUnknown* wait(DWORD timeoutMs, HRESULT& result) {
        if (done_ == nullptr) { result = E_FAIL; return nullptr; }
        if (WaitForSingleObject(done_, timeoutMs) != WAIT_OBJECT_0) {
            result = HRESULT_FROM_WIN32(WAIT_TIMEOUT);
            return nullptr;
        }
        result = activationResult_;
        IUnknown* activated = activated_;
        activated_ = nullptr;
        return activated;
    }

private:
    std::atomic<long> references_{1};
    HANDLE   done_ = nullptr;
    HRESULT  activationResult_ = E_FAIL;
    IUnknown* activated_ = nullptr;
};

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ── Backend ─────────────────────────────────────────────────────────────────

class WasapiBackend final : public AudioBackend {
public:
    WasapiBackend() = default;

    ~WasapiBackend() override { shutdown(); }

    [[nodiscard]] const char* name() const noexcept override { return "wasapi"; }

    Status initialize() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (initialized_) return Status::success();

        apartment_ = std::make_unique<ComApartment>();
        if (!apartment_->ok()) return Status::error("Nie można zainicjalizować COM");

        const HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr,
                                                CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                                enumerator_.putVoid());
        if (FAILED(result))
            return Status::error("MMDeviceEnumerator: " + describeHresult(result));

        notificationClient_ = new WasapiNotificationClient(
            [this](const DeviceChangeEvent& event) { dispatchChange(event); });
        enumerator_->RegisterEndpointNotificationCallback(notificationClient_);

        initialized_ = true;
        Log::info(kLog, "Backend WASAPI gotowy");
        return Status::success();
    }

    void shutdown() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) return;

        if (enumerator_ && notificationClient_ != nullptr)
            enumerator_->UnregisterEndpointNotificationCallback(notificationClient_);
        if (notificationClient_ != nullptr) {
            notificationClient_->Release();
            notificationClient_ = nullptr;
        }

        enumerator_.reset();
        apartment_.reset();
        initialized_ = false;
    }

    [[nodiscard]] std::vector<DeviceInfo> enumerateDevices(DeviceDirection direction) override {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<DeviceInfo> devices;
        if (!enumerator_) return devices;

        const EDataFlow flow = (direction == DeviceDirection::Render) ? eRender : eCapture;

        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator_->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put())))
            return devices;

        const std::string defaultId = defaultDeviceIdLocked(direction);

        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.put()))) continue;

            DeviceInfo info = describeDevice(device.get(), direction);
            if (info.id.empty()) continue;
            info.isDefault = (info.id == defaultId);
            devices.push_back(std::move(info));
        }
        return devices;
    }

    [[nodiscard]] std::string defaultDeviceId(DeviceDirection direction) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return defaultDeviceIdLocked(direction);
    }

    [[nodiscard]] bool deviceExists(const std::string& deviceId) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enumerator_ || deviceId.empty()) return false;

        ComPtr<IMMDevice> device;
        const std::wstring wide = toUtf16(deviceId);
        if (FAILED(enumerator_->GetDevice(wide.c_str(), device.put()))) return false;

        DWORD state = 0;
        if (FAILED(device->GetState(&state))) return false;
        return state == DEVICE_STATE_ACTIVE;
    }

    [[nodiscard]] std::unique_ptr<AudioStream> openRenderStream(
        const StreamConfig& config, RenderCallback* callback, Status& status) override {
        return openStream(config, true, callback, nullptr, status);
    }

    [[nodiscard]] std::unique_ptr<AudioStream> openCaptureStream(
        const StreamConfig& config, CaptureCallback* callback, Status& status) override {
        return openStream(config, false, nullptr, callback, status);
    }

    [[nodiscard]] bool supportsProcessLoopback() const noexcept override {
        // ActivateAudioInterfaceAsync z parametrami process loopback wymaga
        // Windows 10 w wersji 20H1 (build 19041) lub nowszej.
        return true;
    }

    void setDeviceChangeHandler(DeviceChangeHandler handler) override {
        std::lock_guard<std::mutex> lock(handlerMutex_);
        handler_ = std::move(handler);
    }

private:
    void dispatchChange(const DeviceChangeEvent& event) {
        DeviceChangeHandler handler;
        {
            std::lock_guard<std::mutex> lock(handlerMutex_);
            handler = handler_;
        }
        if (handler) handler(event);
    }

    [[nodiscard]] std::string defaultDeviceIdLocked(DeviceDirection direction) {
        if (!enumerator_) return {};
        const EDataFlow flow = (direction == DeviceDirection::Render) ? eRender : eCapture;

        ComPtr<IMMDevice> device;
        if (FAILED(enumerator_->GetDefaultAudioEndpoint(flow, eConsole, device.put()))) return {};

        CoMem<WCHAR> id;
        if (FAILED(device->GetId(id.put()))) return {};
        return toUtf8(id.get());
    }

    [[nodiscard]] static DeviceInfo describeDevice(IMMDevice* device, DeviceDirection direction) {
        DeviceInfo info;
        info.direction = direction;

        CoMem<WCHAR> id;
        if (FAILED(device->GetId(id.put()))) return info;
        info.id = toUtf8(id.get());

        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, properties.put()))) {
            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(kPropertyDeviceFriendlyName, &value)) &&
                value.vt == VT_LPWSTR) {
                info.name = toUtf8(value.pwszVal);
            }
            PropVariantClear(&value);
        }
        if (info.name.empty()) info.name = info.id;

        // Format miksu zdradza natywną częstotliwość i liczbę kanałów.
        ComPtr<IAudioClient> client;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       client.putVoid()))) {
            CoMem<WAVEFORMATEX> mixFormat;
            if (SUCCEEDED(client->GetMixFormat(mixFormat.put())) && mixFormat.get() != nullptr) {
                info.defaultSampleRate = static_cast<double>(mixFormat.get()->nSamplesPerSec);
                info.maxChannels = mixFormat.get()->nChannels;
            }

            REFERENCE_TIME defaultPeriod = 0;
            REFERENCE_TIME minimumPeriod = 0;
            if (SUCCEEDED(client->GetDevicePeriod(&defaultPeriod, &minimumPeriod))) {
                info.defaultBufferFrames = static_cast<int>(
                    info.defaultSampleRate * static_cast<double>(defaultPeriod) / kRefTimesPerSecond);
                info.minBufferFrames = std::max(16, static_cast<int>(
                    info.defaultSampleRate * static_cast<double>(minimumPeriod) / kRefTimesPerSecond));
            }

            // Sprawdzenie, które z typowych częstotliwości urządzenie przyjmie.
            for (double rate : {44100.0, 48000.0, 88200.0, 96000.0, 192000.0}) {
                WAVEFORMATEXTENSIBLE candidate = makeFloatFormat(rate, info.maxChannels);
                CoMem<WAVEFORMATEX> closest;
                const HRESULT supported = client->IsFormatSupported(
                    AUDCLNT_SHAREMODE_SHARED, &candidate.Format, closest.put());
                if (supported == S_OK || supported == S_FALSE)
                    info.supportedSampleRates.push_back(rate);
            }
            if (info.supportedSampleRates.empty())
                info.supportedSampleRates.push_back(info.defaultSampleRate);
        }

        info.supportsLoopback = (direction == DeviceDirection::Render);
        info.isVirtual = info.name.find("Helix") != std::string::npos;
        return info;
    }

    /// Aktywuje IAudioClient dla przechwytywania audio konkretnego procesu.
    [[nodiscard]] static ComPtr<IAudioClient> activateProcessLoopback(std::uint32_t processId,
                                                                      bool includeTree,
                                                                      Status& status) {
        ComPtr<IAudioClient> client;

        AUDIOCLIENT_ACTIVATION_PARAMS params{};
        params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
        params.ProcessLoopbackParams.TargetProcessId = static_cast<DWORD>(processId);
        params.ProcessLoopbackParams.ProcessLoopbackMode =
            includeTree ? PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
                        : PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE;

        PROPVARIANT activation;
        PropVariantInit(&activation);
        activation.vt = VT_BLOB;
        activation.blob.cbSize = sizeof(params);
        activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        auto* handler = new ActivationHandler();
        ComPtr<IActivateAudioInterfaceAsyncOperation> operation;

        const HRESULT started = ActivateAudioInterfaceAsync(
            VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &activation,
            handler, operation.put());

        if (FAILED(started)) {
            handler->Release();
            status = Status::error("Przechwytywanie aplikacji niedostępne: " + describeHresult(started));
            return client;
        }

        HRESULT activationResult = E_FAIL;
        IUnknown* activated = handler->wait(3000, activationResult);
        handler->Release();

        if (FAILED(activationResult) || activated == nullptr) {
            status = Status::error("Aktywacja przechwytywania aplikacji: " +
                                   describeHresult(activationResult));
            if (activated != nullptr) activated->Release();
            return client;
        }

        const HRESULT queried = activated->QueryInterface(__uuidof(IAudioClient), client.putVoid());
        activated->Release();
        if (FAILED(queried))
            status = Status::error("IAudioClient: " + describeHresult(queried));
        return client;
    }

    [[nodiscard]] std::unique_ptr<AudioStream> openStream(const StreamConfig& config, bool isRender,
                                                          RenderCallback* render,
                                                          CaptureCallback* capture, Status& status) {
        std::lock_guard<std::mutex> lock(mutex_);
        status = Status::success();

        if (!initialized_) {
            status = Status::error("Backend WASAPI nie został zainicjalizowany");
            return nullptr;
        }

        ComPtr<IAudioClient> client;
        const bool processLoopback = (!isRender && config.processId != 0);

        if (processLoopback) {
            client = activateProcessLoopback(config.processId, config.includeProcessTree, status);
            if (!client) return nullptr;
        } else {
            ComPtr<IMMDevice> device;
            const std::wstring wide = toUtf16(config.deviceId);
            HRESULT result = enumerator_->GetDevice(wide.c_str(), device.put());
            if (FAILED(result)) {
                status = Status::error("Urządzenie niedostępne: " + describeHresult(result));
                return nullptr;
            }

            result = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, client.putVoid());
            if (FAILED(result)) {
                status = Status::error("Aktywacja urządzenia: " + describeHresult(result));
                return nullptr;
            }
        }

        // Dobór formatu.
        WAVEFORMATEXTENSIBLE requested = makeFloatFormat(config.sampleRate, config.channels);
        const WAVEFORMATEX* chosen = nullptr;
        CoMem<WAVEFORMATEX> mixFormat;
        WAVEFORMATEXTENSIBLE exclusiveFormat{};

        if (processLoopback) {
            // Process loopback wymaga jawnego formatu — WASAPI nie poda tu miksu.
            requested = makeFloatFormat(config.sampleRate, config.channels);
            chosen = &requested.Format;
        } else if (config.exclusive) {
            CoMem<WAVEFORMATEX> closest;
            HRESULT supported = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                          &requested.Format, closest.put());
            if (supported == S_OK) {
                chosen = &requested.Format;
            } else if (closest.get() != nullptr) {
                chosen = closest.get();
            } else {
                // Ostatnia szansa: 16-bit PCM o żądanych parametrach.
                exclusiveFormat = requested;
                exclusiveFormat.Format.wBitsPerSample = 16;
                exclusiveFormat.Samples.wValidBitsPerSample = 16;
                exclusiveFormat.Format.nBlockAlign =
                    static_cast<WORD>(exclusiveFormat.Format.nChannels * 2);
                exclusiveFormat.Format.nAvgBytesPerSec =
                    exclusiveFormat.Format.nSamplesPerSec * exclusiveFormat.Format.nBlockAlign;
                exclusiveFormat.SubFormat = kSubFormatPcm;

                supported = client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE,
                                                      &exclusiveFormat.Format, nullptr);
                if (supported != S_OK) {
                    status = Status::error("Tryb wyłączny nie obsługuje żądanego formatu");
                    return nullptr;
                }
                chosen = &exclusiveFormat.Format;
            }
        } else {
            // Tryb współdzielony pracuje na formacie miksu — to jedyny format,
            // jaki mikser Windows przyjmie bez własnej konwersji.
            const HRESULT result = client->GetMixFormat(mixFormat.put());
            if (FAILED(result) || mixFormat.get() == nullptr) {
                status = Status::error("GetMixFormat: " + describeHresult(result));
                return nullptr;
            }
            chosen = mixFormat.get();
        }

        const SampleFormat sampleFormat = detectSampleFormat(chosen);
        if (sampleFormat == SampleFormat::Unsupported) {
            status = Status::error("Nieobsługiwany format próbek urządzenia");
            return nullptr;
        }

        const int    deviceChannels = chosen->nChannels;
        const double deviceRate     = static_cast<double>(chosen->nSamplesPerSec);

        DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
        if (!isRender && config.loopback && config.processId == 0)
            flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        if (processLoopback)
            flags |= AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;
        if (!config.exclusive && !processLoopback)
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

        // Loopback urządzenia wyjściowego nie może pracować w trybie zdarzeniowym
        // na wszystkich sterownikach — dla niego korzystamy z odpytywania.
        const bool eventDriven = !(config.loopback && config.processId == 0 && !isRender);
        if (!eventDriven) flags &= ~static_cast<DWORD>(AUDCLNT_STREAMFLAGS_EVENTCALLBACK);

        const REFERENCE_TIME duration =
            framesToReferenceTime(std::max(config.bufferFrames * 4, 480), deviceRate);

        HRESULT result = client->Initialize(
            config.exclusive ? AUDCLNT_SHAREMODE_EXCLUSIVE : AUDCLNT_SHAREMODE_SHARED,
            flags, duration, config.exclusive ? duration : 0, chosen, nullptr);

        if (result == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED && config.exclusive) {
            // Dopasowanie bufora zgodnie z zaleceniem sterownika.
            UINT32 alignedFrames = 0;
            if (SUCCEEDED(client->GetBufferSize(&alignedFrames)) && alignedFrames > 0) {
                const REFERENCE_TIME aligned =
                    framesToReferenceTime(static_cast<int>(alignedFrames), deviceRate);
                result = client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, flags, aligned, aligned,
                                            chosen, nullptr);
            }
        }

        if (FAILED(result)) {
            status = Status::error("Inicjalizacja strumienia: " + describeHresult(result));
            return nullptr;
        }

        UINT32 bufferFrames = 0;
        client->GetBufferSize(&bufferFrames);

        REFERENCE_TIME latency = 0;
        client->GetStreamLatency(&latency);
        const double latencyMs = static_cast<double>(latency) / 10000.0;

        Log::info(kLog, std::string(isRender ? "Wyjście" : "Wejście") + ": " +
                            std::to_string(static_cast<int>(deviceRate)) + " Hz, " +
                            std::to_string(deviceChannels) + " kan., bufor " +
                            std::to_string(bufferFrames) + " ramek");

        return std::make_unique<WasapiStream>(std::move(client), config, isRender, render, capture,
                                              sampleFormat, deviceChannels, deviceRate,
                                              static_cast<int>(bufferFrames), latencyMs);
    }

    std::mutex mutex_;
    std::mutex handlerMutex_;
    std::unique_ptr<ComApartment>  apartment_;
    ComPtr<IMMDeviceEnumerator>    enumerator_;
    WasapiNotificationClient*      notificationClient_ = nullptr;
    DeviceChangeHandler            handler_;
    bool                           initialized_ = false;
};

std::unique_ptr<AudioBackend> createWasapiBackend() {
    auto backend = std::make_unique<WasapiBackend>();
    return backend;
}

} // namespace helix::core
