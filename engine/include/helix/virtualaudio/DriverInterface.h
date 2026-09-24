// Moduł sterownika wirtualnego audio (spec §7).
//
// Sterownik jest celowo oddzielony od GUI i od silnika: aplikacja tylko pyta
// o stan i zleca instalację/aktualizację/usunięcie pakietu. Dzięki temu
// wymiana sterownika nie wymaga przebudowy miksera.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "helix/Types.h"

namespace helix::virtualaudio {

enum class DriverState : std::uint8_t {
    NotInstalled,   ///< pakiet sterownika nieobecny w systemie
    Installed,      ///< zainstalowany i zgodny z wersją aplikacji
    NeedsUpdate,    ///< zainstalowany, ale w starszej wersji
    Error           ///< sterownik obecny, lecz w stanie błędu
};

[[nodiscard]] const char* driverStateName(DriverState state) noexcept;

struct DriverStatus {
    DriverState state = DriverState::NotInstalled;
    std::string installedVersion;
    std::string requiredVersion;
    std::string message;
    std::vector<std::string> endpoints;   ///< nazwy punktów końcowych widocznych w systemie
    bool        requiresElevation = true;
};

/// Kontrakt modułu sterownika.
class DriverInterface {
public:
    virtual ~DriverInterface() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// Odpytuje system o stan pakietu sterownika.
    [[nodiscard]] virtual DriverStatus query() = 0;

    /// Instaluje pakiet (ścieżka do pliku .inf lub katalogu pakietu).
    virtual Status install(const std::string& packagePath) = 0;

    /// Aktualizuje istniejącą instalację.
    virtual Status update(const std::string& packagePath) = 0;

    /// Usuwa pakiet z systemu.
    virtual Status uninstall() = 0;

    /// Ustawia liczbę punktów końcowych udostępnianych przez sterownik.
    virtual Status configureEndpoints(int inputs, int outputs) = 0;

    /// Czy operacje wymagają podniesionych uprawnień.
    [[nodiscard]] virtual bool requiresElevation() const noexcept = 0;
};

/// Implementacja właściwa dla platformy. Poza Windows zwraca wariant programowy,
/// który raportuje brak sterownika i odmawia instalacji, ale nie blokuje
/// działania transportu przez pamięć współdzieloną.
[[nodiscard]] std::unique_ptr<DriverInterface> createPlatformDriverInterface();

/// Wariant programowy — używany na platformach bez sterownika i w testach.
[[nodiscard]] std::unique_ptr<DriverInterface> createSoftwareDriverInterface();

} // namespace helix::virtualaudio
