// helix-cli — klient serwera sterującego. Przydatny do diagnostyki,
// skryptów i testów integracyjnych bez uruchamiania GUI.
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>

#include "helix/app/Json.h"
#include "helix/app/EngineController.h"

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using SocketHandle = SOCKET;
  #define HELIX_INVALID_SOCKET INVALID_SOCKET
  #define HELIX_CLOSE_SOCKET closesocket
#else
  #include <arpa/inet.h>
  #include <netinet/in.h>
  #include <sys/socket.h>
  #include <unistd.h>
  using SocketHandle = int;
  #define HELIX_INVALID_SOCKET (-1)
  #define HELIX_CLOSE_SOCKET ::close
#endif

namespace {

using helix::app::JsonValue;

void printUsage() {
    std::cout <<
        "helix-cli — klient sterujący Helix Audio Mixer\n\n"
        "Użycie: helix-cli [opcje] <komenda> [klucz=wartość ...]\n\n"
        "  --host <adres>          domyślnie 127.0.0.1\n"
        "  --port <numer>          domyślnie z pliku endpointu lub 47811\n"
        "  --config-dir <ścieżka>  katalog konfiguracji (token, endpoint)\n"
        "  --token <token>         token uwierzytelniający\n"
        "  --watch                 nasłuchuj zdarzeń zamiast wysyłać komendę\n"
        "  --raw                   wypisz surowy JSON bez formatowania\n"
        "  -h, --help              ta pomoc\n\n"
        "Przykłady:\n"
        "  helix-cli engine.status\n"
        "  helix-cli channel.setVolume channel=Game value=0.5\n"
        "  helix-cli routing.set channel=Music bus=B1 value=true\n"
        "  helix-cli --watch\n";
}

/// Konwersja \"klucz=wartość\" na JSON z rozpoznaniem typów.
JsonValue parseArgument(const std::string& text) {
    if (text == "true")  return JsonValue(true);
    if (text == "false") return JsonValue(false);
    if (text == "null")  return JsonValue();

    // Liczba tylko wtedy, gdy cały napis jest liczbą.
    try {
        std::size_t consumed = 0;
        const double number = std::stod(text, &consumed);
        if (consumed == text.size()) return JsonValue(number);
    } catch (...) {
        // nie liczba — zostaje napis
    }

    if (!text.empty() && (text.front() == '{' || text.front() == '[')) {
        std::string error;
        JsonValue parsed = JsonValue::parse(text, &error);
        if (error.empty()) return parsed;
    }
    return JsonValue(text);
}

bool readEndpoint(const std::string& configDir, std::string& host, int& port, std::string& token) {
    namespace fs = std::filesystem;
    const std::string path = (fs::path(configDir) / "control-endpoint.json").string();

    JsonValue document;
    std::string error;
    if (!helix::app::readJsonFile(path, document, error)) return false;

    if (document["address"].isString()) host = document["address"].asString();
    if (document["port"].isNumber()) port = document["port"].asInt(port);
    if (document["token"].isString()) token = document["token"].asString();
    return true;
}

SocketHandle connectTo(const std::string& host, int port, std::string& error) {
#if defined(_WIN32)
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        error = "nie można zainicjalizować Winsock";
        return HELIX_INVALID_SOCKET;
    }
#endif

    SocketHandle handle = socket(AF_INET, SOCK_STREAM, 0);
    if (handle == HELIX_INVALID_SOCKET) {
        error = "nie można utworzyć gniazda";
        return HELIX_INVALID_SOCKET;
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port   = htons(static_cast<std::uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        error = "nieprawidłowy adres: " + host;
        HELIX_CLOSE_SOCKET(handle);
        return HELIX_INVALID_SOCKET;
    }

    if (connect(handle, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        error = "nie można połączyć się z " + host + ":" + std::to_string(port) +
                " — czy helix-engine działa?";
        HELIX_CLOSE_SOCKET(handle);
        return HELIX_INVALID_SOCKET;
    }
    return handle;
}

bool sendLine(SocketHandle handle, const std::string& line) {
    const std::string payload = line + "\n";
    std::size_t sent = 0;
    while (sent < payload.size()) {
#if defined(_WIN32)
        const int written = send(handle, payload.data() + sent,
                                 static_cast<int>(payload.size() - sent), 0);
#else
        const auto written = send(handle, payload.data() + sent, payload.size() - sent, 0);
#endif
        if (written <= 0) return false;
        sent += static_cast<std::size_t>(written);
    }
    return true;
}

bool readLine(SocketHandle handle, std::string& buffer, std::string& line) {
    std::size_t newline;
    while ((newline = buffer.find('\n')) == std::string::npos) {
        char chunk[4096];
#if defined(_WIN32)
        const int received = recv(handle, chunk, sizeof(chunk), 0);
#else
        const auto received = recv(handle, chunk, sizeof(chunk), 0);
#endif
        if (received <= 0) return false;
        buffer.append(chunk, static_cast<std::size_t>(received));
    }
    line = buffer.substr(0, newline);
    buffer.erase(0, newline + 1);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    std::string host = "127.0.0.1";
    int port = 47811;
    std::string token;
    std::string configDir = helix::app::EngineController::defaultConfigDirectory();
    bool watch = false;
    bool raw = false;
    std::string command;
    JsonValue args = JsonValue::makeObject();

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        if (argument == "-h" || argument == "--help") { printUsage(); return 0; }
        if (argument == "--host" && i + 1 < argc) { host = argv[++i]; continue; }
        if (argument == "--port" && i + 1 < argc) { port = std::stoi(argv[++i]); continue; }
        if (argument == "--token" && i + 1 < argc) { token = argv[++i]; continue; }
        if (argument == "--config-dir" && i + 1 < argc) { configDir = argv[++i]; continue; }
        if (argument == "--watch") { watch = true; continue; }
        if (argument == "--raw") { raw = true; continue; }

        const std::size_t equals = argument.find('=');
        if (command.empty() && equals == std::string::npos) { command = argument; continue; }
        if (equals != std::string::npos) {
            args.set(argument.substr(0, equals), parseArgument(argument.substr(equals + 1)));
            continue;
        }

        std::cerr << "Nieznany argument: " << argument << '\n';
        return 2;
    }

    if (command.empty() && !watch) { printUsage(); return 2; }

    std::string discoveredToken;
    int discoveredPort = port;
    std::string discoveredHost = host;
    if (readEndpoint(configDir, discoveredHost, discoveredPort, discoveredToken)) {
        if (token.empty()) token = discoveredToken;
        if (port == 47811) port = discoveredPort;
    }

    std::string error;
    SocketHandle handle = connectTo(host, port, error);
    if (handle == HELIX_INVALID_SOCKET) {
        std::cerr << "Błąd: " << error << '\n';
        return 1;
    }

    std::string buffer;
    std::string line;

    // Powitanie.
    if (!readLine(handle, buffer, line)) {
        std::cerr << "Błąd: połączenie zerwane\n";
        HELIX_CLOSE_SOCKET(handle);
        return 1;
    }

    const JsonValue hello = JsonValue::parse(line);
    if (hello["data"]["requiresAuth"].asBool(false)) {
        JsonValue request = JsonValue::makeObject();
        request.set("id", JsonValue(0));
        request.set("cmd", JsonValue("auth"));
        JsonValue authArgs = JsonValue::makeObject();
        authArgs.set("token", JsonValue(token));
        request.set("args", std::move(authArgs));

        if (!sendLine(handle, request.dump()) || !readLine(handle, buffer, line)) {
            std::cerr << "Błąd: uwierzytelnianie nie powiodło się\n";
            HELIX_CLOSE_SOCKET(handle);
            return 1;
        }

        const JsonValue response = JsonValue::parse(line);
        if (!response["ok"].asBool(false)) {
            std::cerr << "Błąd: " << response["error"].asString("odmowa dostępu") << '\n';
            HELIX_CLOSE_SOCKET(handle);
            return 1;
        }
    }

    if (watch) {
        std::cout << "Nasłuchiwanie zdarzeń (Ctrl+C kończy)…\n";
        while (readLine(handle, buffer, line)) {
            const JsonValue event = JsonValue::parse(line);
            if (!event["event"].isString()) continue;
            std::cout << event.dump(raw ? 0 : 2) << std::endl;
        }
        HELIX_CLOSE_SOCKET(handle);
        return 0;
    }

    JsonValue request = JsonValue::makeObject();
    request.set("id", JsonValue(1));
    request.set("cmd", JsonValue(command));
    request.set("args", args);

    if (!sendLine(handle, request.dump())) {
        std::cerr << "Błąd: nie można wysłać komendy\n";
        HELIX_CLOSE_SOCKET(handle);
        return 1;
    }

    // Zdarzenia (np. pomiary) mogą wyprzedzić odpowiedź — czekamy na właściwe id.
    int exitCode = 1;
    while (readLine(handle, buffer, line)) {
        const JsonValue response = JsonValue::parse(line);
        if (response["event"].isString()) continue;

        if (response["ok"].asBool(false)) {
            std::cout << response["result"].dump(raw ? 0 : 2) << std::endl;
            exitCode = 0;
        } else {
            std::cerr << "Błąd: " << response["error"].asString("nieznany błąd") << '\n';
            exitCode = 1;
        }
        break;
    }

    HELIX_CLOSE_SOCKET(handle);
    return exitCode;
}
