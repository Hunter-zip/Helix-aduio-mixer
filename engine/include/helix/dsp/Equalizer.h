// Equalizer parametryczny 10-pasmowy (spec §9) — każde pasmo to niezależny biquad.
#pragma once

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "helix/Denormal.h"
#include "helix/Parameter.h"
#include "helix/dsp/AudioPlugin.h"
#include "helix/dsp/Biquad.h"

namespace helix::dsp {

inline constexpr int kEqBands = 10;

/// Każde pasmo wnosi pięć parametrów (enabled, type, freq, gain, Q).
inline constexpr int kEqParametersPerBand = 5;

static_assert(static_cast<std::size_t>(kEqBands * kEqParametersPerBand) <= kMaxPluginParameters,
              "Equalizer nie mieści się w limicie parametrów pluginu");

/// Domyślne rozłożenie pasm — od filtra dolnozaporowego po górną półkę.
struct EqBandDefault {
    FilterType type;
    float frequency;
    float gainDb;
    float q;
    bool  enabled;
};

inline const std::array<EqBandDefault, kEqBands>& eqDefaults() noexcept {
    static const std::array<EqBandDefault, kEqBands> kDefaults{{
        {FilterType::LowCut,       80.0f, 0.0f, 0.707f, false},
        {FilterType::LowShelf,    120.0f, 0.0f, 0.707f, false},
        {FilterType::Bell,        250.0f, 0.0f, 1.000f, false},
        {FilterType::Bell,        500.0f, 0.0f, 1.000f, false},
        {FilterType::Bell,       1000.0f, 0.0f, 1.000f, false},
        {FilterType::Bell,       2000.0f, 0.0f, 1.000f, false},
        {FilterType::Bell,       4000.0f, 0.0f, 1.000f, false},
        {FilterType::Bell,       8000.0f, 0.0f, 1.000f, false},
        {FilterType::HighShelf, 12000.0f, 0.0f, 0.707f, false},
        {FilterType::HighCut,   18000.0f, 0.0f, 0.707f, false},
    }};
    return kDefaults;
}

class EqualizerPlugin final : public ParameterizedPlugin {
public:
    EqualizerPlugin() {
        const auto& defaults = eqDefaults();
        for (int b = 0; b < kEqBands; ++b) {
            const std::string prefix = "band" + std::to_string(b) + ".";
            const auto& d = defaults[static_cast<std::size_t>(b)];
            addParameter({prefix + "enabled", "Band " + std::to_string(b + 1) + " On", "",
                          0.0f, 1.0f, d.enabled ? 1.0f : 0.0f, ParamScale::Boolean, {}});
            addParameter({prefix + "type", "Band " + std::to_string(b + 1) + " Type", "",
                          0.0f, 7.0f, static_cast<float>(d.type), ParamScale::Choice,
                          {"lowcut", "highcut", "bell", "lowshelf", "highshelf", "notch", "bandpass", "allpass"}});
            addParameter({prefix + "freq", "Band " + std::to_string(b + 1) + " Freq", "Hz",
                          20.0f, 22000.0f, d.frequency, ParamScale::Logarithmic, {}});
            addParameter({prefix + "gain", "Band " + std::to_string(b + 1) + " Gain", "dB",
                          -24.0f, 24.0f, d.gainDb, ParamScale::Decibels, {}});
            addParameter({prefix + "q", "Band " + std::to_string(b + 1) + " Q", "",
                          0.1f, 18.0f, d.q, ParamScale::Logarithmic, {}});
        }
    }

    [[nodiscard]] const char* typeId() const noexcept override { return "equalizer"; }
    [[nodiscard]] const char* displayName() const noexcept override { return "Equalizer"; }

    void initialize(const ProcessContext& context) override {
        sampleRate_ = context.sampleRate;
        channels_   = std::min(kMaxStreamChannels, std::max(1, context.channels));
        markDirty();
        reset();
    }

    void reset() noexcept override {
        for (auto& band : states_)
            for (auto& state : band) state.reset();
    }

    void process(AudioBufferView& buffer) noexcept override {
        if (consumeDirty()) updateCoefficients();

        const int frames   = buffer.frames();
        const int channels = std::min(buffer.channels(), channels_);
        if (frames <= 0 || channels <= 0 || activeCount_ == 0) return;

        for (int c = 0; c < channels; ++c) {
            Sample* data = buffer.channel(c);
            for (int a = 0; a < activeCount_; ++a) {
                const int band = activeBands_[static_cast<std::size_t>(a)];
                const BiquadCoefficients& coeff = coefficients_[static_cast<std::size_t>(band)];
                BiquadState& state = states_[static_cast<std::size_t>(band)][static_cast<std::size_t>(c)];
                for (int i = 0; i < frames; ++i)
                    data[i] = state.process(data[i], coeff);
            }
        }
    }

    /// Charakterystyka amplitudowa dla wykresu w GUI (spec §9).
    /// Wołane z wątku sterującego — nie dotyka stanu filtrów.
    [[nodiscard]] std::vector<double> magnitudeResponseDb(const std::vector<double>& frequencies) const {
        std::vector<double> out(frequencies.size(), 0.0);
        for (int b = 0; b < kEqBands; ++b) {
            if (raw(paramIndex(b, 0)) < 0.5f) continue;
            const auto type = static_cast<FilterType>(
                std::clamp(static_cast<int>(raw(paramIndex(b, 1)) + 0.5f), 0, 7));
            const auto coeff = BiquadCoefficients::design(type, raw(paramIndex(b, 2)),
                                                          raw(paramIndex(b, 4)), raw(paramIndex(b, 3)),
                                                          sampleRate_);
            for (std::size_t i = 0; i < frequencies.size(); ++i)
                out[i] += coeff.magnitudeDb(frequencies[i], sampleRate_);
        }
        return out;
    }

    [[nodiscard]] static constexpr int bandCount() noexcept { return kEqBands; }

private:
    static constexpr std::size_t paramIndex(int band, int slot) noexcept {
        return static_cast<std::size_t>(band) * static_cast<std::size_t>(kEqParametersPerBand) +
               static_cast<std::size_t>(slot);
    }

    void updateCoefficients() noexcept {
        activeCount_ = 0;
        for (int b = 0; b < kEqBands; ++b) {
            const bool enabled = raw(paramIndex(b, 0)) >= 0.5f;
            if (!enabled) continue;

            const auto type = static_cast<FilterType>(
                std::clamp(static_cast<int>(raw(paramIndex(b, 1)) + 0.5f), 0, 7));
            coefficients_[static_cast<std::size_t>(b)] = BiquadCoefficients::design(
                type, raw(paramIndex(b, 2)), raw(paramIndex(b, 4)), raw(paramIndex(b, 3)), sampleRate_);
            activeBands_[static_cast<std::size_t>(activeCount_++)] = b;
        }
    }

    double sampleRate_ = kDefaultSampleRate;
    int    channels_   = 2;

    std::array<BiquadCoefficients, kEqBands> coefficients_{};
    std::array<std::array<BiquadState, kMaxStreamChannels>, kEqBands> states_{};
    std::array<int, kEqBands> activeBands_{};
    int activeCount_ = 0;
};

} // namespace helix::dsp
