// Mierniki poziomu. Wartości liczone w silniku, GUI tylko je odczytuje (spec §13).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

#include "helix/AudioBuffer.h"
#include "helix/Types.h"

namespace helix {

/// Migawka pomiaru przekazywana do GUI.
struct MeterSnapshot {
    float rmsLeft   = 0.0f;
    float rmsRight  = 0.0f;
    float peakLeft  = 0.0f;
    float peakRight = 0.0f;
    bool  clipping  = false;
    std::uint32_t clipCount = 0;
};

/// Miernik RMS + peak z klasyczną balistyką (peak hold + opadanie).
/// process() wołany z wątku audio, snapshot() z wątku sterującego.
class LevelMeter {
public:
    void prepare(double sampleRate) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : kDefaultSampleRate;
        // Okno RMS 50 ms — kompromis między czytelnością a responsywnością.
        rmsCoeff_ = static_cast<float>(std::exp(-1.0 / (0.050 * sampleRate_)));
        // Peak opada 20 dB/s po czasie przytrzymania 800 ms.
        peakDecayPerSample_ = static_cast<float>(std::pow(10.0, -20.0 / (20.0 * sampleRate_)));
        holdFrames_ = static_cast<int>(0.800 * sampleRate_);
        reset();
    }

    void reset() noexcept {
        for (int c = 0; c < 2; ++c) {
            meanSquare_[c] = 0.0f;
            peakHold_[c]   = 0.0f;
            holdCounter_[c] = 0;
            rmsOut_[c].store(0.0f, std::memory_order_relaxed);
            peakOut_[c].store(0.0f, std::memory_order_relaxed);
        }
        clipping_.store(false, std::memory_order_relaxed);
        clipCount_.store(0, std::memory_order_relaxed);
    }

    void process(const AudioBufferView& buffer) noexcept {
        const int frames = buffer.frames();
        if (frames <= 0) return;

        const int channels = std::min(2, buffer.channels());
        bool clipped = false;
        std::uint32_t newClips = 0;

        for (int c = 0; c < channels; ++c) {
            const Sample* data = buffer.channel(c);
            float ms   = meanSquare_[c];
            float peak = 0.0f;

            for (int i = 0; i < frames; ++i) {
                const float s = data[i];
                ms = rmsCoeff_ * ms + (1.0f - rmsCoeff_) * (s * s);
                const float a = std::fabs(s);
                if (a > peak) peak = a;
            }

            meanSquare_[c] = ms;

            if (peak >= kClipThreshold) { clipped = true; ++newClips; }

            if (peak >= peakHold_[c]) {
                peakHold_[c]    = peak;
                holdCounter_[c] = holdFrames_;
            } else if (holdCounter_[c] > frames) {
                holdCounter_[c] -= frames;
            } else {
                holdCounter_[c] = 0;
                peakHold_[c] *= std::pow(peakDecayPerSample_, static_cast<float>(frames));
                if (peakHold_[c] < peak) peakHold_[c] = peak;
            }

            rmsOut_[c].store(std::sqrt(ms), std::memory_order_relaxed);
            peakOut_[c].store(peakHold_[c], std::memory_order_relaxed);
        }

        if (channels == 1) { // mono — duplikuj do prawego kanału dla GUI
            rmsOut_[1].store(rmsOut_[0].load(std::memory_order_relaxed), std::memory_order_relaxed);
            peakOut_[1].store(peakOut_[0].load(std::memory_order_relaxed), std::memory_order_relaxed);
        }

        if (clipped) {
            clipping_.store(true, std::memory_order_relaxed);
            clipCount_.fetch_add(newClips, std::memory_order_relaxed);
            clipHoldRemaining_ = holdFrames_;
        } else if (clipHoldRemaining_ > 0) {
            clipHoldRemaining_ -= frames;
            if (clipHoldRemaining_ <= 0) clipping_.store(false, std::memory_order_relaxed);
        }
    }

    [[nodiscard]] MeterSnapshot snapshot() const noexcept {
        MeterSnapshot s;
        s.rmsLeft   = rmsOut_[0].load(std::memory_order_relaxed);
        s.rmsRight  = rmsOut_[1].load(std::memory_order_relaxed);
        s.peakLeft  = peakOut_[0].load(std::memory_order_relaxed);
        s.peakRight = peakOut_[1].load(std::memory_order_relaxed);
        s.clipping  = clipping_.load(std::memory_order_relaxed);
        s.clipCount = clipCount_.load(std::memory_order_relaxed);
        return s;
    }

    void clearClip() noexcept {
        clipping_.store(false, std::memory_order_relaxed);
        clipCount_.store(0, std::memory_order_relaxed);
        clipHoldRemaining_ = 0;
    }

private:
    static constexpr float kClipThreshold = 0.999f;

    double sampleRate_ = kDefaultSampleRate;
    float  rmsCoeff_   = 0.999f;
    float  peakDecayPerSample_ = 0.9999f;
    int    holdFrames_ = 38400;

    float meanSquare_[2]{};
    float peakHold_[2]{};
    int   holdCounter_[2]{};
    int   clipHoldRemaining_ = 0;

    std::atomic<float> rmsOut_[2]{};
    std::atomic<float> peakOut_[2]{};
    std::atomic<bool>  clipping_{false};
    std::atomic<std::uint32_t> clipCount_{0};
};

} // namespace helix
