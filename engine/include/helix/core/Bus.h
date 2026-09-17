// Magistrala wyjściowa (spec §6/§7/§14): A1..An fizyczne, B1..Bn wirtualne.
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "helix/AudioBuffer.h"
#include "helix/Meter.h"
#include "helix/Parameter.h"
#include "helix/RingBuffer.h"
#include "helix/Types.h"
#include "helix/dsp/Limiter.h"

namespace helix::core {

/// Migawka stanu magistrali dla GUI.
struct BusState {
    BusId       id = kInvalidBus;
    int         index = -1;
    std::string name;
    std::string label;        ///< "A1", "B1", ...
    BusKind     kind = BusKind::Physical;
    std::string deviceId;
    std::string deviceName;
    int         channels = 2;
    float       volume = 1.0f;
    bool        muted = false;
    bool        limiterEnabled = true;
    bool        followsMaster = true;
    bool        deviceReady = false;
};

class Bus {
public:
    Bus(BusId id, int index, std::string name, std::string label, BusKind kind);

    [[nodiscard]] BusId id() const noexcept { return id_; }
    [[nodiscard]] int index() const noexcept { return index_; }
    [[nodiscard]] BusKind kind() const noexcept { return kind_; }

    [[nodiscard]] const std::string& name() const { return name_; }
    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] const std::string& label() const { return label_; }

    [[nodiscard]] const std::string& deviceId() const { return deviceId_; }
    void setDeviceId(std::string id) { deviceId_ = std::move(id); }
    [[nodiscard]] const std::string& deviceName() const { return deviceName_; }
    void setDeviceName(std::string name) { deviceName_ = std::move(name); }

    void setVolume(float linear) noexcept;
    [[nodiscard]] float volume() const noexcept { return volumeTarget_.load(std::memory_order_relaxed); }

    void setMuted(bool muted) noexcept { muted_.store(muted, std::memory_order_relaxed); }
    [[nodiscard]] bool muted() const noexcept { return muted_.load(std::memory_order_relaxed); }

    void setLimiterEnabled(bool enabled) noexcept { limiterEnabled_.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool limiterEnabled() const noexcept { return limiterEnabled_.load(std::memory_order_relaxed); }

    void setFollowsMaster(bool follows) noexcept { followsMaster_.store(follows, std::memory_order_relaxed); }
    [[nodiscard]] bool followsMaster() const noexcept { return followsMaster_.load(std::memory_order_relaxed); }

    void setDeviceReady(bool ready) noexcept { deviceReady_.store(ready, std::memory_order_relaxed); }
    [[nodiscard]] bool deviceReady() const noexcept { return deviceReady_.load(std::memory_order_relaxed); }

    [[nodiscard]] int channels() const noexcept { return channels_; }

    void prepare(const dsp::ProcessContext& context, int ringFrames);

    [[nodiscard]] dsp::LimiterPlugin& limiter() noexcept { return limiter_; }

    // ── Wątek audio ─────────────────────────────────────────────────────────
    void beginBlock(int frames) noexcept;
    [[nodiscard]] AudioBufferView buffer() noexcept { return mix_.view(activeFrames_); }

    /// Głośność magistrali, limiter i pomiar. `masterStart`/`masterEnd` to rampa
    /// Mastera w obrębie bloku (1.0 dla magistral niepodążających za Masterem).
    void finishBlock(float masterStart, float masterEnd) noexcept;

    /// Przepisuje gotowy blok do bufora urządzenia (magistrale nie-podstawowe).
    void pushToDevice() noexcept;

    [[nodiscard]] AudioRingBuffer& output() noexcept { return output_; }

    [[nodiscard]] MeterSnapshot meter() const noexcept { return meter_.snapshot(); }
    void clearClip() noexcept { meter_.clearClip(); }

private:
    BusId       id_;
    int         index_;
    std::string name_;
    std::string label_;
    BusKind     kind_;
    std::string deviceId_;
    std::string deviceName_;
    int         channels_ = 2;

    std::atomic<float> volumeTarget_{1.0f};
    std::atomic<bool>  muted_{false};
    std::atomic<bool>  limiterEnabled_{true};
    std::atomic<bool>  followsMaster_{true};
    std::atomic<bool>  deviceReady_{false};

    SmoothedValue volumeSmoothed_;
    SmoothedValue muteSmoothed_;

    dsp::LimiterPlugin limiter_;
    AudioBuffer        mix_;
    AudioRingBuffer    output_;
    LevelMeter         meter_;
    int                activeFrames_ = 0;
    std::vector<Sample> interleaveScratch_;
};

} // namespace helix::core
