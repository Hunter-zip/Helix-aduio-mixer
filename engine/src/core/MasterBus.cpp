#include "helix/core/MasterBus.h"

#include <algorithm>

namespace helix::core {

void MasterBus::prepare(const dsp::ProcessContext& context) {
    volumeSmoothed_.prepare(context.sampleRate, 25.0, volumeTarget_.load(std::memory_order_relaxed));
    volumeSmoothed_.snapToTarget();
    muteSmoothed_.prepare(context.sampleRate, 12.0, muted_.load(std::memory_order_relaxed) ? 0.0f : 1.0f);
    muteSmoothed_.snapToTarget();
    meter_.prepare(context.sampleRate);
}

void MasterBus::setVolume(float linear) noexcept {
    volumeTarget_.store(std::clamp(linear, 0.0f, 2.0f), std::memory_order_relaxed);
}

std::pair<float, float> MasterBus::gainRamp(int frames) noexcept {
    volumeSmoothed_.setTarget(volumeTarget_.load(std::memory_order_relaxed));
    volumeSmoothed_.updateTarget();
    muteSmoothed_.setTarget(muted_.load(std::memory_order_relaxed) ? 0.0f : 1.0f);
    muteSmoothed_.updateTarget();

    const float start = volumeSmoothed_.current() * muteSmoothed_.current();
    const float end   = volumeSmoothed_.valueAfter(frames) * muteSmoothed_.valueAfter(frames);

    volumeSmoothed_.skip(frames);
    muteSmoothed_.skip(frames);
    return {start, end};
}

} // namespace helix::core
