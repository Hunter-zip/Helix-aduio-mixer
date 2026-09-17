// Zrzut i odtworzenie pełnej konfiguracji miksera (spec §15, §24).
#include <algorithm>

#include "helix/Log.h"
#include "helix/app/EngineController.h"
#include "helix/dsp/PluginRegistry.h"

namespace helix::app {

namespace {
constexpr const char* kLog = "Profile";

const char* busKindName(BusKind kind) {
    return kind == BusKind::Physical ? "physical" : "virtual";
}

BusKind parseBusKind(const std::string& text) {
    return text == "virtual" ? BusKind::Virtual : BusKind::Physical;
}

const char* sourceKindName(SourceKind kind) {
    switch (kind) {
        case SourceKind::None:          return "none";
        case SourceKind::PhysicalInput: return "input";
        case SourceKind::Loopback:      return "loopback";
        case SourceKind::Application:   return "application";
        case SourceKind::VirtualDevice: return "virtual";
    }
    return "none";
}

SourceKind parseSourceKind(const std::string& text) {
    if (text == "input")       return SourceKind::PhysicalInput;
    if (text == "loopback")    return SourceKind::Loopback;
    if (text == "application") return SourceKind::Application;
    if (text == "virtual")     return SourceKind::VirtualDevice;
    return SourceKind::None;
}

} // namespace

JsonValue EngineController::captureProfile() const {
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    JsonValue document = JsonValue::makeObject();
    document.set("schemaVersion", JsonValue(kConfigSchemaVersion));
    document.set("profile", JsonValue(profiles_->activeProfile()));

    // Master.
    core::MasterBus& master = engine_->master();
    document.set("masterVolume", JsonValue(master.volume()));
    JsonValue masterObject = JsonValue::makeObject();
    masterObject.set("volume", JsonValue(master.volume()));
    masterObject.set("muted", JsonValue(master.muted()));
    masterObject.set("limiter", JsonValue(master.limiterEnabled()));
    masterObject.set("ceilingDb", JsonValue(master.limiterCeilingDb()));
    document.set("master", std::move(masterObject));

    // Ustawienia urządzeń.
    const core::DeviceSettings settings = devices_->settings();
    JsonValue deviceSettings = JsonValue::makeObject();
    deviceSettings.set("sampleRate", JsonValue(settings.sampleRate));
    deviceSettings.set("blockFrames", JsonValue(settings.blockFrames));
    deviceSettings.set("channels", JsonValue(settings.channels));
    deviceSettings.set("exclusive", JsonValue(settings.exclusive));
    deviceSettings.set("followSystemDefault", JsonValue(settings.followSystemDefault));
    document.set("deviceSettings", std::move(deviceSettings));

    // Magistrale.
    JsonValue buses = JsonValue::makeArray();
    for (core::Bus* bus : engine_->buses()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("label", JsonValue(bus->label()));
        entry.set("name", JsonValue(bus->name()));
        entry.set("kind", JsonValue(busKindName(bus->kind())));
        entry.set("volume", JsonValue(bus->volume()));
        entry.set("muted", JsonValue(bus->muted()));
        entry.set("limiter", JsonValue(bus->limiterEnabled()));
        entry.set("followsMaster", JsonValue(bus->followsMaster()));
        entry.set("deviceId", JsonValue(bus->deviceId()));
        entry.set("primary", JsonValue(engine_->primaryBus() == bus->id()));
        buses.push(std::move(entry));
    }
    document.set("buses", std::move(buses));

    // Kanały z pełnym łańcuchem DSP.
    JsonValue channels = JsonValue::makeArray();
    for (core::Channel* channel : engine_->channels()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("name", JsonValue(channel->name()));
        entry.set("icon", JsonValue(channel->icon()));
        entry.set("volume", JsonValue(channel->volume()));
        entry.set("muted", JsonValue(channel->muted()));
        entry.set("solo", JsonValue(channel->solo()));
        entry.set("pan", JsonValue(channel->pan()));
        entry.set("gainDb", JsonValue(channel->gainDb()));
        entry.set("enabled", JsonValue(channel->enabled()));

        JsonValue effects = JsonValue::makeArray();
        for (const auto& effect : channels_->effectStates(channel->id())) {
            JsonValue effectEntry = JsonValue::makeObject();
            effectEntry.set("type", JsonValue(effect.typeId));
            effectEntry.set("enabled", JsonValue(effect.enabled));

            JsonValue parameters = JsonValue::makeObject();
            for (std::size_t i = 0; i < effect.parameters.size(); ++i)
                parameters.set(effect.parameters[i].id, JsonValue(effect.values[i]));
            effectEntry.set("parameters", std::move(parameters));
            effects.push(std::move(effectEntry));
        }
        entry.set("effects", std::move(effects));

        // Zachowujemy też prostą listę wyjść — czytelną dla człowieka.
        JsonValue outputs = JsonValue::makeArray();
        for (core::Bus* bus : engine_->buses())
            if (engine_->routing().isEnabled(channel->index(), bus->index()))
                outputs.push(JsonValue(bus->label()));
        entry.set("outputs", std::move(outputs));

        channels.push(std::move(entry));
    }
    document.set("channels", std::move(channels));

    // Macierz routingu.
    JsonValue routing = JsonValue::makeArray();
    for (core::Channel* channel : engine_->channels()) {
        for (core::Bus* bus : engine_->buses()) {
            if (!engine_->routing().isEnabled(channel->index(), bus->index())) continue;
            JsonValue entry = JsonValue::makeObject();
            entry.set("channel", JsonValue(channel->name()));
            entry.set("bus", JsonValue(bus->label()));
            entry.set("enabled", JsonValue(true));
            entry.set("gain", JsonValue(engine_->routing().sendGain(channel->index(), bus->index())));
            routing.push(std::move(entry));
        }
    }
    document.set("routing", std::move(routing));

    // Źródła.
    JsonValue sources = JsonValue::makeArray();
    for (core::InputSource* source : engine_->sources()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("name", JsonValue(source->name()));
        entry.set("kind", JsonValue(sourceKindName(source->kind())));
        entry.set("deviceId", JsonValue(source->deviceId()));
        entry.set("processName", JsonValue(source->processName()));
        entry.set("gain", JsonValue(source->sourceGain()));

        std::string channelName;
        if (core::Channel* channel = engine_->findChannel(source->channel()))
            channelName = channel->name();
        entry.set("channel", JsonValue(channelName));
        sources.push(std::move(entry));
    }
    document.set("sources", std::move(sources));

    // Przypisania aplikacji.
    JsonValue assignments = JsonValue::makeObject();
    for (const auto& [executable, channelName] : assignments_.all())
        assignments.set(executable, JsonValue(channelName));
    document.set("applicationAssignments", std::move(assignments));

    // Skróty klawiszowe.
    JsonValue hotkeys = JsonValue::makeArray();
    for (const auto& binding : hotkeys_->bindings()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue(binding.id));
        entry.set("combo", JsonValue(binding.combo));
        entry.set("action", JsonValue(binding.action));
        entry.set("args", binding.args);
        entry.set("enabled", JsonValue(binding.enabled));
        hotkeys.push(std::move(entry));
    }
    document.set("hotkeys", std::move(hotkeys));

    // Reguły automatyzacji.
    JsonValue rules = JsonValue::makeArray();
    for (const auto& rule : automation_.rules()) rules.push(rule.toJson());
    document.set("automation", std::move(rules));

    // Wirtualne urządzenia.
    JsonValue endpoints = JsonValue::makeArray();
    for (const auto& endpoint : virtualDevices_->states()) {
        JsonValue entry = JsonValue::makeObject();
        entry.set("id", JsonValue(endpoint.id));
        entry.set("enabled", JsonValue(endpoint.enabled));

        std::string busLabel;
        if (core::Bus* bus = engine_->findBus(endpoint.bus)) busLabel = bus->label();
        entry.set("bus", JsonValue(busLabel));

        std::string sourceName;
        if (core::InputSource* source = engine_->findSource(endpoint.source))
            sourceName = source->name();
        entry.set("source", JsonValue(sourceName));
        endpoints.push(std::move(entry));
    }
    document.set("virtualDevices", std::move(endpoints));

    return document;
}

Status EngineController::applyProfile(const JsonValue& document) {
    if (!document.isObject()) return Status::error("Profil musi być obiektem JSON");

    // Reguła automatyzacji mogłaby wywołać kolejne wczytanie profilu — licznik
    // zagnieżdżenia przerywa taką pętlę zamiast zakleszczać wątek sterujący.
    struct DepthGuard {
        std::atomic<int>& counter;
        explicit DepthGuard(std::atomic<int>& c) : counter(c) { counter.fetch_add(1); }
        ~DepthGuard() { counter.fetch_sub(1); }
    } depthGuard(applyDepth_);

    if (applyDepth_.load() > 4)
        return Status::error("Wykryto pętlę wczytywania profili");

    std::lock_guard<std::recursive_mutex> lock(mutex_);

    // 1. Ustawienia urządzeń — tylko gdy faktycznie się zmieniły, żeby nie
    //    przerywać odtwarzania bez potrzeby (spec §23).
    const JsonValue& deviceSettings = document["deviceSettings"];
    if (deviceSettings.isObject()) {
        core::DeviceSettings settings = devices_->settings();
        const double sampleRate = deviceSettings["sampleRate"].asNumber(settings.sampleRate);
        const int blockFrames   = deviceSettings["blockFrames"].asInt(settings.blockFrames);
        const int channelCount  = deviceSettings["channels"].asInt(settings.channels);
        const bool exclusive    = deviceSettings["exclusive"].asBool(settings.exclusive);

        if (sampleRate != settings.sampleRate || blockFrames != settings.blockFrames ||
            channelCount != settings.channels || exclusive != settings.exclusive) {
            settings.sampleRate  = sampleRate;
            settings.blockFrames = blockFrames;
            settings.channels    = channelCount;
            settings.exclusive   = exclusive;
            settings.followSystemDefault =
                deviceSettings["followSystemDefault"].asBool(settings.followSystemDefault);
            const Status status = devices_->applySettings(settings);
            if (!status) Log::warn(kLog, "Ustawienia urządzeń: " + status.message);
        }
    }

    // 2. Magistrale — dopasowanie po etykiecie, brakujące tworzymy.
    const JsonValue& buses = document["buses"];
    for (std::size_t i = 0; i < buses.size(); ++i) {
        const JsonValue& entry = buses[i];
        const std::string label = entry["label"].asString();
        if (label.empty()) continue;

        core::Bus* bus = nullptr;
        for (core::Bus* candidate : engine_->buses())
            if (candidate->label() == label) { bus = candidate; break; }

        if (bus == nullptr) {
            bus = engine_->addBus(entry["name"].asString(label), label,
                                  parseBusKind(entry["kind"].asString("physical")));
            if (bus == nullptr) continue;
        }

        bus->setName(entry["name"].asString(bus->name()));
        bus->setVolume(entry["volume"].asFloat(bus->volume()));
        bus->setMuted(entry["muted"].asBool(bus->muted()));
        bus->setLimiterEnabled(entry["limiter"].asBool(bus->limiterEnabled()));
        bus->setFollowsMaster(entry["followsMaster"].asBool(bus->followsMaster()));

        if (entry["primary"].asBool(false)) engine_->setPrimaryBus(bus->id());

        const std::string deviceId = entry["deviceId"].asString();
        if (deviceId != bus->deviceId()) devices_->bindBus(bus->id(), deviceId);
    }

    // 3. Kanały.
    const JsonValue& channelList = document["channels"];
    for (std::size_t i = 0; i < channelList.size(); ++i) {
        const JsonValue& entry = channelList[i];
        const std::string name = entry["name"].asString();
        if (name.empty()) continue;

        core::Channel* channel = nullptr;
        for (core::Channel* candidate : engine_->channels())
            if (candidate->name() == name) { channel = candidate; break; }

        if (channel == nullptr) {
            channel = channels_->createChannel(name, entry["icon"].asString("channel"), false);
            if (channel == nullptr) continue;
        }

        channel->setIcon(entry["icon"].asString(channel->icon()));
        channel->setVolume(entry["volume"].asFloat(channel->volume()));
        channel->setMuted(entry["muted"].asBool(channel->muted()));
        channel->setSolo(entry["solo"].asBool(channel->solo()));
        channel->setPan(entry["pan"].asFloat(channel->pan()));
        channel->setEnabled(entry["enabled"].asBool(channel->enabled()));

        // Łańcuch efektów odtwarzamy dokładnie w zapisanej kolejności.
        const JsonValue& effects = entry["effects"];
        if (effects.isArray()) {
            channels_->clearEffects(channel->id());
            for (std::size_t e = 0; e < effects.size(); ++e) {
                const JsonValue& effectEntry = effects[e];
                const std::string typeId = effectEntry["type"].asString();
                if (typeId.empty()) continue;

                PluginId created = 0;
                if (!channels_->addEffect(channel->id(), typeId, -1, &created)) {
                    Log::warn(kLog, "Nieznany efekt w profilu: " + typeId);
                    continue;
                }
                channels_->setEffectEnabled(channel->id(), created, effectEntry["enabled"].asBool(true));

                const JsonValue& parameters = effectEntry["parameters"];
                for (const auto& [parameterId, value] : parameters.members())
                    channels_->setEffectParameter(channel->id(), created, parameterId, value.asFloat());
            }
        }

        channel->setGainDb(entry["gainDb"].asFloat(channel->gainDb()));
    }

    // 4. Routing — najpierw czyścimy, potem odtwarzamy zapisane połączenia.
    const JsonValue& routing = document["routing"];
    if (routing.isArray()) {
        for (core::Channel* channel : engine_->channels())
            engine_->routing().clearChannel(channel->index());

        for (std::size_t i = 0; i < routing.size(); ++i) {
            const JsonValue& entry = routing[i];
            const std::string channelName = entry["channel"].asString();
            const std::string busLabel    = entry["bus"].asString();

            core::Channel* channel = nullptr;
            for (core::Channel* candidate : engine_->channels())
                if (candidate->name() == channelName) { channel = candidate; break; }

            core::Bus* bus = nullptr;
            for (core::Bus* candidate : engine_->buses())
                if (candidate->label() == busLabel || candidate->name() == busLabel) {
                    bus = candidate;
                    break;
                }

            if (channel == nullptr || bus == nullptr) continue;

            const float gain = entry["gain"].asFloat(1.0f);
            engine_->routing().setSendGain(channel->index(), bus->index(), gain);
            engine_->routing().setEnabled(channel->index(), bus->index(), entry["enabled"].asBool(true));
        }
    }

    // 5. Źródła.
    const JsonValue& sources = document["sources"];
    for (std::size_t i = 0; i < sources.size(); ++i) {
        const JsonValue& entry = sources[i];
        const std::string name = entry["name"].asString();
        if (name.empty()) continue;

        core::InputSource* source = nullptr;
        for (core::InputSource* candidate : engine_->sources())
            if (candidate->name() == name) { source = candidate; break; }

        if (source == nullptr) {
            source = engine_->addSource(parseSourceKind(entry["kind"].asString("none")), name);
            if (source == nullptr) continue;
        }

        source->setProcessName(entry["processName"].asString(source->processName()));
        source->setSourceGain(entry["gain"].asFloat(source->sourceGain()));

        const std::string channelName = entry["channel"].asString();
        ChannelId target = kInvalidChannel;
        for (core::Channel* candidate : engine_->channels())
            if (candidate->name() == channelName) { target = candidate->id(); break; }
        engine_->assignSource(source->id(), target);

        const std::string deviceId = entry["deviceId"].asString();
        if (!deviceId.empty() && deviceId != source->deviceId())
            devices_->bindSourceToDevice(source->id(), deviceId,
                                         source->kind() == SourceKind::Loopback);
    }

    // 6. Master.
    const JsonValue& master = document["master"];
    if (master.isObject()) {
        engine_->master().setVolume(master["volume"].asFloat(engine_->master().volume()));
        engine_->master().setMuted(master["muted"].asBool(engine_->master().muted()));
        engine_->master().setLimiterEnabled(master["limiter"].asBool(engine_->master().limiterEnabled()));
        engine_->master().setLimiterCeilingDb(
            master["ceilingDb"].asFloat(engine_->master().limiterCeilingDb()));
    } else if (document.contains("masterVolume")) {
        engine_->master().setVolume(document["masterVolume"].asFloat(engine_->master().volume()));
    }

    // 7. Przypisania aplikacji.
    const JsonValue& applicationAssignments = document["applicationAssignments"];
    if (applicationAssignments.isObject()) {
        std::map<std::string, std::string> entries;
        for (const auto& [executable, channelName] : applicationAssignments.members())
            entries[executable] = channelName.asString();
        assignments_.replaceAll(std::move(entries));
    }

    // 8. Skróty klawiszowe.
    const JsonValue& hotkeys = document["hotkeys"];
    if (hotkeys.isArray()) {
        hotkeys_->clear();
        for (std::size_t i = 0; i < hotkeys.size(); ++i) {
            const JsonValue& entry = hotkeys[i];
            HotkeyBinding binding;
            binding.id      = entry["id"].asString();
            binding.combo   = entry["combo"].asString();
            binding.action  = entry["action"].asString();
            binding.args    = entry["args"];
            binding.enabled = entry["enabled"].asBool(true);
            if (binding.id.empty() || binding.combo.empty()) continue;
            const Status status = hotkeys_->bind(std::move(binding));
            if (!status) Log::warn(kLog, status.message);
        }
    }

    // 9. Automatyzacja.
    const JsonValue& automationRules = document["automation"];
    if (automationRules.isArray()) {
        std::vector<AutomationRule> rules;
        for (std::size_t i = 0; i < automationRules.size(); ++i) {
            AutomationRule rule;
            std::string error;
            if (AutomationRule::fromJson(automationRules[i], rule, error)) rules.push_back(std::move(rule));
            else Log::warn(kLog, "Reguła automatyzacji: " + error);
        }
        automation_.setRules(std::move(rules));
    }

    // 10. Wirtualne urządzenia.
    const JsonValue& endpoints = document["virtualDevices"];
    for (std::size_t i = 0; i < endpoints.size(); ++i) {
        const JsonValue& entry = endpoints[i];
        const std::string id = entry["id"].asString();
        if (id.empty()) continue;

        const std::string busLabel = entry["bus"].asString();
        if (!busLabel.empty()) {
            for (core::Bus* bus : engine_->buses())
                if (bus->label() == busLabel) { virtualDevices_->bindOutput(id, bus->id()); break; }
        }

        const std::string sourceName = entry["source"].asString();
        if (!sourceName.empty()) {
            for (core::InputSource* source : engine_->sources())
                if (source->name() == sourceName) { virtualDevices_->bindInput(id, source->id()); break; }
        }

        virtualDevices_->setEndpointEnabled(id, entry["enabled"].asBool(false));
    }

    engine_->commitGraph();

    // Reguły uruchomione tym zdarzeniem wracają przez dispatch() w tym samym
    // wątku — rekurencyjny mutex pozwala im wejść bez zakleszczenia.
    JsonValue context = JsonValue::makeObject();
    context.set("profile", JsonValue(document["profile"].asString()));
    automation_.fire(TriggerType::ProfileActivated, context);

    return Status::success();
}

} // namespace helix::app
