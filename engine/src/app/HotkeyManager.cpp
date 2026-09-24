#include "helix/app/HotkeyManager.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <sstream>

#include "helix/Log.h"

#if defined(_WIN32)
#include "helix/platform/WindowsHotkeys.h"
#endif

namespace helix::app {

namespace {
constexpr const char* kLog = "HotkeyManager";

struct NamedKey {
    const char*   name;
    std::uint32_t code;
};

/// Kody klawiszy w konwencji Windows Virtual-Key — to platforma docelowa,
/// a pozostałe backendy i tak mapują je na własne odpowiedniki.
const std::array<NamedKey, 34> kNamedKeys{{
    {"space", 0x20}, {"enter", 0x0D}, {"return", 0x0D}, {"tab", 0x09}, {"escape", 0x1B},
    {"esc", 0x1B}, {"backspace", 0x08}, {"delete", 0x2E}, {"insert", 0x2D},
    {"home", 0x24}, {"end", 0x23}, {"pageup", 0x21}, {"pagedown", 0x22},
    {"left", 0x25}, {"up", 0x26}, {"right", 0x27}, {"down", 0x28},
    {"f1", 0x70}, {"f2", 0x71}, {"f3", 0x72}, {"f4", 0x73}, {"f5", 0x74}, {"f6", 0x75},
    {"f7", 0x76}, {"f8", 0x77}, {"f9", 0x78}, {"f10", 0x79}, {"f11", 0x7A}, {"f12", 0x7B},
    {"plus", 0xBB}, {"minus", 0xBD}, {"comma", 0xBC}, {"period", 0xBE}, {"slash", 0xBF},
}};

std::string lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

std::string trim(const std::string& text) {
    const auto begin = text.find_first_not_of(" \t");
    if (begin == std::string::npos) return {};
    const auto end = text.find_last_not_of(" \t");
    return text.substr(begin, end - begin + 1);
}

/// Backend bez globalnego przechwytywania — skróty można wyzwalać programowo.
class NullHotkeyBackend final : public HotkeyBackend {
public:
    [[nodiscard]] const char* name() const noexcept override { return "null"; }

    Status start(Callback callback) override {
        callback_ = std::move(callback);
        return Status::success();
    }

    void stop() override { callback_ = nullptr; }

    Status registerHotkey(const std::string&, std::uint32_t, std::uint32_t) override {
        return Status::success();
    }

    void unregisterHotkey(const std::string&) override {}

    [[nodiscard]] bool supportsGlobalCapture() const noexcept override { return false; }

private:
    Callback callback_;
};

} // namespace

std::unique_ptr<HotkeyBackend> createPlatformHotkeyBackend() {
#if defined(_WIN32)
    if (auto backend = createWindowsHotkeyBackend()) return backend;
    Log::warn(kLog, "Rejestracja skrótów systemowych niedostępna");
#endif
    return std::make_unique<NullHotkeyBackend>();
}

HotkeyManager::HotkeyManager(std::unique_ptr<HotkeyBackend> backend)
    : backend_(backend ? std::move(backend) : createPlatformHotkeyBackend()) {}

HotkeyManager::~HotkeyManager() { stop(); }

bool HotkeyManager::parseCombo(const std::string& combo, std::uint32_t& modifiers, std::uint32_t& keyCode) {
    modifiers = kModNone;
    keyCode = 0;

    std::vector<std::string> parts;
    std::stringstream stream(combo);
    std::string token;
    while (std::getline(stream, token, '+')) {
        const std::string trimmed = trim(token);
        if (!trimmed.empty()) parts.push_back(trimmed);
    }
    if (parts.empty()) return false;

    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string key = lower(parts[i]);
        const bool isLast = (i + 1 == parts.size());

        if (key == "ctrl" || key == "control") { modifiers |= kModCtrl;  continue; }
        if (key == "alt")                      { modifiers |= kModAlt;   continue; }
        if (key == "shift")                    { modifiers |= kModShift; continue; }
        if (key == "win" || key == "super" || key == "meta") { modifiers |= kModWin; continue; }

        if (!isLast) return false;  // modyfikator po klawiszu głównym = błąd zapisu

        if (key.size() == 1) {
            const char c = key[0];
            if (c >= 'a' && c <= 'z') { keyCode = static_cast<std::uint32_t>(std::toupper(c)); continue; }
            if (c >= '0' && c <= '9') { keyCode = static_cast<std::uint32_t>(c); continue; }
            return false;
        }

        const auto it = std::find_if(kNamedKeys.begin(), kNamedKeys.end(),
                                     [&](const NamedKey& named) { return key == named.name; });
        if (it == kNamedKeys.end()) return false;
        keyCode = it->code;
    }

    return keyCode != 0;
}

std::string HotkeyManager::formatCombo(std::uint32_t modifiers, std::uint32_t keyCode) {
    std::string result;
    if (modifiers & kModCtrl)  result += "Ctrl+";
    if (modifiers & kModAlt)   result += "Alt+";
    if (modifiers & kModShift) result += "Shift+";
    if (modifiers & kModWin)   result += "Win+";

    if ((keyCode >= 'A' && keyCode <= 'Z') || (keyCode >= '0' && keyCode <= '9')) {
        result.push_back(static_cast<char>(keyCode));
        return result;
    }

    const auto it = std::find_if(kNamedKeys.begin(), kNamedKeys.end(),
                                 [&](const NamedKey& named) { return named.code == keyCode; });
    if (it != kNamedKeys.end()) {
        std::string name = it->name;
        if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        result += name;
    } else {
        result += "0x" + std::to_string(keyCode);
    }
    return result;
}

std::vector<HotkeyBinding> HotkeyManager::defaultBindings() {
    auto make = [](const char* id, const char* combo, const char* action,
                   const char* argKey, JsonValue argValue) {
        HotkeyBinding binding;
        binding.id     = id;
        binding.combo  = combo;
        binding.action = action;
        binding.args   = JsonValue::makeObject();
        if (argKey != nullptr) binding.args.set(argKey, std::move(argValue));
        parseCombo(binding.combo, binding.modifiers, binding.keyCode);
        return binding;
    };

    return {
        make("mute-microphone", "Ctrl+Alt+M", "channel.toggleMute", "channel", JsonValue("Microphone")),
        make("mute-game",       "Ctrl+Alt+G", "channel.toggleMute", "channel", JsonValue("Game")),
        make("mute-chat",       "Ctrl+Alt+C", "channel.toggleMute", "channel", JsonValue("Chat")),
        make("profile-gaming",  "Ctrl+Alt+1", "profile.load",       "name",    JsonValue("Gaming")),
        make("profile-stream",  "Ctrl+Alt+2", "profile.load",       "name",    JsonValue("Streaming")),
    };
}

Status HotkeyManager::start() {
    std::vector<HotkeyBinding> snapshot;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (started_) return Status::success();
        snapshot = bindings_;
    }

    const Status status = backend_->start([this](const std::string& id) { onHotkey(id); });
    if (!status) return status;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        started_ = true;
    }

    Status result = Status::success();
    for (auto& binding : snapshot) {
        const Status bindStatus = bind(binding);
        if (!bindStatus && result) result = bindStatus;
    }
    return result;
}

void HotkeyManager::stop() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        for (const auto& binding : bindings_) ids.push_back(binding.id);
        started_ = false;
    }

    for (const auto& id : ids) backend_->unregisterHotkey(id);
    backend_->stop();

    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& binding : bindings_) binding.registered = false;
}

void HotkeyManager::setDispatcher(Dispatcher dispatcher) {
    std::lock_guard<std::mutex> lock(mutex_);
    dispatcher_ = std::move(dispatcher);
}

Status HotkeyManager::bind(HotkeyBinding binding) {
    if (binding.id.empty()) return Status::error("Skrót musi mieć identyfikator");

    if (!parseCombo(binding.combo, binding.modifiers, binding.keyCode))
        return Status::error("Nieprawidłowy skrót: " + binding.combo);

    bool shouldRegister = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shouldRegister = started_ && binding.enabled;

        const auto it = std::find_if(bindings_.begin(), bindings_.end(),
                                     [&](const HotkeyBinding& b) { return b.id == binding.id; });
        if (it != bindings_.end()) *it = binding;
        else bindings_.push_back(binding);
    }

    backend_->unregisterHotkey(binding.id);

    if (!shouldRegister) return Status::success();

    const Status status = backend_->registerHotkey(binding.id, binding.modifiers, binding.keyCode);

    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = std::find_if(bindings_.begin(), bindings_.end(),
                                 [&](const HotkeyBinding& b) { return b.id == binding.id; });
    if (it != bindings_.end()) {
        it->registered = static_cast<bool>(status);
        it->lastError  = status.message;
    }

    if (!status)
        Log::warn(kLog, "Nie udało się zarejestrować " + binding.combo + ": " + status.message);
    return status;
}

bool HotkeyManager::unbind(const std::string& id) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(bindings_.begin(), bindings_.end(),
                                     [&](const HotkeyBinding& b) { return b.id == id; });
        if (it == bindings_.end()) return false;
        bindings_.erase(it);
    }
    backend_->unregisterHotkey(id);
    return true;
}

void HotkeyManager::clear() {
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& binding : bindings_) ids.push_back(binding.id);
        bindings_.clear();
    }
    for (const auto& id : ids) backend_->unregisterHotkey(id);
}

std::vector<HotkeyBinding> HotkeyManager::bindings() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return bindings_;
}

bool HotkeyManager::trigger(const std::string& id) {
    HotkeyBinding binding;
    Dispatcher dispatcher;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = std::find_if(bindings_.begin(), bindings_.end(),
                                     [&](const HotkeyBinding& b) { return b.id == id; });
        if (it == bindings_.end() || !it->enabled) return false;
        binding = *it;
        dispatcher = dispatcher_;
    }

    if (dispatcher) dispatcher(binding);
    return true;
}

bool HotkeyManager::supportsGlobalCapture() const noexcept { return backend_->supportsGlobalCapture(); }

void HotkeyManager::onHotkey(const std::string& id) { trigger(id); }

} // namespace helix::app
