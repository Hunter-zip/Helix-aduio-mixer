// Źródło sygnału zasilające kanał (spec §5).
#pragma once

#include <atomic>
#include <memory>
#include <string>

#include "helix/Meter.h"
#include "helix/RingBuffer.h"
#include "helix/Types.h"

namespace helix::core {

/// Migawka stanu źródła dla GUI.
struct SourceState {
    SourceId    id = kInvalidSource;
    SourceKind  kind = SourceKind::None;
    std::string name;
    std::string deviceId;
    std::string processName;
    std::uint32_t processId = 0;
    ChannelId   channel = kInvalidChannel;
    int         channels = 2;
    double      sampleRate = kDefaultSampleRate;
    bool        active = false;
    bool        streaming = false;
    std::uint64_t underruns = 0;
    std::uint64_t overruns = 0;
};

/// Bufor wejściowy: wątek przechwytujący zapisuje, wątek audio czyta.
/// Utrata źródła (zamknięta aplikacja, odpięte urządzenie) nie zatrzymuje silnika —
/// kanał po prostu dostaje ciszę (spec §23).
class InputSource {
public:
    InputSource(SourceId id, SourceKind kind, std::string name);

    [[nodiscard]] SourceId id() const noexcept { return id_; }
    [[nodiscard]] SourceKind kind() const noexcept { return kind_; }
    [[nodiscard]] const std::string& name() const { return name_; }
    void setName(std::string name) { name_ = std::move(name); }

    [[nodiscard]] const std::string& deviceId() const { return deviceId_; }
    void setDeviceId(std::string id) { deviceId_ = std::move(id); }

    [[nodiscard]] const std::string& processName() const { return processName_; }
    void setProcessName(std::string name) { processName_ = std::move(name); }

    [[nodiscard]] std::uint32_t processId() const noexcept { return processId_.load(std::memory_order_relaxed); }
    void setProcessId(std::uint32_t pid) noexcept { processId_.store(pid, std::memory_order_relaxed); }

    void setChannel(ChannelId channel) noexcept { channel_.store(channel, std::memory_order_relaxed); }
    [[nodiscard]] ChannelId channel() const noexcept { return channel_.load(std::memory_order_relaxed); }

    void setActive(bool active) noexcept { active_.store(active, std::memory_order_relaxed); }
    [[nodiscard]] bool active() const noexcept { return active_.load(std::memory_order_relaxed); }

    void setStreaming(bool streaming) noexcept { streaming_.store(streaming, std::memory_order_relaxed); }
    [[nodiscard]] bool streaming() const noexcept { return streaming_.load(std::memory_order_relaxed); }

    void setSourceGain(float gain) noexcept { gain_.store(gain, std::memory_order_relaxed); }
    [[nodiscard]] float sourceGain() const noexcept { return gain_.load(std::memory_order_relaxed); }

    void prepare(int channels, double sampleRate, int ringFrames);

    [[nodiscard]] AudioRingBuffer& ring() noexcept { return ring_; }
    [[nodiscard]] const AudioRingBuffer& ring() const noexcept { return ring_; }
    [[nodiscard]] int channels() const noexcept { return channels_; }
    [[nodiscard]] double sampleRate() const noexcept { return sampleRate_; }

    [[nodiscard]] LevelMeter& meter() noexcept { return meter_; }
    [[nodiscard]] MeterSnapshot meterSnapshot() const noexcept { return meter_.snapshot(); }

private:
    SourceId    id_;
    SourceKind  kind_;
    std::string name_;
    std::string deviceId_;
    std::string processName_;

    std::atomic<std::uint32_t> processId_{0};
    std::atomic<ChannelId>     channel_{kInvalidChannel};
    std::atomic<bool>          active_{false};
    std::atomic<bool>          streaming_{false};
    std::atomic<float>         gain_{1.0f};

    AudioRingBuffer ring_;
    LevelMeter      meter_;
    int             channels_ = 2;
    double          sampleRate_ = kDefaultSampleRate;
};

} // namespace helix::core
