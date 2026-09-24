// Iteracyjna FFT radix-2 (in-place) — używana przez spektralną redukcję szumów.
// Tablice obrotów liczone raz w prepare(); process() nie alokuje.
#pragma once

#include <cmath>
#include <cstddef>
#include <vector>

namespace helix::dsp {

class Fft {
public:
    /// `order` to log2 rozmiaru (np. 9 → 512 punktów).
    void prepare(int order) {
        order_ = order;
        size_  = 1 << order;
        cosTable_.resize(static_cast<std::size_t>(size_ / 2));
        sinTable_.resize(static_cast<std::size_t>(size_ / 2));
        reverse_.resize(static_cast<std::size_t>(size_));

        constexpr double kTwoPi = 6.283185307179586476925286766559;
        for (int i = 0; i < size_ / 2; ++i) {
            const double angle = kTwoPi * static_cast<double>(i) / static_cast<double>(size_);
            cosTable_[static_cast<std::size_t>(i)] = std::cos(angle);
            sinTable_[static_cast<std::size_t>(i)] = std::sin(angle);
        }

        for (int i = 0; i < size_; ++i) {
            int rev = 0;
            for (int b = 0; b < order_; ++b)
                if (i & (1 << b)) rev |= 1 << (order_ - 1 - b);
            reverse_[static_cast<std::size_t>(i)] = rev;
        }
    }

    [[nodiscard]] int size() const noexcept { return size_; }

    /// Transformata w przód (bez normalizacji).
    void forward(float* real, float* imag) const noexcept { transform(real, imag, false); }

    /// Transformata odwrotna z normalizacją 1/N.
    void inverse(float* real, float* imag) const noexcept {
        transform(real, imag, true);
        const float scale = 1.0f / static_cast<float>(size_);
        for (int i = 0; i < size_; ++i) { real[i] *= scale; imag[i] *= scale; }
    }

private:
    void transform(float* real, float* imag, bool inverseTransform) const noexcept {
        // Permutacja bit-reverse.
        for (int i = 0; i < size_; ++i) {
            const int j = reverse_[static_cast<std::size_t>(i)];
            if (j > i) {
                std::swap(real[i], real[j]);
                std::swap(imag[i], imag[j]);
            }
        }

        for (int len = 2; len <= size_; len <<= 1) {
            const int half = len >> 1;
            const int step = size_ / len;
            for (int i = 0; i < size_; i += len) {
                for (int j = 0; j < half; ++j) {
                    const auto t = static_cast<std::size_t>(j * step);
                    const float wr = static_cast<float>(cosTable_[t]);
                    const float wi = static_cast<float>(inverseTransform ? sinTable_[t] : -sinTable_[t]);
                    const int a = i + j;
                    const int b = i + j + half;
                    const float xr = real[b] * wr - imag[b] * wi;
                    const float xi = real[b] * wi + imag[b] * wr;
                    real[b] = real[a] - xr;
                    imag[b] = imag[a] - xi;
                    real[a] += xr;
                    imag[a] += xi;
                }
            }
        }
    }

    int order_ = 0;
    int size_  = 1;
    std::vector<double> cosTable_;
    std::vector<double> sinTable_;
    std::vector<int>    reverse_;
};

} // namespace helix::dsp
