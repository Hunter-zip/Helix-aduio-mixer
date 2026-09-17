#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

#include "TestFramework.h"
#include "helix/app/AppDetection.h"
#include "helix/app/AutomationEngine.h"
#include "helix/app/ControlServer.h"
#include "helix/app/EngineController.h"
#include "helix/app/HotkeyManager.h"
#include "helix/app/ProfileManager.h"

using namespace helix;
using namespace helix::app;

namespace {

std::string scratchDirectory(const std::string& name) {
    namespace fs = std::filesystem;
    const auto directory = fs::temp_directory_path() / ("helix-test-" + name);
    fs::remove_all(directory);
    fs::create_directories(directory);
    return directory.string();
}

EngineController::Options testOptions(const std::string& directory) {
    EngineController::Options options;
    options.configDirectory = directory;
    options.forceNullBackend = true;
    options.forceManualAppDetector = true;   // na Windows domyślny jest detektor WASAPI
    options.createDefaultProfiles = false;
    options.autoStart = false;
    options.enableHotkeys = false;
    options.enableVirtualDevices = false;
    options.blockFrames = 128;
    return options;
}

} // namespace

// ── Skróty klawiszowe ───────────────────────────────────────────────────────

TEST("hotkey/parsowanie kombinacji") {
    std::uint32_t modifiers = 0;
    std::uint32_t key = 0;

    CHECK(HotkeyManager::parseCombo("Ctrl+Alt+M", modifiers, key));
    CHECK_EQ(modifiers, static_cast<std::uint32_t>(kModCtrl | kModAlt));
    CHECK_EQ(key, static_cast<std::uint32_t>('M'));

    CHECK(HotkeyManager::parseCombo("ctrl+alt+1", modifiers, key));
    CHECK_EQ(key, static_cast<std::uint32_t>('1'));

    CHECK(HotkeyManager::parseCombo("Shift+F5", modifiers, key));
    CHECK_EQ(modifiers, static_cast<std::uint32_t>(kModShift));
    CHECK_EQ(key, 0x74u);

    CHECK(HotkeyManager::parseCombo("Win+Space", modifiers, key));
    CHECK_EQ(modifiers, static_cast<std::uint32_t>(kModWin));

    CHECK(!HotkeyManager::parseCombo("Ctrl+Alt", modifiers, key));
    CHECK(!HotkeyManager::parseCombo("Ctrl+Nieistnieje", modifiers, key));
    CHECK(!HotkeyManager::parseCombo("", modifiers, key));
    CHECK(!HotkeyManager::parseCombo("M+Ctrl", modifiers, key));
}

TEST("hotkey/format kombinacji wraca do tej samej postaci") {
    std::uint32_t modifiers = 0;
    std::uint32_t key = 0;
    CHECK(HotkeyManager::parseCombo("Ctrl+Alt+M", modifiers, key));
    CHECK_STR_EQ(HotkeyManager::formatCombo(modifiers, key), "Ctrl+Alt+M");
}

TEST("hotkey/wyzwolenie uruchamia akcję") {
    HotkeyManager manager;
    CHECK(manager.start().ok);

    std::string executed;
    manager.setDispatcher([&](const HotkeyBinding& binding) { executed = binding.action; });

    HotkeyBinding binding;
    binding.id = "test";
    binding.combo = "Ctrl+Alt+T";
    binding.action = "channel.toggleMute";
    CHECK(manager.bind(binding).ok);

    CHECK(manager.trigger("test"));
    CHECK_STR_EQ(executed, "channel.toggleMute");

    CHECK(!manager.trigger("nie-ma"));
    CHECK(manager.unbind("test"));
    CHECK(!manager.trigger("test"));
    manager.stop();
}

TEST("hotkey/domyślne skróty zgodne ze specyfikacją") {
    const auto bindings = HotkeyManager::defaultBindings();
    CHECK_EQ(bindings.size(), std::size_t{5});
    CHECK_STR_EQ(bindings[0].combo, "Ctrl+Alt+M");
    CHECK_STR_EQ(bindings[0].args["channel"].asString(), "Microphone");
    CHECK_STR_EQ(bindings[3].action, "profile.load");
}

// ── Automatyzacja ───────────────────────────────────────────────────────────

TEST("automation/warunek dopasowuje kontekst bez względu na wielkość liter") {
    JsonValue condition = JsonValue::makeObject();
    condition.set("executable", JsonValue("Discord.exe"));

    JsonValue context = JsonValue::makeObject();
    context.set("executable", JsonValue("discord.EXE"));
    context.set("processId", JsonValue(1234));

    CHECK(AutomationEngine::matches(condition, context));

    context.set("executable", JsonValue("spotify.exe"));
    CHECK(!AutomationEngine::matches(condition, context));

    CHECK(AutomationEngine::matches(JsonValue::makeObject(), context));
}

TEST("automation/reguła uruchamia akcję i dostaje kontekst") {
    AutomationEngine automation;

    std::string firedAction;
    JsonValue firedArgs;
    automation.setExecutor([&](const std::string& action, const JsonValue& args) {
        firedAction = action;
        firedArgs = args;
        return Status::success();
    });

    AutomationRule rule;
    rule.id = "discord";
    rule.trigger = TriggerType::AppStarted;
    rule.condition = JsonValue::makeObject();
    rule.condition.set("executable", JsonValue("Discord.exe"));

    AutomationAction action;
    action.action = "app.assign";
    action.args = JsonValue::makeObject();
    action.args.set("channel", JsonValue("Chat"));
    rule.actions.push_back(action);

    CHECK(automation.addRule(rule).ok);

    JsonValue context = JsonValue::makeObject();
    context.set("executable", JsonValue("Discord.exe"));
    context.set("processId", JsonValue(4242));

    CHECK_EQ(automation.fire(TriggerType::AppStarted, context), 1);
    CHECK_STR_EQ(firedAction, "app.assign");
    CHECK_STR_EQ(firedArgs["channel"].asString(), "Chat");
    CHECK_EQ(firedArgs["processId"].asInt(), 4242);   // kontekst uzupełnia argumenty

    // Inny wyzwalacz nic nie robi.
    CHECK_EQ(automation.fire(TriggerType::AppStopped, context), 0);

    // Wyłączona reguła też nie.
    CHECK(automation.setRuleEnabled("discord", false));
    CHECK_EQ(automation.fire(TriggerType::AppStarted, context), 0);
}

TEST("automation/serializacja reguł w obie strony") {
    const auto rules = AutomationEngine::defaultRules();
    CHECK(rules.size() >= 4);

    for (const auto& rule : rules) {
        const JsonValue json = rule.toJson();
        AutomationRule restored;
        std::string error;
        CHECK_MSG(AutomationRule::fromJson(json, restored, error), error);
        CHECK_STR_EQ(restored.id, rule.id);
        CHECK_EQ(static_cast<int>(restored.trigger), static_cast<int>(rule.trigger));
        CHECK_EQ(restored.actions.size(), rule.actions.size());
    }

    AutomationRule broken;
    std::string error;
    CHECK(!AutomationRule::fromJson(JsonValue::parse(R"({"name":"bez id"})"), broken, error));
    CHECK(!error.empty());
    CHECK(!AutomationRule::fromJson(JsonValue::parse(R"({"id":"a","trigger":"nieznany"})"), broken, error));
}

// ── Przypisania aplikacji ───────────────────────────────────────────────────

TEST("app/przypisania normalizują nazwę pliku") {
    AppAssignments assignments;
    assignments.set("C:\\Program Files\\Discord\\Discord.exe", "Chat");

    CHECK_STR_EQ(assignments.lookup("discord.exe"), "Chat");
    CHECK_STR_EQ(assignments.lookup("/usr/bin/DISCORD.EXE"), "Chat");
    CHECK_STR_EQ(assignments.lookup("spotify.exe"), "");

    CHECK(assignments.remove("Discord.exe"));
    CHECK_STR_EQ(assignments.lookup("discord.exe"), "");
}

TEST("app/detektor zgłasza pojawienie się i zniknięcie aplikacji") {
    ManualAppDetector detector;

    std::vector<std::pair<std::string, bool>> events;
    detector.setHandler([&](const AudioApplication& application, bool appeared) {
        events.emplace_back(application.executable, appeared);
    });

    AudioApplication discord;
    discord.processId = 100;
    discord.executable = "Discord.exe";
    detector.simulateStart(discord);
    detector.poll();

    CHECK_EQ(events.size(), std::size_t{1});
    CHECK_STR_EQ(events[0].first, "Discord.exe");
    CHECK(events[0].second);

    detector.poll();   // bez zmian — brak nowych zdarzeń
    CHECK_EQ(events.size(), std::size_t{1});

    detector.simulateStop(100);
    detector.poll();
    CHECK_EQ(events.size(), std::size_t{2});
    CHECK(!events[1].second);
    CHECK(detector.known().empty());
}

// ── Profile ─────────────────────────────────────────────────────────────────

TEST("profile/zapis, odczyt i lista") {
    const std::string directory = scratchDirectory("profiles");

    JsonValue stored = JsonValue::makeObject();
    stored.set("masterVolume", JsonValue(0.6f));

    JsonValue applied;
    ProfileManager manager(
        directory,
        [&] { return stored; },
        [&](const JsonValue& document) { applied = document; return Status::success(); });

    CHECK(manager.save("Gaming", "profil testowy").ok);
    CHECK(manager.exists("Gaming"));
    CHECK_STR_EQ(manager.activeProfile(), "Gaming");

    const auto profiles = manager.list();
    CHECK_EQ(profiles.size(), std::size_t{1});
    CHECK_STR_EQ(profiles[0].name, "Gaming");
    CHECK_STR_EQ(profiles[0].description, "profil testowy");

    CHECK(manager.load("Gaming").ok);
    CHECK_NEAR(applied["masterVolume"].asNumber(), 0.6, 1e-6);
    CHECK_EQ(applied["schemaVersion"].asInt(), kConfigSchemaVersion);

    CHECK(manager.duplicate("Gaming", "Kopia").ok);
    CHECK(manager.exists("Kopia"));

    CHECK(manager.rename("Kopia", "Streaming").ok);
    CHECK(!manager.exists("Kopia"));
    CHECK(manager.exists("Streaming"));

    CHECK(manager.remove("Streaming").ok);
    CHECK(!manager.exists("Streaming"));
    CHECK(!manager.remove("Nie-ma").ok);

    std::filesystem::remove_all(directory);
}

TEST("profile/nazwy z nieprawidłowymi znakami są odrzucane") {
    CHECK(ProfileManager::isValidName("Gaming"));
    CHECK(ProfileManager::isValidName("Mój profil 2"));
    CHECK(!ProfileManager::isValidName(""));
    CHECK(!ProfileManager::isValidName("a/b"));
    CHECK(!ProfileManager::isValidName("a\\b"));
    CHECK(!ProfileManager::isValidName("C:profil"));
    CHECK(!ProfileManager::isValidName(std::string(200, 'x')));
}

TEST("profile/migracja schematu v1 → v2") {
    JsonValue document = JsonValue::parse(R"({
        "schemaVersion": 1,
        "profile": "Stary",
        "masterVolume": 0.85,
        "channels": [
            {"name": "Game", "volume": 0.8, "muted": false, "outputs": ["A1", "B1"]},
            {"name": "Chat", "volume": 0.6, "muted": false, "outputs": ["A1"]}
        ]
    })");

    std::string message;
    CHECK_MSG(ProfileManager::migrate(document, message), message);
    CHECK_EQ(document["schemaVersion"].asInt(), kConfigSchemaVersion);
    CHECK(!message.empty());

    const JsonValue& routing = document["routing"];
    CHECK_EQ(routing.size(), std::size_t{3});
    CHECK_STR_EQ(routing[0]["channel"].asString(), "Game");
    CHECK_STR_EQ(routing[0]["bus"].asString(), "A1");
    CHECK(routing[0]["enabled"].asBool());
}

TEST("profile/profil z przyszłej wersji jest odrzucany") {
    JsonValue document = JsonValue::makeObject();
    document.set("schemaVersion", JsonValue(kConfigSchemaVersion + 5));

    std::string message;
    CHECK(!ProfileManager::migrate(document, message));
    CHECK(message.find("nowszej") != std::string::npos);
}

// ── Kontroler ───────────────────────────────────────────────────────────────

TEST("controller/start, migawka i podstawowe komendy") {
    const std::string directory = scratchDirectory("controller");

    EngineController controller;
    const Status initialized = controller.initialize(testOptions(directory));
    CHECK_MSG(initialized.ok, initialized.message);

    const JsonValue snapshot = controller.snapshot();
    CHECK_EQ(snapshot["channels"].size(), std::size_t{8});
    CHECK_EQ(snapshot["buses"].size(), std::size_t{4});
    CHECK_STR_EQ(snapshot["channels"][0]["name"].asString(), "Game");
    CHECK_STR_EQ(snapshot["buses"][0]["label"].asString(), "A1");
    CHECK(snapshot["plugins"].size() >= 7);

    Status status;
    JsonValue args = JsonValue::makeObject();
    args.set("channel", JsonValue("Game"));
    args.set("value", JsonValue(0.42));

    const JsonValue result = controller.dispatch("channel.setVolume", args, status);
    CHECK_MSG(status.ok, status.message);
    CHECK_NEAR(result["volume"].asNumber(), 0.42, 1e-6);

    // Nieznana komenda musi zgłosić błąd, a nie wywrócić proces.
    controller.dispatch("nie.ma.takiej", args, status);
    CHECK(!status.ok);

    // Nieznany kanał również.
    args.set("channel", JsonValue("Nieistniejący"));
    controller.dispatch("channel.setVolume", args, status);
    CHECK(!status.ok);

    controller.shutdown();
    std::filesystem::remove_all(directory);
}

TEST("controller/routing przez komendy") {
    const std::string directory = scratchDirectory("routing");

    EngineController controller;
    CHECK(controller.initialize(testOptions(directory)).ok);

    Status status;
    JsonValue args = JsonValue::makeObject();
    args.set("channel", JsonValue("Music"));
    args.set("bus", JsonValue("B2"));
    args.set("value", JsonValue(true));
    controller.dispatch("routing.set", args, status);
    CHECK_MSG(status.ok, status.message);

    const JsonValue routing = controller.dispatch("routing.get", JsonValue::makeObject(), status);
    bool found = false;
    for (std::size_t i = 0; i < routing.size(); ++i) {
        if (routing[i]["name"].asString() != "Music") continue;
        const JsonValue& buses = routing[i]["buses"];
        for (std::size_t b = 0; b < buses.size(); ++b)
            if (buses[b]["label"].asString() == "B2" && buses[b]["enabled"].asBool()) found = true;
    }
    CHECK(found);

    controller.shutdown();
    std::filesystem::remove_all(directory);
}

TEST("controller/efekty: dodanie, parametr, kolejność, usunięcie") {
    const std::string directory = scratchDirectory("effects");

    EngineController controller;
    CHECK(controller.initialize(testOptions(directory)).ok);

    Status status;
    JsonValue args = JsonValue::makeObject();
    args.set("channel", JsonValue("Microphone"));

    const JsonValue effects = controller.dispatch("effect.list", args, status);
    CHECK_MSG(status.ok, status.message);
    CHECK_EQ(effects.size(), std::size_t{7});

    // Włącz EQ i ustaw pasmo.
    PluginId equalizerId = 0;
    for (std::size_t i = 0; i < effects.size(); ++i)
        if (effects[i]["type"].asString() == "equalizer") equalizerId = effects[i]["id"].asUint();
    CHECK(equalizerId != 0);

    args.set("effect", JsonValue(equalizerId));
    args.set("value", JsonValue(true));
    controller.dispatch("effect.setEnabled", args, status);
    CHECK(status.ok);

    args.set("param", JsonValue("band4.gain"));
    args.set("value", JsonValue(6.0));
    controller.dispatch("effect.setParam", args, status);
    CHECK(status.ok);

    args.set("param", JsonValue("band4.enabled"));
    args.set("value", JsonValue(1.0));
    controller.dispatch("effect.setParam", args, status);
    CHECK(status.ok);

    // Charakterystyka EQ dla wykresu.
    JsonValue responseArgs = JsonValue::makeObject();
    responseArgs.set("channel", JsonValue("Microphone"));
    responseArgs.set("effect", JsonValue(equalizerId));
    responseArgs.set("points", JsonValue(32));
    const JsonValue response = controller.dispatch("effect.eqResponse", responseArgs, status);
    CHECK_MSG(status.ok, status.message);
    CHECK_EQ(response["gainDb"].size(), std::size_t{32});

    // Nieznany parametr → błąd.
    args.set("param", JsonValue("nie.ma"));
    controller.dispatch("effect.setParam", args, status);
    CHECK(!status.ok);

    // Usunięcie efektu.
    JsonValue removeArgs = JsonValue::makeObject();
    removeArgs.set("channel", JsonValue("Microphone"));
    removeArgs.set("effect", JsonValue(equalizerId));
    const JsonValue after = controller.dispatch("effect.remove", removeArgs, status);
    CHECK_MSG(status.ok, status.message);
    CHECK_EQ(after.size(), std::size_t{6});

    controller.shutdown();
    std::filesystem::remove_all(directory);
}

TEST("controller/profil zapisuje i odtwarza pełną konfigurację") {
    const std::string directory = scratchDirectory("profile-roundtrip");

    EngineController controller;
    CHECK(controller.initialize(testOptions(directory)).ok);

    Status status;
    JsonValue args = JsonValue::makeObject();
    args.set("channel", JsonValue("Game"));
    args.set("value", JsonValue(0.33));
    controller.dispatch("channel.setVolume", args, status);
    CHECK(status.ok);

    args.set("value", JsonValue(true));
    controller.dispatch("channel.setMute", args, status);
    CHECK(status.ok);

    JsonValue routingArgs = JsonValue::makeObject();
    routingArgs.set("channel", JsonValue("Game"));
    routingArgs.set("bus", JsonValue("B2"));
    routingArgs.set("value", JsonValue(true));
    controller.dispatch("routing.set", routingArgs, status);
    CHECK(status.ok);

    JsonValue saveArgs = JsonValue::makeObject();
    saveArgs.set("name", JsonValue("Test"));
    controller.dispatch("profile.save", saveArgs, status);
    CHECK_MSG(status.ok, status.message);

    // Zmieniamy stan…
    args.set("value", JsonValue(0.9));
    controller.dispatch("channel.setVolume", args, status);
    args.set("value", JsonValue(false));
    controller.dispatch("channel.setMute", args, status);
    routingArgs.set("value", JsonValue(false));
    controller.dispatch("routing.set", routingArgs, status);

    // …i wczytujemy profil z powrotem.
    controller.dispatch("profile.load", saveArgs, status);
    CHECK_MSG(status.ok, status.message);

    const JsonValue snapshot = controller.snapshot();
    bool checked = false;
    for (std::size_t i = 0; i < snapshot["channels"].size(); ++i) {
        const JsonValue& channel = snapshot["channels"][i];
        if (channel["name"].asString() != "Game") continue;
        CHECK_NEAR(channel["volume"].asNumber(), 0.33, 1e-5);
        CHECK(channel["muted"].asBool());

        bool routedToB2 = false;
        for (std::size_t o = 0; o < channel["outputs"].size(); ++o)
            if (channel["outputs"][o]["label"].asString() == "B2") routedToB2 = true;
        CHECK(routedToB2);
        checked = true;
    }
    CHECK(checked);
    CHECK_STR_EQ(snapshot["profile"].asString(), "Test");

    controller.shutdown();
    std::filesystem::remove_all(directory);
}

TEST("controller/przypisanie aplikacji do kanału") {
    const std::string directory = scratchDirectory("assign");

    EngineController controller;
    CHECK(controller.initialize(testOptions(directory)).ok);

    Status status;
    JsonValue args = JsonValue::makeObject();
    args.set("executable", JsonValue("Discord.exe"));
    args.set("channel", JsonValue("Chat"));
    controller.dispatch("app.assign", args, status);
    CHECK_MSG(status.ok, status.message);

    const JsonValue assignments = controller.dispatch("app.assignments", JsonValue::makeObject(), status);
    CHECK_STR_EQ(assignments["discord.exe"].asString(), "Chat");

    // Powstało źródło typu „application” przypisane do kanału Chat.
    const JsonValue sources = controller.dispatch("source.list", JsonValue::makeObject(), status);
    bool found = false;
    for (std::size_t i = 0; i < sources.size(); ++i)
        if (sources[i]["kind"].asString() == "application" &&
            sources[i]["processName"].asString() == "Discord.exe") found = true;
    CHECK(found);

    controller.dispatch("app.unassign", args, status);
    CHECK(status.ok);

    controller.shutdown();
    std::filesystem::remove_all(directory);
}

TEST("controller/automatyzacja przypisuje aplikację po starcie") {
    const std::string directory = scratchDirectory("automation-flow");

    EngineController controller;
    CHECK(controller.initialize(testOptions(directory)).ok);

    auto* detector = dynamic_cast<ManualAppDetector*>(&controller.appDetector());
    CHECK_MSG(detector != nullptr, "oczekiwano detektora sterowanego ręcznie");

    AudioApplication spotify;
    spotify.processId = 777;
    spotify.executable = "Spotify.exe";
    detector->simulateStart(spotify);
    detector->poll();   // uruchamia regułę "Spotify → Music"

    Status status;
    const JsonValue assignments = controller.dispatch("app.assignments", JsonValue::makeObject(), status);
    CHECK_STR_EQ(assignments["spotify.exe"].asString(), "Music");

    controller.shutdown();
    std::filesystem::remove_all(directory);
}

// ── Serwer sterujący ────────────────────────────────────────────────────────

TEST("control/protokół: uwierzytelnianie i komendy") {
    const std::string directory = scratchDirectory("control");

    EngineController controller;
    CHECK(controller.initialize(testOptions(directory)).ok);

    ControlServer server(controller);
    ControlServer::Options options;
    options.port = 47931;
    options.meterHz = 0;   // bez wątku pomiarowego w teście
    options.tokenPath = (std::filesystem::path(directory) / "token").string();
    CHECK_MSG(server.start(options).ok, "serwer nie wystartował");
    CHECK(!server.token().empty());

    bool authenticated = false;

    // Bez tokenu komenda musi zostać odrzucona.
    JsonValue response = JsonValue::parse(
        server.handleLine(R"({"id":1,"cmd":"engine.status"})", authenticated));
    CHECK(!response["ok"].asBool(true));
    CHECK(response["error"].asString().find("uwierzytelnienie") != std::string::npos);

    // Zły token też.
    response = JsonValue::parse(
        server.handleLine(R"({"id":2,"cmd":"auth","args":{"token":"zly"}})", authenticated));
    CHECK(!response["ok"].asBool(true));
    CHECK(!authenticated);

    // Poprawny token otwiera sesję.
    JsonValue request = JsonValue::makeObject();
    request.set("id", JsonValue(3));
    request.set("cmd", JsonValue("auth"));
    JsonValue authArgs = JsonValue::makeObject();
    authArgs.set("token", JsonValue(server.token()));
    request.set("args", std::move(authArgs));

    response = JsonValue::parse(server.handleLine(request.dump(), authenticated));
    CHECK(response["ok"].asBool(false));
    CHECK(authenticated);
    CHECK(response["result"]["channels"].size() > 0);

    // Teraz komenda przechodzi.
    response = JsonValue::parse(
        server.handleLine(R"({"id":4,"cmd":"channel.setVolume","args":{"channel":"Chat","value":0.25}})",
                          authenticated));
    CHECK_MSG(response["ok"].asBool(false), response["error"].asString());
    CHECK_NEAR(response["result"]["volume"].asNumber(), 0.25, 1e-6);
    CHECK_EQ(response["id"].asInt(), 4);

    // Uszkodzony JSON nie wywraca serwera.
    response = JsonValue::parse(server.handleLine("{to nie jest json", authenticated));
    CHECK(!response["ok"].asBool(true));

    // Brak pola cmd.
    response = JsonValue::parse(server.handleLine(R"({"id":5})", authenticated));
    CHECK(!response["ok"].asBool(true));

    server.stop();
    controller.shutdown();
    std::filesystem::remove_all(directory);
}

TEST("control/pomiary mają strukturę oczekiwaną przez GUI") {
    const std::string directory = scratchDirectory("meters");

    EngineController controller;
    CHECK(controller.initialize(testOptions(directory)).ok);

    const JsonValue meters = controller.meters();
    CHECK(meters["channels"].size() > 0);
    CHECK(meters["buses"].size() > 0);
    CHECK(meters["master"].isObject());
    CHECK(meters["channels"][0]["output"].contains("peakL"));
    CHECK(meters["channels"][0]["output"].contains("rmsR"));
    CHECK(meters["master"].contains("clip"));

    controller.shutdown();
    std::filesystem::remove_all(directory);
}

TEST("controller/równoległe komendy z wielu wątków") {
    const std::string directory = scratchDirectory("concurrent");

    EngineController controller;
    CHECK(controller.initialize(testOptions(directory)).ok);

    // Komendy przychodzą z wątków klientów serwera, z wątku skrótów i z
    // automatyzacji — dostęp musi być serializowany, a stan spójny.
    std::atomic<bool> stop{false};
    std::atomic<int>  errors{0};

    std::vector<std::thread> workers;
    for (int w = 0; w < 4; ++w) {
        workers.emplace_back([&controller, &stop, &errors, w] {
            int counter = 0;
            while (!stop.load(std::memory_order_acquire)) {
                Status status;
                JsonValue args = JsonValue::makeObject();

                switch ((counter + w) % 5) {
                    case 0:
                        args.set("channel", JsonValue("Game"));
                        args.set("value", JsonValue(0.2 + 0.1 * (counter % 5)));
                        controller.dispatch("channel.setVolume", args, status);
                        break;
                    case 1:
                        args.set("channel", JsonValue("Chat"));
                        controller.dispatch("channel.toggleMute", args, status);
                        break;
                    case 2:
                        args.set("channel", JsonValue("Music"));
                        args.set("bus", JsonValue("B1"));
                        args.set("value", JsonValue((counter % 2) == 0));
                        controller.dispatch("routing.set", args, status);
                        break;
                    case 3:
                        (void)controller.snapshot();
                        break;
                    default:
                        (void)controller.meters();
                        break;
                }

                if (!status) errors.fetch_add(1, std::memory_order_relaxed);
                ++counter;
            }
        });
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    stop.store(true, std::memory_order_release);
    for (auto& worker : workers) worker.join();

    CHECK_EQ(errors.load(), 0);

    // Stan po burzy musi nadal być spójny.
    const JsonValue snapshot = controller.snapshot();
    CHECK_EQ(snapshot["channels"].size(), std::size_t{8});

    controller.shutdown();
    std::filesystem::remove_all(directory);
}

TEST("control/lista komend jest kompletna i bez duplikatów") {
    const auto commands = EngineController::commandNames();
    CHECK(commands.size() > 60);

    std::vector<std::string> sorted = commands;
    std::sort(sorted.begin(), sorted.end());
    CHECK(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end());

    for (const char* required : {"channel.setVolume", "routing.set", "profile.load",
                                 "master.setVolume", "effect.setParam", "virtual.list",
                                 "device.test", "hotkey.bind", "automation.add"}) {
        CHECK_MSG(std::find(commands.begin(), commands.end(), required) != commands.end(),
                  std::string("brak komendy: ") + required);
    }
}
