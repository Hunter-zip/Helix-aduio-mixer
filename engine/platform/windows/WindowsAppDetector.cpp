// Enumeracja sesji audio poszczególnych aplikacji (spec §5).
//
// Windows udostępnia sesje przez IAudioSessionManager2. Dla każdej sesji
// pobieramy PID, a z niego nazwę pliku wykonywalnego — to ona jest kluczem
// przypisania do kanału, więc restart aplikacji nie gubi konfiguracji.
#include "helix/platform/WindowsAppDetector.h"

#include "WasapiCommon.h"

#include <psapi.h>

#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "helix/Log.h"

namespace helix::app {

using namespace helix::platform;

namespace {

constexpr const char* kLog = "AppDetector";

/// Nazwa pliku wykonywalnego procesu o danym PID.
std::string executableForProcess(DWORD processId) {
    if (processId == 0) return {};

    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process == nullptr) return {};

    WCHAR path[MAX_PATH]{};
    DWORD size = MAX_PATH;
    std::string result;

    if (QueryFullProcessImageNameW(process, 0, path, &size) != 0) {
        const std::string full = toUtf8(path);
        const std::size_t separator = full.find_last_of("\\/");
        result = (separator == std::string::npos) ? full : full.substr(separator + 1);
    }

    CloseHandle(process);
    return result;
}

class WindowsAppDetector final : public AppDetector {
public:
    [[nodiscard]] const char* name() const noexcept override { return "wasapi-sessions"; }

    Status start() override {
        std::lock_guard<std::mutex> lock(comMutex_);
        apartment_ = std::make_unique<ComApartment>();
        if (!apartment_->ok()) return Status::error("Nie można zainicjalizować COM");

        const HRESULT result = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                                __uuidof(IMMDeviceEnumerator), enumerator_.putVoid());
        if (FAILED(result))
            return Status::error("MMDeviceEnumerator: " + describeHresult(result));
        return Status::success();
    }

    void stop() override {
        std::lock_guard<std::mutex> lock(comMutex_);
        enumerator_.reset();
        apartment_.reset();
    }

    [[nodiscard]] std::vector<AudioApplication> enumerate() override {
        std::lock_guard<std::mutex> lock(comMutex_);
        std::vector<AudioApplication> applications;
        if (!enumerator_) return applications;

        // Sesje zbieramy ze wszystkich aktywnych urządzeń wyjściowych — aplikacja
        // może grać na dowolnym z nich (spec §5: zmiana urządzenia przez aplikację).
        ComPtr<IMMDeviceCollection> collection;
        if (FAILED(enumerator_->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, collection.put())))
            return applications;

        UINT deviceCount = 0;
        collection->GetCount(&deviceCount);

        std::map<DWORD, AudioApplication> unique;

        for (UINT d = 0; d < deviceCount; ++d) {
            ComPtr<IMMDevice> device;
            if (FAILED(collection->Item(d, device.put()))) continue;

            ComPtr<IAudioSessionManager2> manager;
            if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                        manager.putVoid())))
                continue;

            ComPtr<IAudioSessionEnumerator> sessions;
            if (FAILED(manager->GetSessionEnumerator(sessions.put()))) continue;

            int sessionCount = 0;
            sessions->GetCount(&sessionCount);

            for (int s = 0; s < sessionCount; ++s) {
                ComPtr<IAudioSessionControl> control;
                if (FAILED(sessions->GetSession(s, control.put()))) continue;

                ComPtr<IAudioSessionControl2> control2;
                if (FAILED(control->QueryInterface(__uuidof(IAudioSessionControl2),
                                                   control2.putVoid())))
                    continue;

                // Sesje systemowe pomijamy — użytkownika interesują aplikacje.
                if (control2->IsSystemSoundsSession() == S_OK) continue;

                DWORD processId = 0;
                if (FAILED(control2->GetProcessId(&processId)) || processId == 0) continue;

                AudioSessionState state = AudioSessionStateInactive;
                control->GetState(&state);
                if (state == AudioSessionStateExpired) continue;

                AudioApplication application;
                application.processId  = static_cast<std::uint32_t>(processId);
                application.executable = executableForProcess(processId);
                if (application.executable.empty()) continue;

                CoMem<WCHAR> displayName;
                if (SUCCEEDED(control->GetDisplayName(displayName.put())) &&
                    displayName.get() != nullptr)
                    application.displayName = toUtf8(displayName.get());
                if (application.displayName.empty()) application.displayName = application.executable;

                CoMem<WCHAR> sessionId;
                if (SUCCEEDED(control2->GetSessionInstanceIdentifier(sessionId.put())) &&
                    sessionId.get() != nullptr)
                    application.sessionId = toUtf8(sessionId.get());

                application.active = (state == AudioSessionStateActive);

                ComPtr<ISimpleAudioVolume> volume;
                if (SUCCEEDED(control2->QueryInterface(__uuidof(ISimpleAudioVolume),
                                                       volume.putVoid()))) {
                    float level = 1.0f;
                    BOOL muted = FALSE;
                    volume->GetMasterVolume(&level);
                    volume->GetMute(&muted);
                    application.volume = level;
                    application.muted  = (muted != FALSE);
                }

                // Ta sama aplikacja może mieć sesje na kilku urządzeniach —
                // scalamy po PID, preferując sesję aktywną.
                const auto existing = unique.find(processId);
                if (existing == unique.end() || (application.active && !existing->second.active))
                    unique[processId] = std::move(application);
            }
        }

        applications.reserve(unique.size());
        for (auto& [processId, application] : unique) applications.push_back(std::move(application));
        return applications;
    }

private:
    std::mutex                    comMutex_;
    std::unique_ptr<ComApartment> apartment_;
    ComPtr<IMMDeviceEnumerator>   enumerator_;
};

} // namespace

std::unique_ptr<AppDetector> createWindowsAppDetector() {
    auto detector = std::make_unique<WindowsAppDetector>();
    Log::debug(kLog, "Detektor sesji WASAPI utworzony");
    return detector;
}

} // namespace helix::app
