#include "helix/core/ChannelManager.h"

#include <algorithm>

#include "helix/Log.h"
#include "helix/dsp/Equalizer.h"

namespace helix::core {

namespace {
constexpr const char* kLog = "ChannelManager";
} // namespace

const std::vector<DefaultChannelSpec>& defaultChannelLayout() {
    static const std::vector<DefaultChannelSpec> kLayout{
        {"Game",       "gamepad"},
        {"Chat",       "chat"},
        {"Music",      "music"},
        {"Media",      "media"},
        {"Microphone", "mic"},
        {"System",     "system"},
        {"Aux 1",      "aux"},
        {"Aux 2",      "aux"},
    };
    return kLayout;
}

ChannelManager::ChannelManager(AudioEngine& engine) : engine_(engine) {}

void ChannelManager::createDefaultLayout() {
    for (const auto& spec : defaultChannelLayout())
        createChannel(spec.name, spec.icon, true);
    engine_.commitGraph();
}

Channel* ChannelManager::createChannel(std::string name, std::string icon, bool withDefaultChain) {
    Channel* channel = engine_.addChannel(std::move(name));
    if (channel == nullptr) return nullptr;

    channel->setIcon(std::move(icon));
    channel->setVolume(0.8f);

    if (withDefaultChain) buildDefaultChain(channel->id());

    engine_.commitGraph();
    return channel;
}

bool ChannelManager::removeChannel(ChannelId id) {
    const bool removed = engine_.removeChannel(id);
    if (removed) engine_.commitGraph();
    return removed;
}

bool ChannelManager::renameChannel(ChannelId id, std::string name) {
    Channel* channel = channelOrNull(id);
    if (channel == nullptr) return false;
    channel->setName(std::move(name));
    return true;
}

bool ChannelManager::buildDefaultChain(ChannelId id) {
    Channel* channel = channelOrNull(id);
    if (channel == nullptr) return false;

    for (const auto& typeId : dsp::PluginRegistry::defaultChainOrder()) {
        PluginId created = 0;
        if (!addEffect(id, typeId, -1, &created)) {
            Log::warn(kLog, "Nie udało się utworzyć efektu: " + typeId);
            continue;
        }
        // Domyślnie aktywne są tylko efekty neutralne dla sygnału;
        // resztę użytkownik włącza świadomie (spec §19).
        const bool enabledByDefault = (typeId == "gain" || typeId == "limiter");
        setEffectEnabled(id, created, enabledByDefault);
    }
    return true;
}

bool ChannelManager::addEffect(ChannelId channelId, const std::string& typeId, int position, PluginId* created) {
    Channel* channel = channelOrNull(channelId);
    if (channel == nullptr) return false;

    dsp::PluginPtr plugin = dsp::PluginRegistry::instance().create(typeId);
    if (!plugin) return false;

    const PluginId id = engine_.nextPluginId();
    plugin->setInstanceId(id);

    if (!channel->effects().insert(std::move(plugin), position)) return false;
    if (created != nullptr) *created = id;
    return true;
}

bool ChannelManager::removeEffect(ChannelId channelId, PluginId pluginId) {
    Channel* channel = channelOrNull(channelId);
    if (channel == nullptr) return false;

    dsp::PluginPtr detached = channel->effects().detach(pluginId);
    if (!detached) return false;

    // Zwolnienie dopiero po barierze bloków — wątek audio może jeszcze być w środku.
    dsp::AudioPlugin* raw = detached.release();
    engine_.retire([raw] { delete raw; });
    return true;
}

bool ChannelManager::reorderEffects(ChannelId channelId, const std::vector<PluginId>& order) {
    Channel* channel = channelOrNull(channelId);
    if (channel == nullptr) return false;
    return channel->effects().reorder(order);
}

bool ChannelManager::setEffectEnabled(ChannelId channelId, PluginId pluginId, bool enabled) {
    Channel* channel = channelOrNull(channelId);
    if (channel == nullptr) return false;
    dsp::AudioPlugin* plugin = channel->effects().find(pluginId);
    if (plugin == nullptr) return false;
    plugin->setEnabled(enabled);
    return true;
}

bool ChannelManager::setEffectParameter(ChannelId channelId, PluginId pluginId,
                                        const std::string& parameter, float value) {
    Channel* channel = channelOrNull(channelId);
    if (channel == nullptr) return false;
    dsp::AudioPlugin* plugin = channel->effects().find(pluginId);
    if (plugin == nullptr) return false;

    if (!plugin->setParameter(parameter, value)) return false;

    // Gain kanału jest widoczny również jako parametr kanału (spec §4).
    if (std::string_view(plugin->typeId()) == "gain" && parameter == "gain")
        channel->setGainDb(value);
    return true;
}

bool ChannelManager::clearEffects(ChannelId channelId) {
    Channel* channel = channelOrNull(channelId);
    if (channel == nullptr) return false;

    for (dsp::AudioPlugin* plugin : channel->effects().ordered())
        removeEffect(channelId, plugin->instanceId());
    return true;
}

std::vector<EffectState> ChannelManager::effectStates(ChannelId channelId) const {
    std::vector<EffectState> result;
    Channel* channel = channelOrNull(channelId);
    if (channel == nullptr) return result;

    for (dsp::AudioPlugin* plugin : channel->effects().ordered()) {
        EffectState state;
        state.id            = plugin->instanceId();
        state.typeId        = plugin->typeId();
        state.displayName   = plugin->displayName();
        state.enabled       = plugin->isEnabled();
        state.latencyFrames = plugin->latencyFrames();
        state.parameters    = plugin->getParameters();
        state.values.reserve(state.parameters.size());
        for (const auto& parameter : state.parameters)
            state.values.push_back(plugin->getParameter(parameter.id));
        result.push_back(std::move(state));
    }
    return result;
}

ChannelState ChannelManager::stateOf(const Channel& channel) const {
    ChannelState state;
    state.id      = channel.id();
    state.index   = channel.index();
    state.name    = channel.name();
    state.icon    = channel.icon();
    state.volume  = channel.volume();
    state.gainDb  = channel.gainDb();
    state.pan     = channel.pan();
    state.muted   = channel.muted();
    state.solo    = channel.solo();
    state.enabled = channel.enabled();

    for (const InputSource* source : engine_.sources())
        if (source->channel() == channel.id()) state.sources.push_back(source->id());

    for (const Bus* bus : engine_.buses())
        if (engine_.routing().isEnabled(channel.index(), bus->index()))
            state.outputs.push_back(bus->id());

    return state;
}

std::vector<ChannelState> ChannelManager::states() const {
    std::vector<ChannelState> result;
    for (const Channel* channel : engine_.channels())
        result.push_back(stateOf(*channel));
    return result;
}

std::vector<double> ChannelManager::equalizerResponse(ChannelId channelId, PluginId pluginId,
                                                      const std::vector<double>& frequencies) const {
    Channel* channel = channelOrNull(channelId);
    if (channel == nullptr) return {};
    dsp::AudioPlugin* plugin = channel->effects().find(pluginId);
    if (plugin == nullptr) return {};

    auto* equalizer = dynamic_cast<dsp::EqualizerPlugin*>(plugin);
    if (equalizer == nullptr) return {};
    return equalizer->magnitudeResponseDb(frequencies);
}

void ChannelManager::collectGarbage() { engine_.collectGarbage(); }

Channel* ChannelManager::channelOrNull(ChannelId id) const { return engine_.findChannel(id); }

} // namespace helix::core
