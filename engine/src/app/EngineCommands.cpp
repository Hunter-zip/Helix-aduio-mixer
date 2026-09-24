// Dyspozytor komend sterujących (spec §26, §28).
//
// To jedyna droga, którą GUI, skróty klawiszowe i automatyzacja dotykają silnika.
#include <algorithm>
#include <cmath>

#include "helix/Log.h"
#include "helix/app/EngineController.h"
#include "helix/dsp/PluginRegistry.h"

namespace helix::app {

namespace {

JsonValue okResult() { return JsonValue::makeObject(); }

float clampf(float value, float low, float high) {
    return value < low ? low : (value > high ? high : value);
}

} // namespace

std::vector<std::string> EngineController::commandNames() {
    return {
        "engine.status", "engine.start", "engine.stop", "engine.resetStats", "engine.snapshot",
        "engine.meters", "engine.commands",
        "device.list", "device.settings", "device.apply", "device.test",
        "channel.list", "channel.add", "channel.remove", "channel.rename", "channel.setIcon",
        "channel.setVolume", "channel.setMute", "channel.toggleMute", "channel.setSolo",
        "channel.setPan", "channel.setGain", "channel.setEnabled", "channel.clearClip",
        "effect.list", "effect.add", "effect.remove", "effect.reorder", "effect.setEnabled",
        "effect.setParam", "effect.eqResponse",
        "bus.list", "bus.add", "bus.remove", "bus.setVolume", "bus.setMute", "bus.setLimiter",
        "bus.setDevice", "bus.setFollowsMaster", "bus.setPrimary",
        "routing.get", "routing.set", "routing.setGain",
        "master.get", "master.setVolume", "master.setMute", "master.setLimiter",
        "source.list", "source.add", "source.remove", "source.assign", "source.setGain",
        "source.bindDevice", "source.bindProcess", "source.unbind",
        "app.list", "app.assign", "app.unassign", "app.assignments",
        "profile.list", "profile.save", "profile.load", "profile.delete", "profile.rename",
        "profile.duplicate", "profile.active",
        "hotkey.list", "hotkey.bind", "hotkey.unbind", "hotkey.trigger",
        "automation.list", "automation.add", "automation.remove", "automation.setEnabled",
        "virtual.list", "virtual.setEnabled", "virtual.bindOutput", "virtual.bindInput",
        "virtual.driverStatus", "virtual.install", "virtual.update", "virtual.uninstall",
        "plugins.list",
    };
}

JsonValue EngineController::dispatch(const std::string& command, const JsonValue& args, Status& status) {
    status = Status::success();

    // Cały dyspozytor jest serializowany: komendy przychodzą z wątków klientów
    // serwera, z wątku skrótów i z automatyzacji.
    std::lock_guard<std::recursive_mutex> lock(mutex_);

    if (!initialized_.load(std::memory_order_acquire) && command != "engine.commands") {
        status = Status::error("Silnik nie jest zainicjalizowany");
        return JsonValue();
    }

    // ── Silnik ──────────────────────────────────────────────────────────────

    if (command == "engine.commands") {
        JsonValue list = JsonValue::makeArray();
        for (const auto& name : commandNames()) list.push(JsonValue(name));
        return list;
    }

    if (command == "engine.status" || command == "engine.snapshot") return snapshot();
    if (command == "engine.meters") return meters();

    if (command == "engine.start") {
        status = devices_->start();
        return okResult();
    }

    if (command == "engine.stop") {
        devices_->stop();
        return okResult();
    }

    if (command == "engine.resetStats") {
        engine_->resetStats();
        return okResult();
    }

    // ── Urządzenia ──────────────────────────────────────────────────────────

    if (command == "device.list" || command == "device.settings") return devicesJson();

    if (command == "device.apply") {
        core::DeviceSettings settings = devices_->settings();
        settings.sampleRate  = args["sampleRate"].asNumber(settings.sampleRate);
        settings.blockFrames = args["blockFrames"].asInt(settings.blockFrames);
        settings.channels    = args["channels"].asInt(settings.channels);
        settings.exclusive   = args["exclusive"].asBool(settings.exclusive);
        settings.followSystemDefault =
            args["followSystemDefault"].asBool(settings.followSystemDefault);
        status = devices_->applySettings(settings);
        if (status && virtualDevices_->isRunning())
            virtualDevices_->start(settings.sampleRate, settings.blockFrames);
        return devicesJson();
    }

    if (command == "device.test") {
        status = devices_->testDevice(args["deviceId"].asString(),
                                      args["duration"].asNumber(0.6));
        return okResult();
    }

    // ── Kanały ──────────────────────────────────────────────────────────────

    if (command == "channel.list") {
        JsonValue list = JsonValue::makeArray();
        for (core::Channel* channel : engine_->channels()) list.push(channelJson(*channel));
        return list;
    }

    if (command == "channel.add") {
        const std::string name = args["name"].asString();
        if (name.empty()) { status = Status::error("Podaj nazwę kanału"); return JsonValue(); }

        core::Channel* channel = channels_->createChannel(
            name, args["icon"].asString("channel"), args["defaultChain"].asBool(true));
        if (channel == nullptr) { status = Status::error("Nie udało się utworzyć kanału"); return JsonValue(); }

        // Nowy kanał domyślnie trafia na wyjście podstawowe.
        if (core::Bus* primary = engine_->findBus(engine_->primaryBus()))
            engine_->routing().setEnabled(channel->index(), primary->index(), true);
        return channelJson(*channel);
    }

    if (command == "channel.remove") {
        core::Channel* channel = resolveChannel(args);
        if (channel == nullptr) { status = Status::error("Nieznany kanał"); return JsonValue(); }
        if (!channels_->removeChannel(channel->id()))
            status = Status::error("Nie udało się usunąć kanału");
        return okResult();
    }

    if (command == "channel.rename") {
        core::Channel* channel = resolveChannel(args);
        const std::string name = args["name"].asString();
        if (channel == nullptr) { status = Status::error("Nieznany kanał"); return JsonValue(); }
        if (name.empty()) { status = Status::error("Podaj nową nazwę"); return JsonValue(); }
        channel->setName(name);
        return channelJson(*channel);
    }

    if (command == "channel.setIcon") {
        core::Channel* channel = resolveChannel(args);
        if (channel == nullptr) { status = Status::error("Nieznany kanał"); return JsonValue(); }
        channel->setIcon(args["icon"].asString("channel"));
        return channelJson(*channel);
    }

    if (command == "channel.setVolume" || command == "channel.setMute" ||
        command == "channel.toggleMute" || command == "channel.setSolo" ||
        command == "channel.setPan" || command == "channel.setGain" ||
        command == "channel.setEnabled" || command == "channel.clearClip") {

        core::Channel* channel = resolveChannel(args);
        if (channel == nullptr) { status = Status::error("Nieznany kanał"); return JsonValue(); }

        if (command == "channel.setVolume") {
            channel->setVolume(clampf(args["value"].asFloat(channel->volume()), 0.0f, 2.0f));
        } else if (command == "channel.setMute" || command == "channel.toggleMute") {
            const bool muted = (command == "channel.toggleMute")
                                   ? !channel->muted()
                                   : args["value"].asBool(!channel->muted());
            channel->setMuted(muted);
            JsonValue context = JsonValue::makeObject();
            context.set("channel", JsonValue(channel->name()));
            automation_.fire(muted ? TriggerType::ChannelMuted : TriggerType::ChannelUnmuted, context);
        } else if (command == "channel.setSolo") {
            channel->setSolo(args["value"].asBool(!channel->solo()));
            engine_->commitGraph();   // Solo zmienia logikę wyciszania w grafie
        } else if (command == "channel.setPan") {
            channel->setPan(clampf(args["value"].asFloat(channel->pan()), -1.0f, 1.0f));
        } else if (command == "channel.setGain") {
            channel->setGainDb(clampf(args["value"].asFloat(channel->gainDb()), -40.0f, 40.0f));
        } else if (command == "channel.setEnabled") {
            channel->setEnabled(args["value"].asBool(!channel->enabled()));
        } else {
            channel->clearClip();
        }
        return channelJson(*channel);
    }

    // ── Efekty ──────────────────────────────────────────────────────────────

    if (command.rfind("effect.", 0) == 0) {
        core::Channel* channel = resolveChannel(args);
        if (channel == nullptr) { status = Status::error("Nieznany kanał"); return JsonValue(); }

        if (command == "effect.list") return effectsJson(channel->id());

        if (command == "effect.add") {
            const std::string type = args["type"].asString();
            PluginId created = 0;
            if (!channels_->addEffect(channel->id(), type, args["position"].asInt(-1), &created)) {
                status = Status::error("Nie udało się dodać efektu: " + type);
                return JsonValue();
            }
            JsonValue result = JsonValue::makeObject();
            result.set("id", JsonValue(created));
            result.set("effects", effectsJson(channel->id()));
            return result;
        }

        if (command == "effect.remove") {
            if (!channels_->removeEffect(channel->id(), args["effect"].asUint()))
                status = Status::error("Nieznany efekt");
            return effectsJson(channel->id());
        }

        if (command == "effect.reorder") {
            std::vector<PluginId> order;
            const JsonValue& list = args["order"];
            for (std::size_t i = 0; i < list.size(); ++i) order.push_back(list[i].asUint());
            if (!channels_->reorderEffects(channel->id(), order))
                status = Status::error("Nie udało się zmienić kolejności");
            return effectsJson(channel->id());
        }

        if (command == "effect.setEnabled") {
            if (!channels_->setEffectEnabled(channel->id(), args["effect"].asUint(),
                                             args["value"].asBool(true)))
                status = Status::error("Nieznany efekt");
            return effectsJson(channel->id());
        }

        if (command == "effect.setParam") {
            if (!channels_->setEffectParameter(channel->id(), args["effect"].asUint(),
                                               args["param"].asString(), args["value"].asFloat()))
                status = Status::error("Nieznany efekt lub parametr");
            return effectsJson(channel->id());
        }

        if (command == "effect.eqResponse") {
            std::vector<double> frequencies;
            const JsonValue& list = args["frequencies"];
            if (list.isArray() && list.size() > 0) {
                for (std::size_t i = 0; i < list.size(); ++i) frequencies.push_back(list[i].asNumber());
            } else {
                // Domyślnie 128 punktów w skali logarytmicznej 20 Hz – 20 kHz.
                const int points = std::max(2, args["points"].asInt(128));
                for (int i = 0; i < points; ++i) {
                    const double t = static_cast<double>(i) / static_cast<double>(points - 1);
                    frequencies.push_back(20.0 * std::pow(1000.0, t));
                }
            }

            const auto response =
                channels_->equalizerResponse(channel->id(), args["effect"].asUint(), frequencies);
            if (response.empty()) {
                status = Status::error("Wybrany efekt nie jest equalizerem");
                return JsonValue();
            }

            JsonValue result = JsonValue::makeObject();
            JsonValue freqArray = JsonValue::makeArray();
            JsonValue gainArray = JsonValue::makeArray();
            for (std::size_t i = 0; i < response.size(); ++i) {
                freqArray.push(JsonValue(frequencies[i]));
                gainArray.push(JsonValue(response[i]));
            }
            result.set("frequencies", std::move(freqArray));
            result.set("gainDb", std::move(gainArray));
            return result;
        }

        status = Status::error("Nieznana komenda: " + command);
        return JsonValue();
    }

    // ── Magistrale ──────────────────────────────────────────────────────────

    if (command == "bus.list") {
        JsonValue list = JsonValue::makeArray();
        for (core::Bus* bus : engine_->buses()) list.push(busJson(*bus));
        return list;
    }

    if (command == "bus.add") {
        const std::string label = args["label"].asString();
        const std::string name  = args["name"].asString(label);
        if (label.empty()) { status = Status::error("Podaj etykietę magistrali"); return JsonValue(); }

        const BusKind kind = args["kind"].asString("physical") == "virtual" ? BusKind::Virtual
                                                                           : BusKind::Physical;
        core::Bus* bus = engine_->addBus(name, label, kind);
        if (bus == nullptr) { status = Status::error("Nie udało się utworzyć magistrali"); return JsonValue(); }
        bus->setFollowsMaster(kind == BusKind::Physical);
        engine_->commitGraph();
        return busJson(*bus);
    }

    if (command == "bus.remove") {
        core::Bus* bus = resolveBus(args);
        if (bus == nullptr) { status = Status::error("Nieznana magistrala"); return JsonValue(); }
        if (!engine_->removeBus(bus->id())) status = Status::error("Nie udało się usunąć magistrali");
        return okResult();
    }

    if (command.rfind("bus.", 0) == 0) {
        core::Bus* bus = resolveBus(args);
        if (bus == nullptr) { status = Status::error("Nieznana magistrala"); return JsonValue(); }

        if (command == "bus.setVolume") {
            bus->setVolume(clampf(args["value"].asFloat(bus->volume()), 0.0f, 2.0f));
        } else if (command == "bus.setMute") {
            bus->setMuted(args["value"].asBool(!bus->muted()));
        } else if (command == "bus.setLimiter") {
            bus->setLimiterEnabled(args["value"].asBool(!bus->limiterEnabled()));
            if (args.contains("ceilingDb"))
                bus->limiter().setParameter("ceiling", args["ceilingDb"].asFloat(-1.0f));
        } else if (command == "bus.setDevice") {
            status = devices_->bindBus(bus->id(), args["deviceId"].asString());
        } else if (command == "bus.setFollowsMaster") {
            bus->setFollowsMaster(args["value"].asBool(!bus->followsMaster()));
        } else if (command == "bus.setPrimary") {
            engine_->setPrimaryBus(bus->id());
            // Zmiana magistrali zegarowej wymaga ponownego otwarcia strumieni.
            if (devices_->isRunning()) {
                devices_->stop();
                status = devices_->start();
            }
        } else {
            status = Status::error("Nieznana komenda: " + command);
            return JsonValue();
        }
        return busJson(*bus);
    }

    // ── Routing ─────────────────────────────────────────────────────────────

    if (command == "routing.get") return routingJson();

    if (command == "routing.set" || command == "routing.setGain") {
        core::Channel* channel = resolveChannel(args);
        core::Bus* bus = resolveBus(args);
        if (channel == nullptr) { status = Status::error("Nieznany kanał"); return JsonValue(); }
        if (bus == nullptr) { status = Status::error("Nieznana magistrala"); return JsonValue(); }

        if (command == "routing.set") {
            engine_->routing().setEnabled(channel->index(), bus->index(), args["value"].asBool(true));
        } else {
            engine_->routing().setSendGain(channel->index(), bus->index(),
                                           clampf(args["value"].asFloat(1.0f), 0.0f, 4.0f));
        }
        return routingJson();
    }

    // ── Master ──────────────────────────────────────────────────────────────

    if (command == "master.get") return masterJson();

    if (command == "master.setVolume") {
        engine_->master().setVolume(clampf(args["value"].asFloat(engine_->master().volume()), 0.0f, 2.0f));
        return masterJson();
    }

    if (command == "master.setMute") {
        engine_->master().setMuted(args["value"].asBool(!engine_->master().muted()));
        return masterJson();
    }

    if (command == "master.setLimiter") {
        core::MasterBus& master = engine_->master();
        const bool enabled = args["value"].asBool(!master.limiterEnabled());
        master.setLimiterEnabled(enabled);
        if (args.contains("ceilingDb")) master.setLimiterCeilingDb(args["ceilingDb"].asFloat(-1.0f));

        // Limiter Master działa na wszystkich magistralach podążających za Masterem.
        for (core::Bus* bus : engine_->buses()) {
            if (!bus->followsMaster()) continue;
            bus->setLimiterEnabled(enabled);
            bus->limiter().setParameter("ceiling", master.limiterCeilingDb());
        }
        return masterJson();
    }

    // ── Źródła ──────────────────────────────────────────────────────────────

    if (command == "source.list") {
        JsonValue list = JsonValue::makeArray();
        for (core::InputSource* source : engine_->sources()) list.push(sourceJson(*source));
        return list;
    }

    if (command == "source.add") {
        const std::string name = args["name"].asString();
        if (name.empty()) { status = Status::error("Podaj nazwę źródła"); return JsonValue(); }

        const std::string kindText = args["kind"].asString("input");
        SourceKind kind = SourceKind::PhysicalInput;
        if (kindText == "loopback")         kind = SourceKind::Loopback;
        else if (kindText == "application") kind = SourceKind::Application;
        else if (kindText == "virtual")     kind = SourceKind::VirtualDevice;

        core::InputSource* source = engine_->addSource(kind, name);
        if (source == nullptr) { status = Status::error("Nie udało się utworzyć źródła"); return JsonValue(); }
        engine_->commitGraph();
        return sourceJson(*source);
    }

    if (command == "source.remove") {
        core::InputSource* source = resolveSource(args);
        if (source == nullptr) { status = Status::error("Nieznane źródło"); return JsonValue(); }
        devices_->unbindSource(source->id());
        if (!engine_->removeSource(source->id())) status = Status::error("Nie udało się usunąć źródła");
        engine_->commitGraph();
        return okResult();
    }

    if (command.rfind("source.", 0) == 0) {
        core::InputSource* source = resolveSource(args);
        if (source == nullptr) { status = Status::error("Nieznane źródło"); return JsonValue(); }

        if (command == "source.assign") {
            core::Channel* channel = resolveChannel(args);
            const ChannelId target = (channel != nullptr) ? channel->id() : kInvalidChannel;
            if (!engine_->assignSource(source->id(), target))
                status = Status::error("Nie udało się przypisać źródła");
        } else if (command == "source.setGain") {
            source->setSourceGain(clampf(args["value"].asFloat(source->sourceGain()), 0.0f, 4.0f));
        } else if (command == "source.bindDevice") {
            status = devices_->bindSourceToDevice(source->id(), args["deviceId"].asString(),
                                                  args["loopback"].asBool(false));
        } else if (command == "source.bindProcess") {
            status = devices_->bindSourceToProcess(source->id(), args["processId"].asUint(),
                                                   args["executable"].asString());
        } else if (command == "source.unbind") {
            devices_->unbindSource(source->id());
        } else {
            status = Status::error("Nieznana komenda: " + command);
            return JsonValue();
        }
        return sourceJson(*source);
    }

    // ── Aplikacje ───────────────────────────────────────────────────────────

    if (command == "app.list") {
        JsonValue list = JsonValue::makeArray();
        for (const auto& application : appDetector_->known()) {
            JsonValue entry = JsonValue::makeObject();
            entry.set("processId", JsonValue(application.processId));
            entry.set("executable", JsonValue(application.executable));
            entry.set("displayName", JsonValue(application.displayName));
            entry.set("active", JsonValue(application.active));
            entry.set("channel", JsonValue(assignments_.lookup(application.executable)));
            list.push(std::move(entry));
        }
        return list;
    }

    if (command == "app.assignments") {
        JsonValue map = JsonValue::makeObject();
        for (const auto& [executable, channelName] : assignments_.all())
            map.set(executable, JsonValue(channelName));
        return map;
    }

    if (command == "app.assign") {
        const std::string executable = args["executable"].asString();
        const std::string channelName = args["channel"].asString();
        if (executable.empty() || channelName.empty()) {
            status = Status::error("Podaj aplikację i kanał");
            return JsonValue();
        }
        status = assignApplication(executable, channelName, args["processId"].asUint());
        return okResult();
    }

    if (command == "app.unassign") {
        const std::string executable = args["executable"].asString();
        if (!assignments_.remove(executable)) status = Status::error("Brak takiego przypisania");
        for (core::InputSource* source : engine_->sources()) {
            if (source->kind() != SourceKind::Application) continue;
            if (AppAssignments::normalize(source->processName()) != AppAssignments::normalize(executable))
                continue;
            devices_->unbindSource(source->id());
            engine_->assignSource(source->id(), kInvalidChannel);
        }
        return okResult();
    }

    // ── Profile ─────────────────────────────────────────────────────────────

    if (command == "profile.list") {
        JsonValue list = JsonValue::makeArray();
        for (const auto& profile : profiles_->list()) {
            JsonValue entry = JsonValue::makeObject();
            entry.set("name", JsonValue(profile.name));
            entry.set("description", JsonValue(profile.description));
            entry.set("schemaVersion", JsonValue(profile.schemaVersion));
            entry.set("active", JsonValue(profile.name == profiles_->activeProfile()));
            list.push(std::move(entry));
        }
        return list;
    }

    if (command == "profile.active") {
        JsonValue result = JsonValue::makeObject();
        result.set("name", JsonValue(profiles_->activeProfile()));
        return result;
    }

    if (command == "profile.save") {
        status = profiles_->save(args["name"].asString(profiles_->activeProfile()),
                                 args["description"].asString());
        return okResult();
    }

    if (command == "profile.load") {
        status = profiles_->load(args["name"].asString());
        return status ? snapshot() : JsonValue();
    }

    if (command == "profile.delete") {
        status = profiles_->remove(args["name"].asString());
        return okResult();
    }

    if (command == "profile.rename") {
        status = profiles_->rename(args["from"].asString(), args["to"].asString());
        return okResult();
    }

    if (command == "profile.duplicate") {
        status = profiles_->duplicate(args["from"].asString(), args["to"].asString());
        return okResult();
    }

    // ── Skróty klawiszowe ───────────────────────────────────────────────────

    if (command == "hotkey.list") {
        JsonValue list = JsonValue::makeArray();
        for (const auto& binding : hotkeys_->bindings()) {
            JsonValue entry = JsonValue::makeObject();
            entry.set("id", JsonValue(binding.id));
            entry.set("combo", JsonValue(binding.combo));
            entry.set("action", JsonValue(binding.action));
            entry.set("args", binding.args);
            entry.set("enabled", JsonValue(binding.enabled));
            entry.set("registered", JsonValue(binding.registered));
            list.push(std::move(entry));
        }
        return list;
    }

    if (command == "hotkey.bind") {
        HotkeyBinding binding;
        binding.id      = args["id"].asString();
        binding.combo   = args["combo"].asString();
        binding.action  = args["action"].asString();
        binding.args    = args["args"];
        binding.enabled = args["enabled"].asBool(true);
        status = hotkeys_->bind(std::move(binding));
        return okResult();
    }

    if (command == "hotkey.unbind") {
        if (!hotkeys_->unbind(args["id"].asString())) status = Status::error("Nieznany skrót");
        return okResult();
    }

    if (command == "hotkey.trigger") {
        if (!hotkeys_->trigger(args["id"].asString())) status = Status::error("Nieznany skrót");
        return okResult();
    }

    // ── Automatyzacja ───────────────────────────────────────────────────────

    if (command == "automation.list") {
        JsonValue list = JsonValue::makeArray();
        for (const auto& rule : automation_.rules()) list.push(rule.toJson());
        return list;
    }

    if (command == "automation.add") {
        AutomationRule rule;
        std::string error;
        if (!AutomationRule::fromJson(args["rule"].isObject() ? args["rule"] : args, rule, error)) {
            status = Status::error("Nieprawidłowa reguła: " + error);
            return JsonValue();
        }
        status = automation_.addRule(std::move(rule));
        return okResult();
    }

    if (command == "automation.remove") {
        if (!automation_.removeRule(args["id"].asString())) status = Status::error("Nieznana reguła");
        return okResult();
    }

    if (command == "automation.setEnabled") {
        if (!automation_.setRuleEnabled(args["id"].asString(), args["value"].asBool(true)))
            status = Status::error("Nieznana reguła");
        return okResult();
    }

    // ── Wirtualne urządzenia ────────────────────────────────────────────────

    if (command == "virtual.list") {
        JsonValue list = JsonValue::makeArray();
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
            entry.set("underruns", JsonValue(static_cast<double>(endpoint.underruns)));
            entry.set("overruns", JsonValue(static_cast<double>(endpoint.overruns)));
            list.push(std::move(entry));
        }
        return list;
    }

    if (command == "virtual.setEnabled") {
        status = virtualDevices_->setEndpointEnabled(args["id"].asString(), args["enabled"].asBool(true));
        return okResult();
    }

    if (command == "virtual.bindOutput") {
        core::Bus* bus = resolveBus(args);
        status = virtualDevices_->bindOutput(args["id"].asString(),
                                             bus != nullptr ? bus->id() : kInvalidBus);
        return okResult();
    }

    if (command == "virtual.bindInput") {
        core::InputSource* source = resolveSource(args);
        status = virtualDevices_->bindInput(args["id"].asString(),
                                            source != nullptr ? source->id() : kInvalidSource);
        return okResult();
    }

    if (command == "virtual.driverStatus") {
        const virtualaudio::DriverStatus driver = virtualDevices_->driverStatus();
        JsonValue result = JsonValue::makeObject();
        result.set("state", JsonValue(virtualaudio::driverStateName(driver.state)));
        result.set("installedVersion", JsonValue(driver.installedVersion));
        result.set("requiredVersion", JsonValue(driver.requiredVersion));
        result.set("message", JsonValue(driver.message));
        result.set("requiresElevation", JsonValue(driver.requiresElevation));

        JsonValue endpoints = JsonValue::makeArray();
        for (const auto& endpoint : driver.endpoints) endpoints.push(JsonValue(endpoint));
        result.set("endpoints", std::move(endpoints));
        return result;
    }

    if (command == "virtual.install") {
        status = virtualDevices_->driver().install(args["package"].asString());
        return okResult();
    }

    if (command == "virtual.update") {
        status = virtualDevices_->driver().update(args["package"].asString());
        return okResult();
    }

    if (command == "virtual.uninstall") {
        status = virtualDevices_->driver().uninstall();
        return okResult();
    }

    // ── Pluginy ─────────────────────────────────────────────────────────────

    if (command == "plugins.list") {
        JsonValue list = JsonValue::makeArray();
        for (const auto& descriptor : dsp::PluginRegistry::instance().descriptors()) {
            JsonValue entry = JsonValue::makeObject();
            entry.set("type", JsonValue(descriptor.typeId));
            entry.set("name", JsonValue(descriptor.displayName));
            entry.set("category", JsonValue(descriptor.category));
            list.push(std::move(entry));
        }
        return list;
    }

    status = Status::error("Nieznana komenda: " + command);
    return JsonValue();
}

} // namespace helix::app
