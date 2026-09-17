// Programowy backend audio: symuluje urządzenia bez dostępu do sprzętu.
//
// Zastosowania:
//  * platformy inne niż Windows (rozwój, CI),
//  * testy jednostkowe i integracyjne (tryb sterowany ręcznie),
//  * tryb awaryjny, gdy backend systemowy nie wystartuje (spec §23).
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "helix/core/AudioBackend.h"

namespace helix::core {

class NullBackend final : public AudioBackend {
public:
    /// `freeRunning = true` uruchamia wątek zegara symulujący pracę karty.
    /// `false` oznacza tryb testowy — bloki wymusza się metodą `pump()`.
    explicit NullBackend(bool freeRunning = true);
    ~NullBackend() override;

    [[nodiscard]] const char* name() const noexcept override { return "null"; }

    Status initialize() override;
    void   shutdown() override;

    [[nodiscard]] std::vector<DeviceInfo> enumerateDevices(DeviceDirection direction) override;
    [[nodiscard]] std::string defaultDeviceId(DeviceDirection direction) override;
    [[nodiscard]] bool deviceExists(const std::string& deviceId) override;

    [[nodiscard]] std::unique_ptr<AudioStream> openRenderStream(
        const StreamConfig& config, RenderCallback* callback, Status& status) override;

    [[nodiscard]] std::unique_ptr<AudioStream> openCaptureStream(
        const StreamConfig& config, CaptureCallback* callback, Status& status) override;

    [[nodiscard]] bool supportsProcessLoopback() const noexcept override { return true; }

    void setDeviceChangeHandler(DeviceChangeHandler handler) override;

    // ── Sterowanie na potrzeby testów i symulacji hot-plug ───────────────────

    /// Wymusza przetworzenie jednego bloku na wszystkich działających strumieniach.
    void pump(int frames);

    void addDevice(DeviceInfo info);
    bool removeDevice(const std::string& deviceId);
    void setDefaultDevice(DeviceDirection direction, const std::string& deviceId);

    /// Generator sygnału dla strumieni przechwytujących (domyślnie cisza).
    using CaptureGenerator = std::function<void(const std::string& deviceId, Sample* interleaved,
                                                int frames, int channels)>;
    void setCaptureGenerator(CaptureGenerator generator);

    /// Ostatni blok zapisany przez silnik do urządzenia renderującego — do asercji w testach.
    [[nodiscard]] std::vector<Sample> lastRenderedBlock(const std::string& deviceId) const;

private:
    class Stream;
    friend class Stream;

    void registerStream(Stream* stream);
    void storeRendered(const std::string& deviceId, const Sample* data, std::size_t count);
    void unregisterStream(Stream* stream);
    void notify(const DeviceChangeEvent& event);
    void clockLoop();

    mutable std::mutex      mutex_;
    std::vector<DeviceInfo> devices_;
    std::vector<Stream*>    streams_;
    DeviceChangeHandler     handler_;
    CaptureGenerator        generator_;
    std::atomic<bool>       running_{false};
    bool                    freeRunning_;
    std::unique_ptr<std::thread> clock_;

    mutable std::mutex renderedMutex_;
    std::vector<std::pair<std::string, std::vector<Sample>>> rendered_;
};

} // namespace helix::core
