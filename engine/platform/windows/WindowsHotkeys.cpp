// Globalne skróty klawiszowe (spec §17) — działają, gdy aplikacja jest w tle.
//
// RegisterHotKey wiąże skrót z wątkiem, który go rejestruje, i dostarcza
// WM_HOTKEY do jego kolejki komunikatów. Dlatego cała obsługa żyje w jednym
// dedykowanym wątku z własną pętlą komunikatów.
#include "helix/platform/WindowsHotkeys.h"

#include "WasapiCommon.h"

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "helix/Log.h"

namespace helix::app {

namespace {

constexpr const char* kLog = "Hotkeys";
constexpr UINT kMessageRegister   = WM_APP + 1;
constexpr UINT kMessageUnregister = WM_APP + 2;

UINT toWindowsModifiers(std::uint32_t modifiers) {
    UINT result = MOD_NOREPEAT;
    if (modifiers & kModAlt)   result |= MOD_ALT;
    if (modifiers & kModCtrl)  result |= MOD_CONTROL;
    if (modifiers & kModShift) result |= MOD_SHIFT;
    if (modifiers & kModWin)   result |= MOD_WIN;
    return result;
}

class WindowsHotkeyBackend final : public HotkeyBackend {
public:
    ~WindowsHotkeyBackend() override { stop(); }

    [[nodiscard]] const char* name() const noexcept override { return "windows"; }

    Status start(Callback callback) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (running_) return Status::success();

        callback_ = std::move(callback);
        running_ = true;
        ready_ = false;
        startupError_.clear();

        thread_ = std::make_unique<std::thread>([this] { run(); });
        readyCondition_.wait(lock, [this] { return ready_; });

        if (!startupError_.empty()) {
            running_ = false;
            lock.unlock();
            if (thread_ && thread_->joinable()) thread_->join();
            thread_.reset();
            return Status::error(startupError_);
        }
        return Status::success();
    }

    void stop() override {
        DWORD threadId = 0;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!running_) return;
            running_ = false;
            threadId = threadId_;
        }

        if (threadId != 0) PostThreadMessageW(threadId, WM_QUIT, 0, 0);
        if (thread_ && thread_->joinable()) thread_->join();
        thread_.reset();

        std::lock_guard<std::mutex> lock(mutex_);
        registered_.clear();
        callback_ = nullptr;
    }

    Status registerHotkey(const std::string& id, std::uint32_t modifiers,
                          std::uint32_t keyCode) override {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!running_) return Status::error("Obsługa skrótów nie została uruchomiona");

        const int hotkeyId = nextHotkeyId_++;
        pending_[hotkeyId] = PendingRegistration{toWindowsModifiers(modifiers),
                                                 static_cast<UINT>(keyCode), id};
        const DWORD threadId = threadId_;
        lock.unlock();

        if (threadId == 0) return Status::error("Wątek skrótów nie działa");
        PostThreadMessageW(threadId, kMessageRegister, static_cast<WPARAM>(hotkeyId), 0);

        // Czekamy na wynik rejestracji — użytkownik musi wiedzieć od razu,
        // że skrót jest zajęty przez inną aplikację.
        lock.lock();
        resultCondition_.wait_for(lock, std::chrono::milliseconds(500),
                                  [&] { return results_.count(hotkeyId) > 0; });

        const auto result = results_.find(hotkeyId);
        if (result == results_.end()) return Status::error("Przekroczono czas rejestracji skrótu");

        const bool ok = result->second;
        results_.erase(result);
        if (!ok) return Status::error("Skrót jest już zajęty przez inną aplikację");
        return Status::success();
    }

    void unregisterHotkey(const std::string& id) override {
        std::unique_lock<std::mutex> lock(mutex_);
        int hotkeyId = -1;
        for (const auto& [key, value] : registered_) {
            if (value == id) { hotkeyId = key; break; }
        }
        if (hotkeyId < 0) return;

        const DWORD threadId = threadId_;
        lock.unlock();
        if (threadId != 0)
            PostThreadMessageW(threadId, kMessageUnregister, static_cast<WPARAM>(hotkeyId), 0);
    }

    [[nodiscard]] bool supportsGlobalCapture() const noexcept override { return true; }

private:
    struct PendingRegistration {
        UINT        modifiers;
        UINT        keyCode;
        std::string id;
    };

    void run() {
        // Wymuszenie utworzenia kolejki komunikatów dla tego wątku.
        MSG message;
        PeekMessageW(&message, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            threadId_ = GetCurrentThreadId();
            ready_ = true;
        }
        readyCondition_.notify_all();

        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
            if (message.message == WM_HOTKEY) {
                std::string id;
                Callback callback;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto it = registered_.find(static_cast<int>(message.wParam));
                    if (it != registered_.end()) id = it->second;
                    callback = callback_;
                }
                if (!id.empty() && callback) callback(id);

            } else if (message.message == kMessageRegister) {
                const int hotkeyId = static_cast<int>(message.wParam);
                PendingRegistration registration;
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    const auto it = pending_.find(hotkeyId);
                    if (it == pending_.end()) continue;
                    registration = it->second;
                    pending_.erase(it);
                }

                const BOOL ok = RegisterHotKey(nullptr, hotkeyId, registration.modifiers,
                                               registration.keyCode);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    results_[hotkeyId] = (ok != FALSE);
                    if (ok != FALSE) registered_[hotkeyId] = registration.id;
                }
                resultCondition_.notify_all();

            } else if (message.message == kMessageUnregister) {
                const int hotkeyId = static_cast<int>(message.wParam);
                UnregisterHotKey(nullptr, hotkeyId);
                std::lock_guard<std::mutex> lock(mutex_);
                registered_.erase(hotkeyId);
            }
        }

        // Sprzątanie: każdy zarejestrowany skrót musi zostać zwolniony w tym wątku.
        std::map<int, std::string> toRelease;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            toRelease = registered_;
            registered_.clear();
            threadId_ = 0;
        }
        for (const auto& [hotkeyId, id] : toRelease) UnregisterHotKey(nullptr, hotkeyId);
    }

    std::mutex                    mutex_;
    std::condition_variable       readyCondition_;
    std::condition_variable       resultCondition_;
    std::unique_ptr<std::thread>  thread_;
    Callback                      callback_;
    std::map<int, std::string>    registered_;
    std::map<int, PendingRegistration> pending_;
    std::map<int, bool>           results_;
    DWORD                         threadId_ = 0;
    int                           nextHotkeyId_ = 1;
    bool                          running_ = false;
    bool                          ready_ = false;
    std::string                   startupError_;
};

} // namespace

std::unique_ptr<HotkeyBackend> createWindowsHotkeyBackend() {
    Log::debug(kLog, "Backend skrótów Windows utworzony");
    return std::make_unique<WindowsHotkeyBackend>();
}

} // namespace helix::app
