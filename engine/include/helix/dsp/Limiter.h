// Limiter brickwall z look-ahead — zabezpieczenie przed clippingiem (spec §11).
// Używany zarówno na kanale, jak i jako limiter Master.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "helix/Denormal.h"
#include "helix/Parameter.h"
#include "helix/dsp/AudioPlugin.h"

namespace helix::dsp {

class LimiterPlugin final : public ParameterizedPlugin {
public:
    LimiterPlugin() {
        addParameter({"ceiling",   "Ceiling",   "dB", -24.0f,  0.0f, -1.0f, ParamScale::Decibels, {}});
        addParameter({"lookahead", "Lookahead", "ms",   0.0f,  10.0f, 1.5f, ParamScale::Linear, {}});
        addParameter({"release",   "Release",   "ms",   1.0f, 1000.0f, 80.0f, ParamScale::Logarithmic, {}});
    }

    [[nodiscard]] const char* typeId() const noexcept override { return "limiter"; }
    [[nodiscard]] const char* displayName() const noexcept override { return "Limiter"; }

    void initialize(const ProcessContext& context) override {
        sampleRate_ = context.sampleRate;
        channels_   = std::max(1, context.channels);

        // Maksymalny look-ahead prealokowany raz — w RT nie alokujemy niczego.
        maxLookahead_ = std::max(1, static_cast<int>(0.010 * sampleRate_) + 2);
        delay_.assign(static_cast<std::size_t>(maxLookahead_) * static_cast<std::size_t>(channels_), 0.0f);
        dequeValues_.assign(static_cast<std::size_t>(maxLookahead_) + 1, 1.0f);
        dequeIndices_.assign(static_cast<std::size_t>(maxLookahead_) + 1, 0);

        markDirty();
        reset();
    }

    void reset() noexcept override {
        std::fill(delay_.begin(), delay_.end(), 0.0f);
        dequeHead_ = 0;
        dequeTail_ = 0;
        writeIndex_ = 0;
        sampleCounter_ = 0;
        smoothedGain_ = 1.0f;
        reductionOut_.store(0.0f, std::memory_order_relaxed);
    }

    [[nodiscard]] int latencyFrames() const noexcept override { return lookaheadFrames_; }

    void process(AudioBufferView& buffer) noexcept override {
        if (consumeDirty()) updateCoefficients();

        const int frames   = buffer.frames();
        const int channels = std::min(buffer.channels(), channels_);
        if (frames <= 0 || channels <= 0) return;

        const int window = lookaheadFrames_;
        float maxReduction = 0.0f;

        for (int i = 0; i < frames; ++i) {
            // 1. Peak bieżącej próbki → wymagane wzmocnienie, by nie przekroczyć sufitu.
            float peak = 0.0f;
            for (int c = 0; c < channels; ++c)
                peak = std::max(peak, std::fabs(buffer.channel(c)[i]));

            const float required = (peak > ceiling_) ? (ceiling_ / peak) : 1.0f;

            // 2. Zapis próbek do linii opóźniającej.
            const std::size_t slot = static_cast<std::size_t>(writeIndex_) * static_cast<std::size_t>(channels_);
            for (int c = 0; c < channels_; ++c)
                delay_[slot + static_cast<std::size_t>(c)] =
                    (c < channels) ? buffer.channel(c)[i] : 0.0f;

            // 3. Minimum kroczące wzmocnienia w oknie look-ahead (monotoniczny deque).
            pushGain(required, sampleCounter_);
            popExpired(sampleCounter_ - static_cast<std::int64_t>(window) + 1);
            const float windowMin = dequeValues_[static_cast<std::size_t>(dequeHead_)];

            // 4. Wygładzanie: w dół natychmiast (look-ahead już to przewidział),
            //    w górę zgodnie z czasem release.
            if (windowMin < smoothedGain_) smoothedGain_ = windowMin;
            else smoothedGain_ = windowMin + (smoothedGain_ - windowMin) * releaseCoeff_;
            smoothedGain_ = flushDenormal(smoothedGain_);

            // 5. Odczyt próbki opóźnionej o okno look-ahead i aplikacja wzmocnienia.
            const int readIndex = (writeIndex_ + maxLookahead_ - window) % maxLookahead_;
            const std::size_t readSlot =
                static_cast<std::size_t>(readIndex) * static_cast<std::size_t>(channels_);

            for (int c = 0; c < channels; ++c) {
                float v = delay_[readSlot + static_cast<std::size_t>(c)] * smoothedGain_;
                // Twarde zabezpieczenie: nic nie wychodzi ponad sufit.
                v = std::clamp(v, -ceiling_, ceiling_);
                buffer.channel(c)[i] = v;
            }

            maxReduction = std::max(maxReduction, -gainToDb(smoothedGain_));
            writeIndex_ = (writeIndex_ + 1) % maxLookahead_;
            ++sampleCounter_;
        }

        reductionOut_.store(maxReduction, std::memory_order_relaxed);
    }

    /// Chwilowa redukcja w dB (wskaźnik w GUI).
    [[nodiscard]] float gainReductionDb() const noexcept {
        return reductionOut_.load(std::memory_order_relaxed);
    }

private:
    void updateCoefficients() noexcept {
        ceiling_ = dbToGain(raw(0));
        const int requested = static_cast<int>(raw(1) * 0.001 * sampleRate_);
        const int newLookahead = std::clamp(requested, 1, maxLookahead_ - 1);
        if (newLookahead != lookaheadFrames_) {
            lookaheadFrames_ = newLookahead;
            // Zmiana okna unieważnia deque — czyścimy go bez alokacji.
            dequeHead_ = dequeTail_ = 0;
            smoothedGain_ = 1.0f;
        }
        const double ms = std::max(0.01, static_cast<double>(raw(2)));
        releaseCoeff_ = static_cast<float>(std::exp(-1.0 / (ms * 0.001 * sampleRate_)));
    }

    void pushGain(float value, std::int64_t index) noexcept {
        while (dequeTail_ > dequeHead_ &&
               dequeValues_[static_cast<std::size_t>(dequeTail_ - 1)] >= value)
            --dequeTail_;

        const std::size_t cap = dequeValues_.size();
        if (static_cast<std::size_t>(dequeTail_) >= cap) {
            // Kompaktowanie bufora deque — przesuwamy zawartość na początek.
            const int n = dequeTail_ - dequeHead_;
            for (int k = 0; k < n; ++k) {
                dequeValues_[static_cast<std::size_t>(k)] =
                    dequeValues_[static_cast<std::size_t>(dequeHead_ + k)];
                dequeIndices_[static_cast<std::size_t>(k)] =
                    dequeIndices_[static_cast<std::size_t>(dequeHead_ + k)];
            }
            dequeHead_ = 0;
            dequeTail_ = n;
        }

        dequeValues_[static_cast<std::size_t>(dequeTail_)]  = value;
        dequeIndices_[static_cast<std::size_t>(dequeTail_)] = index;
        ++dequeTail_;
    }

    void popExpired(std::int64_t oldestAllowed) noexcept {
        while (dequeTail_ > dequeHead_ &&
               dequeIndices_[static_cast<std::size_t>(dequeHead_)] < oldestAllowed)
            ++dequeHead_;
        if (dequeHead_ == dequeTail_) { // nie powinno wystąpić, ale zabezpiecza odczyt
            dequeValues_[0]  = 1.0f;
            dequeIndices_[0] = oldestAllowed;
            dequeHead_ = 0;
            dequeTail_ = 1;
        }
    }

    double sampleRate_ = kDefaultSampleRate;
    int    channels_   = 2;

    float ceiling_       = 0.891f;  // -1 dBFS
    float releaseCoeff_  = 0.999f;
    int   lookaheadFrames_ = 72;
    int   maxLookahead_    = 482;

    std::vector<float>        delay_;
    std::vector<float>        dequeValues_;
    std::vector<std::int64_t> dequeIndices_;

    int          dequeHead_ = 0;
    int          dequeTail_ = 0;
    int          writeIndex_ = 0;
    std::int64_t sampleCounter_ = 0;
    float        smoothedGain_ = 1.0f;

    std::atomic<float> reductionOut_{0.0f};
};

} // namespace helix::dsp
