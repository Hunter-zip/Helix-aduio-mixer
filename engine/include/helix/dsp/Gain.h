// Wzmocnienie wejściowe z rampą — pierwszy element łańcucha (spec §8).
#pragma once

#include "helix/Denormal.h"
#include "helix/Parameter.h"
#include "helix/dsp/AudioPlugin.h"

namespace helix::dsp {

class GainPlugin final : public ParameterizedPlugin {
public:
    GainPlugin() {
        addParameter({"gain", "Gain", "dB", -40.0f, 40.0f, 0.0f, ParamScale::Decibels, {}});
        addParameter({"invertPhase", "Invert Phase", "", 0.0f, 1.0f, 0.0f, ParamScale::Boolean, {}});
    }

    [[nodiscard]] const char* typeId() const noexcept override { return "gain"; }
    [[nodiscard]] const char* displayName() const noexcept override { return "Gain"; }

    void initialize(const ProcessContext& context) override {
        smoothed_.prepare(context.sampleRate, 15.0, dbToGain(raw(0)));
        smoothed_.snapToTarget();
        markDirty();
    }

    void reset() noexcept override { smoothed_.snapToTarget(); }

    void process(AudioBufferView& buffer) noexcept override {
        if (consumeDirty()) {
            const float sign = raw(1) >= 0.5f ? -1.0f : 1.0f;
            smoothed_.setTarget(dbToGain(raw(0)) * sign);
        }
        smoothed_.updateTarget();

        const int frames = buffer.frames();
        if (frames <= 0) return;

        if (!smoothed_.isRamping()) {
            const float g = smoothed_.current();
            if (g != 1.0f) buffer.applyGain(g);
            return;
        }

        // Rampa liniowa w obrębie bloku — bez trzasków przy zmianie gainu.
        const float start = smoothed_.current();
        const float end   = smoothed_.valueAfter(frames);
        const float inc   = (end - start) / static_cast<float>(frames);

        for (int c = 0; c < buffer.channels(); ++c) {
            Sample* data = buffer.channel(c);
            float g = start;
            for (int i = 0; i < frames; ++i) { data[i] *= g; g += inc; }
        }
        smoothed_.skip(frames);
    }

private:
    SmoothedValue smoothed_;
};

} // namespace helix::dsp
