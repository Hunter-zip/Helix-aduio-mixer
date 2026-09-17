#include "helix/core/Bus.h"

#include <algorithm>

namespace helix::core {

Bus::Bus(BusId id, int index, std::string name, std::string label, BusKind kind)
    : id_(id), index_(index), name_(std::move(name)), label_(std::move(label)), kind_(kind) {}

void Bus::setVolume(float linear) noexcept {
    volumeTarget_.store(std::clamp(linear, 0.0f, 4.0f), std::memory_order_relaxed);
}

void Bus::prepare(const dsp::ProcessContext& context, int ringFrames) {
    channels_ = context.channels;
    mix_.resize(channels_, context.maxBlockSize);
    mix_.clear();
    activeFrames_ = 0;

    volumeSmoothed_.prepare(context.sampleRate, 20.0, volumeTarget_.load(std::memory_order_relaxed));
    volumeSmoothed_.snapToTarget();
    muteSmoothed_.prepare(context.sampleRate, 12.0, muted_.load(std::memory_order_relaxed) ? 0.0f : 1.0f);
    muteSmoothed_.snapToTarget();

    limiter_.initialize(context);
    meter_.prepare(context.sampleRate);

    output_.reset(channels_, std::max(ringFrames, context.maxBlockSize * 4));
    interleaveScratch_.assign(
        static_cast<std::size_t>(context.maxBlockSize) * static_cast<std::size_t>(channels_), 0.0f);
}

void Bus::beginBlock(int frames) noexcept {
    activeFrames_ = std::min(frames, mix_.capacity());
    mix_.setActiveFrames(activeFrames_);
    mix_.view(activeFrames_).clear();
}

void Bus::finishBlock(float masterStart, float masterEnd) noexcept {
    const int frames = activeFrames_;
    if (frames <= 0) return;

    AudioBufferView view = mix_.view(frames);

    volumeSmoothed_.setTarget(volumeTarget_.load(std::memory_order_relaxed));
    volumeSmoothed_.updateTarget();
    muteSmoothed_.setTarget(muted_.load(std::memory_order_relaxed) ? 0.0f : 1.0f);
    muteSmoothed_.updateTarget();

    const float volStart  = volumeSmoothed_.current()          * masterStart;
    const float volEnd    = volumeSmoothed_.valueAfter(frames) * masterEnd;
    const float muteStart = muteSmoothed_.current();
    const float muteEnd   = muteSmoothed_.valueAfter(frames);

    const float inv = 1.0f / static_cast<float>(frames);
    const float volStep  = (volEnd  - volStart)  * inv;
    const float muteStep = (muteEnd - muteStart) * inv;

    for (int c = 0; c < view.channels(); ++c) {
        Sample* data = view.channel(c);
        float v = volStart, m = muteStart;
        for (int i = 0; i < frames; ++i) {
            data[i] *= v * m;
            v += volStep;
            m += muteStep;
        }
    }

    volumeSmoothed_.skip(frames);
    muteSmoothed_.skip(frames);

    if (limiterEnabled_.load(std::memory_order_relaxed))
        limiter_.process(view);

    meter_.process(view);
}

void Bus::pushToDevice() noexcept {
    const int frames = activeFrames_;
    if (frames <= 0) return;

    AudioBufferView view = mix_.view(frames);
    const int channels = view.channels();
    Sample* scratch = interleaveScratch_.data();

    for (int c = 0; c < channels; ++c) {
        const Sample* src = view.channel(c);
        for (int i = 0; i < frames; ++i)
            scratch[static_cast<std::size_t>(i) * static_cast<std::size_t>(channels)
                    + static_cast<std::size_t>(c)] = src[i];
    }

    output_.write(scratch, frames);
}

} // namespace helix::core
