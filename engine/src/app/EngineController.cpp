#include "helix/app/EngineController.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>

#include "helix/Log.h"
#include "helix/core/NullBackend.h"
#include "helix/dsp/PluginRegistry.h"

namespace helix::app {

namespace {
constexpr const char* kLog = "Controller";

/// Domyślne magistrale: A = wyjścia fizyczne, B = wyjścia wirtualne (spec §6).
struct BusSpec {
    const char* label;
    const char* name;
    BusKind kind;
};

const std::vector<BusSpec>& defaultBusLayout() {
    static const std::vector<BusSpec> kBuses{
        {"A1", "Headphones",    BusKind::Physical},
        {"A2", "Speakers",      BusKind::Physical},
        {"B1", "Stream Output", BusKind::Virtual},
        {"B2", "Chat Output",   BusKind::Virtual},
    };
    return kBuses;
}

} // namespace

EngineController::EngineController() = default;

EngineController::~EngineController() { shutdown(); }

std::string EngineController::defaultConfigDirectory() {
    namespace fs = std::filesystem;
#if defined(_WIN32)
    if (const char* appData = std::getenv("APPDATA"))
        return (fs::path(appData) / "Helix" / "AudioMixer").string();
    return (fs::path(".") / "helix-config").string();
#else
    if (const char* configHome = std::getenv("XDG_CONFIG_HOME"))
        return (fs::path(configHome) / "helix-audio-mixer").string();
    if (const char* home = std::getenv("HOME"))
        return (fs::path(home) / ".config" / "helix-audio-mixer").string();
    return (fs::path(".") / "helix-config").string();
#endif
}

Status EngineController::initialize(Options options) {
    if (initialized_.load(std::memory_order_acquire))
        return Status::error("Kontroler jest już zainicjalizowany");

    options_ = std::move(options);
    configDirectory_ = options_.configDirectory.empty() ? defaultConfigDirectory()
                                                        : options_.configDirectory;

    std::error_code code;
    std::filesystem::create_directories(configDirectory_, code);
    if (code)
        return Status::error("Nie można utworzyć katalogu konfiguracji: " + code.message());

    backend_ = options_.forceNullBackend ? core::createNullBackend(true)
                                         : core::createPlatformBackend();
    engine_   = std::make_unique<core::AudioEngine>();
    devices_  = std::make_unique<core::DeviceManager>(*engine_, *backend_);
    channels_ = std::make_unique<core::ChannelManager>(*engine_);
    virtualDevices_ = std::make_unique<virtualaudio::VirtualDeviceManager>(
        *engine_, virtualaudio::createPlatformDriverInterface());

    profiles_ = std::make_unique<ProfileManager>(
        (std::filesystem::path(configDirectory_) / "profiles").string(),
        [this] { return captureProfile(); },
        [this](const JsonValue& document) { return applyProfile(document); });

    hotkeys_     = std::make_unique<HotkeyManager>();
    appDetector_ = options_.forceManualAppDetector ? std::make_unique<ManualAppDetector>()
                                                   : createPlatformAppDetector();

    core::EngineFormat format;
    format.sampleRate  = options_.sampleRate;
    format.blockFrames = options_.blockFrames;
    format.channels    = options_.channels;

    const Status configured = engine_->configure(format);
    if (!configured) return configured;

    const Status backendStatus = devices_->initialize();
    if (!backendStatus) {
        Log::error(kLog, "Backend audio nie wystartował: " + backendStatus.message);
        return backendStatus;
    }

    core::DeviceSettings settings;
    settings.sampleRate  = options_.sampleRate;
    settings.blockFrames = options_.blockFrames;
    settings.channels    = options_.channels;
    settings.exclusive   = options_.exclusive;
    const Status settingsStatus = devices_->applySettings(settings);
    if (!settingsStatus) return settingsStatus;

    buildDefaultLayout();
    wireAutomation();

    if (options_.enableVirtualDevices) {
        virtualDevices_->createDefaultEndpoints();
        const Status virtualStatus = virtualDevices_->start(options_.sampleRate, options_.blockFrames);
        if (!virtualStatus)
            Log::warn(kLog, "Transport wirtualny: " + virtualStatus.message);
    }

    if (options_.enableHotkeys) {
        for (auto& binding : HotkeyManager::defaultBindings()) hotkeys_->bind(std::move(binding));
        const Status hotkeyStatus = hotkeys_->start();
        if (!hotkeyStatus) Log::warn(kLog, "Skróty klawiszowe: " + hotkeyStatus.message);
    }

    appDetector_->setHandler([this](const AudioApplication& app, bool appeared) {
        onAppEvent(app, appeared);
    });
    const Status detectorStatus = appDetector_->start();
    if (!detectorStatus) Log::warn(kLog, "Wykrywanie aplikacji: " + detectorStatus.message);

    if (options_.createDefaultProfiles) {
        const Status defaults = profiles_->createDefaults();
        if (!defaults) Log::warn(kLog, defaults.message);
    }

    if (!options_.startupProfile.empty()) {
        const Status loaded = profiles_->load(options_.startupProfile);
        if (!loaded) Log::warn(kLog, "Profil startowy: " + loaded.message);
    }

    if (options_.autoStart) {
        const Status started = devices_->start();
        if (!started) Log::warn(kLog, "Start urządzeń: " + started.message);
        automation_.fire(TriggerType::EngineStarted, JsonValue::makeObject());
    }

    initialized_.store(true, std::memory_order_release);
    Log::info(kLog, "Helix Audio Mixer gotowy (konfiguracja: " + configDirectory_ + ")");
    return Status::success();
}

void EngineController::shutdown() {
    if (!initialized_.exchange(false, std::memory_order_acq_rel)) return;

    if (hotkeys_)        hotkeys_->stop();
    if (appDetector_)    appDetector_->stop();
    if (virtualDevices_) virtualDevices_->stop();
    if (devices_)        devices_->shutdown();

    if (engine_) {
        engine_->setRunning(false);
        engine_->collectGarbage();
    }

    virtualDevices_.reset();
    profiles_.reset();
    channels_.reset();
    devices_.reset();
    engine_.reset();
    backend_.reset();
    hotkeys_.reset();
    appDetector_.reset();
    Log::info(kLog, "Zatrzymano");
}

void EngineController::tick() {
    if (!initialized_.load(std::memory_order_acquire)) return;

    std::lock_guard<std::recursive_mutex> lock(mutex_);
    ++tickCounter_;

    // Zwalnianie obiektów odstawionych przez wątek audio — zawsze.
    engine_->collectGarbage();

    // Wykrywanie aplikacji co ~0,5 s przy tickach 20 Hz.
    if (tickCounter_ % 10 == 0 && appDetector_) appDetector_->poll();

    // Odświeżanie powiązań urządzeń co ~2 s (siatka bezpieczeństwa dla hot-plug).
    if (tickCounter_ % 40 == 0 && devices_) devices_->refreshBindings();
}

void EngineController::buildDefaultLayout() {
    // Magistrale.
    for (const auto& spec : defaultBusLayout()) {
        core::Bus* bus = engine_->addBus(spec.name, spec.label, spec.kind);
        if (bus == nullptr) continue;
        bus->setVolume(1.0f);
        bus->setFollowsMaster(spec.kind == BusKind::Physical);
    }

    // Kanały ze standardowym łańcuchem DSP.
    channels_->createDefaultLayout();

    // Domyślne urządzenie wyjściowe dla A1.
    const auto buses = engine_->buses();
    if (!buses.empty()) {
        engine_->setPrimaryBus(buses.front()->id());
        const std::string defaultRender = devices_->defaultDevice(DeviceDirection::Render);
        if (!defaultRender.empty()) devices_->bindBus(buses.front()->id(), defaultRender);
    }

    // Źródło mikrofonu — domyślne urządzenie wejściowe systemu.
    core::InputSource* microphone = engine_->addSource(SourceKind::PhysicalInput, "Microphone");
    if (microphone != nullptr) {
        for (core::Channel* channel : engine_->channels()) {
            if (channel->name() == "Microphone") {
                engine_->assignSource(microphone->id(), channel->id());
                break;
            }
        }
        const std::string defaultCapture = devices_->defaultDevice(DeviceDirection::Capture);
        if (!defaultCapture.empty())
            devices_->bindSourceToDevice(microphone->id(), defaultCapture, false);
    }

    // Źródło dźwięku systemowego (loopback urządzenia wyjściowego).
    core::InputSource* system = engine_->addSource(SourceKind::Loopback, "System");
    if (system != nullptr) {
        for (core::Channel* channel : engine_->channels()) {
            if (channel->name() == "System") {
                engine_->assignSource(system->id(), channel->id());
                break;
            }
        }
        const std::string defaultRender = devices_->defaultDevice(DeviceDirection::Render);
        if (!defaultRender.empty())
            devices_->bindSourceToDevice(system->id(), defaultRender, true);
    }

    applyDefaultRouting();

    // Wirtualne wyjścia domyślnie wiszą na magistralach B.
    engine_->commitGraph();
}

void EngineController::applyDefaultRouting() {
    const auto channels = engine_->channels();
    const auto buses    = engine_->buses();
    if (channels.empty() || buses.empty()) return;

    core::RoutingMatrix& routing = engine_->routing();
    const int primaryIndex = buses.front()->index();

    // Każdy kanał trafia na wyjście podstawowe; mikrofon dodatkowo na magistralę
    // czatu i streamową — to układ z przykładu w specyfikacji §6.
    for (core::Channel* channel : channels) {
        routing.setEnabled(channel->index(), primaryIndex, true);

        if (channel->name() == "Microphone") {
            for (core::Bus* bus : buses)
                if (bus->kind() == BusKind::Virtual)
                    routing.setEnabled(channel->index(), bus->index(), true);
        } else if (channel->name() == "Game" || channel->name() == "Music" ||
                   channel->name() == "Media") {
            for (core::Bus* bus : buses)
                if (bus->label() == "B1") routing.setEnabled(channel->index(), bus->index(), true);
        }
    }
}

void EngineController::wireAutomation() {
    automation_.setExecutor([this](const std::string& action, const JsonValue& args) {
        Status status = Status::success();
        dispatch(action, args, status);
        return status;
    });
    automation_.setRules(AutomationEngine::defaultRules());

    hotkeys_->setDispatcher([this](const HotkeyBinding& binding) {
        Status status = Status::success();
        dispatch(binding.action, binding.args, status);
        if (!status) Log::warn(kLog, "Skrót " + binding.combo + ": " + status.message);
    });

    devices_->setChangeHandler([this](const core::DeviceChangeEvent& event) {
        JsonValue context = JsonValue::makeObject();
        context.set("deviceId", JsonValue(event.deviceId));
        context.set("direction", JsonValue(event.direction == DeviceDirection::Render ? "render" : "capture"));

        const TriggerType trigger = (event.type == core::DeviceChangeEvent::Type::Removed)
                                        ? TriggerType::DeviceDisconnected
                                        : TriggerType::DeviceConnected;
        automation_.fire(trigger, context);
    });
}

// ── Rozwiązywanie referencji ────────────────────────────────────────────────

core::Channel* EngineController::resolveChannel(const JsonValue& args, const char* key) const {
    const JsonValue& value = args[key];
    if (value.isNumber()) return engine_->findChannel(value.asUint());

    const std::string name = value.asString();
    if (name.empty()) return nullptr;
    for (core::Channel* channel : engine_->channels())
        if (channel->name() == name) return channel;
    return nullptr;
}

core::Bus* EngineController::resolveBus(const JsonValue& args, const char* key) const {
    const JsonValue& value = args[key];
    if (value.isNumber()) return engine_->findBus(value.asUint());

    const std::string name = value.asString();
    if (name.empty()) return nullptr;
    for (core::Bus* bus : engine_->buses())
        if (bus->label() == name || bus->name() == name) return bus;
    return nullptr;
}

core::InputSource* EngineController::resolveSource(const JsonValue& args, const char* key) const {
    const JsonValue& value = args[key];
    if (value.isNumber()) return engine_->findSource(value.asUint());

    const std::string name = value.asString();
    if (name.empty()) return nullptr;
    for (core::InputSource* source : engine_->sources())
        if (source->name() == name) return source;
    return nullptr;
}

// ── Serializacja stanu ──────────────────────────────────────────────────────

JsonValue EngineController::channelJson(const core::Channel& channel) const {
    JsonValue value = JsonValue::makeObject();
    value.set("id", JsonValue(channel.id()));
    value.set("index", JsonValue(channel.index()));
    value.set("name", JsonValue(channel.name()));
    value.set("icon", JsonValue(channel.icon()));
    value.set("volume", JsonValue(channel.volume()));
    value.set("gainDb", JsonValue(channel.gainDb()));
    value.set("pan", JsonValue(channel.pan()));
    value.set("muted", JsonValue(channel.muted()));
    value.set("solo", JsonValue(channel.solo()));
    value.set("enabled", JsonValue(channel.enabled()));
    value.set("latencyFrames", JsonValue(channel.latencyFrames()));

    JsonValue outputs = JsonValue::makeArray();
    for (core::Bus* bus : engine_->buses()) {
        if (!engine_->routing().isEnabled(channel.index(), bus->index())) continue;
        JsonValue entry = JsonValue::makeObject();
        entry.set("bus", JsonValue(bus->id()));
        entry.set("label", JsonValue(bus->label()));
        entry.set("gain", JsonValue(engine_->routing().sendGain(channel.index(), bus->index())));
        outputs.push(std::move(entry));
    }
    value.set("outputs", std::move(outputs));

    JsonValue sources = JsonValue::makeArray();
    for (core::InputSource* source : engine_->sources())
        if (source->channel() == channel.id()) sources.push(JsonValue(source->id()));
    value.set("sources", std::move(sources));

    value.set("effects", effectsJson(channel.id()));
    return value;
}

JsonValue EngineController::effectsJson(ChannelId channelId) const {
    JsonValue list = JsonValue::makeArray();
    for (const auto& effect : channels_->effectStates(channelId)) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue(effect.id));
        entry.set("type", JsonValue(effect.typeId));
        entry.set("name", JsonValue(effect.displayName));
        entry.set("enabled", JsonValue(effect.enabled));
        entry.set("latencyFrames", JsonValue(effect.latencyFrames));

        JsonValue parameters = JsonValue::makeArray();
        for (std::size_t i = 0; i < effect.parameters.size(); ++i) {
            const auto& descriptor = effect.parameters[i];
            JsonValue parameter = JsonValue::makeObject();
            parameter.set("id", JsonValue(descriptor.id));
            parameter.set("name", JsonValue(descriptor.name));
            parameter.set("unit", JsonValue(descriptor.unit));
            parameter.set("min", JsonValue(descriptor.minValue));
            parameter.set("max", JsonValue(descriptor.maxValue));
            parameter.set("default", JsonValue(descriptor.defaultValue));
            parameter.set("value", JsonValue(effect.values[i]));

            switch (descriptor.scale) {
                case dsp::ParamScale::Linear:      parameter.set("scale", JsonValue("linear")); break;
                case dsp::ParamScale::Logarithmic: parameter.set("scale", JsonValue("log")); break;
                case dsp::ParamScale::Decibels:    parameter.set("scale", JsonValue("db")); break;
                case dsp::ParamScale::Boolean:     parameter.set("scale", JsonValue("bool")); break;
                case dsp::ParamScale::Choice:      parameter.set("scale", JsonValue("choice")); break;
            }

            if (!descriptor.choices.empty()) {
                JsonValue choices = JsonValue::makeArray();
                for (const auto& choice : descriptor.choices) choices.push(JsonValue(choice));
                parameter.set("choices", std::move(choices));
            }
            parameters.push(std::move(parameter));
        }
        entry.set("parameters", std::move(parameters));
        list.push(std::move(entry));
    }
    return list;
}

JsonValue EngineController::busJson(const core::Bus& bus) const {
    const core::BindingState binding = devices_->busBinding(bus.id());

    JsonValue value = JsonValue::makeObject();
    value.set("id", JsonValue(bus.id()));
    value.set("index", JsonValue(bus.index()));
    value.set("label", JsonValue(bus.label()));
    value.set("name", JsonValue(bus.name()));
    value.set("kind", JsonValue(bus.kind() == BusKind::Physical ? "physical" : "virtual"));
    value.set("volume", JsonValue(bus.volume()));
    value.set("muted", JsonValue(bus.muted()));
    value.set("limiter", JsonValue(bus.limiterEnabled()));
    value.set("followsMaster", JsonValue(bus.followsMaster()));
    value.set("primary", JsonValue(engine_->primaryBus() == bus.id()));
    value.set("deviceId", JsonValue(binding.deviceId));
    value.set("deviceName", JsonValue(binding.deviceName));
    value.set("deviceReady", JsonValue(bus.deviceReady()));
    value.set("latencyMs", JsonValue(binding.latencyMs));
    if (!binding.lastError.empty()) value.set("error", JsonValue(binding.lastError));
    return value;
}

JsonValue EngineController::sourceJson(const core::InputSource& source) const {
    const core::BindingState binding = devices_->sourceBinding(source.id());

    JsonValue value = JsonValue::makeObject();
    value.set("id", JsonValue(source.id()));
    value.set("name", JsonValue(source.name()));
    switch (source.kind()) {
        case SourceKind::None:          value.set("kind", JsonValue("none")); break;
        case SourceKind::PhysicalInput: value.set("kind", JsonValue("input")); break;
        case SourceKind::Loopback:      value.set("kind", JsonValue("loopback")); break;
        case SourceKind::Application:   value.set("kind", JsonValue("application")); break;
        case SourceKind::VirtualDevice: value.set("kind", JsonValue("virtual")); break;
    }
    value.set("channel", JsonValue(source.channel()));
    value.set("deviceId", JsonValue(binding.deviceId));
    value.set("deviceName", JsonValue(binding.deviceName));
    value.set("processId", JsonValue(source.processId()));
    value.set("processName", JsonValue(source.processName()));
    value.set("active", JsonValue(source.active()));
    value.set("streaming", JsonValue(source.streaming()));
    value.set("gain", JsonValue(source.sourceGain()));
    value.set("underruns", JsonValue(static_cast<double>(source.ring().underruns())));
    value.set("overruns", JsonValue(static_cast<double>(source.ring().overruns())));
    if (!binding.lastError.empty()) value.set("error", JsonValue(binding.lastError));
    return value;
}

JsonValue EngineController::routingJson() const {
    JsonValue matrix = JsonValue::makeArray();
    for (core::Channel* channel : engine_->channels()) {
        JsonValue row = JsonValue::makeObject();
        row.set("channel", JsonValue(channel->id()));
        row.set("name", JsonValue(channel->name()));

        JsonValue cells = JsonValue::makeArray();
        for (core::Bus* bus : engine_->buses()) {
            JsonValue cell = JsonValue::makeObject();
            cell.set("bus", JsonValue(bus->id()));
            cell.set("label", JsonValue(bus->label()));
            cell.set("enabled", JsonValue(engine_->routing().isEnabled(channel->index(), bus->index())));
            cell.set("gain", JsonValue(engine_->routing().sendGain(channel->index(), bus->index())));
            cells.push(std::move(cell));
        }
        row.set("buses", std::move(cells));
        matrix.push(std::move(row));
    }
    return matrix;
}

JsonValue EngineController::devicesJson() const {
    auto serialize = [](const std::vector<core::DeviceInfo>& list) {
        JsonValue array = JsonValue::makeArray();
        for (const auto& device : list) {
            JsonValue entry = JsonValue::makeObject();
            entry.set("id", JsonValue(device.id));
            entry.set("name", JsonValue(device.name));
            entry.set("default", JsonValue(device.isDefault));
            entry.set("virtual", JsonValue(device.isVirtual));
            entry.set("loopback", JsonValue(device.supportsLoopback));
            entry.set("channels", JsonValue(device.maxChannels));
            entry.set("defaultSampleRate", JsonValue(device.defaultSampleRate));
            entry.set("minBufferFrames", JsonValue(device.minBufferFrames));

            JsonValue rates = JsonValue::makeArray();
            for (double rate : device.supportedSampleRates) rates.push(JsonValue(rate));
            entry.set("sampleRates", std::move(rates));
            array.push(std::move(entry));
        }
        return array;
    };

    JsonValue value = JsonValue::makeObject();
    value.set("render", serialize(devices_->devices(DeviceDirection::Render)));
    value.set("capture", serialize(devices_->devices(DeviceDirection::Capture)));

    const core::DeviceSettings settings = devices_->settings();
    JsonValue current = JsonValue::makeObject();
    current.set("sampleRate", JsonValue(settings.sampleRate));
    current.set("blockFrames", JsonValue(settings.blockFrames));
    current.set("channels", JsonValue(settings.channels));
    current.set("exclusive", JsonValue(settings.exclusive));
    current.set("followSystemDefault", JsonValue(settings.followSystemDefault));
    value.set("settings", std::move(current));
    value.set("backend", JsonValue(backend_->name()));
    return value;
}

JsonValue EngineController::masterJson() const {
    core::MasterBus& master = engine_->master();
    JsonValue value = JsonValue::makeObject();
    value.set("volume", JsonValue(master.volume()));
    value.set("muted", JsonValue(master.muted()));
    value.set("limiter", JsonValue(master.limiterEnabled()));
    value.set("ceilingDb", JsonValue(master.limiterCeilingDb()));
    value.set("primaryBus", JsonValue(master.primaryBus()));
    return value;
}

JsonValue EngineController::statsJson() const {
    const core::EngineStats stats = engine_->stats();
    JsonValue value = JsonValue::makeObject();
    value.set("running", JsonValue(stats.running));
    value.set("blocks", JsonValue(static_cast<double>(stats.blocksProcessed)));
    value.set("xruns", JsonValue(static_cast<double>(stats.xruns)));
    value.set("cpuLoad", JsonValue(stats.cpuLoad));
    value.set("peakCpuLoad", JsonValue(stats.peakCpuLoad));
    value.set("sampleRate", JsonValue(stats.sampleRate));
    value.set("blockFrames", JsonValue(stats.blockFrames));
    value.set("deviceLatencyMs", JsonValue(devices_->primaryLatencyMs()));
    value.set("totalLatencyMs", JsonValue(devices_->totalLatencyMs()));
    return value;
}

JsonValue EngineController::snapshot() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    JsonValue value = JsonValue::makeObject();
    value.set("schemaVersion", JsonValue(kConfigSchemaVersion));

    JsonValue channelList = JsonValue::makeArray();
    for (core::Channel* channel : engine_->channels()) channelList.push(channelJson(*channel));
    value.set("channels", std::move(channelList));

    JsonValue busList = JsonValue::makeArray();
    for (core::Bus* bus : engine_->buses()) busList.push(busJson(*bus));
    value.set("buses", std::move(busList));

    JsonValue sourceList = JsonValue::makeArray();
    for (core::InputSource* source : engine_->sources()) sourceList.push(sourceJson(*source));
    value.set("sources", std::move(sourceList));

    value.set("routing", routingJson());
    value.set("master", masterJson());
    value.set("devices", devicesJson());
    value.set("stats", statsJson());
    value.set("profile", JsonValue(profiles_->activeProfile()));

    JsonValue profileList = JsonValue::makeArray();
    for (const auto& profile : profiles_->list()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("name", JsonValue(profile.name));
        entry.set("description", JsonValue(profile.description));
        entry.set("active", JsonValue(profile.name == profiles_->activeProfile()));
        profileList.push(std::move(entry));
    }
    value.set("profiles", std::move(profileList));

    JsonValue hotkeyList = JsonValue::makeArray();
    for (const auto& binding : hotkeys_->bindings()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue(binding.id));
        entry.set("combo", JsonValue(binding.combo));
        entry.set("action", JsonValue(binding.action));
        entry.set("args", binding.args);
        entry.set("enabled", JsonValue(binding.enabled));
        entry.set("registered", JsonValue(binding.registered));
        if (!binding.lastError.empty()) entry.set("error", JsonValue(binding.lastError));
        hotkeyList.push(std::move(entry));
    }
    value.set("hotkeys", std::move(hotkeyList));
    value.set("hotkeysGlobal", JsonValue(hotkeys_->supportsGlobalCapture()));

    JsonValue ruleList = JsonValue::makeArray();
    for (const auto& rule : automation_.rules()) ruleList.push(rule.toJson());
    value.set("automation", std::move(ruleList));

    JsonValue virtualList = JsonValue::makeArray();
    for (const auto& endpoint : virtualDevices_->states()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue(endpoint.id));
        entry.set("name", JsonValue(endpoint.name));
        entry.set("kind", JsonValue(endpoint.kind == virtualaudio::VirtualEndpointKind::Output
                                        ? "output" : "input"));
        entry.set("enabled", JsonValue(endpoint.enabled));
        entry.set("transportReady", JsonValue(endpoint.transportReady));
        entry.set("clientConnected", JsonValue(endpoint.clientConnected));
        entry.set("bus", JsonValue(endpoint.bus));
        entry.set("source", JsonValue(endpoint.source));
        entry.set("channels", JsonValue(endpoint.channels));
        virtualList.push(std::move(entry));
    }
    value.set("virtualDevices", std::move(virtualList));

    const virtualaudio::DriverStatus driver = virtualDevices_->driverStatus();
    JsonValue driverJson = JsonValue::makeObject();
    driverJson.set("state", JsonValue(virtualaudio::driverStateName(driver.state)));
    driverJson.set("installedVersion", JsonValue(driver.installedVersion));
    driverJson.set("requiredVersion", JsonValue(driver.requiredVersion));
    driverJson.set("message", JsonValue(driver.message));
    driverJson.set("requiresElevation", JsonValue(driver.requiresElevation));
    value.set("driver", std::move(driverJson));

    JsonValue applicationList = JsonValue::makeArray();
    for (const auto& application : appDetector_->known()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("processId", JsonValue(application.processId));
        entry.set("executable", JsonValue(application.executable));
        entry.set("displayName", JsonValue(application.displayName));
        entry.set("active", JsonValue(application.active));
        entry.set("channel", JsonValue(assignments_.lookup(application.executable)));
        applicationList.push(std::move(entry));
    }
    value.set("applications", std::move(applicationList));

    JsonValue assignmentMap = JsonValue::makeObject();
    for (const auto& [executable, channelName] : assignments_.all())
        assignmentMap.set(executable, JsonValue(channelName));
    value.set("assignments", std::move(assignmentMap));

    JsonValue pluginList = JsonValue::makeArray();
    for (const auto& descriptor : dsp::PluginRegistry::instance().descriptors()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("type", JsonValue(descriptor.typeId));
        entry.set("name", JsonValue(descriptor.displayName));
        entry.set("category", JsonValue(descriptor.category));
        pluginList.push(std::move(entry));
    }
    value.set("plugins", std::move(pluginList));

    return value;
}

JsonValue EngineController::meters() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    auto meterJson = [](const MeterSnapshot& meter) {
        JsonValue value = JsonValue::makeObject();
        value.set("rmsL", JsonValue(meter.rmsLeft));
        value.set("rmsR", JsonValue(meter.rmsRight));
        value.set("peakL", JsonValue(meter.peakLeft));
        value.set("peakR", JsonValue(meter.peakRight));
        value.set("clip", JsonValue(meter.clipping));
        return value;
    };

    JsonValue value = JsonValue::makeObject();

    JsonValue channelMeters = JsonValue::makeArray();
    for (core::Channel* channel : engine_->channels()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue(channel->id()));
        entry.set("input", meterJson(channel->inputMeter()));
        entry.set("output", meterJson(channel->outputMeter()));
        channelMeters.push(std::move(entry));
    }
    value.set("channels", std::move(channelMeters));

    JsonValue busMeters = JsonValue::makeArray();
    for (core::Bus* bus : engine_->buses()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue(bus->id()));
        entry.set("meter", meterJson(bus->meter()));
        busMeters.push(std::move(entry));
    }
    value.set("buses", std::move(busMeters));

    value.set("master", meterJson(engine_->master().meterSnapshot()));

    const core::EngineStats stats = engine_->stats();
    value.set("cpuLoad", JsonValue(stats.cpuLoad));
    value.set("xruns", JsonValue(static_cast<double>(stats.xruns)));
    return value;
}

// ── Aplikacje ───────────────────────────────────────────────────────────────

void EngineController::onAppEvent(const AudioApplication& application, bool appeared) {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    JsonValue context = JsonValue::makeObject();
    context.set("executable", JsonValue(application.executable));
    context.set("displayName", JsonValue(application.displayName));
    context.set("processId", JsonValue(application.processId));

    if (appeared) {
        // Zapamiętane przypisanie ma pierwszeństwo przed regułami (spec §5).
        const std::string channelName = assignments_.lookup(application.executable);
        if (!channelName.empty())
            assignApplication(application.executable, channelName, application.processId);

        automation_.fire(TriggerType::AppStarted, context);
    } else {
        for (core::InputSource* source : engine_->sources()) {
            if (source->kind() == SourceKind::Application &&
                source->processId() == application.processId) {
                devices_->unbindSource(source->id());
                source->setProcessId(0);
            }
        }
        automation_.fire(TriggerType::AppStopped, context);
    }
}

Status EngineController::assignApplication(const std::string& executable,
                                           const std::string& channelName,
                                           std::uint32_t processId) {
    core::Channel* channel = nullptr;
    for (core::Channel* candidate : engine_->channels())
        if (candidate->name() == channelName) { channel = candidate; break; }
    if (channel == nullptr) return Status::error("Nieznany kanał: " + channelName);

    assignments_.set(executable, channelName);

    // Znajdź (lub utwórz) źródło dla tej aplikacji.
    const std::string key = AppAssignments::normalize(executable);
    core::InputSource* source = nullptr;
    for (core::InputSource* candidate : engine_->sources()) {
        if (candidate->kind() != SourceKind::Application) continue;
        if (AppAssignments::normalize(candidate->processName()) == key) { source = candidate; break; }
    }

    if (source == nullptr) {
        source = engine_->addSource(SourceKind::Application, executable);
        if (source == nullptr) return Status::error("Nie udało się utworzyć źródła");
        source->setProcessName(executable);
    }

    engine_->assignSource(source->id(), channel->id());

    if (processId != 0) {
        const Status status = devices_->bindSourceToProcess(source->id(), processId, executable);
        if (!status) {
            Log::warn(kLog, "Przechwytywanie " + executable + ": " + status.message);
            return status;
        }
    }

    Log::info(kLog, executable + " → " + channelName);
    return Status::success();
}

} // namespace helix::app
