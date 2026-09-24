// Zarządzanie kanałami i ich łańcuchami DSP (spec §4, §8, §26).
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "helix/Types.h"
#include "helix/core/AudioEngine.h"
#include "helix/dsp/PluginRegistry.h"

namespace helix::core {

/// Opis efektu w łańcuchu — migawka dla GUI.
struct EffectState {
    PluginId    id = 0;
    std::string typeId;
    std::string displayName;
    bool        enabled = true;
    int         latencyFrames = 0;
    std::vector<dsp::PluginParameter> parameters;
    std::vector<float> values;
};

/// Domyślny zestaw kanałów wg specyfikacji §4.
struct DefaultChannelSpec {
    const char* name;
    const char* icon;
};

[[nodiscard]] const std::vector<DefaultChannelSpec>& defaultChannelLayout();

class ChannelManager {
public:
    explicit ChannelManager(AudioEngine& engine);

    /// Tworzy komplet domyślnych kanałów (Game, Chat, Music, …).
    void createDefaultLayout();

    /// Nowy kanał; `withDefaultChain` dokłada standardowy łańcuch efektów.
    Channel* createChannel(std::string name, std::string icon = "channel", bool withDefaultChain = true);
    bool removeChannel(ChannelId id);
    bool renameChannel(ChannelId id, std::string name);

    /// Buduje standardowy łańcuch (spec §8) na istniejącym kanale.
    bool buildDefaultChain(ChannelId id);

    bool addEffect(ChannelId channel, const std::string& typeId, int position = -1, PluginId* created = nullptr);
    bool removeEffect(ChannelId channel, PluginId plugin);
    bool reorderEffects(ChannelId channel, const std::vector<PluginId>& order);
    bool setEffectEnabled(ChannelId channel, PluginId plugin, bool enabled);
    bool setEffectParameter(ChannelId channel, PluginId plugin, const std::string& parameter, float value);
    bool clearEffects(ChannelId channel);

    [[nodiscard]] std::vector<EffectState> effectStates(ChannelId channel) const;
    [[nodiscard]] ChannelState stateOf(const Channel& channel) const;
    [[nodiscard]] std::vector<ChannelState> states() const;

    /// Charakterystyka EQ dla wykresu w GUI (spec §9). Pusty wynik = brak EQ.
    [[nodiscard]] std::vector<double> equalizerResponse(ChannelId channel, PluginId plugin,
                                                        const std::vector<double>& frequencies) const;

    /// Zwalnia pluginy odstawione przez wątek audio.
    void collectGarbage();

private:
    [[nodiscard]] Channel* channelOrNull(ChannelId id) const;

    AudioEngine& engine_;
};

} // namespace helix::core
