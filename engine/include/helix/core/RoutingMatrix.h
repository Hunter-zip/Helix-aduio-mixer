// Macierz routingu kanał × magistrala (spec §6).
//
// Zapis z wątku sterującego jest atomowy, odczyt w wątku audio bezblokadowy.
// Zmiana routingu przechodzi przez krótką rampę wewnątrz bloku, więc nie słychać
// trzasków ani przerw (spec §22).
#pragma once

#include <array>
#include <atomic>
#include <vector>

#include "helix/Types.h"

namespace helix::core {

class RoutingMatrix {
public:
    RoutingMatrix() { clear(); }

    void clear() noexcept {
        for (auto& row : target_)
            for (auto& cell : row) cell.store(0.0f, std::memory_order_relaxed);
        for (auto& row : gain_)
            for (auto& cell : row) cell.store(1.0f, std::memory_order_relaxed);
        for (auto& row : applied_)
            for (auto& cell : row) cell = 0.0f;
    }

    /// Włącza/wyłącza połączenie (wątek sterujący).
    void setEnabled(int channelIndex, int busIndex, bool enabled) noexcept {
        if (!valid(channelIndex, busIndex)) return;
        const float g = enabled ? gain_[idx(channelIndex)][idx(busIndex)].load(std::memory_order_relaxed)
                                : 0.0f;
        target_[idx(channelIndex)][idx(busIndex)].store(g, std::memory_order_relaxed);
        enabled_[idx(channelIndex)][idx(busIndex)].store(enabled, std::memory_order_relaxed);
    }

    [[nodiscard]] bool isEnabled(int channelIndex, int busIndex) const noexcept {
        if (!valid(channelIndex, busIndex)) return false;
        return enabled_[idx(channelIndex)][idx(busIndex)].load(std::memory_order_relaxed);
    }

    /// Wzmocnienie wysyłki (domyślnie 1.0 = 0 dB).
    void setSendGain(int channelIndex, int busIndex, float gain) noexcept {
        if (!valid(channelIndex, busIndex)) return;
        gain_[idx(channelIndex)][idx(busIndex)].store(gain, std::memory_order_relaxed);
        if (enabled_[idx(channelIndex)][idx(busIndex)].load(std::memory_order_relaxed))
            target_[idx(channelIndex)][idx(busIndex)].store(gain, std::memory_order_relaxed);
    }

    [[nodiscard]] float sendGain(int channelIndex, int busIndex) const noexcept {
        if (!valid(channelIndex, busIndex)) return 0.0f;
        return gain_[idx(channelIndex)][idx(busIndex)].load(std::memory_order_relaxed);
    }

    /// Kasuje wiersz (kanał usunięty lub przypisany na nowo).
    void clearChannel(int channelIndex) noexcept {
        if (channelIndex < 0 || channelIndex >= kMaxChannels) return;
        for (int b = 0; b < kMaxBuses; ++b) {
            target_[idx(channelIndex)][idx(b)].store(0.0f, std::memory_order_relaxed);
            enabled_[idx(channelIndex)][idx(b)].store(false, std::memory_order_relaxed);
        }
    }

    void clearBus(int busIndex) noexcept {
        if (busIndex < 0 || busIndex >= kMaxBuses) return;
        for (int c = 0; c < kMaxChannels; ++c) {
            target_[idx(c)][idx(busIndex)].store(0.0f, std::memory_order_relaxed);
            enabled_[idx(c)][idx(busIndex)].store(false, std::memory_order_relaxed);
        }
    }

    // ── Wątek audio ─────────────────────────────────────────────────────────

    /// Wzmocnienie na początku bloku (stan po poprzednim bloku).
    [[nodiscard]] float currentGain(int channelIndex, int busIndex) const noexcept {
        return applied_[idx(channelIndex)][idx(busIndex)];
    }

    /// Wzmocnienie docelowe na koniec bloku.
    [[nodiscard]] float targetGain(int channelIndex, int busIndex) const noexcept {
        return target_[idx(channelIndex)][idx(busIndex)].load(std::memory_order_relaxed);
    }

    /// Zatwierdza wzmocnienie po przetworzeniu bloku.
    void commit(int channelIndex, int busIndex, float value) noexcept {
        applied_[idx(channelIndex)][idx(busIndex)] = value;
    }

    /// Czy w tym bloku jakikolwiek sygnał płynie z kanału do magistrali.
    [[nodiscard]] bool active(int channelIndex, int busIndex) const noexcept {
        return currentGain(channelIndex, busIndex) != 0.0f || targetGain(channelIndex, busIndex) != 0.0f;
    }

private:
    static constexpr std::size_t idx(int v) noexcept { return static_cast<std::size_t>(v); }

    static bool valid(int channelIndex, int busIndex) noexcept {
        return channelIndex >= 0 && channelIndex < kMaxChannels &&
               busIndex     >= 0 && busIndex     < kMaxBuses;
    }

    std::array<std::array<std::atomic<float>, kMaxBuses>, kMaxChannels> target_{};
    std::array<std::array<std::atomic<float>, kMaxBuses>, kMaxChannels> gain_{};
    std::array<std::array<std::atomic<bool>,  kMaxBuses>, kMaxChannels> enabled_{};
    std::array<std::array<float, kMaxBuses>, kMaxChannels> applied_{};
};

} // namespace helix::core
