// Test kontraktu czasu rzeczywistego (spec §22):
// w callbacku audio nie wolno alokować pamięci ani brać blokad.
//
// Globalny operator new jest podmieniony i zlicza alokacje. Jeśli jakikolwiek
// fragment toru sygnałowego sięgnie po pamięć, ten test to wychwyci.
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <new>
#include <string>
#include <vector>

#include "helix/core/AudioEngine.h"
#include "helix/core/ChannelManager.h"

namespace {

std::atomic<bool>          g_watching{false};
std::atomic<std::uint64_t> g_allocations{0};
std::atomic<std::uint64_t> g_frees{0};

void countAllocation() {
    if (g_watching.load(std::memory_order_relaxed))
        g_allocations.fetch_add(1, std::memory_order_relaxed);
}

void countFree() {
    if (g_watching.load(std::memory_order_relaxed))
        g_frees.fetch_add(1, std::memory_order_relaxed);
}

int g_failures = 0;

void check(bool condition, const std::string& message) {
    if (condition) {
        std::printf("  \033[32mOK\033[0m   %s\n", message.c_str());
    } else {
        std::printf("  \033[31mFAIL\033[0m %s\n", message.c_str());
        ++g_failures;
    }
}

} // namespace

void* operator new(std::size_t size) {
    countAllocation();
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}

void* operator new[](std::size_t size) {
    countAllocation();
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept { countFree(); std::free(memory); }
void operator delete[](void* memory) noexcept { countFree(); std::free(memory); }
void operator delete(void* memory, std::size_t) noexcept { countFree(); std::free(memory); }
void operator delete[](void* memory, std::size_t) noexcept { countFree(); std::free(memory); }

int main() {
    using namespace helix;
    using namespace helix::core;

    std::printf("Helix — kontrakt czasu rzeczywistego\n\n");

    constexpr int kBlock = 192;
    constexpr double kSampleRate = 48000.0;

    AudioEngine engine;
    EngineFormat format;
    format.sampleRate  = kSampleRate;
    format.blockFrames = kBlock;
    format.channels    = 2;
    engine.configure(format);

    ChannelManager manager(engine);

    Bus* headphones = engine.addBus("Headphones", "A1", BusKind::Physical);
    Bus* stream     = engine.addBus("Stream", "B1", BusKind::Virtual);
    engine.setPrimaryBus(headphones->id());

    // Pełny scenariusz: osiem kanałów, każdy z kompletnym łańcuchem DSP,
    // wszystkie efekty włączone, routing na dwie magistrale.
    std::vector<InputSource*> sources;
    for (const auto& spec : defaultChannelLayout()) {
        Channel* channel = manager.createChannel(spec.name, spec.icon, true);
        if (channel == nullptr) continue;

        for (const auto& effect : manager.effectStates(channel->id()))
            manager.setEffectEnabled(channel->id(), effect.id, true);

        engine.routing().setEnabled(channel->index(), headphones->index(), true);
        engine.routing().setEnabled(channel->index(), stream->index(), true);

        InputSource* source = engine.addSource(SourceKind::PhysicalInput, std::string(spec.name) + " src");
        engine.assignSource(source->id(), channel->id());
        source->setActive(true);
        sources.push_back(source);
    }

    engine.commitGraph();
    engine.setRunning(true);

    std::vector<Sample> device(static_cast<std::size_t>(kBlock) * 2u, 0.0f);
    std::vector<Sample> feed(static_cast<std::size_t>(kBlock) * 2u, 0.3f);

    // Rozgrzewka poza obserwacją — pierwsze bloki dogrzewają bufory i cache.
    for (int i = 0; i < 32; ++i) {
        for (InputSource* source : sources) source->ring().write(feed.data(), kBlock);
        engine.renderBlock(device.data(), kBlock, 2);
    }

    // Właściwy pomiar.
    g_allocations.store(0, std::memory_order_relaxed);
    g_frees.store(0, std::memory_order_relaxed);
    g_watching.store(true, std::memory_order_relaxed);

    for (int i = 0; i < 500; ++i) {
        engine.renderBlock(device.data(), kBlock, 2);
    }

    g_watching.store(false, std::memory_order_relaxed);

    const std::uint64_t allocations = g_allocations.load(std::memory_order_relaxed);
    const std::uint64_t frees = g_frees.load(std::memory_order_relaxed);

    check(allocations == 0,
          "renderBlock nie alokuje pamięci (zarejestrowano " + std::to_string(allocations) + ")");
    check(frees == 0,
          "renderBlock nie zwalnia pamięci (zarejestrowano " + std::to_string(frees) + ")");

    // Zmiana parametrów z wątku sterującego również nie może wymusić alokacji w RT.
    // Listę kanałów pobieramy przed obserwacją — samo jej zbudowanie alokuje
    // w wątku sterującym i nie ma nic wspólnego z kontraktem RT.
    const std::vector<Channel*> allChannels = engine.channels();

    g_allocations.store(0, std::memory_order_relaxed);
    g_watching.store(true, std::memory_order_relaxed);

    for (int i = 0; i < 200; ++i) {
        for (Channel* channel : allChannels) {
            channel->setVolume(0.3f + 0.4f * static_cast<float>(i % 2));
            channel->setMuted((i % 7) == 0);
            channel->setPan(static_cast<float>((i % 5) - 2) * 0.25f);
        }
        engine.routing().setEnabled(0, stream->index(), (i % 3) == 0);
        engine.master().setVolume(0.5f + 0.2f * static_cast<float>(i % 2));
        engine.renderBlock(device.data(), kBlock, 2);
    }

    g_watching.store(false, std::memory_order_relaxed);
    const std::uint64_t duringChanges = g_allocations.load(std::memory_order_relaxed);
    check(duringChanges == 0,
          "zmiany parametrów nie alokują w wątku audio (zarejestrowano " +
              std::to_string(duringChanges) + ")");

    // Silnik ma realnie przeliczać sygnał, a nie omijać tor.
    const EngineStats stats = engine.stats();
    check(stats.blocksProcessed >= 700,
          "przetworzono bloki: " + std::to_string(stats.blocksProcessed));
    check(stats.cpuLoad > 0.0 && stats.cpuLoad < 1.0,
          "obciążenie CPU w sensownym zakresie: " + std::to_string(stats.cpuLoad));

    engine.setRunning(false);
    engine.collectGarbage();

    std::printf("\n%s\n", g_failures == 0 ? "Kontrakt RT spełniony" : "Naruszenie kontraktu RT");
    return g_failures == 0 ? 0 : 1;
}
