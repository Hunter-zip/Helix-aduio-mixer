// Globalne skróty klawiszowe (spec §17) — działają także gdy okno jest w tle.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "helix/Types.h"
#include "helix/app/Json.h"

namespace helix::app {

/// Modyfikatory niezależne od platformy.
enum HotkeyModifier : std::uint32_t {
    kModNone  = 0,
    kModAlt   = 1u << 0,
    kModCtrl  = 1u << 1,
    kModShift = 1u << 2,
    kModWin   = 1u << 3,
};

/// Pojedyncze powiązanie skrótu z akcją.
struct HotkeyBinding {
    std::string   id;
    std::string   combo;      ///< np. "Ctrl+Alt+M"
    std::string   action;     ///< nazwa komendy sterującej
    JsonValue     args;       ///< argumenty komendy
    bool          enabled = true;
    std::uint32_t modifiers = 0;
    std::uint32_t keyCode = 0;
    bool          registered = false;
    std::string   lastError;
};

/// Warstwa systemowa rejestrująca skróty.
class HotkeyBackend {
public:
    using Callback = std::function<void(const std::string& id)>;

    virtual ~HotkeyBackend() = default;
    [[nodiscard]] virtual const char* name() const noexcept = 0;
    virtual Status start(Callback callback) = 0;
    virtual void   stop() = 0;
    virtual Status registerHotkey(const std::string& id, std::uint32_t modifiers, std::uint32_t keyCode) = 0;
    virtual void   unregisterHotkey(const std::string& id) = 0;
    [[nodiscard]] virtual bool supportsGlobalCapture() const noexcept = 0;
};

[[nodiscard]] std::unique_ptr<HotkeyBackend> createPlatformHotkeyBackend();

class HotkeyManager {
public:
    using Dispatcher = std::function<void(const HotkeyBinding&)>;

    explicit HotkeyManager(std::unique_ptr<HotkeyBackend> backend = nullptr);
    ~HotkeyManager();

    HotkeyManager(const HotkeyManager&) = delete;
    HotkeyManager& operator=(const HotkeyManager&) = delete;

    Status start();
    void   stop();

    void setDispatcher(Dispatcher dispatcher);

    /// Dodaje lub nadpisuje skrót.
    Status bind(HotkeyBinding binding);
    bool   unbind(const std::string& id);
    void   clear();

    [[nodiscard]] std::vector<HotkeyBinding> bindings() const;

    /// Wyzwala akcję ręcznie (GUI, testy, platformy bez globalnego przechwytywania).
    bool trigger(const std::string& id);

    [[nodiscard]] bool supportsGlobalCapture() const noexcept;

    /// Parsuje zapis typu "Ctrl+Alt+M". Zwraca false przy nieznanym klawiszu.
    static bool parseCombo(const std::string& combo, std::uint32_t& modifiers, std::uint32_t& keyCode);

    /// Zamienia kod klawisza i modyfikatory z powrotem na tekst.
    [[nodiscard]] static std::string formatCombo(std::uint32_t modifiers, std::uint32_t keyCode);

    /// Domyślny zestaw skrótów ze specyfikacji §17.
    [[nodiscard]] static std::vector<HotkeyBinding> defaultBindings();

private:
    void onHotkey(const std::string& id);

    mutable std::mutex             mutex_;
    std::unique_ptr<HotkeyBackend> backend_;
    std::vector<HotkeyBinding>     bindings_;
    Dispatcher                     dispatcher_;
    bool                           started_ = false;
};

} // namespace helix::app
