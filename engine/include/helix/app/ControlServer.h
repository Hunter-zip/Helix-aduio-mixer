// Serwer sterujący: protokół JSON-lines po TCP na pętli zwrotnej (spec §28).
//
// GUI działa w osobnym procesie. Jego awaria, restart albo brak nie mają
// żadnego wpływu na wątek audio — to jest sedno wymagania z §28.
//
// Protokół:
//   żądanie   {"id":1,"cmd":"channel.setVolume","args":{...}}
//   odpowiedź {"id":1,"ok":true,"result":{...}}
//             {"id":1,"ok":false,"error":"..."}
//   zdarzenie {"event":"meters","data":{...}}
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "helix/Types.h"
#include "helix/app/EngineController.h"
#include "helix/app/Json.h"

namespace helix::app {

class ControlServer {
public:
    struct Options {
        std::string address   = "127.0.0.1";
        int         port      = 47811;
        std::string tokenPath;        ///< plik z tokenem; pusty = brak uwierzytelniania
        int         meterHz   = 30;   ///< częstotliwość rozsyłania pomiarów (spec §13)
        int         maxClients = 8;
    };

    explicit ControlServer(EngineController& controller);
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    Status start(const Options& options);
    void   stop();
    [[nodiscard]] bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    [[nodiscard]] int port() const noexcept { return options_.port; }
    [[nodiscard]] const std::string& token() const noexcept { return token_; }

    /// Rozsyła zdarzenie do wszystkich uwierzytelnionych klientów.
    void broadcast(const std::string& event, const JsonValue& data);

    /// Przetwarza pojedyncze żądanie w formie tekstowej (używane też przez CLI i testy).
    [[nodiscard]] std::string handleLine(const std::string& line, bool& authenticated);

private:
    struct Client;

    void acceptLoop();
    void clientLoop(std::shared_ptr<Client> client);
    void meterLoop();
    void sendTo(Client& client, const std::string& payload);

    [[nodiscard]] Status prepareToken();

    EngineController& controller_;
    Options           options_;
    std::string       token_;

    std::atomic<bool> running_{false};
    /// Gniazdo nasłuchujące zamyka stop() z innego wątku niż acceptLoop() —
    /// dostęp musi być atomowy.
    std::atomic<int>  listenSocket_{-1};

    std::unique_ptr<std::thread> acceptThread_;
    std::unique_ptr<std::thread> meterThread_;

    struct ClientThread {
        std::shared_ptr<Client>      client;
        std::unique_ptr<std::thread> thread;
    };

    /// Zwalnia wątki klientów, którzy się rozłączyli. Wołane przy każdym
    /// nowym połączeniu, żeby lista nie rosła przez cały czas pracy.
    void reapFinishedClients();

    mutable std::mutex clientsMutex_;
    std::vector<std::shared_ptr<Client>> clients_;
    std::vector<ClientThread>            clientThreads_;
};

} // namespace helix::app
