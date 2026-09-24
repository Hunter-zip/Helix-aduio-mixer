// Konwersja częstotliwości próbkowania (spec §20/§21) z kompensacją dryftu zegara.
// Interpolacja Catmull-Rom — dobry kompromis jakość/koszt dla toru miksera.
//
// Model użycia: push/pull. Wątek przechwytujący wrzuca ramki urządzenia,
// a wyciąga ramki w częstotliwości silnika. Bufor wewnętrzny jest prealokowany.
#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "helix/Types.h"

namespace helix::dsp {

class Resampler {
public:
    void prepare(int channels, double inputRate, double outputRate, int maxOutputFrames) {
        channels_   = std::max(1, std::min(channels, kMaxStreamChannels));
        inputRate_  = inputRate  > 0.0 ? inputRate  : kDefaultSampleRate;
        outputRate_ = outputRate > 0.0 ? outputRate : kDefaultSampleRate;
        baseRatio_  = inputRate_ / outputRate_;
        driftRatio_ = 1.0;

        // Pojemność: najgorszy przypadek zapotrzebowania na jeden blok + margines.
        capacity_ = static_cast<int>(std::ceil(std::max(1, maxOutputFrames) * baseRatio_ * 2.0)) + 64;
        fifo_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(capacity_), 0.0f);
        reset();
    }

    void reset() noexcept {
        std::fill(fifo_.begin(), fifo_.end(), 0.0f);
        count_ = 1;    // jedna ramka historii dla punktu x[-1]
        pos_   = 1.0;
        driftRatio_ = 1.0;
    }

    [[nodiscard]] bool isPassthrough() const noexcept {
        return std::fabs(baseRatio_ * driftRatio_ - 1.0) < 1.0e-12;
    }

    [[nodiscard]] int channels() const noexcept { return channels_; }
    [[nodiscard]] double inputRate() const noexcept { return inputRate_; }
    [[nodiscard]] double outputRate() const noexcept { return outputRate_; }
    [[nodiscard]] double ratio() const noexcept { return baseRatio_ * driftRatio_; }

    /// Delikatna korekta tempa (±0,5%) kompensująca dryft zegarów urządzeń.
    void setDriftCompensation(double ratio) noexcept {
        driftRatio_ = std::clamp(ratio, 0.995, 1.005);
    }

    /// Ile ramek wejściowych zmieści się jeszcze w buforze.
    [[nodiscard]] int freeInputFrames() const noexcept { return capacity_ - count_; }

    /// Ile ramek wyjściowych da się wyprodukować z tego, co już jest w buforze.
    [[nodiscard]] int availableOutputFrames() const noexcept {
        const double r = ratio();
        const double limit = static_cast<double>(count_) - 3.0; // potrzebny x[idx+2]
        if (pos_ > limit) return 0;
        return static_cast<int>(std::floor((limit - pos_) / r)) + 1;
    }

    /// Dokłada ramki wejściowe (planarne). Zwraca liczbę przyjętych ramek.
    int pushInput(const Sample* const* input, int frames) noexcept {
        compact();
        const int accepted = std::min(frames, freeInputFrames());
        for (int c = 0; c < channels_; ++c) {
            Sample* dst = channelData(c) + count_;
            const Sample* src = input[c];
            std::copy(src, src + accepted, dst);
        }
        count_ += accepted;
        return accepted;
    }

    /// Dokłada ramki z bufora z przeplotem.
    int pushInterleaved(const Sample* interleaved, int frames, int sourceChannels) noexcept {
        compact();
        const int accepted = std::min(frames, freeInputFrames());
        for (int c = 0; c < channels_; ++c) {
            Sample* dst = channelData(c) + count_;
            const int srcChannel = std::min(c, sourceChannels - 1);
            for (int i = 0; i < accepted; ++i)
                dst[i] = interleaved[static_cast<std::size_t>(i) * static_cast<std::size_t>(sourceChannels)
                                     + static_cast<std::size_t>(srcChannel)];
        }
        count_ += accepted;
        return accepted;
    }

    /// Produkuje do `frames` ramek wyjściowych. Zwraca faktyczną liczbę
    /// (mniejszą, gdy zabrakło materiału wejściowego).
    int pullOutput(Sample* const* output, int frames) noexcept {
        const double r = ratio();
        int produced = 0;

        while (produced < frames) {
            const auto idx = static_cast<int>(std::floor(pos_));
            if (idx + 2 > count_ - 1) break;   // brak materiału na interpolację
            const auto frac = static_cast<float>(pos_ - static_cast<double>(idx));

            for (int c = 0; c < channels_; ++c) {
                const Sample* in = channelData(c);
                output[c][produced] = catmullRom(in[idx - 1], in[idx], in[idx + 1], in[idx + 2], frac);
            }
            pos_ += r;
            ++produced;
        }

        return produced;
    }

private:
    [[nodiscard]] Sample* channelData(int channel) noexcept {
        return fifo_.data() + static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_);
    }

    [[nodiscard]] const Sample* channelData(int channel) const noexcept {
        return fifo_.data() + static_cast<std::size_t>(channel) * static_cast<std::size_t>(capacity_);
    }

    /// Przesuwa zawartość na początek bufora, zostawiając jedną ramkę historii.
    void compact() noexcept {
        const int keepFrom = static_cast<int>(std::floor(pos_)) - 1;
        if (keepFrom <= 0) return;
        const int remaining = count_ - keepFrom;
        for (int c = 0; c < channels_; ++c) {
            Sample* data = channelData(c);
            std::copy(data + keepFrom, data + count_, data);
        }
        count_ = remaining;
        pos_  -= keepFrom;
    }

    static float catmullRom(float xm1, float x0, float x1, float x2, float t) noexcept {
        const float c0 = x0;
        const float c1 = 0.5f * (x1 - xm1);
        const float c2 = xm1 - 2.5f * x0 + 2.0f * x1 - 0.5f * x2;
        const float c3 = 0.5f * (x2 - xm1) + 1.5f * (x0 - x1);
        return ((c3 * t + c2) * t + c1) * t + c0;
    }

    int    channels_   = 1;
    int    capacity_   = 64;
    int    count_      = 1;
    double pos_        = 1.0;
    double inputRate_  = kDefaultSampleRate;
    double outputRate_ = kDefaultSampleRate;
    double baseRatio_  = 1.0;
    double driftRatio_ = 1.0;

    std::vector<Sample> fifo_;
};

} // namespace helix::dsp
