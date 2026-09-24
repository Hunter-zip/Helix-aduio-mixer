// Łańcuch efektów kanału (spec §8) — kolejność zmienialna w czasie rzeczywistym.
//
// Model bezpieczeństwa: sloty trzymają właścicielskie wskaźniki zarządzane przez
// wątek sterujący. Wątek audio czyta wyłącznie opublikowaną tablicę kolejności
// (podwójne buforowanie + atomowy indeks), więc nigdy nie widzi stanu pośredniego.
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <string_view>
#include <vector>

#include "helix/dsp/AudioPlugin.h"

namespace helix::dsp {

inline constexpr int kMaxEffectSlots = 16;

class EffectChain {
public:
    EffectChain() {
        for (auto& slot : slots_) slot.store(nullptr, std::memory_order_relaxed);
        orders_[0].count = 0;
        orders_[1].count = 0;
        activeOrder_.store(0, std::memory_order_release);
    }

    ~EffectChain() {
        for (auto& slot : slots_) delete slot.exchange(nullptr, std::memory_order_acq_rel);
    }

    EffectChain(const EffectChain&) = delete;
    EffectChain& operator=(const EffectChain&) = delete;

    void setContext(const ProcessContext& context) noexcept { context_ = context; }
    [[nodiscard]] const ProcessContext& context() const noexcept { return context_; }

    // ── Wątek audio ─────────────────────────────────────────────────────────

    void process(AudioBufferView& buffer) noexcept {
        const Order& order = orders_[static_cast<std::size_t>(activeOrder_.load(std::memory_order_acquire))];
        for (int i = 0; i < order.count; ++i) {
            AudioPlugin* plugin = slots_[static_cast<std::size_t>(order.slot[i])]
                                      .load(std::memory_order_acquire);
            if (plugin != nullptr && plugin->isEnabled())
                plugin->process(buffer);
        }
    }

    void resetState() noexcept {
        for (auto& slot : slots_)
            if (auto* p = slot.load(std::memory_order_acquire)) p->reset();
    }

    // ── Wątek sterujący ─────────────────────────────────────────────────────

    /// Wstawia plugin na pozycję `position` (-1 = na koniec). Przejmuje własność.
    /// Zwraca false, gdy brak wolnych slotów.
    bool insert(PluginPtr plugin, int position = -1) {
        if (!plugin) return false;

        int freeSlot = -1;
        for (int i = 0; i < kMaxEffectSlots; ++i) {
            if (slots_[static_cast<std::size_t>(i)].load(std::memory_order_acquire) == nullptr) {
                freeSlot = i;
                break;
            }
        }
        if (freeSlot < 0) return false;

        plugin->initialize(context_);

        const Order& current = activeOrderRef();
        if (current.count >= kMaxEffectSlots) return false;

        // Najpierw slot (wątek audio go jeszcze nie widzi — nie ma go w kolejności).
        slots_[static_cast<std::size_t>(freeSlot)].store(plugin.release(), std::memory_order_release);

        Order next = current;
        const int index = (position < 0 || position > next.count) ? next.count : position;
        for (int i = next.count; i > index; --i) next.slot[i] = next.slot[i - 1];
        next.slot[index] = freeSlot;
        ++next.count;
        publish(next);
        return true;
    }

    /// Usuwa plugin z łańcucha i zwraca go wywołującemu. Wątek audio przestaje
    /// go widzieć natychmiast po publikacji kolejności, ale zwolnienie pamięci
    /// musi poczekać na barierę bloku (patrz AudioEngine::retire).
    [[nodiscard]] PluginPtr detach(PluginId id) {
        const int slot = slotOf(id);
        if (slot < 0) return nullptr;

        const Order& current = activeOrderRef();
        Order next;
        next.count = 0;
        for (int i = 0; i < current.count; ++i)
            if (current.slot[i] != slot) next.slot[next.count++] = current.slot[i];
        publish(next);

        return PluginPtr(slots_[static_cast<std::size_t>(slot)].exchange(nullptr, std::memory_order_acq_rel));
    }

    /// Ustawia nową kolejność wg identyfikatorów. Pluginy pominięte na liście
    /// zachowują względną kolejność i trafiają na koniec.
    bool reorder(const std::vector<PluginId>& ids) {
        const Order& current = activeOrderRef();
        Order next;
        next.count = 0;
        std::array<bool, kMaxEffectSlots> used{};

        for (PluginId id : ids) {
            const int slot = slotOf(id);
            if (slot < 0) return false;
            if (used[static_cast<std::size_t>(slot)]) continue;
            used[static_cast<std::size_t>(slot)] = true;
            next.slot[next.count++] = slot;
        }
        for (int i = 0; i < current.count; ++i) {
            const int slot = current.slot[i];
            if (!used[static_cast<std::size_t>(slot)]) next.slot[next.count++] = slot;
        }

        publish(next);
        return true;
    }

    /// Pierwszy plugin danego typu (np. "gain") w łańcuchu.
    [[nodiscard]] AudioPlugin* findByType(const char* typeId) const noexcept {
        const Order& order = activeOrderRef();
        for (int i = 0; i < order.count; ++i) {
            auto* p = slots_[static_cast<std::size_t>(order.slot[i])].load(std::memory_order_acquire);
            if (p != nullptr && std::string_view(p->typeId()) == typeId) return p;
        }
        return nullptr;
    }

    [[nodiscard]] AudioPlugin* find(PluginId id) const noexcept {
        const int slot = slotOf(id);
        return slot < 0 ? nullptr : slots_[static_cast<std::size_t>(slot)].load(std::memory_order_acquire);
    }

    /// Pluginy w kolejności przetwarzania (wątek sterujący).
    [[nodiscard]] std::vector<AudioPlugin*> ordered() const {
        const Order& order = activeOrderRef();
        std::vector<AudioPlugin*> result;
        result.reserve(static_cast<std::size_t>(order.count));
        for (int i = 0; i < order.count; ++i)
            if (auto* p = slots_[static_cast<std::size_t>(order.slot[i])].load(std::memory_order_acquire))
                result.push_back(p);
        return result;
    }

    [[nodiscard]] int size() const noexcept { return activeOrderRef().count; }

    /// Sumaryczne opóźnienie wnoszone przez włączone efekty.
    [[nodiscard]] int latencyFrames() const noexcept {
        int total = 0;
        const Order& order = activeOrderRef();
        for (int i = 0; i < order.count; ++i) {
            auto* p = slots_[static_cast<std::size_t>(order.slot[i])].load(std::memory_order_acquire);
            if (p != nullptr && p->isEnabled()) total += p->latencyFrames();
        }
        return total;
    }

    /// Przygotowuje wszystkie pluginy pod nowy format (zmiana sample rate itp.).
    void reinitialize(const ProcessContext& context) {
        context_ = context;
        for (auto& slot : slots_)
            if (auto* p = slot.load(std::memory_order_acquire)) p->initialize(context_);
    }

private:
    struct Order {
        int count = 0;
        int slot[kMaxEffectSlots]{};   // tablica C: indeksowanie int-em bez rzutowań
    };

    [[nodiscard]] const Order& activeOrderRef() const noexcept {
        return orders_[static_cast<std::size_t>(activeOrder_.load(std::memory_order_acquire))];
    }

    void publish(const Order& next) noexcept {
        const int inactive = 1 - activeOrder_.load(std::memory_order_acquire);
        orders_[static_cast<std::size_t>(inactive)] = next;
        activeOrder_.store(inactive, std::memory_order_release);
    }

    [[nodiscard]] int slotOf(PluginId id) const noexcept {
        for (int i = 0; i < kMaxEffectSlots; ++i) {
            auto* p = slots_[static_cast<std::size_t>(i)].load(std::memory_order_acquire);
            if (p != nullptr && p->instanceId() == id) return i;
        }
        return -1;
    }

    std::array<std::atomic<AudioPlugin*>, kMaxEffectSlots> slots_{};
    std::array<Order, 2> orders_{};
    std::atomic<int> activeOrder_{0};
    ProcessContext   context_{};
};

} // namespace helix::dsp
