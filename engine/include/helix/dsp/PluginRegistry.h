// Fabryka efektów (spec §25) — nowe typy dokłada się rejestracją, bez zmian w silniku.
#pragma once

#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "helix/dsp/AudioPlugin.h"

namespace helix::dsp {

/// Opis typu efektu dla GUI.
struct PluginDescriptor {
    std::string typeId;
    std::string displayName;
    std::string category;
};

class PluginRegistry {
public:
    using Factory = std::function<PluginPtr()>;

    static PluginRegistry& instance();

    /// Rejestruje typ efektu. Nadpisuje istniejący wpis o tym samym typeId.
    void registerType(PluginDescriptor descriptor, Factory factory);

    [[nodiscard]] PluginPtr create(const std::string& typeId) const;

    [[nodiscard]] std::vector<PluginDescriptor> descriptors() const;

    [[nodiscard]] bool contains(const std::string& typeId) const;

    /// Kolejność domyślnego łańcucha kanału (spec §8).
    [[nodiscard]] static std::vector<std::string> defaultChainOrder();

private:
    PluginRegistry();

    mutable std::mutex mutex_;
    std::map<std::string, std::pair<PluginDescriptor, Factory>> factories_;
};

} // namespace helix::dsp
