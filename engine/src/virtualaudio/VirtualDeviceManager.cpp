#include "helix/virtualaudio/VirtualDeviceManager.h"

#include <algorithm>
#include <chrono>

#include "helix/Log.h"

namespace helix::virtualaudio {

namespace {
constexpr const char* kLog = "VirtualAudio";

/// Zapas transportu: 16 bloków wystarcza, by klient o innym harmonogramie
/// nie gubił danych, a opóźnienie pozostało niskie.
constexpr int kTransportBlocks = 16;
} // namespace

const std::vector<VirtualEndpointSpec>& defaultVirtualEndpoints() {
    static const std::vector<VirtualEndpointSpec> kEndpoints{
        {"virtual-mixer-input",  "Virtual Mixer Input",  VirtualEndpointKind::Input,  2},
        {"virtual-mixer-output", "Virtual Mixer Output", VirtualEndpointKind::Output, 2},
        {"game-output",          "Game Output",          VirtualEndpointKind::Output, 2},
        {"chat-output",          "Chat Output",          VirtualEndpointKind::Output, 2},
        {"stream-output",        "Stream Output",        VirtualEndpointKind::Output, 2},
        {"microphone-output",    "Microphone Output",    VirtualEndpointKind::Output, 2},
    };
    return kEndpoints;
}

VirtualDeviceManager::VirtualDeviceManager(core::AudioEngine& engine,
                                           std::unique_ptr<DriverInterface> driver)
    : engine_(engine), driver_(std::move(driver)) {
    if (!driver_) driver_ = createSoftwareDriverInterface();
}

VirtualDeviceManager::~VirtualDeviceManager() { stop(); }

void VirtualDeviceManager::createDefaultEndpoints() {
    for (const auto& spec : defaultVirtualEndpoints()) {
        const Status status = addEndpoint(spec);
        if (!status) Log::warn(kLog, status.message);
    }
}

Status VirtualDeviceManager::addEndpoint(const VirtualEndpointSpec& spec) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (findLocked(spec.id) != nullptr)
        return Status::error("Punkt końcowy już istnieje: " + spec.id);

    auto endpoint = std::make_unique<Endpoint>();
    endpoint->spec = spec;
    endpoints_.push_back(std::move(endpoint));
    return Status::success();
}

bool VirtualDeviceManager::removeEndpoint(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(endpoints_.begin(), endpoints_.end(),
                                 [&](const auto& e) { return e->spec.id == id; });
    if (it == endpoints_.end()) return false;
    (*it)->transport.close();
    endpoints_.erase(it);
    return true;
}

Status VirtualDeviceManager::setEndpointEnabled(const std::string& id, bool enabled) {
    std::lock_guard<std::mutex> lock(mutex_);
    Endpoint* endpoint = findLocked(id);
    if (endpoint == nullptr) return Status::error("Nieznany punkt końcowy: " + id);

    endpoint->enabled = enabled;

    if (!enabled) {
        endpoint->transport.close();
        return Status::success();
    }

    if (!running_.load(std::memory_order_acquire)) return Status::success();
    if (endpoint->transport.isOpen()) return Status::success();

    const Status status = endpoint->transport.create(endpoint->spec.id, endpoint->spec.channels,
                                                     sampleRate_, blockFrames_ * kTransportBlocks);
    if (!status) {
        endpoint->enabled = false;
        return status;
    }

    endpoint->scratch.assign(static_cast<std::size_t>(blockFrames_ * kTransportBlocks) *
                                 static_cast<std::size_t>(endpoint->spec.channels), 0.0f);
    Log::info(kLog, "Transport gotowy: " + endpoint->spec.name);
    return Status::success();
}

Status VirtualDeviceManager::bindOutput(const std::string& id, BusId bus) {
    std::lock_guard<std::mutex> lock(mutex_);
    Endpoint* endpoint = findLocked(id);
    if (endpoint == nullptr) return Status::error("Nieznany punkt końcowy: " + id);
    if (endpoint->spec.kind != VirtualEndpointKind::Output)
        return Status::error("Punkt końcowy nie jest wyjściem: " + id);
    if (bus != kInvalidBus && engine_.findBus(bus) == nullptr)
        return Status::error("Nieznana magistrala");

    endpoint->bus = bus;
    return Status::success();
}

Status VirtualDeviceManager::bindInput(const std::string& id, SourceId source) {
    std::lock_guard<std::mutex> lock(mutex_);
    Endpoint* endpoint = findLocked(id);
    if (endpoint == nullptr) return Status::error("Nieznany punkt końcowy: " + id);
    if (endpoint->spec.kind != VirtualEndpointKind::Input)
        return Status::error("Punkt końcowy nie jest wejściem: " + id);
    if (source != kInvalidSource && engine_.findSource(source) == nullptr)
        return Status::error("Nieznane źródło");

    endpoint->source = source;
    return Status::success();
}

std::vector<VirtualEndpointState> VirtualDeviceManager::states() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<VirtualEndpointState> result;
    result.reserve(endpoints_.size());

    for (const auto& endpoint : endpoints_) {
        VirtualEndpointState state;
        state.id       = endpoint->spec.id;
        state.name     = endpoint->spec.name;
        state.kind     = endpoint->spec.kind;
        state.enabled  = endpoint->enabled;
        state.bus      = endpoint->bus;
        state.source   = endpoint->source;
        state.channels = endpoint->spec.channels;
        state.transportReady = endpoint->transport.isOpen();
        state.sampleRate = endpoint->transport.isOpen() ? endpoint->transport.sampleRate() : sampleRate_;
        state.clientConnected =
            endpoint->transport.isOpen() &&
            endpoint->transport.peerAlive(endpoint->spec.kind == VirtualEndpointKind::Output
                                              ? SharedRingRole::Consumer
                                              : SharedRingRole::Producer);
        state.overruns  = endpoint->transport.overruns();
        state.underruns = endpoint->transport.underruns();
        result.push_back(std::move(state));
    }
    return result;
}

Status VirtualDeviceManager::start(double sampleRate, int blockFrames) {
    stop();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        sampleRate_  = sampleRate > 0.0 ? sampleRate : kDefaultSampleRate;
        blockFrames_ = std::max(32, blockFrames);
    }

    running_.store(true, std::memory_order_release);

    // Otwórz transport dla wszystkich włączonych punktów końcowych.
    std::vector<std::string> enabledIds;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& endpoint : endpoints_)
            if (endpoint->enabled) enabledIds.push_back(endpoint->spec.id);
    }

    Status result = Status::success();
    for (const auto& id : enabledIds) {
        const Status status = setEndpointEnabled(id, true);
        if (!status && result) result = status;
    }

    worker_ = std::make_unique<std::thread>([this] { transportLoop(); });
    return result;
}

void VirtualDeviceManager::stop() {
    running_.store(false, std::memory_order_release);
    if (worker_ && worker_->joinable()) worker_->join();
    worker_.reset();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& endpoint : endpoints_) endpoint->transport.close();
}

VirtualDeviceManager::Endpoint* VirtualDeviceManager::findLocked(const std::string& id) {
    const auto it = std::find_if(endpoints_.begin(), endpoints_.end(),
                                 [&](const auto& e) { return e->spec.id == id; });
    return it == endpoints_.end() ? nullptr : it->get();
}

void VirtualDeviceManager::pump(int frames) {
    std::lock_guard<std::mutex> lock(mutex_);

    for (auto& endpoint : endpoints_) {
        if (!endpoint->enabled || !endpoint->transport.isOpen()) continue;

        const int channels = endpoint->spec.channels;
        const int capacity = static_cast<int>(endpoint->scratch.size() /
                                              static_cast<std::size_t>(std::max(1, channels)));
        const int count = std::min(frames, capacity);
        if (count <= 0) continue;

        if (endpoint->spec.kind == VirtualEndpointKind::Output) {
            core::Bus* bus = (endpoint->bus != kInvalidBus) ? engine_.findBus(endpoint->bus) : nullptr;
            if (bus == nullptr) continue;

            const int available = std::min(count, bus->output().availableToRead());
            if (available <= 0) continue;

            bus->output().readInterleaved(endpoint->scratch.data(), channels, available);
            endpoint->transport.write(endpoint->scratch.data(), available);
        } else {
            core::InputSource* source =
                (endpoint->source != kInvalidSource) ? engine_.findSource(endpoint->source) : nullptr;
            if (source == nullptr) continue;

            const int available = std::min(count, endpoint->transport.availableToRead());
            if (available <= 0) {
                source->setStreaming(endpoint->transport.peerAlive(SharedRingRole::Producer));
                continue;
            }

            endpoint->transport.read(endpoint->scratch.data(), available);
            source->ring().write(endpoint->scratch.data(), available);
            source->setActive(true);
            source->setStreaming(true);
        }
    }
}

void VirtualDeviceManager::transportLoop() {
    using clock = std::chrono::steady_clock;
    auto next = clock::now();

    int frames = blockFrames_;
    double sampleRate = sampleRate_;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frames = blockFrames_;
        sampleRate = sampleRate_;
    }

    // Transport chodzi z połową okresu bloku — z zapasem na wahania planisty.
    const auto period = std::chrono::duration_cast<clock::duration>(
        std::chrono::duration<double>(static_cast<double>(frames) / sampleRate * 0.5));

    while (running_.load(std::memory_order_acquire)) {
        pump(frames);
        next += period;
        const auto now = clock::now();
        if (next > now) std::this_thread::sleep_for(next - now);
        else next = now;
    }
}

} // namespace helix::virtualaudio
