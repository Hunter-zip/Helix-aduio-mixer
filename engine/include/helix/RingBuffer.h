// Bezblokadowy bufor pierścieniowy na ramki audio (przeplot).
// Most między wątkiem urządzenia a wątkiem renderującym silnika.
#pragma once

#include <algorithm>
#include <atomic>
#include <cstring>
#include <vector>

#include "helix/Types.h"

namespace helix {

/// SPSC, pojemność zaokrąglana w górę do potęgi dwójki.
/// Zapis i odczyt są wolne od blokad i alokacji.
class AudioRingBuffer {
public:
    AudioRingBuffer() = default;

    AudioRingBuffer(int numChannels, int capacityFrames) { reset(numChannels, capacityFrames); }

    void reset(int numChannels, int capacityFrames) {
        channels_ = std::max(1, numChannels);
        capacity_ = static_cast<int>(nextPowerOfTwo(static_cast<std::size_t>(std::max(2, capacityFrames))));
        data_.assign(static_cast<std::size_t>(capacity_) * static_cast<std::size_t>(channels_), 0.0f);
        mask_ = static_cast<std::size_t>(capacity_) - 1;
        write_.store(0, std::memory_order_relaxed);
        read_.store(0, std::memory_order_relaxed);
        overruns_.store(0, std::memory_order_relaxed);
        underruns_.store(0, std::memory_order_relaxed);
    }

    [[nodiscard]] int channels() const noexcept { return channels_; }
    [[nodiscard]] int capacityFrames() const noexcept { return capacity_; }

    [[nodiscard]] int availableToRead() const noexcept {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_acquire);
        return static_cast<int>((w - r) & mask_);
    }

    [[nodiscard]] int availableToWrite() const noexcept {
        return capacity_ - 1 - availableToRead();
    }

    /// Zapis z przeplotu. Zwraca liczbę zapisanych ramek; brakujące liczy jako overrun.
    int write(const Sample* interleaved, int frames) noexcept {
        const int canWrite = std::min(frames, availableToWrite());
        if (canWrite < frames)
            overruns_.fetch_add(static_cast<std::uint64_t>(frames - canWrite), std::memory_order_relaxed);
        if (canWrite <= 0) return 0;

        std::size_t w = write_.load(std::memory_order_relaxed);
        const int first = std::min(canWrite, capacity_ - static_cast<int>(w & mask_));
        copyIn(interleaved, w & mask_, first);
        if (canWrite > first)
            copyIn(interleaved + static_cast<std::size_t>(first) * static_cast<std::size_t>(channels_), 0,
                   canWrite - first);

        write_.store((w + static_cast<std::size_t>(canWrite)) & mask_, std::memory_order_release);
        return canWrite;
    }

    /// Zapis ciszy (używany gdy źródło ucichło — utrzymuje ciągłość zegara).
    int writeSilence(int frames) noexcept {
        const int canWrite = std::min(frames, availableToWrite());
        if (canWrite <= 0) return 0;
        std::size_t w = write_.load(std::memory_order_relaxed);
        const int first = std::min(canWrite, capacity_ - static_cast<int>(w & mask_));
        std::memset(slot(w & mask_), 0, frameBytes(first));
        if (canWrite > first) std::memset(slot(0), 0, frameBytes(canWrite - first));
        write_.store((w + static_cast<std::size_t>(canWrite)) & mask_, std::memory_order_release);
        return canWrite;
    }

    /// Odczyt do bufora planarnego (deinterleave). Ramki, których brakuje, są wyzerowane
    /// i policzone jako underrun — silnik nigdy nie blokuje się na źródle (spec §23).
    int readPlanar(Sample* const* destChannels, int destChannelCount, int frames) noexcept {
        const int avail = std::min(frames, availableToRead());
        if (avail < frames)
            underruns_.fetch_add(static_cast<std::uint64_t>(frames - avail), std::memory_order_relaxed);

        std::size_t r = read_.load(std::memory_order_relaxed);
        for (int i = 0; i < avail; ++i) {
            const Sample* src = slot((r + static_cast<std::size_t>(i)) & mask_);
            for (int c = 0; c < destChannelCount; ++c)
                destChannels[c][i] = src[std::min(c, channels_ - 1)];
        }
        for (int c = 0; c < destChannelCount; ++c)
            std::memset(destChannels[c] + avail, 0,
                        static_cast<std::size_t>(frames - avail) * sizeof(Sample));

        if (avail > 0)
            read_.store((r + static_cast<std::size_t>(avail)) & mask_, std::memory_order_release);
        return avail;
    }

    /// Odczyt do bufora z przeplotem (dla urządzeń wyjściowych).
    int readInterleaved(Sample* dest, int destChannelCount, int frames) noexcept {
        const int avail = std::min(frames, availableToRead());
        if (avail < frames)
            underruns_.fetch_add(static_cast<std::uint64_t>(frames - avail), std::memory_order_relaxed);

        std::size_t r = read_.load(std::memory_order_relaxed);
        for (int i = 0; i < avail; ++i) {
            const Sample* src = slot((r + static_cast<std::size_t>(i)) & mask_);
            Sample* dst = dest + static_cast<std::size_t>(i) * static_cast<std::size_t>(destChannelCount);
            for (int c = 0; c < destChannelCount; ++c)
                dst[c] = src[std::min(c, channels_ - 1)];
        }
        std::memset(dest + static_cast<std::size_t>(avail) * static_cast<std::size_t>(destChannelCount), 0,
                    static_cast<std::size_t>(frames - avail) * static_cast<std::size_t>(destChannelCount)
                        * sizeof(Sample));

        if (avail > 0)
            read_.store((r + static_cast<std::size_t>(avail)) & mask_, std::memory_order_release);
        return avail;
    }

    /// Porzuca `frames` ramek (kompensacja dryftu zegara — patrz Resampler).
    int discard(int frames) noexcept {
        const int n = std::min(frames, availableToRead());
        if (n > 0) {
            std::size_t r = read_.load(std::memory_order_relaxed);
            read_.store((r + static_cast<std::size_t>(n)) & mask_, std::memory_order_release);
        }
        return n;
    }

    void clear() noexcept {
        read_.store(write_.load(std::memory_order_acquire), std::memory_order_release);
    }

    [[nodiscard]] std::uint64_t overruns()  const noexcept { return overruns_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::uint64_t underruns() const noexcept { return underruns_.load(std::memory_order_relaxed); }

    void resetCounters() noexcept {
        overruns_.store(0, std::memory_order_relaxed);
        underruns_.store(0, std::memory_order_relaxed);
    }

private:
    static std::size_t nextPowerOfTwo(std::size_t v) noexcept {
        std::size_t p = 2;
        while (p < v) p <<= 1;
        return p;
    }

    [[nodiscard]] std::size_t frameBytes(int frames) const noexcept {
        return static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels_) * sizeof(Sample);
    }

    [[nodiscard]] Sample* slot(std::size_t frameIndex) noexcept {
        return data_.data() + frameIndex * static_cast<std::size_t>(channels_);
    }

    [[nodiscard]] const Sample* slot(std::size_t frameIndex) const noexcept {
        return data_.data() + frameIndex * static_cast<std::size_t>(channels_);
    }

    void copyIn(const Sample* src, std::size_t frameIndex, int frames) noexcept {
        std::memcpy(slot(frameIndex), src, frameBytes(frames));
    }

    std::vector<Sample> data_;
    int channels_ = 1;
    int capacity_ = 2;
    std::size_t mask_ = 1;
    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
    std::atomic<std::uint64_t> overruns_{0};
    std::atomic<std::uint64_t> underruns_{0};
};

} // namespace helix
