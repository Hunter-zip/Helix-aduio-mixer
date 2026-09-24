// Abstrakcja systemowego API audio (spec §2/§20).
//
// Silnik nie wie nic o WASAPI. Na Windows wpinany jest WasapiBackend,
// na pozostałych platformach (oraz w testach i CI) — NullBackend.
// Dołożenie kolejnej platformy to implementacja tego interfejsu.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "helix/Types.h"

namespace helix::core {

/// Opis urządzenia zwracany przez backend.
struct DeviceInfo {
    std::string id;
    std::string name;
    DeviceDirection direction = DeviceDirection::Render;
    bool   isDefault  = false;
    bool   isVirtual  = false;   ///< urządzenie wirtualne Helixa
    bool   present    = true;    ///< false = urządzenie zniknęło (hot-plug)
    bool   supportsLoopback = false;
    int    maxChannels = 2;
    double defaultSampleRate = kDefaultSampleRate;
    int    defaultBufferFrames = kDefaultBlockFrames;
    int    minBufferFrames = 32;
    std::vector<double> supportedSampleRates;
};

/// Parametry otwarcia strumienia.
struct StreamConfig {
    std::string deviceId;
    double sampleRate   = kDefaultSampleRate;
    int    channels     = 2;
    int    bufferFrames = kDefaultBlockFrames;
    bool   exclusive    = false;  ///< tryb wyłączny (niższa latencja, blokuje urządzenie)
    bool   loopback     = false;  ///< przechwytywanie wyjścia urządzenia (dźwięk systemu)
    std::uint32_t processId = 0;  ///< >0 = process loopback konkretnej aplikacji (spec §5)
    bool   includeProcessTree = true;
};

/// Callback renderowania — wywoływany z wątku urządzenia o wysokim priorytecie.
/// Obowiązuje kontrakt RT: bez alokacji, bez blokad, bez I/O.
class RenderCallback {
public:
    virtual ~RenderCallback() = default;
    virtual void renderBlock(Sample* interleaved, int frames, int channels) noexcept = 0;
};

/// Callback przechwytywania — wywoływany z wątku urządzenia.
class CaptureCallback {
public:
    virtual ~CaptureCallback() = default;
    virtual void captureBlock(const Sample* interleaved, int frames, int channels) noexcept = 0;
};

/// Uchwyt otwartego strumienia.
class AudioStream {
public:
    virtual ~AudioStream() = default;
    virtual Status start() = 0;
    virtual void   stop() = 0;
    [[nodiscard]] virtual bool isRunning() const = 0;
    [[nodiscard]] virtual double sampleRate() const = 0;
    [[nodiscard]] virtual int channels() const = 0;
    [[nodiscard]] virtual int bufferFrames() const = 0;
    /// Opóźnienie wnoszone przez urządzenie (ms) — do budżetu latencji (spec §22).
    [[nodiscard]] virtual double latencyMs() const = 0;
    /// Liczba wykrytych przerwań/glitchy — diagnostyka stabilności (spec §23).
    [[nodiscard]] virtual std::uint64_t glitchCount() const = 0;
};

/// Powiadomienia systemowe o urządzeniach (spec §20/§23).
struct DeviceChangeEvent {
    enum class Type { Added, Removed, DefaultChanged, StateChanged, FormatChanged };
    Type type = Type::StateChanged;
    std::string deviceId;
    DeviceDirection direction = DeviceDirection::Render;
};

class AudioBackend {
public:
    virtual ~AudioBackend() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;

    virtual Status initialize() = 0;
    virtual void   shutdown() = 0;

    [[nodiscard]] virtual std::vector<DeviceInfo> enumerateDevices(DeviceDirection direction) = 0;
    [[nodiscard]] virtual std::string defaultDeviceId(DeviceDirection direction) = 0;
    [[nodiscard]] virtual bool deviceExists(const std::string& deviceId) = 0;

    /// Otwiera strumień. Zwraca nullptr przy błędzie; `status` opisuje powód.
    [[nodiscard]] virtual std::unique_ptr<AudioStream> openRenderStream(
        const StreamConfig& config, RenderCallback* callback, Status& status) = 0;

    [[nodiscard]] virtual std::unique_ptr<AudioStream> openCaptureStream(
        const StreamConfig& config, CaptureCallback* callback, Status& status) = 0;

    /// Czy backend potrafi przechwycić audio konkretnego procesu (spec §5).
    [[nodiscard]] virtual bool supportsProcessLoopback() const noexcept { return false; }

    using DeviceChangeHandler = std::function<void(const DeviceChangeEvent&)>;
    virtual void setDeviceChangeHandler(DeviceChangeHandler handler) = 0;
};

/// Tworzy backend właściwy dla platformy (WASAPI na Windows, Null poza nim).
[[nodiscard]] std::unique_ptr<AudioBackend> createPlatformBackend();

/// Backend programowy: symuluje urządzenia, napędzany własnym zegarem.
/// Używany na platformach bez natywnego wsparcia, w testach i w trybie offline.
[[nodiscard]] std::unique_ptr<AudioBackend> createNullBackend(bool freeRunning = true);

} // namespace helix::core
