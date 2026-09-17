// Fasada sterująca całym mikserem (spec §26, §28).
//
// GUI nie dotyka silnika bezpośrednio — wysyła komendy JSON i odbiera migawki
// stanu. Dzięki temu interfejs może się zawiesić, zrestartować albo w ogóle
// nie istnieć, a przetwarzanie audio leci dalej.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "helix/Types.h"
#include "helix/app/AppDetection.h"
#include "helix/app/AutomationEngine.h"
#include "helix/app/HotkeyManager.h"
#include "helix/app/Json.h"
#include "helix/app/ProfileManager.h"
#include "helix/core/AudioBackend.h"
#include "helix/core/AudioEngine.h"
#include "helix/core/ChannelManager.h"
#include "helix/core/DeviceManager.h"
#include "helix/virtualaudio/VirtualDeviceManager.h"

namespace helix::app {

class EngineController {
public:
    struct Options {
        std::string configDirectory;     ///< pusty = katalog domyślny systemu
        std::string startupProfile;      ///< pusty = brak automatycznego wczytania
        bool   createDefaultProfiles = true;
        bool   autoStart = true;
        bool   forceNullBackend = false; ///< wymuś backend programowy (testy, tryb awaryjny)
        /// Wymuś detektor aplikacji sterowany programowo zamiast systemowego.
        /// Pozwala testować reguły automatyzacji identycznie na każdej platformie,
        /// bez zależności od sesji audio żywych procesów.
        bool   forceManualAppDetector = false;
        double sampleRate  = kDefaultSampleRate;
        int    blockFrames = kDefaultBlockFrames;
        int    channels    = 2;
        bool   exclusive   = false;
        bool   enableHotkeys = true;
        bool   enableVirtualDevices = true;
    };

    EngineController();
    ~EngineController();

    EngineController(const EngineController&) = delete;
    EngineController& operator=(const EngineController&) = delete;

    Status initialize(Options options);
    void   shutdown();

    /// Prace okresowe wątku sterującego: zwalnianie pamięci, wykrywanie aplikacji,
    /// odświeżanie powiązań urządzeń. Nigdy nie jest wołane z wątku audio.
    void tick();

    /// Wykonuje komendę. `status` opisuje powodzenie, wynik trafia do zwracanej wartości.
    JsonValue dispatch(const std::string& command, const JsonValue& args, Status& status);

    /// Pełna migawka stanu dla GUI.
    [[nodiscard]] JsonValue snapshot() const;

    /// Same pomiary — lekka odpowiedź odpytywana z częstotliwością odświeżania GUI.
    [[nodiscard]] JsonValue meters() const;

    /// Dokument profilu zbudowany z bieżącego stanu.
    [[nodiscard]] JsonValue captureProfile() const;
    Status applyProfile(const JsonValue& document);

    [[nodiscard]] core::AudioEngine&   engine() noexcept { return *engine_; }
    [[nodiscard]] core::DeviceManager& devices() noexcept { return *devices_; }
    [[nodiscard]] core::ChannelManager& channels() noexcept { return *channels_; }
    [[nodiscard]] ProfileManager&      profiles() noexcept { return *profiles_; }
    [[nodiscard]] HotkeyManager&       hotkeys() noexcept { return *hotkeys_; }
    [[nodiscard]] AutomationEngine&    automation() noexcept { return automation_; }
    [[nodiscard]] AppDetector&         appDetector() noexcept { return *appDetector_; }
    [[nodiscard]] AppAssignments&      assignments() noexcept { return assignments_; }
    [[nodiscard]] virtualaudio::VirtualDeviceManager& virtualDevices() noexcept { return *virtualDevices_; }

    [[nodiscard]] const std::string& configDirectory() const noexcept { return configDirectory_; }

    /// Katalog konfiguracji zależny od systemu.
    [[nodiscard]] static std::string defaultConfigDirectory();

    /// Lista nazw obsługiwanych komend — używana przez GUI i CLI.
    [[nodiscard]] static std::vector<std::string> commandNames();

private:
    void buildDefaultLayout();
    void wireAutomation();
    void applyDefaultRouting();

    [[nodiscard]] core::Channel* resolveChannel(const JsonValue& args, const char* key = "channel") const;
    [[nodiscard]] core::Bus*     resolveBus(const JsonValue& args, const char* key = "bus") const;
    [[nodiscard]] core::InputSource* resolveSource(const JsonValue& args, const char* key = "source") const;

    [[nodiscard]] JsonValue channelJson(const core::Channel& channel) const;
    [[nodiscard]] JsonValue busJson(const core::Bus& bus) const;
    [[nodiscard]] JsonValue sourceJson(const core::InputSource& source) const;
    [[nodiscard]] JsonValue effectsJson(ChannelId channel) const;
    [[nodiscard]] JsonValue routingJson() const;
    [[nodiscard]] JsonValue devicesJson() const;
    [[nodiscard]] JsonValue masterJson() const;
    [[nodiscard]] JsonValue statsJson() const;

    void onAppEvent(const AudioApplication& application, bool appeared);
    Status assignApplication(const std::string& executable, const std::string& channelName,
                             std::uint32_t processId);

    Options options_;
    std::string configDirectory_;

    std::unique_ptr<core::AudioBackend>  backend_;
    std::unique_ptr<core::AudioEngine>   engine_;
    std::unique_ptr<core::DeviceManager> devices_;
    std::unique_ptr<core::ChannelManager> channels_;
    std::unique_ptr<ProfileManager>      profiles_;
    std::unique_ptr<HotkeyManager>       hotkeys_;
    std::unique_ptr<AppDetector>         appDetector_;
    std::unique_ptr<virtualaudio::VirtualDeviceManager> virtualDevices_;

    AutomationEngine automation_;
    AppAssignments   assignments_;

    /// Serializuje całe API sterujące. Rekurencyjny, bo reguła automatyzacji
    /// wywołana w trakcie komendy wraca tą samą drogą przez dispatch().
    mutable std::recursive_mutex mutex_;
    std::atomic<bool>  initialized_{false};
    std::atomic<int>   applyDepth_{0};   ///< blokada rekurencji przy wczytywaniu profilu
    std::uint64_t      tickCounter_ = 0;
};

} // namespace helix::app
