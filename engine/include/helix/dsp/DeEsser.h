// De-esser: zwrotnica Linkwitz-Riley 2. rzędu + kompresja pasma wysokiego (spec §8).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>

#include "helix/Denormal.h"
#include "helix/Parameter.h"
#include "helix/dsp/AudioPlugin.h"
#include "helix/dsp/Biquad.h"

namespace helix::dsp {

class DeEsserPlugin final : public ParameterizedPlugin {
public:
    DeEsserPlugin() {
        addParameter({"frequency", "Frequency", "Hz", 2000.0f, 16000.0f, 6500.0f, ParamScale::Logarithmic, {}});
        addParameter({"threshold", "Threshold", "dB",  -60.0f,     0.0f,  -26.0f, ParamScale::Decibels, {}});
        addParameter({"ratio",     "Ratio",     ":1",    1.0f,    20.0f,    4.0f, ParamScale::Logarithmic, {}});
        addParameter({"release",   "Release",   "ms",    5.0f,   500.0f,   60.0f, ParamScale::Logarithmic, {}});
        addParameter({"listen",    "Listen",    "",      0.0f,     1.0f,    0.0f, ParamScale::Boolean, {}});
    }

    [[nodiscard]] const char* typeId() const noexcept override { return "deesser"; }
    [[nodiscard]] const char* displayName() const noexcept override { return "De-Esser"; }

    void initialize(const ProcessContext& context) override {
        sampleRate_ = context.sampleRate;
        channels_   = std::min(kMaxStreamChannels, std::max(1, context.channels));
        markDirty();
        reset();
    }

    void reset() noexcept override {
        for (auto& s : lowStage1_)  s.reset();
        for (auto& s : lowStage2_)  s.reset();
        for (auto& s : highStage1_) s.reset();
        for (auto& s : highStage2_) s.reset();
        gainReductionDb_ = 0.0f;
    }

    void process(AudioBufferView& buffer) noexcept override {
        if (consumeDirty()) updateCoefficients();

        const int frames   = buffer.frames();
        const int channels = std::min(buffer.channels(), channels_);
        if (frames <= 0 || channels <= 0) return;

        float maxReduction = 0.0f;

        for (int i = 0; i < frames; ++i) {
            // Rozdzielenie pasm (LR2: dwa kaskadowe Butterworthy Q=0.707).
            std::array<float, kMaxStreamChannels> low{};
            std::array<float, kMaxStreamChannels> high{};
            float detector = 0.0f;

            for (int c = 0; c < channels; ++c) {
                const float x  = buffer.channel(c)[i];
                const auto  ci = static_cast<std::size_t>(c);
                float l = lowStage1_[ci].process(x, lowCoeff_);
                l       = lowStage2_[ci].process(l, lowCoeff_);
                float h = highStage1_[ci].process(x, highCoeff_);
                h       = highStage2_[ci].process(h, highCoeff_);
                low[ci]  = l;
                high[ci] = h;
                detector = std::max(detector, std::fabs(h));
            }

            const float levelDb = (detector > 1.0e-7f) ? 20.0f * std::log10(detector) : -140.0f;
            const float over = levelDb - thresholdDb_;
            const float targetReduction = (over > 0.0f) ? over * slope_ : 0.0f;

            // Szybki atak (syczenie jest krótkie), release parametryzowany.
            const float coeff = (targetReduction > gainReductionDb_) ? attackCoeff_ : releaseCoeff_;
            gainReductionDb_ = flushDenormal(targetReduction + (gainReductionDb_ - targetReduction) * coeff);
            maxReduction = std::max(maxReduction, gainReductionDb_);

            const float hiGain = dbToGain(-gainReductionDb_);

            for (int c = 0; c < channels; ++c) {
                const auto ci = static_cast<std::size_t>(c);
                // LR2 wymaga odwrócenia fazy jednego pasma, by suma była płaska.
                buffer.channel(c)[i] = listen_ ? (high[ci] * hiGain)
                                               : (low[ci] - high[ci] * hiGain);
            }
        }

        reductionOut_.store(maxReduction, std::memory_order_relaxed);
    }

    [[nodiscard]] float gainReductionDb() const noexcept {
        return reductionOut_.load(std::memory_order_relaxed);
    }

private:
    void updateCoefficients() noexcept {
        const double freq = raw(0);
        lowCoeff_  = BiquadCoefficients::design(FilterType::HighCut, freq, 0.70710678, 0.0, sampleRate_);
        highCoeff_ = BiquadCoefficients::design(FilterType::LowCut,  freq, 0.70710678, 0.0, sampleRate_);
        thresholdDb_ = raw(1);
        slope_       = 1.0f - 1.0f / std::max(1.0f, raw(2));
        attackCoeff_ = timeToCoeff(1.0f);
        releaseCoeff_= timeToCoeff(raw(3));
        listen_      = raw(4) >= 0.5f;
    }

    [[nodiscard]] float timeToCoeff(float milliseconds) const noexcept {
        const double ms = std::max(0.01, static_cast<double>(milliseconds));
        return static_cast<float>(std::exp(-1.0 / (ms * 0.001 * sampleRate_)));
    }

    double sampleRate_ = kDefaultSampleRate;
    int    channels_   = 2;

    BiquadCoefficients lowCoeff_{};
    BiquadCoefficients highCoeff_{};
    std::array<BiquadState, kMaxStreamChannels> lowStage1_{};
    std::array<BiquadState, kMaxStreamChannels> lowStage2_{};
    std::array<BiquadState, kMaxStreamChannels> highStage1_{};
    std::array<BiquadState, kMaxStreamChannels> highStage2_{};

    float thresholdDb_  = -26.0f;
    float slope_        = 0.75f;
    float attackCoeff_  = 0.5f;
    float releaseCoeff_ = 0.99f;
    bool  listen_       = false;

    float gainReductionDb_ = 0.0f;
    std::atomic<float> reductionOut_{0.0f};
};

} // namespace helix::dsp
