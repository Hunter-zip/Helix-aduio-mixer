// Ochrona przed denormalami — kosztują setki cykli na x86 i potrafią
// wywrócić budżet czasowy callbacku audio.
#pragma once

#include <cmath>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
  #define HELIX_HAS_SSE_DENORMAL_CONTROL 1
  #include <xmmintrin.h>
  #include <pmmintrin.h>
#else
  #define HELIX_HAS_SSE_DENORMAL_CONTROL 0
#endif

namespace helix {

/// RAII: ustawia FTZ/DAZ na czas trwania bloku audio i przywraca poprzedni stan.
class ScopedDenormalGuard {
public:
    ScopedDenormalGuard() noexcept {
#if HELIX_HAS_SSE_DENORMAL_CONTROL
        savedCsr_ = _mm_getcsr();
        _mm_setcsr(savedCsr_ | 0x8040u); // FTZ (bit 15) | DAZ (bit 6)
#endif
    }

    ~ScopedDenormalGuard() noexcept {
#if HELIX_HAS_SSE_DENORMAL_CONTROL
        _mm_setcsr(savedCsr_);
#endif
    }

    ScopedDenormalGuard(const ScopedDenormalGuard&) = delete;
    ScopedDenormalGuard& operator=(const ScopedDenormalGuard&) = delete;

private:
#if HELIX_HAS_SSE_DENORMAL_CONTROL
    unsigned int savedCsr_ = 0;
#endif
};

/// Programowe wygaszanie denormali dla stanów filtrów (działa też bez SSE).
inline float flushDenormal(float v) noexcept {
    return (std::fabs(v) < 1.0e-20f) ? 0.0f : v;
}

inline double flushDenormal(double v) noexcept {
    return (std::fabs(v) < 1.0e-100) ? 0.0 : v;
}

} // namespace helix
