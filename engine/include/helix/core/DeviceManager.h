// Zarządzanie urządzeniami i strumieniami (spec §20, §23).
//
// Odpowiada za: enumerację, otwieranie/zamykanie strumieni, reakcję na hot-plug,
// zmianę urządzenia domyślnego Windows oraz kompensację dryftu zegarów.
#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "helix/Types.h"
#include "helix/core/AudioBackend.h"
#include "helix/core/AudioEngine.h"

namespace helix::core {

/// Ustawienia formatu pracy wybierane przez użytkownika (spec §20).
struct DeviceSettings {
    double sampleRate   = kDefaultSampleRate;
    int    blockFrames  = kDefaultBlockFrames;
    int    channels     = 2;
    bool   exclusive    = false;
    bool   followSystemDefault = true;  ///< podążaj za domyślnym urządzeniem Windows
};

/// Informacja o powiązaniu magistrali/źródła z urządzeniem — dla GUI.
struct BindingState {
    std::string deviceId;
    std::string deviceName;
    bool        open = false;
    double      latencyMs = 0.0;
    double      deviceSampleRate = 0.0;
    std::uint64_t glitches = 0;
    std::string lastError;
};

class DeviceManager {
public:
    DeviceManager(AudioEngine& engine, AudioBackend& backend);
    ~DeviceManager();

    DeviceManager(const DeviceManager&) = delete;
    DeviceManager& operator=(const DeviceManager&) = delete;

    Status initialize();
    void   shutdown();

    [[nodiscard]] std::vector<DeviceInfo> devices(DeviceDirection direction) const;
    [[nodiscard]] std::string defaultDevice(DeviceDirection direction) const;

    [[nodiscard]] DeviceSettings settings() const;
    Status applySettings(const DeviceSettings& settings);

    /// Podpina magistralę do urządzenia wyjściowego. Pusty `deviceId` odpina.
    Status bindBus(BusId bus, const std::string& deviceId);
    [[nodiscard]] BindingState busBinding(BusId bus) const;

    /// Podpina źródło do urządzenia wejściowego lub loopbacku urządzenia.
    Status bindSourceToDevice(SourceId source, const std::string& deviceId, bool loopback);

    /// Podpina źródło do strumienia konkretnego procesu (spec §5).
    Status bindSourceToProcess(SourceId source, std::uint32_t processId, const std::string& executable);

    void unbindSource(SourceId source);
    [[nodiscard]] BindingState sourceBinding(SourceId source) const;

    Status start();
    void   stop();
    [[nodiscard]] bool isRunning() const;

    /// Odtwarza krótki sygnał testowy na urządzeniu (spec §20 — „test urządzenia”).
    Status testDevice(const std::string& deviceId, double durationSeconds = 0.6);

    [[nodiscard]] double primaryLatencyMs() const;
    [[nodiscard]] double totalLatencyMs() const;

    using ChangeHandler = std::function<void(const DeviceChangeEvent&)>;
    void setChangeHandler(ChangeHandler handler);

    /// Ponownie otwiera strumienie, których urządzenia zniknęły i wróciły.
    void refreshBindings();

private:
    class BusSink;
    class SourceFeed;

    struct BusBinding {
        BusId busId = kInvalidBus;
        std::string deviceId;
        std::unique_ptr<BusSink> sink;
        std::unique_ptr<AudioStream> stream;
        std::string lastError;
    };

    struct SourceBinding {
        SourceId sourceId = kInvalidSource;
        std::string deviceId;
        std::uint32_t processId = 0;
        bool loopback = false;
        std::unique_ptr<SourceFeed> feed;
        std::unique_ptr<AudioStream> stream;
        std::string lastError;
    };

    void onDeviceChange(const DeviceChangeEvent& event);
    Status openBusStreamLocked(BusBinding& binding);
    Status openSourceStreamLocked(SourceBinding& binding);
    void   closeBusStreamLocked(BusBinding& binding);
    void   closeSourceStreamLocked(SourceBinding& binding);
    [[nodiscard]] BusBinding* findBusLocked(BusId bus);
    [[nodiscard]] SourceBinding* findSourceLocked(SourceId source);
    [[nodiscard]] std::string deviceNameLocked(const std::string& deviceId) const;
    void refreshDeviceCacheLocked() const;

    AudioEngine&  engine_;
    AudioBackend& backend_;

    mutable std::mutex mutex_;
    DeviceSettings     settings_;
    bool               running_ = false;
    bool               initialized_ = false;

    /// Enumeracja urządzeń przez systemowe API jest kosztowna, a GUI odpytuje
    /// o stan regularnie — trzymamy migawkę i odświeżamy ją przy zmianach.
    mutable std::vector<DeviceInfo> deviceCache_;
    mutable bool                    deviceCacheValid_ = false;

    std::vector<BusBinding>    busBindings_;
    std::vector<SourceBinding> sourceBindings_;
    ChangeHandler              changeHandler_;
};

} // namespace helix::core
