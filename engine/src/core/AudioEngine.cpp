#include "helix/core/AudioEngine.h"

#include <algorithm>
#include <cstring>

#include "helix/Denormal.h"
#include "helix/Log.h"

namespace helix::core {

namespace {
constexpr const char* kLog = "AudioEngine";

/// Zapas w buforze magistrali: cztery bloki wystarczą, by wątek urządzenia
/// o innym zegarze zdążył odebrać dane bez przerw.
constexpr int kBusRingBlocks = 8;
} // namespace

AudioEngine::AudioEngine() {
    channelIndexUsed_.fill(false);
    busIndexUsed_.fill(false);
    sourceScratch_.resize(kMaxStreamChannels, kMaxBlockFrames);
    master_.prepare(dsp::ProcessContext{format_.sampleRate, format_.blockFrames, format_.channels});
}

AudioEngine::~AudioEngine() {
    setRunning(false);

    if (const Graph* graph = activeGraph_.exchange(nullptr, std::memory_order_acq_rel))
        delete graph;

    // Po zatrzymaniu silnika nic już nie czyta odstawionych obiektów.
    for (auto& item : retired_)
        if (item.deleter) item.deleter();
    retired_.clear();
}

// ── Konfiguracja ────────────────────────────────────────────────────────────

Status AudioEngine::configure(const EngineFormat& format) {
    if (format.sampleRate < 8000.0 || format.sampleRate > 384000.0)
        return Status::error("Nieobsługiwana częstotliwość próbkowania");
    if (format.blockFrames < 16 || format.blockFrames > kMaxBlockFrames)
        return Status::error("Nieobsługiwany rozmiar bloku");
    if (format.channels < 1 || format.channels > kMaxStreamChannels)
        return Status::error("Nieobsługiwana liczba kanałów");

    const bool wasRunning = isRunning();
    setRunning(false);

    {
        std::lock_guard<std::mutex> lock(structureMutex_);
        format_ = format;
        blockLimit_.store(format.blockFrames, std::memory_order_relaxed);
        sampleRateCached_.store(format.sampleRate, std::memory_order_relaxed);
        prepareObjects();
        rebuildGraphLocked();
    }

    collectGarbage();
    if (wasRunning) setRunning(true);

    Log::info(kLog, "Format: " + std::to_string(static_cast<int>(format.sampleRate)) + " Hz, blok " +
                        std::to_string(format.blockFrames) + " ramek, " +
                        std::to_string(format.channels) + " kan.");
    return Status::success();
}

EngineFormat AudioEngine::format() const {
    std::lock_guard<std::mutex> lock(structureMutex_);
    return format_;
}

void AudioEngine::setRunning(bool running) noexcept {
    running_.store(running, std::memory_order_release);
}

void AudioEngine::prepareObjects() {
    const dsp::ProcessContext context{format_.sampleRate, format_.blockFrames, format_.channels};

    sourceScratch_.resize(kMaxStreamChannels, std::max(format_.blockFrames, kDefaultBlockFrames));

    for (auto& channel : channelStore_) channel->prepare(context);
    for (auto& bus : busStore_) bus->prepare(context, format_.blockFrames * kBusRingBlocks);
    for (auto& source : sourceStore_)
        source->prepare(format_.channels, format_.sampleRate, format_.blockFrames * 16);

    master_.prepare(context);
}

// ── Budowa grafu ────────────────────────────────────────────────────────────

int AudioEngine::allocateChannelIndex() {
    for (int i = 0; i < kMaxChannels; ++i)
        if (!channelIndexUsed_[static_cast<std::size_t>(i)]) {
            channelIndexUsed_[static_cast<std::size_t>(i)] = true;
            return i;
        }
    return -1;
}

int AudioEngine::allocateBusIndex() {
    for (int i = 0; i < kMaxBuses; ++i)
        if (!busIndexUsed_[static_cast<std::size_t>(i)]) {
            busIndexUsed_[static_cast<std::size_t>(i)] = true;
            return i;
        }
    return -1;
}

Channel* AudioEngine::addChannel(std::string name) {
    std::lock_guard<std::mutex> lock(structureMutex_);
    const int index = allocateChannelIndex();
    if (index < 0) {
        Log::warn(kLog, "Osiągnięto limit kanałów");
        return nullptr;
    }

    auto channel = std::make_unique<Channel>(nextChannelId_++, index, std::move(name));
    channel->prepare(dsp::ProcessContext{format_.sampleRate, format_.blockFrames, format_.channels});
    routing_.clearChannel(index);

    Channel* raw = channel.get();
    channelStore_.push_back(std::move(channel));
    return raw;
}

bool AudioEngine::removeChannel(ChannelId id) {
    std::unique_ptr<Channel> removed;
    {
        std::lock_guard<std::mutex> lock(structureMutex_);
        const auto it = std::find_if(channelStore_.begin(), channelStore_.end(),
                                     [id](const auto& c) { return c->id() == id; });
        if (it == channelStore_.end()) return false;

        const int index = (*it)->index();
        routing_.clearChannel(index);
        channelIndexUsed_[static_cast<std::size_t>(index)] = false;

        // Odepnij źródła, które wskazywały na ten kanał.
        for (auto& source : sourceStore_)
            if (source->channel() == id) source->setChannel(kInvalidChannel);

        removed = std::move(*it);
        channelStore_.erase(it);
        rebuildGraphLocked();
    }

    // Kanał zwolnimy dopiero, gdy wątek audio przetworzy nowy graf.
    Channel* raw = removed.release();
    retire([raw] { delete raw; });
    return true;
}

Channel* AudioEngine::findChannel(ChannelId id) const {
    std::lock_guard<std::mutex> lock(structureMutex_);
    const auto it = std::find_if(channelStore_.begin(), channelStore_.end(),
                                 [id](const auto& c) { return c->id() == id; });
    return it == channelStore_.end() ? nullptr : it->get();
}

std::vector<Channel*> AudioEngine::channels() const {
    std::lock_guard<std::mutex> lock(structureMutex_);
    std::vector<Channel*> result;
    result.reserve(channelStore_.size());
    for (const auto& channel : channelStore_) result.push_back(channel.get());
    return result;
}

Bus* AudioEngine::addBus(std::string name, std::string label, BusKind kind) {
    std::lock_guard<std::mutex> lock(structureMutex_);
    const int index = allocateBusIndex();
    if (index < 0) {
        Log::warn(kLog, "Osiągnięto limit magistral");
        return nullptr;
    }

    auto bus = std::make_unique<Bus>(nextBusId_++, index, std::move(name), std::move(label), kind);
    bus->prepare(dsp::ProcessContext{format_.sampleRate, format_.blockFrames, format_.channels},
                 format_.blockFrames * kBusRingBlocks);
    routing_.clearBus(index);

    Bus* raw = bus.get();
    busStore_.push_back(std::move(bus));

    if (master_.primaryBus() == kInvalidBus && kind == BusKind::Physical)
        master_.setPrimaryBus(raw->id());

    return raw;
}

bool AudioEngine::removeBus(BusId id) {
    std::unique_ptr<Bus> removed;
    {
        std::lock_guard<std::mutex> lock(structureMutex_);
        const auto it = std::find_if(busStore_.begin(), busStore_.end(),
                                     [id](const auto& b) { return b->id() == id; });
        if (it == busStore_.end()) return false;

        const int index = (*it)->index();
        routing_.clearBus(index);
        busIndexUsed_[static_cast<std::size_t>(index)] = false;

        removed = std::move(*it);
        busStore_.erase(it);

        if (master_.primaryBus() == id) {
            master_.setPrimaryBus(kInvalidBus);
            for (const auto& bus : busStore_)
                if (bus->kind() == BusKind::Physical) { master_.setPrimaryBus(bus->id()); break; }
        }
        rebuildGraphLocked();
    }

    Bus* raw = removed.release();
    retire([raw] { delete raw; });
    return true;
}

Bus* AudioEngine::findBus(BusId id) const {
    std::lock_guard<std::mutex> lock(structureMutex_);
    const auto it = std::find_if(busStore_.begin(), busStore_.end(),
                                 [id](const auto& b) { return b->id() == id; });
    return it == busStore_.end() ? nullptr : it->get();
}

std::vector<Bus*> AudioEngine::buses() const {
    std::lock_guard<std::mutex> lock(structureMutex_);
    std::vector<Bus*> result;
    result.reserve(busStore_.size());
    for (const auto& bus : busStore_) result.push_back(bus.get());
    return result;
}

InputSource* AudioEngine::addSource(SourceKind kind, std::string name) {
    std::lock_guard<std::mutex> lock(structureMutex_);
    auto source = std::make_unique<InputSource>(nextSourceId_++, kind, std::move(name));
    source->prepare(format_.channels, format_.sampleRate, format_.blockFrames * 16);
    InputSource* raw = source.get();
    sourceStore_.push_back(std::move(source));
    return raw;
}

bool AudioEngine::removeSource(SourceId id) {
    std::unique_ptr<InputSource> removed;
    {
        std::lock_guard<std::mutex> lock(structureMutex_);
        const auto it = std::find_if(sourceStore_.begin(), sourceStore_.end(),
                                     [id](const auto& s) { return s->id() == id; });
        if (it == sourceStore_.end()) return false;
        removed = std::move(*it);
        sourceStore_.erase(it);
        rebuildGraphLocked();
    }

    InputSource* raw = removed.release();
    retire([raw] { delete raw; });
    return true;
}

InputSource* AudioEngine::findSource(SourceId id) const {
    std::lock_guard<std::mutex> lock(structureMutex_);
    const auto it = std::find_if(sourceStore_.begin(), sourceStore_.end(),
                                 [id](const auto& s) { return s->id() == id; });
    return it == sourceStore_.end() ? nullptr : it->get();
}

std::vector<InputSource*> AudioEngine::sources() const {
    std::lock_guard<std::mutex> lock(structureMutex_);
    std::vector<InputSource*> result;
    result.reserve(sourceStore_.size());
    for (const auto& source : sourceStore_) result.push_back(source.get());
    return result;
}

bool AudioEngine::assignSource(SourceId source, ChannelId channel) {
    {
        std::lock_guard<std::mutex> lock(structureMutex_);
        const auto it = std::find_if(sourceStore_.begin(), sourceStore_.end(),
                                     [source](const auto& s) { return s->id() == source; });
        if (it == sourceStore_.end()) return false;

        if (channel != kInvalidChannel) {
            const bool exists = std::any_of(channelStore_.begin(), channelStore_.end(),
                                            [channel](const auto& c) { return c->id() == channel; });
            if (!exists) return false;
        }
        (*it)->setChannel(channel);
    }
    commitGraph();
    return true;
}

void AudioEngine::setPrimaryBus(BusId bus) {
    master_.setPrimaryBus(bus);
    commitGraph();
}

void AudioEngine::commitGraph() {
    std::lock_guard<std::mutex> lock(structureMutex_);
    rebuildGraphLocked();
}

void AudioEngine::rebuildGraphLocked() {
    auto graph = std::make_unique<Graph>();
    graph->channels.reserve(channelStore_.size());
    for (const auto& channel : channelStore_) graph->channels.push_back(channel.get());

    graph->buses.reserve(busStore_.size());
    for (const auto& bus : busStore_) graph->buses.push_back(bus.get());

    graph->sources.reserve(sourceStore_.size());
    graph->sourceChannelIndex.reserve(sourceStore_.size());
    for (const auto& source : sourceStore_) {
        graph->sources.push_back(source.get());
        int channelIndex = -1;
        const ChannelId target = source->channel();
        if (target != kInvalidChannel) {
            for (std::size_t i = 0; i < graph->channels.size(); ++i)
                if (graph->channels[i]->id() == target) { channelIndex = static_cast<int>(i); break; }
        }
        graph->sourceChannelIndex.push_back(channelIndex);
    }

    graph->anySolo = std::any_of(channelStore_.begin(), channelStore_.end(),
                                 [](const auto& c) { return c->solo(); });

    graph->primaryBusIndex = -1;
    const BusId primary = master_.primaryBus();
    for (std::size_t i = 0; i < graph->buses.size(); ++i)
        if (graph->buses[i]->id() == primary) { graph->primaryBusIndex = static_cast<int>(i); break; }

    const Graph* previous = activeGraph_.exchange(graph.release(), std::memory_order_acq_rel);
    if (previous != nullptr)
        retireLocked([previous] { delete previous; });  // mutex już trzymany przez wywołującego
}

void AudioEngine::retire(std::function<void()> deleter) {
    std::lock_guard<std::mutex> lock(structureMutex_);
    retireLocked(std::move(deleter));
}

void AudioEngine::retireLocked(std::function<void()> deleter) {
    retired_.push_back(RetiredObject{blockCounter_.load(std::memory_order_acquire), std::move(deleter)});
}

void AudioEngine::collectGarbage() {
    std::vector<std::function<void()>> ready;
    {
        std::lock_guard<std::mutex> lock(structureMutex_);
        const std::uint64_t now = blockCounter_.load(std::memory_order_acquire);
        const bool stopped = !running_.load(std::memory_order_acquire);

        auto it = retired_.begin();
        while (it != retired_.end()) {
            // Dwa pełne bloki gwarantują, że wątek audio opuścił stary graf.
            if (stopped || now >= it->stamp + 2) {
                ready.push_back(std::move(it->deleter));
                it = retired_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& deleter : ready)
        if (deleter) deleter();
}

// ── Wątek audio ─────────────────────────────────────────────────────────────

void AudioEngine::renderBlock(Sample* interleaved, int frames, int channels) noexcept {
    const std::size_t totalSamples =
        static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);

    if (!isRunning()) {
        std::memset(interleaved, 0, totalSamples * sizeof(Sample));
        return;
    }

    const Graph* graph = activeGraph_.load(std::memory_order_acquire);
    if (graph == nullptr) {
        std::memset(interleaved, 0, totalSamples * sizeof(Sample));
        return;
    }

    const ScopedDenormalGuard denormalGuard;
    const auto started = std::chrono::steady_clock::now();

    const int limit = blockLimit_.load(std::memory_order_relaxed);
    int offset = 0;

    while (offset < frames) {
        const int chunk = std::min(frames - offset, limit);
        processBlockInternal(*graph, chunk);

        Sample* destination = interleaved + static_cast<std::size_t>(offset) *
                                                static_cast<std::size_t>(channels);
        if (graph->primaryBusIndex >= 0) {
            AudioBufferView view =
                graph->buses[static_cast<std::size_t>(graph->primaryBusIndex)]->buffer();
            const int available = view.channels();
            for (int c = 0; c < channels; ++c) {
                const Sample* src = view.channel(std::min(c, available - 1));
                for (int i = 0; i < chunk; ++i)
                    destination[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)
                                + static_cast<std::size_t>(c)] = src[i];
            }
        } else {
            std::memset(destination, 0,
                        static_cast<std::size_t>(chunk) * static_cast<std::size_t>(channels)
                            * sizeof(Sample));
        }

        offset += chunk;
    }

    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double budget = static_cast<double>(frames) / sampleRateCached_.load(std::memory_order_relaxed);
    if (budget > 0.0) {
        const double load = elapsed / budget;
        // Wygładzony wskaźnik obciążenia — pojedynczy skok nie zaburza odczytu.
        const double previous = cpuLoad_.load(std::memory_order_relaxed);
        cpuLoad_.store(previous * 0.9 + load * 0.1, std::memory_order_relaxed);
        if (load > peakCpuLoad_.load(std::memory_order_relaxed))
            peakCpuLoad_.store(load, std::memory_order_relaxed);
        if (load > 1.0) xruns_.fetch_add(1, std::memory_order_relaxed);
    }
}

void AudioEngine::processBlock(int frames) noexcept {
    if (!isRunning()) return;
    const Graph* graph = activeGraph_.load(std::memory_order_acquire);
    if (graph == nullptr) return;

    const ScopedDenormalGuard denormalGuard;
    const int limit = blockLimit_.load(std::memory_order_relaxed);
    int offset = 0;
    while (offset < frames) {
        const int chunk = std::min(frames - offset, limit);
        processBlockInternal(*graph, chunk);
        offset += chunk;
    }
}

void AudioEngine::processBlockInternal(const Graph& graph, int frames) noexcept {
    // 1. Kanały startują od ciszy.
    for (Channel* channel : graph.channels) channel->beginBlock(frames);

    // 2. Źródła → kanały. Brak danych ze źródła oznacza ciszę, nie blokadę.
    Sample* scratchPointers[kMaxStreamChannels];
    for (std::size_t s = 0; s < graph.sources.size(); ++s) {
        const int channelIndex = graph.sourceChannelIndex[s];
        if (channelIndex < 0) continue;

        InputSource* source = graph.sources[s];
        Channel* channel = graph.channels[static_cast<std::size_t>(channelIndex)];
        AudioBufferView channelBuffer = channel->buffer();

        const int useChannels = std::min(channelBuffer.channels(), kMaxStreamChannels);
        for (int c = 0; c < useChannels; ++c)
            scratchPointers[c] = sourceScratch_.channel(c);

        if (!source->active()) {
            source->ring().clear();
            continue;
        }

        source->ring().readPlanar(scratchPointers, useChannels, frames);
        AudioBufferView scratchView(scratchPointers, useChannels, frames);
        source->meter().process(scratchView);
        channelBuffer.addFrom(scratchView, source->sourceGain());
    }

    // 3. DSP kanałów (Solo wycisza pozostałe).
    for (Channel* channel : graph.channels)
        channel->processBlock(graph.anySolo && !channel->solo());

    // 4. Magistrale.
    for (Bus* bus : graph.buses) bus->beginBlock(frames);
    mixToBuses(graph, frames);

    const auto [masterStart, masterEnd] = master_.gainRamp(frames);
    for (Bus* bus : graph.buses) {
        const bool follows = bus->followsMaster();
        bus->finishBlock(follows ? masterStart : 1.0f, follows ? masterEnd : 1.0f);
    }

    // 5. Pomiar Mastera + wypchnięcie magistral pobocznych do ich urządzeń.
    for (std::size_t b = 0; b < graph.buses.size(); ++b) {
        Bus* bus = graph.buses[b];
        if (static_cast<int>(b) == graph.primaryBusIndex) {
            AudioBufferView view = bus->buffer();
            master_.meter().process(view);
        } else {
            bus->pushToDevice();
        }
    }

    blockCounter_.fetch_add(1, std::memory_order_release);
}

void AudioEngine::mixToBuses(const Graph& graph, int frames) noexcept {
    const float inv = 1.0f / static_cast<float>(frames);

    for (Channel* channel : graph.channels) {
        const int channelIndex = channel->index();
        AudioBufferView source = channel->buffer();

        for (Bus* bus : graph.buses) {
            const int busIndex = bus->index();
            const float startGain = routing_.currentGain(channelIndex, busIndex);
            const float endGain   = routing_.targetGain(channelIndex, busIndex);
            if (startGain == 0.0f && endGain == 0.0f) continue;

            AudioBufferView destination = bus->buffer();
            const int channelCount = std::min(destination.channels(), source.channels());
            const float step = (endGain - startGain) * inv;

            for (int c = 0; c < channelCount; ++c) {
                Sample* dst = destination.channel(c);
                const Sample* src = source.channel(c);
                float gain = startGain;
                for (int i = 0; i < frames; ++i) {
                    dst[i] += src[i] * gain;
                    gain += step;
                }
            }

            routing_.commit(channelIndex, busIndex, endGain);
        }
    }
}

// ── Diagnostyka ─────────────────────────────────────────────────────────────

EngineStats AudioEngine::stats() const noexcept {
    EngineStats s;
    s.blocksProcessed = blockCounter_.load(std::memory_order_relaxed);
    s.xruns           = xruns_.load(std::memory_order_relaxed);
    s.cpuLoad         = cpuLoad_.load(std::memory_order_relaxed);
    s.peakCpuLoad     = peakCpuLoad_.load(std::memory_order_relaxed);
    s.sampleRate      = sampleRateCached_.load(std::memory_order_relaxed);
    s.blockFrames     = blockLimit_.load(std::memory_order_relaxed);
    s.running         = running_.load(std::memory_order_acquire);
    s.engineLatencyMs = estimatedLatencyMs(0.0);
    return s;
}

void AudioEngine::resetStats() noexcept {
    xruns_.store(0, std::memory_order_relaxed);
    peakCpuLoad_.store(0.0, std::memory_order_relaxed);
    cpuLoad_.store(0.0, std::memory_order_relaxed);
}

double AudioEngine::estimatedLatencyMs(double deviceLatencyMs) const noexcept {
    const double sampleRate = sampleRateCached_.load(std::memory_order_relaxed);
    const int block = blockLimit_.load(std::memory_order_relaxed);
    double worstChainFrames = 0.0;

    if (const Graph* graph = activeGraph_.load(std::memory_order_acquire)) {
        for (Channel* channel : graph->channels)
            worstChainFrames = std::max(worstChainFrames, static_cast<double>(channel->latencyFrames()));
    }

    const double blockMs = 1000.0 * static_cast<double>(block) / sampleRate;
    const double chainMs = 1000.0 * worstChainFrames / sampleRate;
    return deviceLatencyMs + blockMs + chainMs;
}

} // namespace helix::core
