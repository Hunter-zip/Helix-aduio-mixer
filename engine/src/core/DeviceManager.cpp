#include "helix/core/DeviceManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

#include "helix/AudioBuffer.h"
#include "helix/Log.h"
#include "helix/dsp/Resampler.h"

namespace helix::core {

namespace {
constexpr const char* kLog = "DeviceManager";

/// Docelowe wypełnienie bufora pośredniczącego. Odchyłki koryguje resampler,
/// dzięki czemu zegary karty i silnika nie rozjeżdżają się w nieskończoność.
constexpr double kDriftGain = 2.0e-6;
} // namespace

// ── Wyjście magistrali pobocznej ────────────────────────────────────────────

/// Czyta gotowy blok z magistrali i podaje go urządzeniu, w razie potrzeby
/// przeliczając częstotliwość próbkowania.
class DeviceManager::BusSink final : public RenderCallback {
public:
    BusSink(Bus& bus, double engineRate, int deviceChannels, double deviceRate, int deviceFrames)
        : bus_(bus), deviceChannels_(deviceChannels) {
        planar_.resize(std::max(1, deviceChannels), std::max(deviceFrames, kDefaultBlockFrames));
        engineSide_.resize(std::max(1, deviceChannels), std::max(deviceFrames, kDefaultBlockFrames) * 4);
        resampler_.prepare(deviceChannels, engineRate, deviceRate,
                           std::max(deviceFrames, kDefaultBlockFrames));
        passthrough_ = std::fabs(engineRate - deviceRate) < 1.0e-6;
    }

    void renderBlock(Sample* interleaved, int frames, int channels) noexcept override {
        const std::size_t total = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);

        if (passthrough_) {
            bus_.output().readInterleaved(interleaved, channels, frames);
            updateDrift();
            return;
        }

        // Dokarm resampler tyle, ile potrzeba na ten blok.
        const int needed = static_cast<int>(std::ceil(frames * resampler_.ratio())) + 4;
        Sample* pointers[kMaxStreamChannels];
        const int useChannels = std::min(resampler_.channels(), kMaxStreamChannels);
        for (int c = 0; c < useChannels; ++c) pointers[c] = engineSide_.channel(c);

        const int chunk = std::min(needed, engineSide_.capacity());
        bus_.output().readPlanar(pointers, useChannels, chunk);
        resampler_.pushInput(pointers, chunk);

        Sample* outputs[kMaxStreamChannels];
        for (int c = 0; c < useChannels; ++c) outputs[c] = planar_.channel(c);
        const int produced = resampler_.pullOutput(outputs, std::min(frames, planar_.capacity()));

        std::memset(interleaved, 0, total * sizeof(Sample));
        for (int c = 0; c < channels; ++c) {
            const Sample* src = planar_.channel(std::min(c, useChannels - 1));
            for (int i = 0; i < produced; ++i)
                interleaved[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)
                            + static_cast<std::size_t>(c)] = src[i];
        }
        updateDrift();
    }

private:
    void updateDrift() noexcept {
        // Regulator proporcjonalny: trzymamy bufor w połowie pojemności.
        const double target = bus_.output().capacityFrames() * 0.5;
        const double fill   = bus_.output().availableToRead();
        const double error  = fill - target;
        resampler_.setDriftCompensation(1.0 + error * kDriftGain);
    }

    Bus&            bus_;
    int             deviceChannels_;
    bool            passthrough_ = true;
    AudioBuffer     planar_;
    AudioBuffer     engineSide_;
    dsp::Resampler  resampler_;
};

// ── Wejście źródła ──────────────────────────────────────────────────────────

/// Odbiera blok z urządzenia, przelicza na format silnika i wrzuca do bufora źródła.
class DeviceManager::SourceFeed final : public CaptureCallback {
public:
    SourceFeed(InputSource& source, double engineRate, int engineChannels,
               double deviceRate, int deviceFrames)
        : source_(source), engineChannels_(engineChannels) {
        const int capacity = std::max(deviceFrames, kDefaultBlockFrames) * 4;
        planar_.resize(engineChannels, capacity);
        interleaved_.assign(static_cast<std::size_t>(capacity) *
                                static_cast<std::size_t>(engineChannels), 0.0f);
        resampler_.prepare(engineChannels, deviceRate, engineRate, capacity);
        passthrough_ = std::fabs(engineRate - deviceRate) < 1.0e-6;
    }

    void captureBlock(const Sample* interleaved, int frames, int channels) noexcept override {
        if (passthrough_ && channels == engineChannels_) {
            source_.ring().write(interleaved, frames);
            updateDrift();
            return;
        }

        const int useChannels = std::min(engineChannels_, kMaxStreamChannels);

        if (passthrough_) {
            // Sama zmiana liczby kanałów — przepisujemy z powielaniem/obcięciem.
            const int n = std::min(frames, planar_.capacity());
            for (int c = 0; c < useChannels; ++c) {
                Sample* dst = planar_.channel(c);
                const int srcChannel = std::min(c, channels - 1);
                for (int i = 0; i < n; ++i)
                    dst[i] = interleaved[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)
                                         + static_cast<std::size_t>(srcChannel)];
            }
            writeToRing(n, useChannels);
            updateDrift();
            return;
        }

        resampler_.pushInterleaved(interleaved, frames, channels);

        Sample* outputs[kMaxStreamChannels];
        for (int c = 0; c < useChannels; ++c) outputs[c] = planar_.channel(c);
        const int produced = resampler_.pullOutput(outputs, planar_.capacity());
        writeToRing(produced, useChannels);
        updateDrift();
    }

private:
    void writeToRing(int frames, int channels) noexcept {
        if (frames <= 0) return;
        Sample* out = interleaved_.data();
        for (int c = 0; c < channels; ++c) {
            const Sample* src = planar_.channel(c);
            for (int i = 0; i < frames; ++i)
                out[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)
                    + static_cast<std::size_t>(c)] = src[i];
        }
        source_.ring().write(out, frames);
    }

    void updateDrift() noexcept {
        const double target = source_.ring().capacityFrames() * 0.25;
        const double fill   = source_.ring().availableToRead();
        const double error  = fill - target;
        // Za dużo w buforze → produkuj mniej (zwiększ współczynnik).
        resampler_.setDriftCompensation(1.0 + error * kDriftGain);
    }

    InputSource&        source_;
    int                 engineChannels_;
    bool                passthrough_ = true;
    AudioBuffer         planar_;
    std::vector<Sample> interleaved_;
    dsp::Resampler      resampler_;
};

// ── DeviceManager ───────────────────────────────────────────────────────────

DeviceManager::DeviceManager(AudioEngine& engine, AudioBackend& backend)
    : engine_(engine), backend_(backend) {}

DeviceManager::~DeviceManager() { shutdown(); }

Status DeviceManager::initialize() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (initialized_) return Status::success();

    const Status status = backend_.initialize();
    if (!status) return status;

    backend_.setDeviceChangeHandler([this](const DeviceChangeEvent& event) { onDeviceChange(event); });
    initialized_ = true;
    Log::info(kLog, std::string("Backend audio: ") + backend_.name());
    return Status::success();
}

void DeviceManager::shutdown() {
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return;
    backend_.setDeviceChangeHandler(nullptr);
    busBindings_.clear();
    sourceBindings_.clear();
    backend_.shutdown();
    initialized_ = false;
}

std::vector<DeviceInfo> DeviceManager::devices(DeviceDirection direction) const {
    return const_cast<AudioBackend&>(backend_).enumerateDevices(direction);
}

std::string DeviceManager::defaultDevice(DeviceDirection direction) const {
    return const_cast<AudioBackend&>(backend_).defaultDeviceId(direction);
}

DeviceSettings DeviceManager::settings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return settings_;
}

Status DeviceManager::applySettings(const DeviceSettings& settings) {
    const bool wasRunning = isRunning();
    stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        settings_ = settings;
    }

    EngineFormat format;
    format.sampleRate  = settings.sampleRate;
    format.blockFrames = settings.blockFrames;
    format.channels    = settings.channels;

    const Status status = engine_.configure(format);
    if (!status) return status;

    if (wasRunning) return start();
    return Status::success();
}

Status DeviceManager::bindBus(BusId busId, const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);

    Bus* bus = engine_.findBus(busId);
    if (bus == nullptr) return Status::error("Nieznana magistrala");

    BusBinding* binding = findBusLocked(busId);
    if (binding == nullptr) {
        busBindings_.push_back(BusBinding{busId, {}, nullptr, nullptr, {}});
        binding = &busBindings_.back();
    }

    closeBusStreamLocked(*binding);
    binding->deviceId = deviceId;
    bus->setDeviceId(deviceId);
    bus->setDeviceName(deviceNameLocked(deviceId));
    bus->setDeviceReady(false);

    if (deviceId.empty()) return Status::success();
    if (!running_) return Status::success();

    return openBusStreamLocked(*binding);
}

BindingState DeviceManager::busBinding(BusId busId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    BindingState state;
    for (const auto& binding : busBindings_) {
        if (binding.busId != busId) continue;
        state.deviceId  = binding.deviceId;
        state.deviceName = deviceNameLocked(binding.deviceId);
        state.open      = binding.stream != nullptr && binding.stream->isRunning();
        state.lastError = binding.lastError;
        if (binding.stream) {
            state.latencyMs = binding.stream->latencyMs();
            state.deviceSampleRate = binding.stream->sampleRate();
            state.glitches = binding.stream->glitchCount();
        }
        break;
    }
    return state;
}

Status DeviceManager::bindSourceToDevice(SourceId sourceId, const std::string& deviceId, bool loopback) {
    std::lock_guard<std::mutex> lock(mutex_);

    InputSource* source = engine_.findSource(sourceId);
    if (source == nullptr) return Status::error("Nieznane źródło");

    SourceBinding* binding = findSourceLocked(sourceId);
    if (binding == nullptr) {
        sourceBindings_.push_back(SourceBinding{sourceId, {}, 0, false, nullptr, nullptr, {}});
        binding = &sourceBindings_.back();
    }

    closeSourceStreamLocked(*binding);
    binding->deviceId  = deviceId;
    binding->processId = 0;
    binding->loopback  = loopback;
    source->setDeviceId(deviceId);
    source->setProcessId(0);

    if (deviceId.empty()) {
        source->setActive(false);
        return Status::success();
    }
    if (!running_) return Status::success();
    return openSourceStreamLocked(*binding);
}

Status DeviceManager::bindSourceToProcess(SourceId sourceId, std::uint32_t processId,
                                          const std::string& executable) {
    std::lock_guard<std::mutex> lock(mutex_);

    InputSource* source = engine_.findSource(sourceId);
    if (source == nullptr) return Status::error("Nieznane źródło");
    if (!backend_.supportsProcessLoopback())
        return Status::error("Backend nie obsługuje przechwytywania per aplikacja");

    SourceBinding* binding = findSourceLocked(sourceId);
    if (binding == nullptr) {
        sourceBindings_.push_back(SourceBinding{sourceId, {}, 0, false, nullptr, nullptr, {}});
        binding = &sourceBindings_.back();
    }

    closeSourceStreamLocked(*binding);
    binding->deviceId  = {};
    binding->processId = processId;
    binding->loopback  = true;
    source->setProcessId(processId);
    if (!executable.empty()) source->setProcessName(executable);

    if (processId == 0) {
        source->setActive(false);
        return Status::success();
    }
    if (!running_) return Status::success();
    return openSourceStreamLocked(*binding);
}

void DeviceManager::unbindSource(SourceId sourceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    SourceBinding* binding = findSourceLocked(sourceId);
    if (binding == nullptr) return;
    closeSourceStreamLocked(*binding);
    binding->deviceId.clear();
    binding->processId = 0;
    if (InputSource* source = engine_.findSource(sourceId)) source->setActive(false);
}

BindingState DeviceManager::sourceBinding(SourceId sourceId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    BindingState state;
    for (const auto& binding : sourceBindings_) {
        if (binding.sourceId != sourceId) continue;
        state.deviceId  = binding.deviceId;
        state.deviceName = deviceNameLocked(binding.deviceId);
        state.open      = binding.stream != nullptr && binding.stream->isRunning();
        state.lastError = binding.lastError;
        if (binding.stream) {
            state.latencyMs = binding.stream->latencyMs();
            state.deviceSampleRate = binding.stream->sampleRate();
            state.glitches = binding.stream->glitchCount();
        }
        break;
    }
    return state;
}

Status DeviceManager::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!initialized_) return Status::error("Backend audio nie został zainicjalizowany");
    if (running_) return Status::success();

    running_ = true;
    engine_.setRunning(true);

    Status result = Status::success();
    for (auto& binding : busBindings_) {
        if (binding.deviceId.empty()) continue;
        const Status status = openBusStreamLocked(binding);
        if (!status && result) result = status;  // zapamiętaj pierwszy błąd, ale jedź dalej
    }
    for (auto& binding : sourceBindings_) {
        if (binding.deviceId.empty() && binding.processId == 0) continue;
        const Status status = openSourceStreamLocked(binding);
        if (!status && result) result = status;
    }
    return result;
}

void DeviceManager::stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_) return;

    engine_.setRunning(false);
    for (auto& binding : busBindings_) closeBusStreamLocked(binding);
    for (auto& binding : sourceBindings_) closeSourceStreamLocked(binding);
    running_ = false;
}

bool DeviceManager::isRunning() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

Status DeviceManager::openBusStreamLocked(BusBinding& binding) {
    Bus* bus = engine_.findBus(binding.busId);
    if (bus == nullptr) return Status::error("Nieznana magistrala");
    if (binding.deviceId.empty()) return Status::success();

    const EngineFormat format = engine_.format();

    StreamConfig config;
    config.deviceId     = binding.deviceId;
    config.sampleRate   = settings_.sampleRate;
    config.channels     = format.channels;
    config.bufferFrames = settings_.blockFrames;
    config.exclusive    = settings_.exclusive;

    Status status = Status::success();
    const bool isPrimary = (engine_.primaryBus() == binding.busId);

    std::unique_ptr<AudioStream> stream;
    if (isPrimary) {
        // Magistrala podstawowa napędza zegar silnika — bez pośredniego bufora (spec §14).
        binding.sink.reset();
        stream = backend_.openRenderStream(config, &engine_, status);
    } else {
        auto sink = std::make_unique<BusSink>(*bus, format.sampleRate, config.channels,
                                              config.sampleRate, config.bufferFrames);
        stream = backend_.openRenderStream(config, sink.get(), status);
        if (stream) binding.sink = std::move(sink);
    }

    if (!stream) {
        binding.lastError = status.message;
        bus->setDeviceReady(false);
        Log::warn(kLog, "Nie udało się otworzyć wyjścia " + binding.deviceId + ": " + status.message);
        return status;
    }

    const Status startStatus = stream->start();
    if (!startStatus) {
        binding.lastError = startStatus.message;
        bus->setDeviceReady(false);
        return startStatus;
    }

    binding.stream = std::move(stream);
    binding.lastError.clear();
    bus->setDeviceName(deviceNameLocked(binding.deviceId));
    bus->setDeviceReady(true);
    Log::info(kLog, "Wyjście " + bus->label() + " → " + binding.deviceId);
    return Status::success();
}

Status DeviceManager::openSourceStreamLocked(SourceBinding& binding) {
    InputSource* source = engine_.findSource(binding.sourceId);
    if (source == nullptr) return Status::error("Nieznane źródło");
    if (binding.deviceId.empty() && binding.processId == 0) return Status::success();

    const EngineFormat format = engine_.format();

    StreamConfig config;
    config.deviceId     = binding.deviceId;
    config.sampleRate   = settings_.sampleRate;
    config.channels     = format.channels;
    config.bufferFrames = settings_.blockFrames;
    config.loopback     = binding.loopback;
    config.processId    = binding.processId;

    auto feed = std::make_unique<SourceFeed>(*source, format.sampleRate, format.channels,
                                             config.sampleRate, config.bufferFrames);

    Status status = Status::success();
    auto stream = backend_.openCaptureStream(config, feed.get(), status);
    if (!stream) {
        binding.lastError = status.message;
        source->setActive(false);
        source->setStreaming(false);
        Log::warn(kLog, "Nie udało się otworzyć wejścia: " + status.message);
        return status;
    }

    const Status startStatus = stream->start();
    if (!startStatus) {
        binding.lastError = startStatus.message;
        source->setActive(false);
        return startStatus;
    }

    binding.feed   = std::move(feed);
    binding.stream = std::move(stream);
    binding.lastError.clear();
    source->ring().clear();
    source->setActive(true);
    source->setStreaming(true);
    return Status::success();
}

void DeviceManager::closeBusStreamLocked(BusBinding& binding) {
    if (binding.stream) {
        binding.stream->stop();
        binding.stream.reset();
    }
    binding.sink.reset();
    if (Bus* bus = engine_.findBus(binding.busId)) bus->setDeviceReady(false);
}

void DeviceManager::closeSourceStreamLocked(SourceBinding& binding) {
    if (binding.stream) {
        binding.stream->stop();
        binding.stream.reset();
    }
    binding.feed.reset();
    if (InputSource* source = engine_.findSource(binding.sourceId)) {
        source->setActive(false);
        source->setStreaming(false);
        source->ring().clear();
    }
}

DeviceManager::BusBinding* DeviceManager::findBusLocked(BusId bus) {
    const auto it = std::find_if(busBindings_.begin(), busBindings_.end(),
                                 [bus](const BusBinding& b) { return b.busId == bus; });
    return it == busBindings_.end() ? nullptr : &*it;
}

DeviceManager::SourceBinding* DeviceManager::findSourceLocked(SourceId source) {
    const auto it = std::find_if(sourceBindings_.begin(), sourceBindings_.end(),
                                 [source](const SourceBinding& s) { return s.sourceId == source; });
    return it == sourceBindings_.end() ? nullptr : &*it;
}

void DeviceManager::refreshDeviceCacheLocked() const {
    deviceCache_.clear();
    auto& backend = const_cast<AudioBackend&>(backend_);
    for (auto direction : {DeviceDirection::Render, DeviceDirection::Capture}) {
        auto devices = backend.enumerateDevices(direction);
        deviceCache_.insert(deviceCache_.end(), devices.begin(), devices.end());
    }
    deviceCacheValid_ = true;
}

std::string DeviceManager::deviceNameLocked(const std::string& deviceId) const {
    if (deviceId.empty()) return {};
    if (!deviceCacheValid_) refreshDeviceCacheLocked();

    for (const auto& device : deviceCache_)
        if (device.id == deviceId) return device.name;

    // Nieznane ID może znaczyć, że cache jest nieaktualny — jedna próba odświeżenia.
    refreshDeviceCacheLocked();
    for (const auto& device : deviceCache_)
        if (device.id == deviceId) return device.name;

    return deviceId;
}

void DeviceManager::onDeviceChange(const DeviceChangeEvent& event) {
    ChangeHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = changeHandler_;
    }

    switch (event.type) {
        case DeviceChangeEvent::Type::Removed:
            Log::warn(kLog, "Urządzenie odłączone: " + event.deviceId);
            break;
        case DeviceChangeEvent::Type::Added:
            Log::info(kLog, "Nowe urządzenie: " + event.deviceId);
            break;
        case DeviceChangeEvent::Type::DefaultChanged:
            Log::info(kLog, "Zmiana urządzenia domyślnego: " + event.deviceId);
            break;
        case DeviceChangeEvent::Type::FormatChanged:
            Log::info(kLog, "Zmiana formatu urządzenia: " + event.deviceId);
            break;
        case DeviceChangeEvent::Type::StateChanged:
            break;
    }

    refreshBindings();
    if (handler) handler(event);
}

void DeviceManager::refreshBindings() {
    std::lock_guard<std::mutex> lock(mutex_);
    deviceCacheValid_ = false;   // lista urządzeń mogła się zmienić
    if (!running_) return;

    for (auto& binding : busBindings_) {
        if (binding.deviceId.empty()) continue;
        const bool exists = backend_.deviceExists(binding.deviceId);
        const bool open = binding.stream != nullptr && binding.stream->isRunning();

        if (!exists && open) {
            closeBusStreamLocked(binding);
        } else if (exists && !open) {
            // Urządzenie wróciło — podnosimy strumień bez restartu programu (spec §23).
            const Status status = openBusStreamLocked(binding);
            if (!status) Log::warn(kLog, "Ponowne otwarcie nie powiodło się: " + status.message);
        }
    }

    for (auto& binding : sourceBindings_) {
        if (binding.deviceId.empty()) continue;
        const bool exists = backend_.deviceExists(binding.deviceId);
        const bool open = binding.stream != nullptr && binding.stream->isRunning();

        if (!exists && open) closeSourceStreamLocked(binding);
        else if (exists && !open) openSourceStreamLocked(binding);
    }
}

void DeviceManager::setChangeHandler(ChangeHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    changeHandler_ = std::move(handler);
}

double DeviceManager::primaryLatencyMs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    const BusId primary = engine_.primaryBus();
    for (const auto& binding : busBindings_)
        if (binding.busId == primary && binding.stream) return binding.stream->latencyMs();
    return 0.0;
}

double DeviceManager::totalLatencyMs() const {
    return engine_.estimatedLatencyMs(primaryLatencyMs());
}

// ── Test urządzenia (spec §20) ──────────────────────────────────────────────

namespace {

/// Generator sinusa 440 Hz z obwiednią, żeby test nie strzelał w głośniki.
class ToneCallback final : public RenderCallback {
public:
    ToneCallback(double sampleRate, double durationSeconds)
        : sampleRate_(sampleRate),
          totalFrames_(static_cast<std::int64_t>(sampleRate * durationSeconds)) {}

    void renderBlock(Sample* interleaved, int frames, int channels) noexcept override {
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        const double step = kTwoPi * 440.0 / sampleRate_;

        for (int i = 0; i < frames; ++i) {
            double envelope = 0.0;
            if (position_ < totalFrames_) {
                const double t = static_cast<double>(position_) / static_cast<double>(totalFrames_);
                envelope = 0.25 * std::sin(3.14159265358979 * t); // wejście i wyjście płynne
            }
            const auto value = static_cast<Sample>(std::sin(phase_) * envelope);
            for (int c = 0; c < channels; ++c)
                interleaved[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)
                            + static_cast<std::size_t>(c)] = value;
            phase_ += step;
            if (phase_ > kTwoPi) phase_ -= kTwoPi;
            ++position_;
        }
    }

    [[nodiscard]] bool finished() const noexcept { return position_ >= totalFrames_; }

private:
    double       sampleRate_;
    std::int64_t totalFrames_;
    std::int64_t position_ = 0;
    double       phase_ = 0.0;
};

} // namespace

Status DeviceManager::testDevice(const std::string& deviceId, double durationSeconds) {
    DeviceSettings currentSettings;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!initialized_) return Status::error("Backend audio nie został zainicjalizowany");
        currentSettings = settings_;
    }

    if (!backend_.deviceExists(deviceId))
        return Status::error("Urządzenie nie istnieje: " + deviceId);

    StreamConfig config;
    config.deviceId     = deviceId;
    config.sampleRate   = currentSettings.sampleRate;
    config.channels     = currentSettings.channels;
    config.bufferFrames = currentSettings.blockFrames;

    ToneCallback tone(config.sampleRate, durationSeconds);
    Status status = Status::success();
    auto stream = backend_.openRenderStream(config, &tone, status);
    if (!stream) return status;

    const Status startStatus = stream->start();
    if (!startStatus) return startStatus;

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(static_cast<int>(durationSeconds * 1000.0) + 250);
    while (!tone.finished() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    stream->stop();
    return Status::success();
}

} // namespace helix::core
