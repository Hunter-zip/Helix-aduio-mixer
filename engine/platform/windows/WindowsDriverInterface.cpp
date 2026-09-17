// Instalacja i utrzymanie pakietu sterownika wirtualnego audio (spec §7).
//
// Moduł jest świadomie „cienki”: uruchamia systemowy pnputil i odczytuje stan
// z rejestru oraz z listy punktów końcowych. Sam pakiet sterownika (.inf/.sys/
// .cat) jest osobnym artefaktem — dzięki temu można go podpisać i aktualizować
// niezależnie od aplikacji.
#include "helix/platform/WindowsDriverInterface.h"

#include "WasapiCommon.h"

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "helix/Log.h"

namespace helix::virtualaudio {

using namespace helix::platform;

namespace {

constexpr const char* kLog = "Driver";

/// Wersja pakietu wymagana przez tę wersję aplikacji.
constexpr const char* kRequiredVersion = "1.0";

/// Identyfikator sprzętowy wirtualnego urządzenia Helixa.
constexpr const wchar_t* kHardwareId = L"ROOT\\HelixVirtualAudio";

/// Klucz rejestru z informacją o zainstalowanym pakiecie.
constexpr const wchar_t* kRegistryPath = L"SOFTWARE\\Helix\\AudioMixer\\Driver";

bool isElevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return false;

    TOKEN_ELEVATION elevation{};
    DWORD size = sizeof(elevation);
    const BOOL ok = GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size);
    CloseHandle(token);
    return ok != FALSE && elevation.TokenIsElevated != 0;
}

std::string readRegistryString(const wchar_t* valueName) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, KEY_READ | KEY_WOW64_64KEY, &key) !=
        ERROR_SUCCESS)
        return {};

    WCHAR buffer[256]{};
    DWORD size = sizeof(buffer);
    DWORD type = 0;
    const LSTATUS result = RegQueryValueExW(key, valueName, nullptr, &type,
                                            reinterpret_cast<LPBYTE>(buffer), &size);
    RegCloseKey(key);

    if (result != ERROR_SUCCESS || type != REG_SZ) return {};
    return toUtf8(buffer);
}

/// Uruchamia proces i czeka na zakończenie. Zwraca kod wyjścia lub -1.
int runProcess(const std::wstring& commandLine, std::string& error) {
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION process{};
    std::wstring mutableCommand = commandLine;   // CreateProcessW modyfikuje bufor

    const BOOL created = CreateProcessW(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
                                        CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    if (!created) {
        error = "nie można uruchomić procesu (kod " + std::to_string(GetLastError()) + ")";
        return -1;
    }

    WaitForSingleObject(process.hProcess, 120000);

    DWORD exitCode = 0;
    GetExitCodeProcess(process.hProcess, &exitCode);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return static_cast<int>(exitCode);
}

/// Czy w systemie jest aktywne urządzenie o identyfikatorze sprzętowym Helixa.
bool driverDevicePresent(std::vector<std::string>& endpoints) {
    ComApartment apartment;
    if (!apartment.ok()) return false;

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                __uuidof(IMMDeviceEnumerator), enumerator.putVoid())))
        return false;

    bool found = false;
    for (EDataFlow flow : {eRender, eCapture}) {
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.put())))
            continue;

        UINT count = 0;
        collection->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(i, device.put()))) continue;

            ComPtr<IPropertyStore> properties;
            if (FAILED(device->OpenPropertyStore(STGM_READ, properties.put()))) continue;

            PROPVARIANT value;
            PropVariantInit(&value);
            if (SUCCEEDED(properties->GetValue(kPropertyDeviceFriendlyName, &value)) &&
                value.vt == VT_LPWSTR) {
                const std::string name = toUtf8(value.pwszVal);
                if (name.find("Helix") != std::string::npos) {
                    endpoints.push_back(name);
                    found = true;
                }
            }
            PropVariantClear(&value);
        }
    }
    return found;
}

class WindowsDriverInterface final : public DriverInterface {
public:
    [[nodiscard]] const char* name() const noexcept override { return "windows-pnputil"; }

    [[nodiscard]] DriverStatus query() override {
        DriverStatus status;
        status.requiredVersion  = kRequiredVersion;
        status.requiresElevation = !isElevated();

        const std::string installedVersion = readRegistryString(L"Version");
        const bool devicePresent = driverDevicePresent(status.endpoints);

        if (installedVersion.empty() && !devicePresent) {
            status.state = DriverState::NotInstalled;
            status.message = "Sterownik wirtualnego audio nie jest zainstalowany.";
            return status;
        }

        status.installedVersion = installedVersion.empty() ? std::string("nieznana") : installedVersion;

        if (!devicePresent) {
            status.state = DriverState::Error;
            status.message = "Pakiet sterownika jest zainstalowany, ale urządzenia nie są aktywne. "
                             "Wymagany restart systemu lub ponowna instalacja.";
            return status;
        }

        if (!installedVersion.empty() && installedVersion != kRequiredVersion) {
            status.state = DriverState::NeedsUpdate;
            status.message = "Zainstalowana wersja " + installedVersion + " jest starsza niż wymagana " +
                             kRequiredVersion + ".";
            return status;
        }

        status.state = DriverState::Installed;
        status.message = "Sterownik działa poprawnie.";
        return status;
    }

    Status install(const std::string& packagePath) override { return runPnpUtil(packagePath, false); }

    Status update(const std::string& packagePath) override { return runPnpUtil(packagePath, true); }

    Status uninstall() override {
        if (!isElevated())
            return Status::error("Usunięcie sterownika wymaga uprawnień administratora");

        std::string error;
        // Najpierw usuwamy urządzenie, potem pakiet — inaczej pnputil odmówi.
        std::wstring command = L"pnputil.exe /remove-device /deviceid \"";
        command += kHardwareId;
        command += L"\" /subtree";
        const int removeDevice = runProcess(command, error);
        if (removeDevice < 0) return Status::error("pnputil: " + error);

        const std::string publishedName = readRegistryString(L"PublishedName");
        if (publishedName.empty())
            return Status::error("Nie znaleziono zainstalowanego pakietu sterownika");

        command = L"pnputil.exe /delete-driver " + toUtf16(publishedName) + L" /uninstall /force";
        const int removePackage = runProcess(command, error);
        if (removePackage < 0) return Status::error("pnputil: " + error);
        if (removePackage != 0)
            return Status::error("pnputil zakończył się kodem " + std::to_string(removePackage));

        RegDeleteKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, KEY_WOW64_64KEY, 0);
        Log::info(kLog, "Sterownik usunięty");
        return Status::success();
    }

    Status configureEndpoints(int inputs, int outputs) override {
        if (!isElevated())
            return Status::error("Zmiana liczby urządzeń wymaga uprawnień administratora");
        if (inputs < 0 || outputs < 0 || inputs > 16 || outputs > 16)
            return Status::error("Nieprawidłowa liczba punktów końcowych");

        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, nullptr, 0,
                            KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) != ERROR_SUCCESS)
            return Status::error("Nie można zapisać konfiguracji sterownika");

        const DWORD inputCount  = static_cast<DWORD>(inputs);
        const DWORD outputCount = static_cast<DWORD>(outputs);
        RegSetValueExW(key, L"InputCount", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&inputCount), sizeof(inputCount));
        RegSetValueExW(key, L"OutputCount", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&outputCount), sizeof(outputCount));
        RegCloseKey(key);

        Log::info(kLog, "Konfiguracja punktów końcowych zapisana — zmiana wejdzie po restarcie sterownika");
        return Status::success();
    }

    [[nodiscard]] bool requiresElevation() const noexcept override { return !isElevated(); }

private:
    Status runPnpUtil(const std::string& packagePath, bool isUpdate) {
        if (!isElevated())
            return Status::error("Instalacja sterownika wymaga uprawnień administratora");

        namespace fs = std::filesystem;
        std::error_code code;
        if (packagePath.empty() || !fs::exists(packagePath, code))
            return Status::error("Nie znaleziono pakietu sterownika: " + packagePath);

        const std::wstring command =
            L"pnputil.exe /add-driver \"" + toUtf16(packagePath) + L"\" /install";

        std::string error;
        const int exitCode = runProcess(command, error);
        if (exitCode < 0) return Status::error("pnputil: " + error);
        if (exitCode != 0)
            return Status::error("pnputil zakończył się kodem " + std::to_string(exitCode));

        HKEY key = nullptr;
        if (RegCreateKeyExW(HKEY_LOCAL_MACHINE, kRegistryPath, 0, nullptr, 0,
                            KEY_WRITE | KEY_WOW64_64KEY, nullptr, &key, nullptr) == ERROR_SUCCESS) {
            const std::wstring version = toUtf16(kRequiredVersion);
            RegSetValueExW(key, L"Version", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(version.c_str()),
                           static_cast<DWORD>((version.size() + 1) * sizeof(wchar_t)));
            const std::wstring path = toUtf16(packagePath);
            RegSetValueExW(key, L"PackagePath", 0, REG_SZ,
                           reinterpret_cast<const BYTE*>(path.c_str()),
                           static_cast<DWORD>((path.size() + 1) * sizeof(wchar_t)));
            RegCloseKey(key);
        }

        Log::info(kLog, isUpdate ? "Sterownik zaktualizowany" : "Sterownik zainstalowany");
        return Status::success();
    }
};

} // namespace

std::unique_ptr<DriverInterface> createWindowsDriverInterface() {
    return std::make_unique<WindowsDriverInterface>();
}

} // namespace helix::virtualaudio
