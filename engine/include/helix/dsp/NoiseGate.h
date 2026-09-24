// Bramka szumów z klasyczną obwiednią attack/hold/release (spec §8).
#pragma once

#include <algorithm>
#include <cmath>

#include "helix/Denormal.h"
#include "helix/Parameter.h"
#include "helix/dsp/AudioPlugin.h"

namespace helix::dsp {

class NoiseGatePlugin final : public ParameterizedPlugin {
public:
    NoiseGatePlugin() {
        addParameter({"threshold", "Threshold", "dB", -90.0f,   0.0f, -45.0f, ParamScale::Decibels, {}});
        addParameter({"attack",    "Attack",    "ms",   0.1f, 200.0f,   2.0f, ParamScale::Logarithmic, {}});
        addParameter({"hold",      "Hold",      "ms",   0.0f, 2000.0f, 120.0f, ParamScale::Linear, {}});
        addParameter({"release",   "Release",   "ms",   5.0f, 3000.0f, 180.0f, ParamScale::Logarithmic, {}});
        addParameter({"range",     "Range",     "dB", -90.0f,   0.0f, -60.0f, ParamScale::Decibels, {}});
        addParameter({"hysteresis","Hysteresis","dB",   0.0f,  24.0f,   4.0f, ParamScale::Linear, {}});
    }

    [[nodiscard]] const char* typeId() const noexcept override { return "noisegate"; }
    [[nodiscard]] const char* displayName() const noexcept override { return "Noise Gate"; }

    void initialize(const ProcessContext& context) override {
        sampleRate_ = context.sampleRate;
        markDirty();
        reset();
    }

    void reset() noexcept override {
        envelope_ = 0.0f;
        gain_     = 0.0f;
        holdCounter_ = 0;
        open_     = false;
    }

    void process(AudioBufferView& buffer) noexcept override {
        if (consumeDirty()) updateCoefficients();

        const int frames   = buffer.frames();
        const int channels = buffer.channels();
        if (frames <= 0 || channels <= 0) return;

        for (int i = 0; i < frames; ++i) {
            // Detektor: maksimum z kanałów (bramka pracuje w trybie stereo-link).
            float peak = 0.0f;
            for (int c = 0; c < channels; ++c)
                peak = std::max(peak, std::fabs(buffer.channel(c)[i]));

            // Szybki atak / wolniejszy zanik obwiedni detektora.
            envelope_ = (peak > envelope_) ? peak
                                           : envelope_ + (peak - envelope_) * detectorRelease_;
            envelope_ = flushDenormal(envelope_);

            const float openLevel  = openThreshold_;
            const float closeLevel = closeThreshold_;

            if (!open_ && envelope_ >= openLevel) {
                open_ = true;
                holdCounter_ = holdFrames_;
            } else if (open_ && envelope_ < closeLevel) {
                if (holdCounter_ > 0) --holdCounter_;
                else open_ = false;
            } else if (open_) {
                holdCounter_ = holdFrames_;
            }

            const float target = open_ ? 1.0f : rangeGain_;
            const float coeff  = (target > gain_) ? attackCoeff_ : releaseCoeff_;
            gain_ = flushDenormal(target + (gain_ - target) * coeff);

            for (int c = 0; c < channels; ++c)
                buffer.channel(c)[i] *= gain_;
        }

        currentGain_.store(gain_, std::memory_order_relaxed);
    }

    /// Bieżące tłumienie bramki (dla GUI).
    [[nodiscard]] float currentGain() const noexcept { return currentGain_.load(std::memory_order_relaxed); }

private:
    void updateCoefficients() noexcept {
        const float thresholdDb  = raw(0);
        const float hysteresisDb = raw(5);
        openThreshold_  = dbToGain(thresholdDb);
        closeThreshold_ = dbToGain(thresholdDb - hysteresisDb);
        rangeGain_      = dbToGain(raw(4));
        attackCoeff_    = timeToCoeff(raw(1));
        releaseCoeff_   = timeToCoeff(raw(3));
        detectorRelease_ = 1.0f - timeToCoeff(30.0f); // ~30 ms zanik detektora
        holdFrames_     = static_cast<int>(raw(2) * 0.001 * sampleRate_);
    }

    [[nodiscard]] float timeToCoeff(float milliseconds) const noexcept {
        const double ms = std::max(0.01, static_cast<double>(milliseconds));
        return static_cast<float>(std::exp(-1.0 / (ms * 0.001 * sampleRate_)));
    }

    double sampleRate_ = kDefaultSampleRate;

    float openThreshold_  = 0.0056f;
    float closeThreshold_ = 0.0035f;
    float rangeGain_      = 0.001f;
    float attackCoeff_    = 0.9f;
    float releaseCoeff_   = 0.99f;
    float detectorRelease_ = 0.001f;
    int   holdFrames_     = 5760;

    float envelope_ = 0.0f;
    float gain_     = 0.0f;
    int   holdCounter_ = 0;
    bool  open_     = false;

    std::atomic<float> currentGain_{0.0f};
};

} // namespace helix::dsp
