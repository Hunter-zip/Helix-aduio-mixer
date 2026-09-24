// Kompresor z miękkim kolanem, detekcją peak/RMS i sprzężeniem stereo (spec §10).
#pragma once

#include <algorithm>
#include <cmath>

#include "helix/Denormal.h"
#include "helix/Parameter.h"
#include "helix/dsp/AudioPlugin.h"

namespace helix::dsp {

class CompressorPlugin final : public ParameterizedPlugin {
public:
    CompressorPlugin() {
        addParameter({"threshold", "Threshold", "dB", -60.0f,   0.0f, -18.0f, ParamScale::Decibels, {}});
        addParameter({"ratio",     "Ratio",     ":1",   1.0f,  20.0f,   3.0f, ParamScale::Logarithmic, {}});
        addParameter({"attack",    "Attack",    "ms",   0.1f, 200.0f,  10.0f, ParamScale::Logarithmic, {}});
        addParameter({"release",   "Release",   "ms",   5.0f, 2000.0f, 120.0f, ParamScale::Logarithmic, {}});
        addParameter({"makeup",    "Makeup",    "dB", -12.0f,  24.0f,   0.0f, ParamScale::Decibels, {}});
        addParameter({"knee",      "Knee",      "dB",   0.0f,  24.0f,   6.0f, ParamScale::Linear, {}});
        addParameter({"detector",  "Detector",  "",     0.0f,   1.0f,   0.0f, ParamScale::Choice, {"peak", "rms"}});
        addParameter({"autoMakeup","Auto Makeup","",    0.0f,   1.0f,   0.0f, ParamScale::Boolean, {}});
    }

    [[nodiscard]] const char* typeId() const noexcept override { return "compressor"; }
    [[nodiscard]] const char* displayName() const noexcept override { return "Compressor"; }

    void initialize(const ProcessContext& context) override {
        sampleRate_ = context.sampleRate;
        markDirty();
        reset();
    }

    void reset() noexcept override {
        envelopeDb_ = -120.0f;
        rmsState_   = 0.0f;
        gainReductionDb_ = 0.0f;
    }

    void process(AudioBufferView& buffer) noexcept override {
        if (consumeDirty()) updateCoefficients();

        const int frames   = buffer.frames();
        const int channels = buffer.channels();
        if (frames <= 0 || channels <= 0) return;

        float maxReduction = 0.0f;

        for (int i = 0; i < frames; ++i) {
            float detect;
            if (useRms_) {
                float sum = 0.0f;
                for (int c = 0; c < channels; ++c) {
                    const float s = buffer.channel(c)[i];
                    sum += s * s;
                }
                sum /= static_cast<float>(channels);
                rmsState_ = flushDenormal(rmsCoeff_ * rmsState_ + (1.0f - rmsCoeff_) * sum);
                detect = std::sqrt(rmsState_);
            } else {
                float peak = 0.0f;
                for (int c = 0; c < channels; ++c)
                    peak = std::max(peak, std::fabs(buffer.channel(c)[i]));
                detect = peak;
            }

            const float levelDb = (detect > 1.0e-7f) ? 20.0f * std::log10(detect) : -140.0f;

            // Statyczna charakterystyka z miękkim kolanem.
            const float over = levelDb - thresholdDb_;
            float reductionDb = 0.0f;
            if (knee_ > 0.0001f) {
                if (over >= knee_ * 0.5f)      reductionDb = over * slope_;
                else if (over > -knee_ * 0.5f) {
                    const float t = over + knee_ * 0.5f;
                    reductionDb = slope_ * (t * t) / (2.0f * knee_);
                }
            } else if (over > 0.0f) {
                reductionDb = over * slope_;
            }

            // Wygładzanie w dziedzinie dB: szybciej w dół, wolniej w górę.
            const float coeff = (reductionDb > gainReductionDb_) ? attackCoeff_ : releaseCoeff_;
            gainReductionDb_ = flushDenormal(reductionDb + (gainReductionDb_ - reductionDb) * coeff);
            maxReduction = std::max(maxReduction, gainReductionDb_);

            const float gain = dbToGain(makeupDb_ - gainReductionDb_);
            for (int c = 0; c < channels; ++c)
                buffer.channel(c)[i] *= gain;
        }

        gainReductionOut_.store(maxReduction, std::memory_order_relaxed);
    }

    /// Chwilowa redukcja wzmocnienia w dB (dla wskaźnika GR w GUI).
    [[nodiscard]] float gainReductionDb() const noexcept {
        return gainReductionOut_.load(std::memory_order_relaxed);
    }

private:
    void updateCoefficients() noexcept {
        thresholdDb_ = raw(0);
        const float ratio = std::max(1.0f, raw(1));
        slope_       = 1.0f - 1.0f / ratio;
        attackCoeff_ = timeToCoeff(raw(2));
        releaseCoeff_= timeToCoeff(raw(3));
        knee_        = raw(5);
        useRms_      = raw(6) >= 0.5f;
        rmsCoeff_    = timeToCoeff(20.0f);

        // Auto-makeup: kompensacja połowy teoretycznej redukcji na progu.
        makeupDb_ = (raw(7) >= 0.5f) ? (-thresholdDb_ * slope_ * 0.5f) : raw(4);
    }

    [[nodiscard]] float timeToCoeff(float milliseconds) const noexcept {
        const double ms = std::max(0.01, static_cast<double>(milliseconds));
        return static_cast<float>(std::exp(-1.0 / (ms * 0.001 * sampleRate_)));
    }

    double sampleRate_ = kDefaultSampleRate;

    float thresholdDb_  = -18.0f;
    float slope_        = 0.667f;
    float attackCoeff_  = 0.9f;
    float releaseCoeff_ = 0.99f;
    float makeupDb_     = 0.0f;
    float knee_         = 6.0f;
    float rmsCoeff_     = 0.99f;
    bool  useRms_       = false;

    float envelopeDb_      = -120.0f;
    float rmsState_        = 0.0f;
    float gainReductionDb_ = 0.0f;

    std::atomic<float> gainReductionOut_{0.0f};
};

} // namespace helix::dsp
