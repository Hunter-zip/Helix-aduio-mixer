// Profile konfiguracji (spec §15, §24).
//
// Profil to dokument JSON z wersją schematu. Wczytanie starszej wersji
// uruchamia migrację, więc aktualizacja programu nie kasuje ustawień.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "helix/Types.h"
#include "helix/app/Json.h"

namespace helix::app {

struct ProfileInfo {
    std::string name;
    std::string path;
    int         schemaVersion = 0;
    bool        active = false;
    std::string description;
};

class ProfileManager {
public:
    /// `capture` buduje dokument z bieżącego stanu, `apply` wgrywa dokument do silnika.
    using Capture = std::function<JsonValue()>;
    using Apply   = std::function<Status(const JsonValue&)>;

    ProfileManager(std::string directory, Capture capture, Apply apply);

    [[nodiscard]] const std::string& directory() const noexcept { return directory_; }

    [[nodiscard]] std::vector<ProfileInfo> list() const;
    [[nodiscard]] bool exists(const std::string& name) const;

    /// Zapisuje bieżący stan pod wskazaną nazwą.
    Status save(const std::string& name, const std::string& description = {});

    /// Wczytuje profil i stosuje go do silnika — bez restartu programu (spec §15).
    Status load(const std::string& name);

    Status remove(const std::string& name);
    Status rename(const std::string& from, const std::string& to);
    Status duplicate(const std::string& from, const std::string& to);

    [[nodiscard]] std::string activeProfile() const;
    void setActiveProfile(std::string name);

    /// Tworzy zestaw profili startowych (Gaming, Streaming, Music, Work, Movie).
    Status createDefaults();

    /// Odczytuje dokument profilu bez stosowania go.
    Status read(const std::string& name, JsonValue& out) const;

    /// Zapisuje gotowy dokument (np. po edycji w GUI).
    Status write(const std::string& name, const JsonValue& document);

    /// Migracja schematu do bieżącej wersji. Zwraca false przy wersji z przyszłości.
    static bool migrate(JsonValue& document, std::string& message);

    /// Nazwa pliku dla profilu (sanityzowana).
    [[nodiscard]] std::string pathFor(const std::string& name) const;

    /// Czy nazwa profilu jest dopuszczalna.
    [[nodiscard]] static bool isValidName(const std::string& name);

private:
    std::string directory_;
    Capture     capture_;
    Apply       apply_;
    std::string active_;
};

} // namespace helix::app
