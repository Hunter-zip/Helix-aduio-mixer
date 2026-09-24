#include "helix/core/Channel.h"

#include <algorithm>
#include <cmath>

namespace helix::core {

namespace {
/// Prawo balansu: w środku oba kanały na 0 dB, skręcanie tłumi przeciwną stronę.
inline void balanceGains(float pan, float& left, float& right) noexcept {
    left  = (pan <= 0.0f) ? 1.0f : std::max(0.0f, 1.0f - pan);
    right = (pan >= 0.0f) ? 1.0f : std::max(0.0f, 1.0f + pan);
}
} // namespace

Channel::Channel(ChannelId id, int index, std::string name)
    : id_(id), index_(index), name_(std::move(name)) {}

void Channel::setVolume(float linear) noexcept {
    volumeTarget_.store(std::clamp(linear, 0.0f, 4.0f), std::memory_order_relaxed);
}

void Channel::setGainDb(float db) noexcept {
    const float clamped = std::clamp(db, -40.0f, 40.0f);
    gainDb_.store(clamped, std::memory_order_relaxed);
    // Gain jest elementem łańcucha (spec §8) — trzymamy oba widoki w zgodzie.
    if (auto* plugin = effects_.findByType("gain"))
        plugin->setParameter("gain", clamped);
}

void Channel::setPan(float pan) noexcept {
    panTarget_.store(std::clamp(pan, -1.0f, 1.0f), std::memory_order_relaxed);
}

void Channel::prepare(const dsp::ProcessContext& context) {
    buffer_.resize(context.channels, context.maxBlockSize);
    buffer_.clear();
    activeFrames_ = 0;

    volumeSmoothed_.prepare(context.sampleRate, 20.0, volumeTarget_.load(std::memory_order_relaxed));
    volumeSmoothed_.snapToTarget();
    panSmoothed_.prepare(context.sampleRate, 20.0, panTarget_.load(std::memory_order_relaxed));
    panSmoothed_.snapToTarget();
    muteSmoothed_.prepare(context.sampleRate, 12.0, muted_.load(std::memory_order_relaxed) ? 0.0f : 1.0f);
    muteSmoothed_.snapToTarget();

    inputMeter_.prepare(context.sampleRate);
    outputMeter_.prepare(context.sampleRate);

    effects_.reinitialize(context);
}

void Channel::beginBlock(int frames) noexcept {
    activeFrames_ = std::min(frames, buffer_.capacity());
    buffer_.setActiveFrames(activeFrames_);
    buffer_.view(activeFrames_).clear();
}

void Channel::processBlock(bool muteOverride) noexcept {
    const int frames = activeFrames_;
    if (frames <= 0) return;

    AudioBufferView view = buffer_.view(frames);

    inputMeter_.process(view);

    if (enabled_.load(std::memory_order_relaxed))
        effects_.process(view);

    // Głośność, pan i mute w jednym przebiegu — jedna rampa, zero trzasków.
    volumeSmoothed_.setTarget(volumeTarget_.load(std::memory_order_relaxed));
    volumeSmoothed_.updateTarget();
    panSmoothed_.setTarget(panTarget_.load(std::memory_order_relaxed));
    panSmoothed_.updateTarget();

    const bool silence = muteOverride || muted_.load(std::memory_order_relaxed) ||
                         !enabled_.load(std::memory_order_relaxed);
    muteSmoothed_.setTarget(silence ? 0.0f : 1.0f);
    muteSmoothed_.updateTarget();

    const float volStart  = volumeSmoothed_.current();
    const float volEnd    = volumeSmoothed_.valueAfter(frames);
    const float panStart  = panSmoothed_.current();
    const float panEnd    = panSmoothed_.valueAfter(frames);
    const float muteStart = muteSmoothed_.current();
    const float muteEnd   = muteSmoothed_.valueAfter(frames);

    const float inv = 1.0f / static_cast<float>(frames);
    const float volStep  = (volEnd  - volStart)  * inv;
    const float panStep  = (panEnd  - panStart)  * inv;
    const float muteStep = (muteEnd - muteStart) * inv;

    const int channels = view.channels();
    if (channels >= 2) {
        Sample* left  = view.channel(0);
        Sample* right = view.channel(1);
        float vol = volStart, pan = panStart, mute = muteStart;
        for (int i = 0; i < frames; ++i) {
            float gl, gr;
            balanceGains(pan, gl, gr);
            const float g = vol * mute;
            left[i]  *= g * gl;
            right[i] *= g * gr;
            vol  += volStep;
            pan  += panStep;
            mute += muteStep;
        }
        // Kanały powyżej stereo dostają samo volume/mute.
        for (int c = 2; c < channels; ++c) {
            Sample* data = view.channel(c);
            float v = volStart, m = muteStart;
            for (int i = 0; i < frames; ++i) {
                data[i] *= v * m;
                v += volStep;
                m += muteStep;
            }
        }
    } else {
        for (int c = 0; c < channels; ++c) {
            Sample* data = view.channel(c);
            float v = volStart, m = muteStart;
            for (int i = 0; i < frames; ++i) {
                data[i] *= v * m;
                v += volStep;
                m += muteStep;
            }
        }
    }

    volumeSmoothed_.skip(frames);
    panSmoothed_.skip(frames);
    muteSmoothed_.skip(frames);

    outputMeter_.process(view);
}

} // namespace helix::core
