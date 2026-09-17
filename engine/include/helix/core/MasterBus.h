// Sekcja Master (spec §14) — globalna głośność, mute, mierniki i limiter.
#pragma once

#include <atomic>
#include <utility>

#include "helix/Meter.h"
#include "helix/Parameter.h"
#include "helix/Types.h"
#include "helix/dsp/AudioPlugin.h"

namespace helix::core {

/// Migawka Mastera dla GUI.
struct MasterState {
    float  volume = 0.85f;
    bool   muted = false;
    bool   limiterEnabled = true;
    float  limiterCeilingDb = -1.0f;
    BusId  primaryBus = kInvalidBus;
    MeterSnapshot meter;
};

/// Master nie wprowadza dodatkowego buforowania — jest wyłącznie etapem
/// wzmocnienia doklejanym do magistral oznaczonych jako „podążające za Masterem”.
class MasterBus {
public:
    void prepare(const dsp::ProcessContext& context);

    void setVolume(float linear) noexcept;
    [[nodiscard]] float volume() const noexcept { return volumeTarget_.load(std::memory_order_relaxed); }

    void setMuted(bool muted) noexcept { muted_.store(muted, std::memory_order_relaxed); }
    [[nodiscard]] bool muted() const noexcept { return muted_.load(std::memory_order_relaxed); }

    void setLimiterEnabled(bool enabled) noexcept { limiterEnabled_.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool limiterEnabled() const noexcept { return limiterEnabled_.load(std::memory_order_relaxed); }

    void setLimiterCeilingDb(float db) noexcept { ceilingDb_.store(db, std::memory_order_relaxed); }
    [[nodiscard]] float limiterCeilingDb() const noexcept { return ceilingDb_.load(std::memory_order_relaxed); }

    void setPrimaryBus(BusId bus) noexcept { primaryBus_.store(bus, std::memory_order_relaxed); }
    [[nodiscard]] BusId primaryBus() const noexcept { return primaryBus_.load(std::memory_order_relaxed); }

    // ── Wątek audio ─────────────────────────────────────────────────────────

    /// Zwraca (wzmocnienie na początku bloku, wzmocnienie na końcu bloku).
    /// Mute i volume mają wspólną rampę, więc przełączanie jest bezklikowe.
    [[nodiscard]] std::pair<float, float> gainRamp(int frames) noexcept;

    [[nodiscard]] LevelMeter& meter() noexcept { return meter_; }
    [[nodiscard]] MeterSnapshot meterSnapshot() const noexcept { return meter_.snapshot(); }

private:
    std::atomic<float> volumeTarget_{0.85f};
    std::atomic<bool>  muted_{false};
    std::atomic<bool>  limiterEnabled_{true};
    std::atomic<float> ceilingDb_{-1.0f};
    std::atomic<BusId> primaryBus_{kInvalidBus};

    SmoothedValue volumeSmoothed_;
    SmoothedValue muteSmoothed_;
    LevelMeter    meter_;
};

} // namespace helix::core
