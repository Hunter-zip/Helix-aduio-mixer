// Parametry sterowane z GUI, odczytywane w wątku audio — bez blokad,
// z wygładzaniem, żeby zmiana głośności czy routingu nie trzaskała (spec §22).
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

#include "helix/Types.h"

namespace helix {

/// Konwersje poziomów.
inline float dbToGain(float db) noexcept { return std::pow(10.0f, db * 0.05f); }

inline float gainToDb(float gain) noexcept {
    return gain > 1.0e-9f ? 20.0f * std::log10(gain) : -180.0f;
}

/// Wartość z liniową rampą. `setTarget` wołane z dowolnego wątku,
/// `advance`/`nextValue` wyłącznie z wątku audio.
class SmoothedValue {
public:
    void prepare(double sampleRate, double rampMilliseconds, float initialValue) noexcept {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : kDefaultSampleRate;
        rampMs_     = std::max(0.0, rampMilliseconds);
        rampFrames_ = std::max(1, static_cast<int>(sampleRate_ * rampMs_ * 0.001));
        target_.store(initialValue, std::memory_order_relaxed);
        current_   = initialValue;
        // `goal_` musi ruszyć z tej samej wartości co `current_`. Bez tego
        // updateTarget() uznaje pierwszą zmianę na wartość równą domyślnemu
        // `goal_` za brak zmiany i rampa nigdy nie startuje.
        goal_      = initialValue;
        remaining_ = 0;
        step_      = 0.0f;
    }

    void setTarget(float value) noexcept { target_.store(value, std::memory_order_relaxed); }

    [[nodiscard]] float target() const noexcept { return target_.load(std::memory_order_relaxed); }

    [[nodiscard]] float current() const noexcept { return current_; }

    /// Natychmiastowy skok (np. przy starcie strumienia) — bez rampy.
    void snapToTarget() noexcept {
        current_   = target_.load(std::memory_order_relaxed);
        goal_      = current_;
        remaining_ = 0;
        step_      = 0.0f;
    }

    /// Wykrywa nowy cel i planuje rampę. Wołane raz na blok w wątku audio.
    void updateTarget() noexcept {
        const float t = target_.load(std::memory_order_relaxed);
        if (t == goal_) return;
        goal_      = t;
        remaining_ = rampFrames_;
        step_      = (goal_ - current_) / static_cast<float>(rampFrames_);
    }

    /// Kolejna próbka rampy.
    [[nodiscard]] float nextValue() noexcept {
        if (remaining_ <= 0) return current_;
        current_ += step_;
        if (--remaining_ == 0) current_ = goal_;
        return current_;
    }

    /// Czy w tym bloku trzeba interpolować per-próbkę.
    [[nodiscard]] bool isRamping() const noexcept { return remaining_ > 0; }

    /// Przeskakuje `frames` próbek rampy (gdy blok i tak jest mnożony liniowo).
    void skip(int frames) noexcept {
        if (remaining_ <= 0) return;
        const int n = std::min(frames, remaining_);
        current_ += step_ * static_cast<float>(n);
        remaining_ -= n;
        if (remaining_ == 0) current_ = goal_;
    }

    /// Wartość, jaką parametr osiągnie po `frames` próbkach — do interpolacji blokowej.
    [[nodiscard]] float valueAfter(int frames) const noexcept {
        if (remaining_ <= 0) return current_;
        const int n = std::min(frames, remaining_);
        return (n == remaining_) ? goal_ : current_ + step_ * static_cast<float>(n);
    }

private:
    std::atomic<float> target_{0.0f};
    float  current_   = 0.0f;
    float  goal_      = 0.0f;
    float  step_      = 0.0f;
    int    remaining_ = 0;
    int    rampFrames_ = 1;
    double sampleRate_ = kDefaultSampleRate;
    double rampMs_     = 0.0;
};

/// Atomowa flaga bool o jasnej semantyce dla pary GUI/audio.
class AtomicFlagValue {
public:
    explicit AtomicFlagValue(bool initial = false) : value_(initial) {}
    void set(bool v) noexcept { value_.store(v, std::memory_order_relaxed); }
    [[nodiscard]] bool get() const noexcept { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> value_;
};

} // namespace helix
