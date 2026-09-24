#include <atomic>
#include <thread>
#include <vector>

#include "TestFramework.h"
#include "helix/RingBuffer.h"
#include "helix/SpscQueue.h"
#include "helix/core/AudioEngine.h"

using namespace helix;

TEST("spsc/kolejka zachowuje kolejność i sygnalizuje przepełnienie") {
    SpscQueue<int> queue(4);
    CHECK(queue.empty());

    // Pojemność jest zaokrąglana w górę do potęgi dwójki minus jeden slot.
    const int capacity = static_cast<int>(queue.capacity());
    CHECK(capacity >= 4);

    for (int i = 0; i < capacity; ++i) CHECK(queue.push(i));
    CHECK(!queue.push(99));   // pełna
    CHECK_EQ(static_cast<int>(queue.size()), capacity);

    int value = -1;
    for (int i = 0; i < capacity; ++i) {
        CHECK(queue.pop(value));
        CHECK_EQ(value, i);
    }
    CHECK(!queue.pop(value));
    CHECK(queue.empty());
}

TEST("spsc/producent i konsument na dwóch wątkach") {
    constexpr int kCount = 200000;
    SpscQueue<int> queue(1024);

    std::atomic<bool> producerDone{false};
    std::thread producer([&] {
        for (int i = 0; i < kCount;) {
            if (queue.push(i)) ++i;
            else std::this_thread::yield();
        }
        producerDone.store(true, std::memory_order_release);
    });

    int expected = 0;
    int value = 0;
    while (expected < kCount) {
        if (queue.pop(value)) {
            CHECK_EQ(value, expected);
            ++expected;
        } else if (producerDone.load(std::memory_order_acquire) && queue.empty()) {
            break;
        }
    }

    producer.join();
    CHECK_EQ(expected, kCount);
}

TEST("ring/zapis i odczyt zachowują próbki") {
    AudioRingBuffer ring(2, 1024);
    CHECK_EQ(ring.availableToRead(), 0);

    std::vector<Sample> input(256 * 2);
    for (std::size_t i = 0; i < input.size(); i += 2) {
        input[i]     = static_cast<Sample>(i) * 0.001f;
        input[i + 1] = -static_cast<Sample>(i) * 0.001f;
    }

    CHECK_EQ(ring.write(input.data(), 256), 256);
    CHECK_EQ(ring.availableToRead(), 256);

    std::vector<Sample> left(256), right(256);
    Sample* planar[2] = {left.data(), right.data()};
    CHECK_EQ(ring.readPlanar(planar, 2, 256), 256);

    for (int i = 0; i < 256; ++i) {
        CHECK_NEAR(left[static_cast<std::size_t>(i)], input[static_cast<std::size_t>(i) * 2], 1e-9);
        CHECK_NEAR(right[static_cast<std::size_t>(i)], input[static_cast<std::size_t>(i) * 2 + 1], 1e-9);
    }
    CHECK_EQ(ring.availableToRead(), 0);
}

TEST("ring/niedomiar zwraca ciszę i jest liczony") {
    AudioRingBuffer ring(2, 512);
    std::vector<Sample> input(64 * 2, 0.5f);
    ring.write(input.data(), 64);

    std::vector<Sample> left(128, -1.0f), right(128, -1.0f);
    Sample* planar[2] = {left.data(), right.data()};
    CHECK_EQ(ring.readPlanar(planar, 2, 128), 64);

    for (int i = 64; i < 128; ++i)
        CHECK_NEAR(left[static_cast<std::size_t>(i)], 0.0f, 1e-9);
    CHECK_EQ(ring.underruns(), std::uint64_t{64});
}

TEST("ring/nadmiar nie nadpisuje danych") {
    AudioRingBuffer ring(2, 128);
    std::vector<Sample> input(256 * 2, 0.25f);

    const int written = ring.write(input.data(), 256);
    CHECK(written < 256);
    CHECK_EQ(ring.overruns(), static_cast<std::uint64_t>(256 - written));
    CHECK_EQ(ring.availableToRead(), written);
}

TEST("ring/przeplot wyjściowy zgadza się z wejściem") {
    AudioRingBuffer ring(2, 512);
    std::vector<Sample> input(100 * 2);
    for (std::size_t i = 0; i < input.size(); ++i) input[i] = static_cast<Sample>(i);
    ring.write(input.data(), 100);

    std::vector<Sample> output(100 * 2, -1.0f);
    CHECK_EQ(ring.readInterleaved(output.data(), 2, 100), 100);
    for (std::size_t i = 0; i < input.size(); ++i)
        CHECK_NEAR(output[i], input[i], 1e-9);
}

TEST("ring/współbieżny zapis i odczyt nie gubi ciągłości") {
    AudioRingBuffer ring(1, 4096);
    constexpr int kTotal = 100000;

    std::thread writer([&] {
        std::vector<Sample> chunk(64);
        int counter = 0;
        while (counter < kTotal) {
            const int frames = std::min(64, kTotal - counter);
            for (int i = 0; i < frames; ++i)
                chunk[static_cast<std::size_t>(i)] = static_cast<Sample>((counter + i) % 1000);

            int written = 0;
            while (written == 0) {
                written = ring.write(chunk.data(), frames);
                if (written == 0) std::this_thread::yield();
            }
            counter += written;
        }
    });

    std::vector<Sample> output(64);
    Sample* planar[1] = {output.data()};
    int read = 0;
    int mismatches = 0;

    while (read < kTotal) {
        const int available = ring.availableToRead();
        if (available <= 0) { std::this_thread::yield(); continue; }

        const int frames = std::min({64, available, kTotal - read});
        ring.readPlanar(planar, 1, frames);
        for (int i = 0; i < frames; ++i)
            if (output[static_cast<std::size_t>(i)] != static_cast<Sample>((read + i) % 1000)) ++mismatches;
        read += frames;
    }

    writer.join();
    CHECK_EQ(mismatches, 0);
    CHECK_EQ(read, kTotal);
}

TEST("engine/zmiany struktury równolegle z przetwarzaniem") {
    using namespace helix::core;

    AudioEngine engine;
    EngineFormat format;
    format.sampleRate = 48000.0;
    format.blockFrames = 128;
    format.channels = 2;
    engine.configure(format);

    Bus* bus = engine.addBus("A", "A1", BusKind::Physical);
    engine.setPrimaryBus(bus->id());
    Channel* base = engine.addChannel("Base");
    engine.routing().setEnabled(base->index(), bus->index(), true);
    engine.commitGraph();
    engine.setRunning(true);

    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> blocks{0};

    // Wątek „audio” miele bloki, wątek sterujący dokłada i usuwa kanały.
    std::thread audio([&] {
        std::vector<Sample> device(128 * 2, 0.0f);
        while (!stop.load(std::memory_order_acquire)) {
            engine.renderBlock(device.data(), 128, 2);
            blocks.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 200; ++i) {
        Channel* channel = engine.addChannel("Tmp" + std::to_string(i));
        if (channel == nullptr) { engine.collectGarbage(); continue; }
        engine.routing().setEnabled(channel->index(), bus->index(), true);
        engine.commitGraph();
        engine.removeChannel(channel->id());
        engine.collectGarbage();
    }

    stop.store(true, std::memory_order_release);
    audio.join();
    engine.collectGarbage();

    CHECK(blocks.load() > 0);
    CHECK_EQ(engine.channels().size(), std::size_t{1});
}
