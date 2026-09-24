// Wykrywanie aplikacji odtwarzających dźwięk (spec §5, §16).
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "helix/Types.h"

namespace helix::app {

/// Pojedyncza sesja audio aplikacji.
struct AudioApplication {
    std::uint32_t processId = 0;
    std::string   executable;    ///< np. "Discord.exe"
    std::string   displayName;
    std::string   sessionId;     ///< identyfikator sesji systemowej
    bool          active = false;///< aktualnie produkuje dźwięk
    float         volume = 1.0f;
    bool          muted = false;
};

/// Tabela przypisań aplikacja → kanał (spec §5).
/// Przypisanie przeżywa restart aplikacji, bo kluczem jest nazwa pliku.
class AppAssignments {
public:
    void set(const std::string& executable, std::string channelName);
    bool remove(const std::string& executable);
    void clear();

    /// Zwraca nazwę kanału lub pusty napis.
    [[nodiscard]] std::string lookup(const std::string& executable) const;

    [[nodiscard]] std::map<std::string, std::string> all() const;
    void replaceAll(std::map<std::string, std::string> entries);

    /// Normalizacja klucza: małe litery, bez ścieżki.
    [[nodiscard]] static std::string normalize(const std::string& executable);

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::string> entries_;
};

/// Detektor sesji audio. Implementacja systemowa wypełnia `enumerate()`.
class AppDetector {
public:
    /// `appeared = true` gdy aplikacja pojawiła się, `false` gdy zniknęła.
    using Handler = std::function<void(const AudioApplication& app, bool appeared)>;

    virtual ~AppDetector() = default;

    [[nodiscard]] virtual const char* name() const noexcept = 0;
    virtual Status start() { return Status::success(); }
    virtual void   stop() {}

    /// Bieżąca lista sesji audio.
    [[nodiscard]] virtual std::vector<AudioApplication> enumerate() = 0;

    void setHandler(Handler handler);

    /// Porównuje bieżącą listę z poprzednią i zgłasza różnice.
    /// Wołane cyklicznie przez wątek sterujący.
    void poll();

    [[nodiscard]] std::vector<AudioApplication> known() const;

protected:
    mutable std::mutex mutex_;
    std::map<std::uint32_t, AudioApplication> known_;
    Handler handler_;
};

/// Detektor sterowany ręcznie — używany poza Windows oraz w testach.
class ManualAppDetector final : public AppDetector {
public:
    [[nodiscard]] const char* name() const noexcept override { return "manual"; }
    [[nodiscard]] std::vector<AudioApplication> enumerate() override;

    void simulateStart(AudioApplication app);
    void simulateStop(std::uint32_t processId);

private:
    std::mutex listMutex_;
    std::vector<AudioApplication> applications_;
};

[[nodiscard]] std::unique_ptr<AppDetector> createPlatformAppDetector();

} // namespace helix::app
