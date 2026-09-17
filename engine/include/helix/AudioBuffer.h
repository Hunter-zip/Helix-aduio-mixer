// Bufory audio w układzie planarnym (kanał po kanale).
#pragma once

#include <algorithm>
#include <cassert>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include "helix/Types.h"

namespace helix {

/// Nieposiadający widok na bufor planarny. Przekazywany do DSP.
class AudioBufferView {
public:
    AudioBufferView() = default;

    AudioBufferView(Sample* const* channels, int numChannels, int numFrames) noexcept
        : channels_(channels), numChannels_(numChannels), numFrames_(numFrames) {}

    [[nodiscard]] int channels() const noexcept { return numChannels_; }
    [[nodiscard]] int frames()   const noexcept { return numFrames_; }
    [[nodiscard]] bool empty()   const noexcept { return numChannels_ == 0 || numFrames_ == 0; }

    [[nodiscard]] Sample* channel(int index) noexcept {
        assert(index >= 0 && index < numChannels_);
        return channels_[index];
    }

    [[nodiscard]] const Sample* channel(int index) const noexcept {
        assert(index >= 0 && index < numChannels_);
        return channels_[index];
    }

    /// Widok na pierwsze `frames` ramek — bez kopiowania.
    [[nodiscard]] AudioBufferView head(int frames) const noexcept {
        return AudioBufferView(channels_, numChannels_, std::min(frames, numFrames_));
    }

    void clear() noexcept {
        for (int c = 0; c < numChannels_; ++c)
            std::memset(channels_[c], 0, static_cast<std::size_t>(numFrames_) * sizeof(Sample));
    }

    /// Dodaje `other` do bieżącego bufora ze wzmocnieniem `gain`.
    void addFrom(const AudioBufferView& other, float gain = 1.0f) noexcept {
        const int ch = std::min(numChannels_, other.channels());
        const int n  = std::min(numFrames_, other.frames());
        for (int c = 0; c < ch; ++c) {
            Sample* dst = channels_[c];
            const Sample* src = other.channel(c);
            for (int i = 0; i < n; ++i) dst[i] += src[i] * gain;
        }
    }

    void copyFrom(const AudioBufferView& other) noexcept {
        const int ch = std::min(numChannels_, other.channels());
        const int n  = std::min(numFrames_, other.frames());
        for (int c = 0; c < ch; ++c)
            std::memcpy(channels_[c], other.channel(c), static_cast<std::size_t>(n) * sizeof(Sample));
        for (int c = ch; c < numChannels_; ++c)
            std::memset(channels_[c], 0, static_cast<std::size_t>(numFrames_) * sizeof(Sample));
    }

    void applyGain(float gain) noexcept {
        for (int c = 0; c < numChannels_; ++c) {
            Sample* d = channels_[c];
            for (int i = 0; i < numFrames_; ++i) d[i] *= gain;
        }
    }

private:
    Sample* const* channels_ = nullptr;
    int numChannels_ = 0;
    int numFrames_   = 0;
};

/// Posiadający bufor planarny z wyrównaniem 64 B (linia cache).
/// Alokacja wyłącznie poza wątkiem audio — w RT używamy tylko widoków.
class AudioBuffer {
public:
    AudioBuffer() = default;

    AudioBuffer(int numChannels, int numFrames) { resize(numChannels, numFrames); }

    void resize(int numChannels, int numFrames) {
        numChannels_ = std::max(0, numChannels);
        capacity_    = std::max(0, numFrames);
        frames_      = capacity_;

        const std::size_t stride = alignedStride(static_cast<std::size_t>(capacity_));
        storage_.assign(stride * static_cast<std::size_t>(numChannels_) + kAlignment / sizeof(Sample), 0.0f);
        pointers_.resize(static_cast<std::size_t>(numChannels_));

        // Wyrównaj pierwszy kanał do granicy 64 B; kolejne dziedziczą wyrównanie ze stride.
        auto raw = reinterpret_cast<std::uintptr_t>(storage_.data());
        auto aligned = (raw + (kAlignment - 1)) & ~static_cast<std::uintptr_t>(kAlignment - 1);
        auto* base = reinterpret_cast<Sample*>(aligned);

        for (int c = 0; c < numChannels_; ++c)
            pointers_[static_cast<std::size_t>(c)] = base + stride * static_cast<std::size_t>(c);
    }

    /// Ustawia „aktywną” liczbę ramek bez realokacji (musi mieścić się w pojemności).
    void setActiveFrames(int frames) noexcept {
        assert(frames >= 0 && frames <= capacity_);
        frames_ = std::min(std::max(frames, 0), capacity_);
    }

    [[nodiscard]] int channels() const noexcept { return numChannels_; }
    [[nodiscard]] int frames()   const noexcept { return frames_; }
    [[nodiscard]] int capacity() const noexcept { return capacity_; }

    [[nodiscard]] AudioBufferView view() noexcept {
        return AudioBufferView(pointers_.data(), numChannels_, frames_);
    }

    [[nodiscard]] AudioBufferView view(int frames) noexcept {
        return AudioBufferView(pointers_.data(), numChannels_, std::min(frames, capacity_));
    }

    [[nodiscard]] Sample* channel(int index) noexcept {
        assert(index >= 0 && index < numChannels_);
        return pointers_[static_cast<std::size_t>(index)];
    }

    [[nodiscard]] const Sample* channel(int index) const noexcept {
        assert(index >= 0 && index < numChannels_);
        return pointers_[static_cast<std::size_t>(index)];
    }

    void clear() noexcept { view(capacity_).clear(); }

private:
    static constexpr std::size_t kAlignment = 64;

    static std::size_t alignedStride(std::size_t frames) noexcept {
        constexpr std::size_t samplesPerLine = kAlignment / sizeof(Sample);
        return ((frames + samplesPerLine - 1) / samplesPerLine) * samplesPerLine;
    }

    std::vector<Sample>  storage_;
    std::vector<Sample*> pointers_;
    int numChannels_ = 0;
    int frames_      = 0;
    int capacity_    = 0;
};

} // namespace helix
