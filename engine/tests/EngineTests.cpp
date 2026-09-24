#include <cmath>
#include <thread>

#include "TestFramework.h"
#include "helix/core/AudioEngine.h"
#include "helix/core/ChannelManager.h"
#include "helix/core/NullBackend.h"

using namespace helix;
using namespace helix::core;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr int kBlock = 128;

EngineFormat testFormat() {
    EngineFormat format;
    format.sampleRate = kSampleRate;
    format.blockFrames = kBlock;
    format.channels = 2;
    return format;
}

/// Wypełnia bufor źródła stałą wartością — łatwo śledzić sygnał przez tor.
void feedSource(InputSource& source, float value, int frames) {
    std::vector<Sample> interleaved(static_cast<std::size_t>(frames) * 2u, value);
    source.ring().write(interleaved.data(), frames);
}

float busPeak(Bus& bus) {
    AudioBufferView view = bus.buffer();
    float peak = 0.0f;
    for (int c = 0; c < view.channels(); ++c)
        for (int i = 0; i < view.frames(); ++i)
            peak = std::max(peak, std::fabs(view.channel(c)[i]));
    return peak;
}

/// Minimalna scena testowa: jeden kanał, dwie magistrale, jedno źródło.
struct Scene {
    AudioEngine engine;
    Channel* channel = nullptr;
    Bus* busA = nullptr;
    Bus* busB = nullptr;
    InputSource* source = nullptr;

    Scene() {
        engine.configure(testFormat());
        channel = engine.addChannel("Test");
        busA = engine.addBus("A", "A1", BusKind::Physical);
        busB = engine.addBus("B", "B1", BusKind::Virtual);
        source = engine.addSource(SourceKind::PhysicalInput, "Src");

        engine.setPrimaryBus(busA->id());
        engine.assignSource(source->id(), channel->id());
        source->setActive(true);

        channel->setVolume(1.0f);
        engine.master().setVolume(1.0f);
        busA->setLimiterEnabled(false);
        busB->setLimiterEnabled(false);
        busB->setFollowsMaster(false);

        engine.commitGraph();
        engine.setRunning(true);
    }

    void run(float input, int blocks = 4) {
        for (int i = 0; i < blocks; ++i) {
            feedSource(*source, input, kBlock);
            engine.processBlock(kBlock);
        }
    }
};

} // namespace

TEST("engine/sygnał dociera na magistralę wskazaną w routingu") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busB->index(), false);

    scene.run(0.5f, 8);

    CHECK_NEAR(busPeak(*scene.busA), 0.5f, 0.02f);
    CHECK_NEAR(busPeak(*scene.busB), 0.0f, 1e-6f);
}

TEST("engine/jeden kanał może iść na wiele wyjść naraz") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busB->index(), true);

    scene.run(0.4f, 8);

    CHECK_NEAR(busPeak(*scene.busA), 0.4f, 0.02f);
    CHECK_NEAR(busPeak(*scene.busB), 0.4f, 0.02f);
}

TEST("engine/zmiana routingu w locie nie tworzy skoku sygnału") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busB->index(), false);
    scene.run(0.5f, 6);

    // Włączamy wysyłkę i sprawdzamy pierwszy blok po zmianie.
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busB->index(), true);
    feedSource(*scene.source, 0.5f, kBlock);
    scene.engine.processBlock(kBlock);

    AudioBufferView view = scene.busB->buffer();
    CHECK_NEAR(view.channel(0)[0], 0.0f, 0.02f);           // start od zera
    CHECK_NEAR(view.channel(0)[kBlock - 1], 0.5f, 0.02f);  // pełny poziom na końcu bloku

    // Narastanie musi być monotoniczne i drobnokrokowe.
    for (int i = 1; i < kBlock; ++i) {
        const float step = view.channel(0)[i] - view.channel(0)[i - 1];
        CHECK_MSG(step >= -1e-6f && step < 0.02f, "skok w rampie routingu: " + std::to_string(step));
    }
}

TEST("engine/mute wycisza kanał płynnie") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.run(0.5f, 8);
    CHECK_NEAR(busPeak(*scene.busA), 0.5f, 0.02f);

    scene.channel->setMuted(true);
    scene.run(0.5f, 8);
    CHECK_NEAR(busPeak(*scene.busA), 0.0f, 1e-4f);

    scene.channel->setMuted(false);
    scene.run(0.5f, 8);
    CHECK_NEAR(busPeak(*scene.busA), 0.5f, 0.02f);
}

TEST("engine/solo wycisza pozostałe kanały") {
    Scene scene;
    Channel* second = scene.engine.addChannel("Second");
    InputSource* secondSource = scene.engine.addSource(SourceKind::PhysicalInput, "Src2");
    scene.engine.assignSource(secondSource->id(), second->id());
    secondSource->setActive(true);
    second->setVolume(1.0f);

    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.engine.routing().setEnabled(second->index(), scene.busB->index(), true);
    scene.channel->setSolo(true);
    scene.engine.commitGraph();

    for (int i = 0; i < 8; ++i) {
        feedSource(*scene.source, 0.5f, kBlock);
        feedSource(*secondSource, 0.5f, kBlock);
        scene.engine.processBlock(kBlock);
    }

    CHECK_NEAR(busPeak(*scene.busA), 0.5f, 0.02f);
    CHECK_NEAR(busPeak(*scene.busB), 0.0f, 1e-4f);
}

TEST("engine/master skaluje tylko magistrale, które za nim podążają") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busB->index(), true);
    scene.busA->setFollowsMaster(true);
    scene.busB->setFollowsMaster(false);

    scene.engine.master().setVolume(0.5f);
    scene.run(0.8f, 12);

    CHECK_NEAR(busPeak(*scene.busA), 0.4f, 0.02f);
    CHECK_NEAR(busPeak(*scene.busB), 0.8f, 0.02f);
}

TEST("engine/mierniki odzwierciedlają poziom sygnału") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.run(0.5f, 60);

    const MeterSnapshot channelMeter = scene.channel->outputMeter();
    CHECK_NEAR(channelMeter.peakLeft, 0.5f, 0.03f);
    CHECK_NEAR(channelMeter.rmsLeft, 0.5f, 0.05f);
    CHECK(!channelMeter.clipping);

    const MeterSnapshot masterMeter = scene.engine.master().meterSnapshot();
    CHECK_NEAR(masterMeter.peakLeft, 0.5f, 0.03f);
}

TEST("engine/miernik wykrywa clipping") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.run(1.5f, 10);

    CHECK(scene.channel->inputMeter().clipping);
    scene.channel->clearClip();
    CHECK(!scene.channel->inputMeter().clipping);
}

TEST("engine/limiter magistrali chroni przed przesterem") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.busA->setLimiterEnabled(true);
    scene.busA->limiter().setParameter("ceiling", -1.0f);
    scene.busA->limiter().setParameter("lookahead", 1.0f);

    scene.run(2.0f, 20);

    const float ceiling = dbToGain(-1.0f);
    CHECK_MSG(busPeak(*scene.busA) <= ceiling + 1e-5f,
              "limiter przepuścił " + std::to_string(busPeak(*scene.busA)));
}

TEST("engine/nieaktywne źródło daje ciszę zamiast zakłóceń") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.run(0.5f, 8);

    // Symulacja zamkniętej aplikacji: źródło przestaje być aktywne.
    scene.source->setActive(false);
    for (int i = 0; i < 8; ++i) scene.engine.processBlock(kBlock);

    CHECK_NEAR(busPeak(*scene.busA), 0.0f, 1e-4f);
    CHECK(scene.engine.stats().blocksProcessed > 0);
}

TEST("engine/dodanie i usunięcie kanału podczas pracy") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.run(0.3f, 4);

    Channel* extra = scene.engine.addChannel("Extra");
    CHECK(extra != nullptr);
    scene.engine.commitGraph();
    scene.run(0.3f, 4);

    const ChannelId id = extra->id();
    CHECK(scene.engine.removeChannel(id));
    scene.run(0.3f, 4);
    scene.engine.collectGarbage();

    CHECK(scene.engine.findChannel(id) == nullptr);
    CHECK_NEAR(busPeak(*scene.busA), 0.3f, 0.02f);
}

TEST("engine/usunięcie magistrali czyści routing") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busB->index(), true);
    CHECK(scene.engine.routing().isEnabled(scene.channel->index(), scene.busB->index()));

    const int index = scene.busB->index();
    CHECK(scene.engine.removeBus(scene.busB->id()));
    scene.engine.collectGarbage();

    CHECK(!scene.engine.routing().isEnabled(scene.channel->index(), index));
}

TEST("engine/zmiana formatu w trakcie pracy nie wywraca silnika") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);
    scene.run(0.4f, 4);

    EngineFormat format = testFormat();
    format.sampleRate = 96000.0;
    format.blockFrames = 256;
    CHECK(scene.engine.configure(format).ok);
    CHECK(scene.engine.isRunning());

    for (int i = 0; i < 8; ++i) {
        feedSource(*scene.source, 0.4f, 256);
        scene.engine.processBlock(256);
    }
    CHECK_NEAR(busPeak(*scene.busA), 0.4f, 0.03f);
    CHECK_EQ(scene.engine.stats().blockFrames, 256);
}

TEST("engine/odrzuca nieprawidłowy format") {
    AudioEngine engine;
    EngineFormat format = testFormat();
    format.sampleRate = 1000.0;
    CHECK(!engine.configure(format).ok);

    format = testFormat();
    format.blockFrames = 0;
    CHECK(!engine.configure(format).ok);

    format = testFormat();
    format.channels = 99;
    CHECK(!engine.configure(format).ok);
}

TEST("engine/renderBlock wypełnia bufor urządzenia") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);

    std::vector<Sample> device(static_cast<std::size_t>(kBlock) * 2u, -1.0f);
    for (int i = 0; i < 8; ++i) {
        feedSource(*scene.source, 0.25f, kBlock);
        scene.engine.renderBlock(device.data(), kBlock, 2);
    }

    float peak = 0.0f;
    for (const Sample value : device) peak = std::max(peak, std::fabs(value));
    CHECK_NEAR(peak, 0.25f, 0.02f);
}

TEST("engine/zatrzymany silnik wypełnia bufor ciszą") {
    Scene scene;
    scene.engine.setRunning(false);

    std::vector<Sample> device(static_cast<std::size_t>(kBlock) * 2u, 0.7f);
    scene.engine.renderBlock(device.data(), kBlock, 2);

    for (const Sample value : device) CHECK_NEAR(value, 0.0f, 1e-9);
}

TEST("engine/bufor większy niż blok jest dzielony na części") {
    Scene scene;
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);

    const int large = kBlock * 4;
    std::vector<Sample> device(static_cast<std::size_t>(large) * 2u, 0.0f);

    for (int i = 0; i < 4; ++i) {
        feedSource(*scene.source, 0.3f, large);
        scene.engine.renderBlock(device.data(), large, 2);
    }

    float peak = 0.0f;
    for (const Sample value : device) peak = std::max(peak, std::fabs(value));
    CHECK_NEAR(peak, 0.3f, 0.02f);
}

TEST("engine/łańcuch DSP kanału działa w torze") {
    Scene scene;
    ChannelManager manager(scene.engine);
    scene.engine.routing().setEnabled(scene.channel->index(), scene.busA->index(), true);

    PluginId gainId = 0;
    CHECK(manager.addEffect(scene.channel->id(), "gain", -1, &gainId));
    CHECK(manager.setEffectEnabled(scene.channel->id(), gainId, true));
    CHECK(manager.setEffectParameter(scene.channel->id(), gainId, "gain", -6.0f));

    scene.run(0.5f, 20);
    CHECK_NEAR(busPeak(*scene.busA), 0.5f * dbToGain(-6.0f), 0.02f);

    // Wyłączenie efektu przywraca pełen poziom.
    CHECK(manager.setEffectEnabled(scene.channel->id(), gainId, false));
    scene.run(0.5f, 20);
    CHECK_NEAR(busPeak(*scene.busA), 0.5f, 0.02f);

    // Usunięcie efektu w trakcie pracy nie może niczego wywrócić.
    CHECK(manager.removeEffect(scene.channel->id(), gainId));
    scene.run(0.5f, 4);
    manager.collectGarbage();
    CHECK_NEAR(busPeak(*scene.busA), 0.5f, 0.02f);
}

TEST("engine/ChannelManager buduje domyślny układ ze specyfikacji") {
    AudioEngine engine;
    engine.configure(testFormat());
    ChannelManager manager(engine);
    manager.createDefaultLayout();

    const auto channels = engine.channels();
    CHECK_EQ(channels.size(), defaultChannelLayout().size());
    CHECK_STR_EQ(channels[0]->name(), "Game");
    CHECK_STR_EQ(channels[4]->name(), "Microphone");

    // Każdy kanał dostaje pełny łańcuch ze specyfikacji §8.
    const auto effects = manager.effectStates(channels[0]->id());
    CHECK_EQ(effects.size(), std::size_t{7});
    CHECK_STR_EQ(effects[0].typeId, "gain");
    CHECK_STR_EQ(effects[1].typeId, "noisegate");
    CHECK_STR_EQ(effects[6].typeId, "limiter");
}

TEST("engine/limit kanałów jest egzekwowany") {
    AudioEngine engine;
    engine.configure(testFormat());

    int created = 0;
    for (int i = 0; i < kMaxChannels + 5; ++i)
        if (engine.addChannel("K" + std::to_string(i)) != nullptr) ++created;

    CHECK_EQ(created, kMaxChannels);
}

TEST("engine/backend programowy napędza silnik przez callback") {
    NullBackend backend(false);   // tryb sterowany ręcznie
    CHECK(backend.initialize().ok);

    AudioEngine engine;
    engine.configure(testFormat());
    Channel* channel = engine.addChannel("Test");
    Bus* bus = engine.addBus("A", "A1", BusKind::Physical);
    InputSource* source = engine.addSource(SourceKind::PhysicalInput, "Src");
    engine.assignSource(source->id(), channel->id());
    engine.setPrimaryBus(bus->id());
    engine.routing().setEnabled(channel->index(), bus->index(), true);
    channel->setVolume(1.0f);
    bus->setLimiterEnabled(false);
    engine.master().setVolume(1.0f);
    source->setActive(true);
    engine.commitGraph();
    engine.setRunning(true);

    StreamConfig config;
    config.deviceId = "null:speakers";
    config.sampleRate = kSampleRate;
    config.channels = 2;
    config.bufferFrames = kBlock;

    Status status;
    auto stream = backend.openRenderStream(config, &engine, status);
    CHECK_MSG(stream != nullptr, status.message);
    CHECK(stream->start().ok);

    for (int i = 0; i < 12; ++i) {
        feedSource(*source, 0.6f, kBlock);
        backend.pump(kBlock);
    }

    const auto rendered = backend.lastRenderedBlock("null:speakers");
    CHECK(!rendered.empty());
    float peak = 0.0f;
    for (const Sample value : rendered) peak = std::max(peak, std::fabs(value));
    CHECK_NEAR(peak, 0.6f, 0.03f);

    stream->stop();
    backend.shutdown();
}

TEST("engine/hot-plug: zniknięcie urządzenia zgłasza zdarzenie") {
    NullBackend backend(false);
    CHECK(backend.initialize().ok);

    std::string removedDevice;
    backend.setDeviceChangeHandler([&](const DeviceChangeEvent& event) {
        if (event.type == DeviceChangeEvent::Type::Removed) removedDevice = event.deviceId;
    });

    CHECK(backend.deviceExists("null:headphones"));
    CHECK(backend.removeDevice("null:headphones"));
    CHECK_STR_EQ(removedDevice, "null:headphones");
    CHECK(!backend.deviceExists("null:headphones"));

    backend.shutdown();
}
