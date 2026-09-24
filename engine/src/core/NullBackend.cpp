#include "helix/core/NullBackend.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include "helix/Log.h"

namespace helix::core {

namespace {
constexpr const char* kLog = "NullBackend";

DeviceInfo makeDevice(std::string id, std::string name, DeviceDirection direction,
                      bool isDefault, bool loopback = false) {
    DeviceInfo info;
    info.id = std::move(id);
    info.name = std::move(name);
    info.direction = direction;
    info.isDefault = isDefault;
    info.supportsLoopback = loopback;
    info.maxChannels = 2;
    info.defaultSampleRate = kDefaultSampleRate;
    info.defaultBufferFrames = kDefaultBlockFrames;
    info.minBufferFrames = 32;
    info.supportedSampleRates = {44100.0, 48000.0, 96000.0};
    return info;
}
} // namespace

/// Strumień symulowany — trzyma bufor roboczy i woła callback aplikacji.
class NullBackend::Stream final : public AudioStream {
public:
    Stream(NullBackend& backend, StreamConfig config, RenderCallback* render, CaptureCallback* capture)
        : backend_(backend), config_(std::move(config)), render_(render), capture_(capture) {
        buffer_.assign(static_cast<std::size_t>(config_.bufferFrames) *
                           static_cast<std::size_t>(config_.channels), 0.0f);
        backend_.registerStream(this);
    }

    ~Stream() override {
        stop();
        backend_.unregisterStream(this);
    }

    Status start() override {
        running_.store(true, std::memory_order_release);
        return Status::success();
    }

    void stop() override { running_.store(false, std::memory_order_release); }

    [[nodiscard]] bool isRunning() const override { return running_.load(std::memory_order_acquire); }
    [[nodiscard]] double sampleRate() const override { return config_.sampleRate; }
    [[nodiscard]] int channels() const override { return config_.channels; }
    [[nodiscard]] int bufferFrames() const override { return config_.bufferFrames; }

    [[nodiscard]] double latencyMs() const override {
        return 1000.0 * static_cast<double>(config_.bufferFrames) / config_.sampleRate;
    }

    [[nodiscard]] std::uint64_t glitchCount() const override { return 0; }

    [[nodiscard]] const std::string& deviceId() const noexcept { return config_.deviceId; }
    [[nodiscard]] bool isRender() const noexcept { return render_ != nullptr; }

    /// Jeden blok pracy urządzenia.
    void pump(int frames, const NullBackend::CaptureGenerator& generator) {
        if (!isRunning()) return;
        const int n = std::min(frames, config_.bufferFrames);
        const std::size_t total = static_cast<std::size_t>(n) * static_cast<std::size_t>(config_.channels);

        if (render_ != nullptr) {
            std::memset(buffer_.data(), 0, total * sizeof(Sample));
            render_->renderBlock(buffer_.data(), n, config_.channels);
            backend_.storeRendered(config_.deviceId, buffer_.data(), total);
        } else if (capture_ != nullptr) {
            std::memset(buffer_.data(), 0, total * sizeof(Sample));
            if (generator) generator(config_.deviceId, buffer_.data(), n, config_.channels);
            capture_->captureBlock(buffer_.data(), n, config_.channels);
        }
    }

private:
    NullBackend&        backend_;
    StreamConfig        config_;
    RenderCallback*     render_ = nullptr;
    CaptureCallback*    capture_ = nullptr;
    std::vector<Sample> buffer_;
    std::atomic<bool>   running_{false};
};

NullBackend::NullBackend(bool freeRunning) : freeRunning_(freeRunning) {
    devices_.push_back(makeDevice("null:speakers",   "Głośniki (symulowane)", DeviceDirection::Render, true, true));
    devices_.push_back(makeDevice("null:headphones", "Słuchawki (symulowane)", DeviceDirection::Render, false, true));
    devices_.push_back(makeDevice("null:microphone", "Mikrofon (symulowany)", DeviceDirection::Capture, true));
}

NullBackend::~NullBackend() { shutdown(); }

Status NullBackend::initialize() {
    running_.store(true, std::memory_order_release);
    if (freeRunning_ && !clock_)
        clock_ = std::make_unique<std::thread>([this] { clockLoop(); });
    Log::info(kLog, "Backend programowy uruchomiony");
    return Status::success();
}

void NullBackend::shutdown() {
    running_.store(false, std::memory_order_release);
    if (clock_ && clock_->joinable()) clock_->join();
    clock_.reset();
}

std::vector<DeviceInfo> NullBackend::enumerateDevices(DeviceDirection direction) {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<DeviceInfo> result;
    for (const auto& device : devices_)
        if (device.direction == direction) result.push_back(device);
    return result;
}

std::string NullBackend::defaultDeviceId(DeviceDirection direction) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& device : devices_)
        if (device.direction == direction && device.isDefault) return device.id;
    return {};
}

bool NullBackend::deviceExists(const std::string& deviceId) {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::any_of(devices_.begin(), devices_.end(),
                       [&](const DeviceInfo& d) { return d.id == deviceId; });
}

std::unique_ptr<AudioStream> NullBackend::openRenderStream(const StreamConfig& config,
                                                           RenderCallback* callback, Status& status) {
    if (callback == nullptr) {
        status = Status::error("Brak callbacku renderowania");
        return nullptr;
    }
    if (!deviceExists(config.deviceId)) {
        status = Status::error("Urządzenie nie istnieje: " + config.deviceId);
        return nullptr;
    }
    status = Status::success();
    return std::make_unique<Stream>(*this, config, callback, nullptr);
}

std::unique_ptr<AudioStream> NullBackend::openCaptureStream(const StreamConfig& config,
                                                            CaptureCallback* callback, Status& status) {
    if (callback == nullptr) {
        status = Status::error("Brak callbacku przechwytywania");
        return nullptr;
    }
    // Process loopback nie wymaga istniejącego urządzenia — identyfikuje go PID.
    if (config.processId == 0 && !deviceExists(config.deviceId)) {
        status = Status::error("Urządzenie nie istnieje: " + config.deviceId);
        return nullptr;
    }
    status = Status::success();
    return std::make_unique<Stream>(*this, config, nullptr, callback);
}

void NullBackend::setDeviceChangeHandler(DeviceChangeHandler handler) {
    std::lock_guard<std::mutex> lock(mutex_);
    handler_ = std::move(handler);
}

void NullBackend::setCaptureGenerator(CaptureGenerator generator) {
    std::lock_guard<std::mutex> lock(mutex_);
    generator_ = std::move(generator);
}

void NullBackend::pump(int frames) {
    std::vector<Stream*> snapshot;
    CaptureGenerator generator;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        snapshot = streams_;
        generator = generator_;
    }
    // Najpierw wejścia, potem wyjścia — dane z tego bloku trafiają od razu na wyjście.
    for (Stream* stream : snapshot)
        if (!stream->isRender()) stream->pump(frames, generator);
    for (Stream* stream : snapshot)
        if (stream->isRender()) stream->pump(frames, generator);
}

void NullBackend::addDevice(DeviceInfo info) {
    DeviceChangeEvent event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(devices_.begin(), devices_.end(),
                                     [&](const DeviceInfo& d) { return d.id == info.id; });
        event.type = (it == devices_.end()) ? DeviceChangeEvent::Type::Added
                                            : DeviceChangeEvent::Type::StateChanged;
        event.deviceId = info.id;
        event.direction = info.direction;
        if (it == devices_.end()) devices_.push_back(std::move(info));
        else *it = std::move(info);
    }
    notify(event);
}

bool NullBackend::removeDevice(const std::string& deviceId) {
    DeviceChangeEvent event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(devices_.begin(), devices_.end(),
                                     [&](const DeviceInfo& d) { return d.id == deviceId; });
        if (it == devices_.end()) return false;
        event.type = DeviceChangeEvent::Type::Removed;
        event.deviceId = deviceId;
        event.direction = it->direction;
        devices_.erase(it);
    }
    notify(event);
    return true;
}

void NullBackend::setDefaultDevice(DeviceDirection direction, const std::string& deviceId) {
    DeviceChangeEvent event;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& device : devices_)
            if (device.direction == direction) device.isDefault = (device.id == deviceId);
        event.type = DeviceChangeEvent::Type::DefaultChanged;
        event.deviceId = deviceId;
        event.direction = direction;
    }
    notify(event);
}

std::vector<Sample> NullBackend::lastRenderedBlock(const std::string& deviceId) const {
    std::lock_guard<std::mutex> lock(renderedMutex_);
    for (const auto& [id, data] : rendered_)
        if (id == deviceId) return data;
    return {};
}

void NullBackend::storeRendered(const std::string& deviceId, const Sample* data, std::size_t count) {
    std::lock_guard<std::mutex> lock(renderedMutex_);
    for (auto& [id, stored] : rendered_) {
        if (id == deviceId) {
            stored.assign(data, data + count);
            return;
        }
    }
    rendered_.emplace_back(deviceId, std::vector<Sample>(data, data + count));
}

void NullBackend::registerStream(Stream* stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    streams_.push_back(stream);
}

void NullBackend::unregisterStream(Stream* stream) {
    std::lock_guard<std::mutex> lock(mutex_);
    streams_.erase(std::remove(streams_.begin(), streams_.end(), stream), streams_.end());
}

void NullBackend::notify(const DeviceChangeEvent& event) {
    DeviceChangeHandler handler;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        handler = handler_;
    }
    if (handler) handler(event);
}

void NullBackend::clockLoop() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();

    while (running_.load(std::memory_order_acquire)) {
        int frames = kDefaultBlockFrames;
        double sampleRate = kDefaultSampleRate;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (Stream* stream : streams_) {
                if (stream->isRender() && stream->isRunning()) {
                    frames = stream->bufferFrames();
                    sampleRate = stream->sampleRate();
                    break;
                }
            }
        }

        pump(frames);

        const auto period = std::chrono::duration_cast<clock::duration>(
            std::chrono::duration<double>(static_cast<double>(frames) / sampleRate));
        next += period;
        const auto now = clock::now();
        if (next > now) std::this_thread::sleep_for(next - now);
        else next = now;   // spóźnienie — nie nadrabiamy w nieskończoność
    }
}

std::unique_ptr<AudioBackend> createNullBackend(bool freeRunning) {
    return std::make_unique<NullBackend>(freeRunning);
}

} // namespace helix::core
