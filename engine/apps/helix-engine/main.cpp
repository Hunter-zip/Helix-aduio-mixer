// helix-engine — proces silnika audio.
//
// Chodzi bez GUI. Interfejs (Electron, CLI, cokolwiek) łączy się po TCP.
// Zamknięcie lub zawieszenie GUI nie dotyka wątku audio (spec §28).
#include <atomic>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include "helix/Log.h"
#include "helix/app/ControlServer.h"
#include "helix/app/EngineController.h"
#include "helix/app/Json.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace {

std::atomic<bool> g_running{true};

void requestShutdown(int) { g_running.store(false, std::memory_order_release); }

#if defined(_WIN32)
BOOL WINAPI consoleHandler(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        requestShutdown(0);
        return TRUE;
    }
    return FALSE;
}
#endif

void printUsage() {
    std::cout <<
        "helix-engine — silnik Helix Audio Mixer\n\n"
        "Użycie: helix-engine [opcje]\n\n"
        "  --config-dir <ścieżka>   katalog konfiguracji\n"
        "  --profile <nazwa>        profil wczytywany na starcie\n"
        "  --port <numer>           port serwera sterującego (domyślnie 47811)\n"
        "  --sample-rate <Hz>       44100 | 48000 | 96000 (domyślnie 48000)\n"
        "  --block <ramki>          rozmiar bloku (domyślnie 192 = 4 ms @48 kHz)\n"
        "  --channels <n>           liczba kanałów toru (domyślnie 2)\n"
        "  --exclusive              tryb wyłączny urządzenia (niższa latencja)\n"
        "  --null-backend           wymuś backend programowy (diagnostyka)\n"
        "  --no-hotkeys             nie rejestruj globalnych skrótów\n"
        "  --no-virtual             nie uruchamiaj wirtualnych urządzeń\n"
        "  --no-auth                serwer bez tokenu (tylko do testów lokalnych)\n"
        "  --log-level <poziom>     trace|debug|info|warn|error\n"
        "  --log-file <ścieżka>     dodatkowy zapis logu do pliku\n"
        "  -h, --help               ta pomoc\n";
}

bool nextArgument(int argc, char** argv, int& index, std::string& value) {
    if (index + 1 >= argc) return false;
    value = argv[++index];
    return true;
}

} // namespace

int main(int argc, char** argv) {
    using namespace helix;

    app::EngineController::Options options;
    app::ControlServer::Options serverOptions;
    bool useAuthentication = true;
    std::string logFile;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        std::string value;

        if (argument == "-h" || argument == "--help") { printUsage(); return 0; }
        else if (argument == "--config-dir" && nextArgument(argc, argv, i, value)) options.configDirectory = value;
        else if (argument == "--profile" && nextArgument(argc, argv, i, value)) options.startupProfile = value;
        else if (argument == "--port" && nextArgument(argc, argv, i, value)) serverOptions.port = std::stoi(value);
        else if (argument == "--sample-rate" && nextArgument(argc, argv, i, value)) options.sampleRate = std::stod(value);
        else if (argument == "--block" && nextArgument(argc, argv, i, value)) options.blockFrames = std::stoi(value);
        else if (argument == "--channels" && nextArgument(argc, argv, i, value)) options.channels = std::stoi(value);
        else if (argument == "--exclusive") options.exclusive = true;
        else if (argument == "--null-backend") options.forceNullBackend = true;
        else if (argument == "--no-hotkeys") options.enableHotkeys = false;
        else if (argument == "--no-virtual") options.enableVirtualDevices = false;
        else if (argument == "--no-auth") useAuthentication = false;
        else if (argument == "--log-file" && nextArgument(argc, argv, i, value)) logFile = value;
        else if (argument == "--log-level" && nextArgument(argc, argv, i, value)) {
            LogLevel level = LogLevel::Info;
            if (!Log::parseLevel(value, level)) {
                std::cerr << "Nieznany poziom logowania: " << value << '\n';
                return 2;
            }
            Log::setLevel(level);
        } else {
            std::cerr << "Nieznany argument: " << argument << "\n\n";
            printUsage();
            return 2;
        }
    }

    if (!logFile.empty() && !Log::setFile(logFile))
        std::cerr << "Ostrzeżenie: nie można otworzyć pliku logu " << logFile << '\n';

    std::signal(SIGINT, requestShutdown);
    std::signal(SIGTERM, requestShutdown);
#if defined(_WIN32)
    SetConsoleCtrlHandler(consoleHandler, TRUE);
#endif

    app::EngineController controller;
    const Status initialized = controller.initialize(options);
    if (!initialized) {
        std::cerr << "Błąd startu: " << initialized.message << '\n';
        return 1;
    }

    namespace fs = std::filesystem;
    if (useAuthentication)
        serverOptions.tokenPath = (fs::path(controller.configDirectory()) / "control-token").string();

    app::ControlServer server(controller);
    const Status served = server.start(serverOptions);
    if (!served) {
        std::cerr << "Błąd serwera sterującego: " << served.message << '\n';
        controller.shutdown();
        return 1;
    }

    // Plik z adresem serwera — GUI nie musi zgadywać portu ani tokenu.
    {
        app::JsonValue endpoint = app::JsonValue::makeObject();
        endpoint.set("address", app::JsonValue(serverOptions.address));
        endpoint.set("port", app::JsonValue(server.port()));
        endpoint.set("token", app::JsonValue(server.token()));
        endpoint.set("protocol", app::JsonValue(1));

        std::string error;
        const std::string path = (fs::path(controller.configDirectory()) / "control-endpoint.json").string();
        if (!app::writeJsonFile(path, endpoint, error))
            Log::warn("Engine", "Nie zapisano pliku endpointu: " + error);
    }

    Log::info("Engine", "Gotowy. Ctrl+C kończy pracę.");

    // Pętla sterująca: 20 Hz wystarcza na sprzątanie pamięci, wykrywanie
    // aplikacji i hot-plug. Wątek audio pracuje niezależnie.
    constexpr auto kTickPeriod = std::chrono::milliseconds(50);
    auto next = std::chrono::steady_clock::now();

    while (g_running.load(std::memory_order_acquire)) {
        controller.tick();
        next += kTickPeriod;
        const auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_for(next - now);
        else next = now;
    }

    Log::info("Engine", "Zamykanie…");
    server.stop();
    controller.shutdown();
    return 0;
}
