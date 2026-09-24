#include "helix/dsp/PluginRegistry.h"

#include "helix/dsp/Compressor.h"
#include "helix/dsp/DeEsser.h"
#include "helix/dsp/Equalizer.h"
#include "helix/dsp/Gain.h"
#include "helix/dsp/Limiter.h"
#include "helix/dsp/NoiseGate.h"
#include "helix/dsp/NoiseSuppression.h"

namespace helix::dsp {

PluginRegistry& PluginRegistry::instance() {
    static PluginRegistry registry;
    return registry;
}

PluginRegistry::PluginRegistry() {
    registerType({"gain", "Gain", "utility"},
                 [] { return PluginPtr(new GainPlugin()); });
    registerType({"noisegate", "Noise Gate", "dynamics"},
                 [] { return PluginPtr(new NoiseGatePlugin()); });
    registerType({"noisesuppression", "Noise Suppression", "restoration"},
                 [] { return PluginPtr(new NoiseSuppressionPlugin()); });
    registerType({"equalizer", "Equalizer", "eq"},
                 [] { return PluginPtr(new EqualizerPlugin()); });
    registerType({"compressor", "Compressor", "dynamics"},
                 [] { return PluginPtr(new CompressorPlugin()); });
    registerType({"deesser", "De-Esser", "dynamics"},
                 [] { return PluginPtr(new DeEsserPlugin()); });
    registerType({"limiter", "Limiter", "dynamics"},
                 [] { return PluginPtr(new LimiterPlugin()); });
}

void PluginRegistry::registerType(PluginDescriptor descriptor, Factory factory) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string key = descriptor.typeId;
    factories_[key] = std::make_pair(std::move(descriptor), std::move(factory));
}

PluginPtr PluginRegistry::create(const std::string& typeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = factories_.find(typeId);
    if (it == factories_.end()) return nullptr;
    return it->second.second();
}

std::vector<PluginDescriptor> PluginRegistry::descriptors() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PluginDescriptor> result;
    result.reserve(factories_.size());
    for (const auto& [key, value] : factories_) result.push_back(value.first);
    return result;
}

bool PluginRegistry::contains(const std::string& typeId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return factories_.find(typeId) != factories_.end();
}

std::vector<std::string> PluginRegistry::defaultChainOrder() {
    // Kolejność zgodna ze specyfikacją §8.
    return {"gain", "noisegate", "noisesuppression", "equalizer", "compressor", "deesser", "limiter"};
}

} // namespace helix::dsp
