// Helix Audio Mixer — podstawowe typy i stałe silnika.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace helix {

/// Próbka wewnętrzna. Cały tor sygnałowy pracuje na 32-bit float (spec §21);
/// sumowanie magistrali odbywa się w double (patrz MasterBus) tam, gdzie ma to znaczenie.
using Sample = float;

/// Identyfikatory są monotoniczne i nigdy nie są reużywane w obrębie sesji,
/// dzięki czemu GUI nie może trafić komendą w nieistniejący obiekt.
using ChannelId = std::uint32_t;
using BusId     = std::uint32_t;
using SourceId  = std::uint32_t;
using PluginId  = std::uint32_t;

inline constexpr ChannelId kInvalidChannel = 0;
inline constexpr BusId     kInvalidBus     = 0;
inline constexpr SourceId  kInvalidSource  = 0;

/// Maksymalny rozmiar bloku, jaki silnik obsłuży w jednym wywołaniu process().
/// Bufory robocze są prealokowane pod tę wartość — w callbacku nie ma alokacji (spec §22).
inline constexpr int kMaxBlockFrames = 4096;

/// Maksymalna liczba kanałów strumienia (stereo w praktyce, zapas na przyszłość).
inline constexpr int kMaxStreamChannels = 8;

/// Limit liczby kanałów miksera i magistral — pozwala trzymać macierz routingu
/// jako bitset o stałym rozmiarze i przełączać ją atomowo.
inline constexpr int kMaxChannels = 64;
inline constexpr int kMaxBuses    = 32;

/// Wersja formatu pliku konfiguracji (spec §24 — format wersjonowany).
inline constexpr int kConfigSchemaVersion = 2;

/// Domyślne parametry pracy silnika.
inline constexpr double kDefaultSampleRate = 48000.0;
inline constexpr int    kDefaultBlockFrames = 192;   // 4 ms @ 48 kHz

/// Kierunek urządzenia audio.
enum class DeviceDirection : std::uint8_t {
    Render,   ///< wyjście (głośniki, słuchawki, wirtualne wyjście)
    Capture   ///< wejście (mikrofon, loopback aplikacji)
};

/// Rodzaj źródła zasilającego kanał miksera (spec §5).
enum class SourceKind : std::uint8_t {
    None,           ///< kanał bez wejścia (np. tylko suma z innych)
    PhysicalInput,  ///< fizyczne urządzenie wejściowe (mikrofon, line-in)
    Loopback,       ///< loopback urządzenia wyjściowego (dźwięk systemu)
    Application,    ///< process loopback — konkretna aplikacja (spec §5)
    VirtualDevice   ///< własne wirtualne wejście miksera (spec §7)
};

/// Rodzaj magistrali wyjściowej (spec §6/§7).
enum class BusKind : std::uint8_t {
    Physical,   ///< realne urządzenie (A1, A2, ...)
    Virtual     ///< wirtualne urządzenie miksera (B1, B2, ...)
};

/// Wynik operacji sterującej. Silnik nie rzuca wyjątkami przez granicę API.
struct Status {
    bool        ok = true;
    std::string message;

    static Status success() { return Status{true, {}}; }
    static Status error(std::string msg) { return Status{false, std::move(msg)}; }

    explicit operator bool() const noexcept { return ok; }
};

} // namespace helix
