// Biquad (RBJ cookbook) w postaci transposed direct form II, stan w double.
#pragma once

#include <algorithm>
#include <cmath>
#include <string_view>

#include "helix/Denormal.h"
#include "helix/Types.h"

namespace helix::dsp {

/// Typy filtrów dostępne w equalizerze (spec §9).
enum class FilterType : std::uint8_t {
    LowCut = 0,    ///< high-pass
    HighCut,       ///< low-pass
    Bell,          ///< peaking
    LowShelf,
    HighShelf,
    Notch,
    BandPass,
    AllPass
};

inline const char* filterTypeName(FilterType type) noexcept {
    switch (type) {
        case FilterType::LowCut:    return "lowcut";
        case FilterType::HighCut:   return "highcut";
        case FilterType::Bell:      return "bell";
        case FilterType::LowShelf:  return "lowshelf";
        case FilterType::HighShelf: return "highshelf";
        case FilterType::Notch:     return "notch";
        case FilterType::BandPass:  return "bandpass";
        case FilterType::AllPass:   return "allpass";
    }
    return "bell";
}

inline bool parseFilterType(std::string_view name, FilterType& out) noexcept {
    if (name == "lowcut"    || name == "highpass") { out = FilterType::LowCut;    return true; }
    if (name == "highcut"   || name == "lowpass")  { out = FilterType::HighCut;   return true; }
    if (name == "bell"      || name == "peaking")  { out = FilterType::Bell;      return true; }
    if (name == "lowshelf")                        { out = FilterType::LowShelf;  return true; }
    if (name == "highshelf")                       { out = FilterType::HighShelf; return true; }
    if (name == "notch")                           { out = FilterType::Notch;     return true; }
    if (name == "bandpass")                        { out = FilterType::BandPass;  return true; }
    if (name == "allpass")                         { out = FilterType::AllPass;   return true; }
    return false;
}

/// Współczynniki znormalizowane przez a0.
struct BiquadCoefficients {
    double b0 = 1.0, b1 = 0.0, b2 = 0.0, a1 = 0.0, a2 = 0.0;

    /// Projektuje filtr. `gainDb` używane tylko przez Bell/Shelf.
    static BiquadCoefficients design(FilterType type, double frequency, double q,
                                     double gainDb, double sampleRate) noexcept {
        BiquadCoefficients c;
        if (sampleRate <= 0.0) return c;

        // Częstotliwość ograniczona poniżej Nyquista, Q w rozsądnym zakresie.
        const double nyquist = sampleRate * 0.5;
        const double f  = std::min(std::max(frequency, 10.0), nyquist * 0.995);
        const double qq = std::min(std::max(q, 0.05), 40.0);

        const double w0    = 2.0 * 3.14159265358979323846 * f / sampleRate;
        const double cosw0 = std::cos(w0);
        const double sinw0 = std::sin(w0);
        const double alpha = sinw0 / (2.0 * qq);
        const double A     = std::pow(10.0, gainDb / 40.0);

        double b0 = 1.0, b1 = 0.0, b2 = 0.0, a0 = 1.0, a1 = 0.0, a2 = 0.0;

        switch (type) {
            case FilterType::LowCut:
                b0 =  (1.0 + cosw0) * 0.5;
                b1 = -(1.0 + cosw0);
                b2 =  (1.0 + cosw0) * 0.5;
                a0 =   1.0 + alpha;
                a1 =  -2.0 * cosw0;
                a2 =   1.0 - alpha;
                break;

            case FilterType::HighCut:
                b0 =  (1.0 - cosw0) * 0.5;
                b1 =   1.0 - cosw0;
                b2 =  (1.0 - cosw0) * 0.5;
                a0 =   1.0 + alpha;
                a1 =  -2.0 * cosw0;
                a2 =   1.0 - alpha;
                break;

            case FilterType::Bell:
                b0 =  1.0 + alpha * A;
                b1 = -2.0 * cosw0;
                b2 =  1.0 - alpha * A;
                a0 =  1.0 + alpha / A;
                a1 = -2.0 * cosw0;
                a2 =  1.0 - alpha / A;
                break;

            case FilterType::LowShelf: {
                const double twoSqrtAalpha = 2.0 * std::sqrt(A) * alpha;
                b0 =      A * ((A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAalpha);
                b1 =  2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
                b2 =      A * ((A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAalpha);
                a0 =           (A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAalpha;
                a1 =    -2.0 * ((A - 1.0) + (A + 1.0) * cosw0);
                a2 =           (A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAalpha;
                break;
            }

            case FilterType::HighShelf: {
                const double twoSqrtAalpha = 2.0 * std::sqrt(A) * alpha;
                b0 =      A * ((A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAalpha);
                b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
                b2 =      A * ((A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAalpha);
                a0 =           (A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAalpha;
                a1 =     2.0 * ((A - 1.0) - (A + 1.0) * cosw0);
                a2 =           (A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAalpha;
                break;
            }

            case FilterType::Notch:
                b0 =  1.0;
                b1 = -2.0 * cosw0;
                b2 =  1.0;
                a0 =  1.0 + alpha;
                a1 = -2.0 * cosw0;
                a2 =  1.0 - alpha;
                break;

            case FilterType::BandPass: // constant peak gain
                b0 =  alpha;
                b1 =  0.0;
                b2 = -alpha;
                a0 =  1.0 + alpha;
                a1 = -2.0 * cosw0;
                a2 =  1.0 - alpha;
                break;

            case FilterType::AllPass:
                b0 =  1.0 - alpha;
                b1 = -2.0 * cosw0;
                b2 =  1.0 + alpha;
                a0 =  1.0 + alpha;
                a1 = -2.0 * cosw0;
                a2 =  1.0 - alpha;
                break;
        }

        const double inv = (a0 != 0.0) ? 1.0 / a0 : 1.0;
        c.b0 = b0 * inv;
        c.b1 = b1 * inv;
        c.b2 = b2 * inv;
        c.a1 = a1 * inv;
        c.a2 = a2 * inv;
        return c;
    }

    /// Moduł odpowiedzi częstotliwościowej w dB — dla wykresu EQ (spec §9).
    /// Liczone w wątku GUI/sterującym, nie w audio.
    [[nodiscard]] double magnitudeDb(double frequency, double sampleRate) const noexcept {
        if (sampleRate <= 0.0) return 0.0;
        const double w = 2.0 * 3.14159265358979323846 * frequency / sampleRate;
        const double cw = std::cos(w), sw = std::sin(w);
        const double c2w = std::cos(2.0 * w), s2w = std::sin(2.0 * w);

        const double numRe = b0 + b1 * cw + b2 * c2w;
        const double numIm = -(b1 * sw + b2 * s2w);
        const double denRe = 1.0 + a1 * cw + a2 * c2w;
        const double denIm = -(a1 * sw + a2 * s2w);

        const double num = std::sqrt(numRe * numRe + numIm * numIm);
        const double den = std::sqrt(denRe * denRe + denIm * denIm);
        if (den < 1.0e-18) return 0.0;
        const double mag = num / den;
        return 20.0 * std::log10(std::max(mag, 1.0e-9));
    }
};

/// Pojedynczy stopień biquad dla jednego kanału.
class BiquadState {
public:
    void reset() noexcept { z1_ = 0.0; z2_ = 0.0; }

    [[nodiscard]] float process(float input, const BiquadCoefficients& c) noexcept {
        const double x = static_cast<double>(input);
        const double y = c.b0 * x + z1_;
        z1_ = flushDenormal(c.b1 * x - c.a1 * y + z2_);
        z2_ = flushDenormal(c.b2 * x - c.a2 * y);
        return static_cast<float>(y);
    }

private:
    double z1_ = 0.0;
    double z2_ = 0.0;
};

} // namespace helix::dsp
