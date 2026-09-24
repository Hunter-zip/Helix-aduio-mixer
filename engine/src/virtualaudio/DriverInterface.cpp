#include "helix/virtualaudio/DriverInterface.h"

#include "helix/Log.h"

#if defined(_WIN32)
#include "helix/platform/WindowsDriverInterface.h"
#endif

namespace helix::virtualaudio {

const char* driverStateName(DriverState state) noexcept {
    switch (state) {
        case DriverState::NotInstalled: return "not-installed";
        case DriverState::Installed:    return "installed";
        case DriverState::NeedsUpdate:  return "needs-update";
        case DriverState::Error:        return "error";
    }
    return "unknown";
}

namespace {

/// Wariant bez sterownika jądra: punkty końcowe istnieją wyłącznie jako
/// kanały pamięci współdzielonej dla klientów Helixa.
class SoftwareDriverInterface final : public DriverInterface {
public:
    [[nodiscard]] const char* name() const noexcept override { return "software"; }

    [[nodiscard]] DriverStatus query() override {
        DriverStatus status;
        status.state = DriverState::NotInstalled;
        status.requiredVersion = "1.0";
        status.requiresElevation = false;
        status.message = "Sterownik jądra niedostępny na tej platformie. "
                         "Wirtualne punkty końcowe działają przez pamięć współdzieloną.";
        return status;
    }

    Status install(const std::string&) override {
        return Status::error("Instalacja sterownika jest dostępna tylko w systemie Windows");
    }

    Status update(const std::string&) override {
        return Status::error("Aktualizacja sterownika jest dostępna tylko w systemie Windows");
    }

    Status uninstall() override {
        return Status::error("Usuwanie sterownika jest dostępne tylko w systemie Windows");
    }

    Status configureEndpoints(int, int) override {
        // Punkty końcowe w trybie programowym konfiguruje VirtualDeviceManager.
        return Status::success();
    }

    [[nodiscard]] bool requiresElevation() const noexcept override { return false; }
};

} // namespace

std::unique_ptr<DriverInterface> createSoftwareDriverInterface() {
    return std::make_unique<SoftwareDriverInterface>();
}

std::unique_ptr<DriverInterface> createPlatformDriverInterface() {
#if defined(_WIN32)
    if (auto driver = createWindowsDriverInterface()) return driver;
    Log::warn("VirtualAudio", "Moduł sterownika Windows niedostępny — tryb programowy");
#endif
    return createSoftwareDriverInterface();
}

} // namespace helix::virtualaudio
