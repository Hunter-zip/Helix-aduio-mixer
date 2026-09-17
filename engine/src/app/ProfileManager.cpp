#include "helix/app/ProfileManager.h"

#include <algorithm>
#include <filesystem>

#include "helix/Log.h"

namespace helix::app {

namespace {
constexpr const char* kLog = "ProfileManager";

std::string sanitize(const std::string& name) {
    std::string out;
    out.reserve(name.size());
    for (const char c : name) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ' ';
        out.push_back(allowed ? c : '_');
    }
    // Spacje zamieniamy na myślniki, żeby nazwy plików były wygodne w konsoli.
    std::replace(out.begin(), out.end(), ' ', '-');
    return out;
}
} // namespace

ProfileManager::ProfileManager(std::string directory, Capture capture, Apply apply)
    : directory_(std::move(directory)), capture_(std::move(capture)), apply_(std::move(apply)) {}

bool ProfileManager::isValidName(const std::string& name) {
    if (name.empty() || name.size() > 64) return false;
    return std::all_of(name.begin(), name.end(), [](char c) {
        return static_cast<unsigned char>(c) >= 0x20 && c != '/' && c != '\\' && c != ':';
    });
}

std::string ProfileManager::pathFor(const std::string& name) const {
    namespace fs = std::filesystem;
    return (fs::path(directory_) / (sanitize(name) + ".json")).string();
}

std::vector<ProfileInfo> ProfileManager::list() const {
    namespace fs = std::filesystem;
    std::vector<ProfileInfo> result;

    std::error_code code;
    if (!fs::exists(directory_, code)) return result;

    for (const auto& entry : fs::directory_iterator(directory_, code)) {
        if (code) break;
        if (!entry.is_regular_file()) continue;
        if (entry.path().extension() != ".json") continue;

        JsonValue document;
        std::string error;
        if (!readJsonFile(entry.path().string(), document, error)) {
            Log::warn(kLog, "Pomijam uszkodzony profil: " + entry.path().string());
            continue;
        }

        ProfileInfo info;
        info.name          = document["profile"].asString(entry.path().stem().string());
        info.path          = entry.path().string();
        info.schemaVersion = document["schemaVersion"].asInt(1);
        info.description   = document["description"].asString();
        info.active        = (info.name == active_);
        result.push_back(std::move(info));
    }

    std::sort(result.begin(), result.end(),
              [](const ProfileInfo& a, const ProfileInfo& b) { return a.name < b.name; });
    return result;
}

bool ProfileManager::exists(const std::string& name) const {
    std::error_code code;
    return std::filesystem::exists(pathFor(name), code);
}

Status ProfileManager::save(const std::string& name, const std::string& description) {
    if (!isValidName(name)) return Status::error("Nieprawidłowa nazwa profilu");
    if (!capture_) return Status::error("Brak funkcji zapisu stanu");

    JsonValue document = capture_();
    document.set("schemaVersion", JsonValue(kConfigSchemaVersion));
    document.set("profile", JsonValue(name));
    if (!description.empty()) document.set("description", JsonValue(description));

    std::string error;
    if (!writeJsonFile(pathFor(name), document, error))
        return Status::error("Nie udało się zapisać profilu: " + error);

    active_ = name;
    Log::info(kLog, "Zapisano profil: " + name);
    return Status::success();
}

Status ProfileManager::read(const std::string& name, JsonValue& out) const {
    std::string error;
    if (!readJsonFile(pathFor(name), out, error))
        return Status::error("Nie udało się wczytać profilu: " + error);
    return Status::success();
}

Status ProfileManager::write(const std::string& name, const JsonValue& document) {
    if (!isValidName(name)) return Status::error("Nieprawidłowa nazwa profilu");
    std::string error;
    if (!writeJsonFile(pathFor(name), document, error))
        return Status::error("Nie udało się zapisać profilu: " + error);
    return Status::success();
}

Status ProfileManager::load(const std::string& name) {
    if (!apply_) return Status::error("Brak funkcji wczytywania stanu");

    JsonValue document;
    const Status readStatus = read(name, document);
    if (!readStatus) return readStatus;

    std::string message;
    if (!migrate(document, message))
        return Status::error("Nie można wczytać profilu: " + message);
    if (!message.empty()) Log::info(kLog, message);

    const Status applyStatus = apply_(document);
    if (!applyStatus) return applyStatus;

    active_ = document["profile"].asString(name);
    Log::info(kLog, "Aktywny profil: " + active_);
    return Status::success();
}

Status ProfileManager::remove(const std::string& name) {
    std::error_code code;
    if (!std::filesystem::remove(pathFor(name), code))
        return Status::error("Profil nie istnieje: " + name);
    if (active_ == name) active_.clear();
    return Status::success();
}

Status ProfileManager::rename(const std::string& from, const std::string& to) {
    if (!isValidName(to)) return Status::error("Nieprawidłowa nazwa profilu");

    JsonValue document;
    const Status readStatus = read(from, document);
    if (!readStatus) return readStatus;

    document.set("profile", JsonValue(to));
    const Status writeStatus = write(to, document);
    if (!writeStatus) return writeStatus;

    std::error_code code;
    std::filesystem::remove(pathFor(from), code);
    if (active_ == from) active_ = to;
    return Status::success();
}

Status ProfileManager::duplicate(const std::string& from, const std::string& to) {
    if (!isValidName(to)) return Status::error("Nieprawidłowa nazwa profilu");

    JsonValue document;
    const Status readStatus = read(from, document);
    if (!readStatus) return readStatus;

    document.set("profile", JsonValue(to));
    return write(to, document);
}

std::string ProfileManager::activeProfile() const { return active_; }

void ProfileManager::setActiveProfile(std::string name) { active_ = std::move(name); }

bool ProfileManager::migrate(JsonValue& document, std::string& message) {
    message.clear();
    if (!document.isObject()) {
        message = "dokument profilu nie jest obiektem";
        return false;
    }

    int version = document["schemaVersion"].asInt(1);
    if (version > kConfigSchemaVersion) {
        message = "profil pochodzi z nowszej wersji programu (schemat " + std::to_string(version) + ")";
        return false;
    }

    // v1 → v2: „outputs” były listą nazw magistral; teraz trzymamy pełną macierz
    // routingu z wzmocnieniami wysyłek. Stare pola zostają zachowane jako wejście
    // migracji, a brakujące uzupełniamy wartościami domyślnymi.
    if (version < 2) {
        JsonValue routing = document["routing"];
        if (!routing.isArray()) routing = JsonValue::makeArray();

        const JsonValue& channels = document["channels"];
        for (std::size_t i = 0; i < channels.size(); ++i) {
            const JsonValue& channel = channels[i];
            const std::string channelName = channel["name"].asString();
            const JsonValue& outputs = channel["outputs"];
            for (std::size_t o = 0; o < outputs.size(); ++o) {
                JsonValue entry = JsonValue::makeObject();
                entry.set("channel", JsonValue(channelName));
                entry.set("bus", JsonValue(outputs[o].asString()));
                entry.set("enabled", JsonValue(true));
                entry.set("gain", JsonValue(1.0));
                routing.push(std::move(entry));
            }
        }

        document.set("routing", std::move(routing));
        version = 2;
        message = "Profil zmigrowany do schematu " + std::to_string(kConfigSchemaVersion);
    }

    document.set("schemaVersion", JsonValue(kConfigSchemaVersion));
    return true;
}

Status ProfileManager::createDefaults() {
    if (!capture_) return Status::error("Brak funkcji zapisu stanu");

    struct DefaultProfile {
        const char* name;
        const char* description;
    };
    static constexpr DefaultProfile kDefaults[] = {
        {"Gaming",    "Gra na słuchawkach, czat i muzyka w tle"},
        {"Streaming", "Osobna magistrala streamowa dla OBS"},
        {"Music",     "Płaski tor pod odsłuch muzyki"},
        {"Work",      "Mikrofon z redukcją szumów, ciche powiadomienia"},
        {"Movie",     "Media na pierwszym planie, reszta wyciszona"},
    };

    Status result = Status::success();
    for (const auto& profile : kDefaults) {
        if (exists(profile.name)) continue;
        const Status status = save(profile.name, profile.description);
        if (!status && result) result = status;
    }
    return result;
}

} // namespace helix::app
