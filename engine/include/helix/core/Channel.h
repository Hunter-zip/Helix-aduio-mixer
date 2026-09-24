// Kanał miksera (spec §4).
#pragma once

#include <atomic>
#include <string>
#include <vector>

#include "helix/AudioBuffer.h"
#include "helix/Meter.h"
#include "helix/Parameter.h"
#include "helix/Types.h"
#include "helix/dsp/EffectChain.h"

namespace helix::core {

/// Stan kanału widoczny dla GUI (migawka, nie referencja do obiektu RT).
struct ChannelState {
    ChannelId   id = kInvalidChannel;
    int         index = -1;
    std::string name;
    std::string icon;
    float       volume = 1.0f;     ///< 0..1 (skala liniowa suwaka)
    float       gainDb = 0.0f;
    float       pan    = 0.0f;     ///< -1 = lewo, +1 = prawo
    bool        muted  = false;
    bool        solo   = false;
    bool        enabled = true;
    std::vector<SourceId> sources;
    std::vector<BusId>    outputs;
};

/// Kanał: bufor roboczy, łańcuch DSP, parametry i mierniki.
/// Obiekt żyje tak długo, jak referencuje go opublikowany graf silnika.
class Channel {
public:
    Channel(ChannelId id, int index, std::string name);

    [[nodiscard]] ChannelId id() const noexcept { return id_; }
    [[nodiscard]] int index() const noexcept { return index_; }

    // ── Metadane (tylko wątek sterujący) ────────────────────────────────────
    [[nodiscard]] const std::string& name() const { return name_; }
    void setName(std::string name) { name_ = std::move(name); }
    [[nodiscard]] const std::string& icon() const { return icon_; }
    void setIcon(std::string icon) { icon_ = std::move(icon); }

    // ── Parametry (dowolny wątek) ───────────────────────────────────────────
    void setVolume(float linear) noexcept;
    [[nodiscard]] float volume() const noexcept { return volumeTarget_.load(std::memory_order_relaxed); }

    void setGainDb(float db) noexcept;
    [[nodiscard]] float gainDb() const noexcept { return gainDb_.load(std::memory_order_relaxed); }

    void setPan(float pan) noexcept;
    [[nodiscard]] float pan() const noexcept { return panTarget_.load(std::memory_order_relaxed); }

    void setMuted(bool muted) noexcept { muted_.store(muted, std::memory_order_relaxed); }
    [[nodiscard]] bool muted() const noexcept { return muted_.load(std::memory_order_relaxed); }

    void setSolo(bool solo) noexcept { solo_.store(solo, std::memory_order_relaxed); }
    [[nodiscard]] bool solo() const noexcept { return solo_.load(std::memory_order_relaxed); }

    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool enabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    // ── Cykl życia (wątek sterujący) ────────────────────────────────────────
    void prepare(const dsp::ProcessContext& context);

    [[nodiscard]] dsp::EffectChain& effects() noexcept { return effects_; }
    [[nodiscard]] const dsp::EffectChain& effects() const noexcept { return effects_; }

    // ── Wątek audio ─────────────────────────────────────────────────────────

    /// Czyści bufor kanału przed sumowaniem źródeł.
    void beginBlock(int frames) noexcept;

    [[nodiscard]] AudioBufferView buffer() noexcept { return buffer_.view(activeFrames_); }

    /// Pomiar przed DSP, łańcuch efektów, pan/volume, pomiar po DSP.
    /// `muteOverride` pochodzi z logiki Solo na poziomie miksera.
    void processBlock(bool muteOverride) noexcept;

    [[nodiscard]] MeterSnapshot inputMeter() const noexcept { return inputMeter_.snapshot(); }
    [[nodiscard]] MeterSnapshot outputMeter() const noexcept { return outputMeter_.snapshot(); }
    void clearClip() noexcept { inputMeter_.clearClip(); outputMeter_.clearClip(); }

    /// Opóźnienie wnoszone przez łańcuch efektów kanału.
    [[nodiscard]] int latencyFrames() const noexcept { return effects_.latencyFrames(); }

private:
    ChannelId   id_;
    int         index_;
    std::string name_;
    std::string icon_ = "channel";

    std::atomic<float> volumeTarget_{1.0f};
    std::atomic<float> gainDb_{0.0f};
    std::atomic<float> panTarget_{0.0f};
    std::atomic<bool>  muted_{false};
    std::atomic<bool>  solo_{false};
    std::atomic<bool>  enabled_{true};

    SmoothedValue volumeSmoothed_;
    SmoothedValue panSmoothed_;
    SmoothedValue muteSmoothed_;

    dsp::EffectChain effects_;
    AudioBuffer      buffer_;
    int              activeFrames_ = 0;

    LevelMeter inputMeter_;
    LevelMeter outputMeter_;
};

} // namespace helix::core
