// Bufor pierścieniowy w pamięci współdzielonej — kanał transportowy między
// silnikiem Helixa a komponentem wirtualnego urządzenia (spec §7).
//
// Ten sam nagłówek działa po obu stronach: sterownik/klient i silnik mapują
// ten sam obszar i wymieniają się próbkami bez kopii pośrednich i bez blokad.
#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include "helix/Types.h"

namespace helix::virtualaudio {

inline constexpr std::uint32_t kSharedRingMagic   = 0x484C5852u;  // "HLXR"
inline constexpr std::uint32_t kSharedRingVersion = 1u;

/// Nagłówek umieszczony na początku obszaru współdzielonego.
/// Układ pól jest częścią ABI — zmiana wymaga podbicia `kSharedRingVersion`.
struct SharedRingHeader {
    std::uint32_t magic;
    std::uint32_t version;
    std::uint32_t channels;
    std::uint32_t sampleRate;
    std::uint32_t capacityFrames;   ///< potęga dwójki
    std::uint32_t headerBytes;

    std::atomic<std::uint64_t> writeIndex;
    std::atomic<std::uint64_t> readIndex;
    std::atomic<std::uint64_t> overruns;
    std::atomic<std::uint64_t> underruns;
    std::atomic<std::uint32_t> producerAlive;
    std::atomic<std::uint32_t> consumerAlive;
};

static_assert(std::atomic<std::uint64_t>::is_always_lock_free,
              "Transport międzyprocesowy wymaga bezblokadowych atomików 64-bit");

/// Rola procesu w kanale.
enum class SharedRingRole : std::uint8_t {
    Producer,   ///< zapisuje próbki (np. wirtualne wyjście → aplikacja)
    Consumer    ///< odczytuje próbki
};

/// Uchwyt do segmentu pamięci współdzielonej.
class SharedRingBuffer {
public:
    SharedRingBuffer() = default;
    ~SharedRingBuffer();

    SharedRingBuffer(const SharedRingBuffer&) = delete;
    SharedRingBuffer& operator=(const SharedRingBuffer&) = delete;
    SharedRingBuffer(SharedRingBuffer&& other) noexcept;
    SharedRingBuffer& operator=(SharedRingBuffer&& other) noexcept;

    /// Tworzy segment (usuwa poprzedni o tej nazwie, jeśli istniał).
    Status create(const std::string& name, int channels, double sampleRate, int capacityFrames);

    /// Podłącza się do istniejącego segmentu.
    Status open(const std::string& name, SharedRingRole role);

    void close();

    [[nodiscard]] bool isOpen() const noexcept { return header_ != nullptr; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] int channels() const noexcept;
    [[nodiscard]] double sampleRate() const noexcept;
    [[nodiscard]] int capacityFrames() const noexcept;

    [[nodiscard]] int availableToRead() const noexcept;
    [[nodiscard]] int availableToWrite() const noexcept;

    /// Zapis/odczyt ramek w przeplocie. Zwracają liczbę obsłużonych ramek.
    int write(const Sample* interleaved, int frames) noexcept;
    int read(Sample* interleaved, int frames) noexcept;

    /// Znaczniki obecności — pozwalają wykryć, że druga strona zniknęła.
    void markAlive(SharedRingRole role, bool alive) noexcept;
    [[nodiscard]] bool peerAlive(SharedRingRole role) const noexcept;

    [[nodiscard]] std::uint64_t overruns() const noexcept;
    [[nodiscard]] std::uint64_t underruns() const noexcept;

private:
    [[nodiscard]] Sample* data() noexcept;
    [[nodiscard]] const Sample* data() const noexcept;

    std::string       name_;
    SharedRingHeader* header_ = nullptr;
    void*             mapping_ = nullptr;
    std::size_t       mappingBytes_ = 0;
    bool              owner_ = false;
#if defined(_WIN32)
    void* fileMapping_ = nullptr;
#else
    int   fileDescriptor_ = -1;
#endif
};

} // namespace helix::virtualaudio
