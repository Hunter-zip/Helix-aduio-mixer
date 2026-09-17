#include "helix/app/ControlServer.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

#include "helix/Log.h"

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketHandle = SOCKET;
  #define HELIX_INVALID_SOCKET INVALID_SOCKET
  #define HELIX_CLOSE_SOCKET closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <netinet/tcp.h>
  #include <sys/select.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SocketHandle = int;
  #define HELIX_INVALID_SOCKET (-1)
  #define HELIX_CLOSE_SOCKET ::close
#endif

namespace helix::app {

namespace {
constexpr const char* kLog = "ControlServer";
constexpr std::size_t kMaxLineBytes = 1u << 20;   // 1 MiB — zabezpieczenie przed zalaniem

#if defined(_WIN32)
/// Winsock wymaga inicjalizacji na proces; robimy to raz i leniwie.
bool ensureWinsock() {
    static bool initialized = [] {
        WSADATA data{};
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    return initialized;
}
#endif

std::string generateToken() {
    static constexpr char kAlphabet[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    std::random_device device;
    std::mt19937_64 generator(device());
    std::uniform_int_distribution<std::size_t> distribution(0, sizeof(kAlphabet) - 2);

    std::string token;
    token.reserve(40);
    for (int i = 0; i < 40; ++i) token.push_back(kAlphabet[distribution(generator)]);
    return token;
}

/// Porównanie odporne na pomiar czasu — token to sekret.
bool secureEquals(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    unsigned char difference = 0;
    for (std::size_t i = 0; i < a.size(); ++i)
        difference |= static_cast<unsigned char>(a[i] ^ b[i]);
    return difference == 0;
}

} // namespace

struct ControlServer::Client {
    SocketHandle socket = HELIX_INVALID_SOCKET;
    std::atomic<bool> authenticated{false};
    std::atomic<bool> connected{true};
    std::mutex writeMutex;
    std::string inbox;
};

ControlServer::ControlServer(EngineController& controller) : controller_(controller) {}

ControlServer::~ControlServer() { stop(); }

Status ControlServer::prepareToken() {
    if (options_.tokenPath.empty()) {
        token_.clear();
        return Status::success();
    }

    token_ = generateToken();

    namespace fs = std::filesystem;
    std::error_code code;
    const fs::path path(options_.tokenPath);
    if (path.has_parent_path()) fs::create_directories(path.parent_path(), code);

    std::ofstream file(options_.tokenPath, std::ios::trunc);
    if (!file) return Status::error("Nie można zapisać tokenu: " + options_.tokenPath);
    file << token_;
    file.close();

#if !defined(_WIN32)
    // Token ma być czytelny tylko dla właściciela sesji.
    fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, code);
#endif
    return Status::success();
}

Status ControlServer::start(const Options& options) {
    if (running_.load(std::memory_order_acquire)) return Status::success();
    options_ = options;

#if defined(_WIN32)
    if (!ensureWinsock()) return Status::error("Nie można zainicjalizować Winsock");
#endif

    const Status tokenStatus = prepareToken();
    if (!tokenStatus) return tokenStatus;

    SocketHandle handle = socket(AF_INET, SOCK_STREAM, 0);
    if (handle == HELIX_INVALID_SOCKET) return Status::error("Nie można utworzyć gniazda");

    int reuse = 1;
    setsockopt(handle, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port   = htons(static_cast<std::uint16_t>(options_.port));
    if (inet_pton(AF_INET, options_.address.c_str(), &address.sin_addr) != 1) {
        HELIX_CLOSE_SOCKET(handle);
        return Status::error("Nieprawidłowy adres: " + options_.address);
    }

    if (bind(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        HELIX_CLOSE_SOCKET(handle);
        return Status::error("Port " + std::to_string(options_.port) + " jest zajęty");
    }

    if (listen(handle, 8) != 0) {
        HELIX_CLOSE_SOCKET(handle);
        return Status::error("Nie można nasłuchiwać na porcie " + std::to_string(options_.port));
    }

    listenSocket_.store(static_cast<int>(handle), std::memory_order_release);
    running_.store(true, std::memory_order_release);

    acceptThread_ = std::make_unique<std::thread>([this] { acceptLoop(); });
    if (options_.meterHz > 0)
        meterThread_ = std::make_unique<std::thread>([this] { meterLoop(); });

    Log::info(kLog, "Nasłuchiwanie na " + options_.address + ":" + std::to_string(options_.port));
    return Status::success();
}

void ControlServer::stop() {
    if (!running_.exchange(false, std::memory_order_acq_rel)) return;

    const int listener = listenSocket_.exchange(-1, std::memory_order_acq_rel);
    if (listener >= 0) HELIX_CLOSE_SOCKET(static_cast<SocketHandle>(listener));

    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        for (auto& client : clients_) {
            client->connected.store(false, std::memory_order_release);
            if (client->socket != HELIX_INVALID_SOCKET) {
                HELIX_CLOSE_SOCKET(client->socket);
                client->socket = HELIX_INVALID_SOCKET;
            }
        }
    }

    if (acceptThread_ && acceptThread_->joinable()) acceptThread_->join();
    if (meterThread_ && meterThread_->joinable()) meterThread_->join();
    acceptThread_.reset();
    meterThread_.reset();

    std::vector<ClientThread> threads;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        threads = std::move(clientThreads_);
        clientThreads_.clear();
        clients_.clear();
    }
    for (auto& entry : threads)
        if (entry.thread && entry.thread->joinable()) entry.thread->join();

    Log::info(kLog, "Zatrzymano");
}

void ControlServer::acceptLoop() {
    while (running_.load(std::memory_order_acquire)) {
        const int listener = listenSocket_.load(std::memory_order_acquire);
        if (listener < 0) break;

        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(static_cast<SocketHandle>(listener), &readable);

        timeval timeout{};
        timeout.tv_sec = 0;
        timeout.tv_usec = 200000;   // 200 ms — pozwala domknąć wątek po stop()

        const int ready = select(listener + 1, &readable, nullptr, nullptr, &timeout);
        if (ready <= 0) continue;
        if (!running_.load(std::memory_order_acquire)) break;

        sockaddr_in peer{};
#if defined(_WIN32)
        int peerSize = sizeof(peer);
#else
        socklen_t peerSize = sizeof(peer);
#endif
        SocketHandle accepted = accept(static_cast<SocketHandle>(listener),
                                       reinterpret_cast<sockaddr*>(&peer), &peerSize);
        if (accepted == HELIX_INVALID_SOCKET) continue;

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            if (static_cast<int>(clients_.size()) >= options_.maxClients) {
                HELIX_CLOSE_SOCKET(accepted);
                Log::warn(kLog, "Odrzucono połączenie — limit klientów");
                continue;
            }
        }

        int noDelay = 1;
        setsockopt(accepted, IPPROTO_TCP, TCP_NODELAY,
                   reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));

        reapFinishedClients();

        auto client = std::make_shared<Client>();
        client->socket = accepted;
        client->authenticated.store(token_.empty(), std::memory_order_release);

        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            clients_.push_back(client);
            clientThreads_.push_back(ClientThread{
                client,
                std::make_unique<std::thread>([this, client] { clientLoop(client); })});
        }
    }
}

void ControlServer::reapFinishedClients() {
    std::vector<ClientThread> finished;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        auto it = clientThreads_.begin();
        while (it != clientThreads_.end()) {
            const bool alive = it->client && it->client->connected.load(std::memory_order_acquire);
            if (alive) { ++it; continue; }
            finished.push_back(std::move(*it));
            it = clientThreads_.erase(it);
        }
    }
    for (auto& entry : finished)
        if (entry.thread && entry.thread->joinable()) entry.thread->join();
}

void ControlServer::clientLoop(std::shared_ptr<Client> client) {
    std::array<char, 8192> buffer{};

    // Powitanie: klient od razu wie, czy musi się uwierzytelnić.
    {
        JsonValue hello = JsonValue::makeObject();
        hello.set("event", JsonValue("hello"));
        JsonValue data = JsonValue::makeObject();
        data.set("protocol", JsonValue(1));
        data.set("requiresAuth", JsonValue(!token_.empty()));
        data.set("engine", JsonValue("helix"));
        hello.set("data", std::move(data));
        sendTo(*client, hello.dump() + "\n");
    }

    while (running_.load(std::memory_order_acquire) &&
           client->connected.load(std::memory_order_acquire)) {

        const SocketHandle handle = client->socket;
        if (handle == HELIX_INVALID_SOCKET) break;

#if defined(_WIN32)
        const int received = recv(handle, buffer.data(), static_cast<int>(buffer.size()), 0);
#else
        const auto received = recv(handle, buffer.data(), buffer.size(), 0);
#endif
        if (received <= 0) break;

        client->inbox.append(buffer.data(), static_cast<std::size_t>(received));
        if (client->inbox.size() > kMaxLineBytes) {
            Log::warn(kLog, "Klient przekroczył limit rozmiaru żądania");
            break;
        }

        std::size_t newline;
        while ((newline = client->inbox.find('\n')) != std::string::npos) {
            std::string line = client->inbox.substr(0, newline);
            client->inbox.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;

            bool authenticated = client->authenticated.load(std::memory_order_acquire);
            const std::string response = handleLine(line, authenticated);
            client->authenticated.store(authenticated, std::memory_order_release);
            if (!response.empty()) sendTo(*client, response + "\n");
        }
    }

    client->connected.store(false, std::memory_order_release);
    if (client->socket != HELIX_INVALID_SOCKET) {
        HELIX_CLOSE_SOCKET(client->socket);
        client->socket = HELIX_INVALID_SOCKET;
    }

    std::lock_guard<std::mutex> lock(clientsMutex_);
    clients_.erase(std::remove(clients_.begin(), clients_.end(), client), clients_.end());
}

std::string ControlServer::handleLine(const std::string& line, bool& authenticated) {
    std::string parseError;
    const JsonValue request = JsonValue::parse(line, &parseError);

    JsonValue response = JsonValue::makeObject();
    if (!parseError.empty()) {
        response.set("ok", JsonValue(false));
        response.set("error", JsonValue("Nieprawidłowy JSON: " + parseError));
        return response.dump();
    }

    const JsonValue& id = request["id"];
    if (!id.isNull()) response.set("id", id);

    const std::string command = request["cmd"].asString();
    const JsonValue& args = request["args"];

    if (command.empty()) {
        response.set("ok", JsonValue(false));
        response.set("error", JsonValue("Brak pola \"cmd\""));
        return response.dump();
    }

    if (command == "auth") {
        const std::string provided = args["token"].asString();
        authenticated = token_.empty() || secureEquals(token_, provided);
        response.set("ok", JsonValue(authenticated));
        if (!authenticated) response.set("error", JsonValue("Nieprawidłowy token"));
        else response.set("result", controller_.snapshot());
        return response.dump();
    }

    if (!authenticated) {
        response.set("ok", JsonValue(false));
        response.set("error", JsonValue("Wymagane uwierzytelnienie"));
        return response.dump();
    }

    Status status = Status::success();
    JsonValue result = controller_.dispatch(command, args, status);

    response.set("ok", JsonValue(static_cast<bool>(status)));
    if (status) response.set("result", std::move(result));
    else response.set("error", JsonValue(status.message));
    return response.dump();
}

void ControlServer::sendTo(Client& client, const std::string& payload) {
    std::lock_guard<std::mutex> lock(client.writeMutex);
    const SocketHandle handle = client.socket;
    if (handle == HELIX_INVALID_SOCKET) return;

    std::size_t sent = 0;
    while (sent < payload.size()) {
#if defined(_WIN32)
        const int written = send(handle, payload.data() + sent,
                                 static_cast<int>(payload.size() - sent), 0);
#else
        const auto written = send(handle, payload.data() + sent, payload.size() - sent, MSG_NOSIGNAL);
#endif
        if (written <= 0) {
            client.connected.store(false, std::memory_order_release);
            return;
        }
        sent += static_cast<std::size_t>(written);
    }
}

void ControlServer::broadcast(const std::string& event, const JsonValue& data) {
    JsonValue message = JsonValue::makeObject();
    message.set("event", JsonValue(event));
    message.set("data", data);
    const std::string payload = message.dump() + "\n";

    std::vector<std::shared_ptr<Client>> snapshot;
    {
        std::lock_guard<std::mutex> lock(clientsMutex_);
        snapshot = clients_;
    }

    for (auto& client : snapshot) {
        if (!client->authenticated.load(std::memory_order_acquire)) continue;
        if (!client->connected.load(std::memory_order_acquire)) continue;
        sendTo(*client, payload);
    }
}

void ControlServer::meterLoop() {
    using clock = std::chrono::steady_clock;
    const auto period = std::chrono::milliseconds(1000 / std::max(1, options_.meterHz));
    auto next = clock::now();

    while (running_.load(std::memory_order_acquire)) {
        bool anyClient = false;
        {
            std::lock_guard<std::mutex> lock(clientsMutex_);
            anyClient = std::any_of(clients_.begin(), clients_.end(), [](const auto& client) {
                return client->authenticated.load(std::memory_order_acquire);
            });
        }

        // Bez podłączonego GUI nie ma sensu liczyć i serializować pomiarów.
        if (anyClient) broadcast("meters", controller_.meters());

        next += period;
        const auto now = clock::now();
        if (next > now) std::this_thread::sleep_for(next - now);
        else next = now;
    }
}

} // namespace helix::app
