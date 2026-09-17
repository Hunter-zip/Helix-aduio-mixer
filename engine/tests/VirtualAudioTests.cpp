#include <filesystem>
#include <thread>
#include <vector>

#include "TestFramework.h"
#include "helix/core/AudioEngine.h"
#include "helix/virtualaudio/SharedRingBuffer.h"
#include "helix/virtualaudio/VirtualDeviceManager.h"

using namespace helix;
using namespace helix::virtualaudio;

TEST("virtual/segment pamięci współdzielonej przenosi próbki") {
    SharedRingBuffer server;
    const Status created = server.create("test-transport", 2, 48000.0, 1024);
    CHECK_MSG(created.ok, created.message);
    CHECK(server.isOpen());
    CHECK_EQ(server.channels(), 2);
    CHECK_NEAR(server.sampleRate(), 48000.0, 1e-9);

    SharedRingBuffer client;
    const Status opened = client.open("test-transport", SharedRingRole::Consumer);
    CHECK_MSG(opened.ok, opened.message);
    CHECK(server.peerAlive(SharedRingRole::Consumer));

    std::vector<Sample> input(256 * 2);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<Sample>(i) * 0.001f;

    CHECK_EQ(server.write(input.data(), 256), 256);
    CHECK_EQ(client.availableToRead(), 256);

    std::vector<Sample> output(256 * 2, -1.0f);
    CHECK_EQ(client.read(output.data(), 256), 256);
    for (std::size_t i = 0; i < input.size(); ++i) CHECK_NEAR(output[i], input[i], 1e-9);

    CHECK_EQ(client.availableToRead(), 0);
}

TEST("virtual/niedomiar zwraca ciszę i jest liczony") {
    SharedRingBuffer server;
    CHECK(server.create("test-underrun", 1, 48000.0, 512).ok);

    SharedRingBuffer client;
    CHECK(client.open("test-underrun", SharedRingRole::Consumer).ok);

    std::vector<Sample> input(32, 0.5f);
    server.write(input.data(), 32);

    std::vector<Sample> output(64, -1.0f);
    CHECK_EQ(client.read(output.data(), 64), 32);
    for (std::size_t i = 32; i < output.size(); ++i) CHECK_NEAR(output[i], 0.0f, 1e-9);
    CHECK(client.underruns() >= 32);
}

TEST("virtual/otwarcie nieistniejącego segmentu kończy się błędem") {
    SharedRingBuffer client;
    const Status status = client.open("nie-ma-takiego-segmentu-helix", SharedRingRole::Consumer);
    CHECK(!status.ok);
    CHECK(!client.isOpen());
}

TEST("virtual/przepływ z magistrali do klienta") {
    using namespace helix::core;

    AudioEngine engine;
    EngineFormat format;
    format.sampleRate = 48000.0;
    format.blockFrames = 128;
    format.channels = 2;
    engine.configure(format);

    Channel* channel = engine.addChannel("Test");
    Bus* primary = engine.addBus("A", "A1", BusKind::Physical);
    Bus* streamBus = engine.addBus("Stream", "B1", BusKind::Virtual);
    InputSource* source = engine.addSource(SourceKind::PhysicalInput, "Src");

    engine.setPrimaryBus(primary->id());
    engine.assignSource(source->id(), channel->id());
    engine.routing().setEnabled(channel->index(), streamBus->index(), true);
    channel->setVolume(1.0f);
    streamBus->setFollowsMaster(false);
    streamBus->setLimiterEnabled(false);
    source->setActive(true);
    engine.commitGraph();
    engine.setRunning(true);

    VirtualDeviceManager manager(engine, createSoftwareDriverInterface());
    VirtualEndpointSpec spec;
    spec.id = "test-stream-output";
    spec.name = "Test Stream Output";
    spec.kind = VirtualEndpointKind::Output;
    spec.channels = 2;
    CHECK(manager.addEndpoint(spec).ok);
    CHECK(manager.bindOutput(spec.id, streamBus->id()).ok);
    CHECK(manager.setEndpointEnabled(spec.id, true).ok);
    CHECK(manager.start(48000.0, 128).ok);

    SharedRingBuffer client;
    const Status opened = client.open(spec.id, SharedRingRole::Consumer);
    CHECK_MSG(opened.ok, opened.message);

    // Silnik produkuje sygnał; transport ma go przenieść do klienta.
    std::vector<Sample> feed(128 * 2, 0.4f);
    for (int i = 0; i < 20; ++i) {
        source->ring().write(feed.data(), 128);
        engine.processBlock(128);
    }
    manager.pump(2048);

    CHECK(client.availableToRead() > 0);

    std::vector<Sample> received(512 * 2, 0.0f);
    const int frames = client.read(received.data(), 512);
    CHECK(frames > 0);

    float peak = 0.0f;
    for (int i = 0; i < frames * 2; ++i) peak = std::max(peak, std::fabs(received[static_cast<std::size_t>(i)]));
    CHECK_NEAR(peak, 0.4f, 0.03f);

    manager.stop();
    engine.setRunning(false);
}

TEST("virtual/przepływ od klienta do źródła") {
    using namespace helix::core;

    AudioEngine engine;
    EngineFormat format;
    format.sampleRate = 48000.0;
    format.blockFrames = 128;
    format.channels = 2;
    engine.configure(format);

    InputSource* source = engine.addSource(SourceKind::VirtualDevice, "Virtual In");
    engine.commitGraph();

    VirtualDeviceManager manager(engine, createSoftwareDriverInterface());
    VirtualEndpointSpec spec;
    spec.id = "test-virtual-input";
    spec.name = "Test Virtual Input";
    spec.kind = VirtualEndpointKind::Input;
    spec.channels = 2;

    CHECK(manager.addEndpoint(spec).ok);
    CHECK(manager.bindInput(spec.id, source->id()).ok);
    CHECK(manager.setEndpointEnabled(spec.id, true).ok);
    CHECK(manager.start(48000.0, 128).ok);

    SharedRingBuffer client;
    CHECK(client.open(spec.id, SharedRingRole::Producer).ok);

    std::vector<Sample> payload(256 * 2, 0.3f);
    CHECK_EQ(client.write(payload.data(), 256), 256);

    manager.pump(512);

    CHECK(source->ring().availableToRead() >= 256);
    CHECK(source->active());

    manager.stop();
}

TEST("virtual/domyślne punkty końcowe zgodne ze specyfikacją") {
    const auto& endpoints = defaultVirtualEndpoints();
    CHECK_EQ(endpoints.size(), std::size_t{6});

    bool hasStream = false;
    bool hasMixerInput = false;
    for (const auto& endpoint : endpoints) {
        if (endpoint.id == "stream-output") hasStream = true;
        if (endpoint.id == "virtual-mixer-input") {
            hasMixerInput = true;
            CHECK(endpoint.kind == VirtualEndpointKind::Input);
        }
    }
    CHECK(hasStream);
    CHECK(hasMixerInput);
}

TEST("virtual/moduł sterownika raportuje stan") {
    auto driver = createSoftwareDriverInterface();
    CHECK(driver != nullptr);

    const DriverStatus status = driver->query();
    CHECK(status.state == DriverState::NotInstalled);
    CHECK(!status.message.empty());
    CHECK_STR_EQ(driverStateName(DriverState::Installed), "installed");

    // Wariant programowy nie udaje, że potrafi instalować sterownik.
    CHECK(!driver->install("pakiet.inf").ok);
    CHECK(!driver->uninstall().ok);
    CHECK(driver->configureEndpoints(2, 4).ok);
}
