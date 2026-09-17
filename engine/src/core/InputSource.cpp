#include "helix/core/InputSource.h"

#include <algorithm>

namespace helix::core {

InputSource::InputSource(SourceId id, SourceKind kind, std::string name)
    : id_(id), kind_(kind), name_(std::move(name)) {}

void InputSource::prepare(int channels, double sampleRate, int ringFrames) {
    channels_   = std::clamp(channels, 1, kMaxStreamChannels);
    sampleRate_ = sampleRate > 0.0 ? sampleRate : kDefaultSampleRate;
    ring_.reset(channels_, std::max(ringFrames, 1024));
    meter_.prepare(sampleRate_);
}

} // namespace helix::core
