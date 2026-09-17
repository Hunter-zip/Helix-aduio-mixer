// Bezblokadowa kolejka SPSC (single producer / single consumer) dla komend
// i obiektów do zwolnienia. Wątek audio nigdy nie czeka na mutex (spec §22).
#pragma once

#include <atomic>
#include <cstddef>
#include <type_traits>
#include <vector>

namespace helix {

/// Kolejka o stałej pojemności, bez alokacji w push/pop.
/// Wymaga typów trywialnie kopiowalnych — komendy to POD-y (ew. surowe wskaźniki).
template <typename T>
class SpscQueue {
    static_assert(std::is_trivially_copyable_v<T>,
                  "SpscQueue przenosi dane bajtowo — typ musi być trivially copyable");

public:
    explicit SpscQueue(std::size_t capacity)
        : slots_(nextPowerOfTwo(capacity + 1)), mask_(slots_.size() - 1) {}

    /// Producent. Zwraca false gdy kolejka pełna (wywołujący decyduje, co dalej).
    bool push(const T& item) noexcept {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire))
            return false; // pełna
        slots_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    /// Konsument. Zwraca false gdy pusto.
    bool pop(T& out) noexcept {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire))
            return false; // pusta
        out = slots_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size() - 1; }

    [[nodiscard]] std::size_t size() const noexcept {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & mask_;
    }

private:
    static std::size_t nextPowerOfTwo(std::size_t v) noexcept {
        std::size_t p = 2;
        while (p < v) p <<= 1;
        return p;
    }

    std::vector<T> slots_;
    std::size_t    mask_;
    alignas(64) std::atomic<std::size_t> head_{0};
    alignas(64) std::atomic<std::size_t> tail_{0};
};

} // namespace helix
