#include <cmath>
#include <cstdint>
#include <vector>

#include "TestFramework.h"
#include "helix/AudioBuffer.h"
#include "helix/dsp/Compressor.h"
#include "helix/dsp/DeEsser.h"
#include "helix/dsp/EffectChain.h"
#include "helix/dsp/Equalizer.h"
#include "helix/dsp/Fft.h"
#include "helix/dsp/Gain.h"
#include "helix/dsp/Limiter.h"
#include "helix/dsp/NoiseGate.h"
#include "helix/dsp/NoiseSuppression.h"
#include "helix/dsp/PluginRegistry.h"
#include "helix/dsp/Resampler.h"

using namespace helix;
using namespace helix::dsp;

namespace {

constexpr double kSampleRate = 48000.0;
constexpr double kTwoPi = 6.283185307179586476925286766559;

/// Wypełnia bufor sinusem o zadanej częstotliwości i amplitudzie.
void fillSine(AudioBuffer& buffer, double frequency, float amplitude, double startPhase = 0.0) {
    for (int c = 0; c < buffer.channels(); ++c) {
        Sample* data = buffer.channel(c);
        for (int i = 0; i < buffer.frames(); ++i)
            data[i] = amplitude * static_cast<float>(
                std::sin(startPhase + kTwoPi * frequency * i / kSampleRate));
    }
}

float peakOf(const AudioBuffer& buffer) {
    float peak = 0.0f;
    for (int c = 0; c < buffer.channels(); ++c)
        for (int i = 0; i < buffer.frames(); ++i)
            peak = std::max(peak, std::fabs(buffer.channel(c)[i]));
    return peak;
}

float rmsOf(const AudioBuffer& buffer, int channel, int from = 0) {
    double sum = 0.0;
    int count = 0;
    for (int i = from; i < buffer.frames(); ++i) {
        const double v = buffer.channel(channel)[i];
        sum += v * v;
        ++count;
    }
    return count > 0 ? static_cast<float>(std::sqrt(sum / count)) : 0.0f;
}

ProcessContext context(int block = 512, int channels = 2) {
    return ProcessContext{kSampleRate, block, channels};
}

/// Przepuszcza sygnał przez plugin w kilku blokach, żeby obwiednie się ustaliły.
void runBlocks(AudioPlugin& plugin, AudioBuffer& buffer, int blocks) {
    for (int i = 0; i < blocks; ++i) {
        AudioBufferView view = buffer.view();
        plugin.process(view);
    }
}

} // namespace

TEST("dsp/fft odtwarza sygnał po transformacie odwrotnej") {
    Fft fft;
    fft.prepare(8);
    const int size = fft.size();

    std::vector<float> real(static_cast<std::size_t>(size));
    std::vector<float> imag(static_cast<std::size_t>(size), 0.0f);
    for (int i = 0; i < size; ++i)
        real[static_cast<std::size_t>(i)] =
            static_cast<float>(std::sin(kTwoPi * 5.0 * i / size) + 0.3 * std::cos(kTwoPi * 11.0 * i / size));

    const std::vector<float> original = real;
    fft.forward(real.data(), imag.data());
    fft.inverse(real.data(), imag.data());

    for (int i = 0; i < size; ++i)
        CHECK_NEAR(real[static_cast<std::size_t>(i)], original[static_cast<std::size_t>(i)], 1e-4);
}

TEST("dsp/fft wskazuje właściwy prążek") {
    Fft fft;
    fft.prepare(9);
    const int size = fft.size();

    std::vector<float> real(static_cast<std::size_t>(size));
    std::vector<float> imag(static_cast<std::size_t>(size), 0.0f);
    constexpr int kBin = 17;
    for (int i = 0; i < size; ++i)
        real[static_cast<std::size_t>(i)] = static_cast<float>(std::sin(kTwoPi * kBin * i / size));

    fft.forward(real.data(), imag.data());

    int strongest = 0;
    double best = 0.0;
    for (int k = 1; k < size / 2; ++k) {
        const double magnitude = std::hypot(real[static_cast<std::size_t>(k)],
                                            imag[static_cast<std::size_t>(k)]);
        if (magnitude > best) { best = magnitude; strongest = k; }
    }
    CHECK_EQ(strongest, kBin);
}

TEST("dsp/biquad — charakterystyka filtra dolnoprzepustowego") {
    const auto coefficients =
        BiquadCoefficients::design(FilterType::HighCut, 1000.0, 0.7071, 0.0, kSampleRate);

    // W paśmie przepustowym ~0 dB, na częstotliwości granicznej ~-3 dB, wyżej mocne tłumienie.
    CHECK_NEAR(coefficients.magnitudeDb(100.0, kSampleRate), 0.0, 0.3);
    CHECK_NEAR(coefficients.magnitudeDb(1000.0, kSampleRate), -3.0, 0.6);
    CHECK(coefficients.magnitudeDb(10000.0, kSampleRate) < -30.0);
}

TEST("dsp/biquad — bell podnosi tylko swoje pasmo") {
    const auto coefficients =
        BiquadCoefficients::design(FilterType::Bell, 1000.0, 2.0, 6.0, kSampleRate);
    CHECK_NEAR(coefficients.magnitudeDb(1000.0, kSampleRate), 6.0, 0.15);
    CHECK_NEAR(coefficients.magnitudeDb(100.0, kSampleRate), 0.0, 0.6);
    CHECK_NEAR(coefficients.magnitudeDb(10000.0, kSampleRate), 0.0, 0.6);
}

TEST("dsp/gain — zmiana poziomu bez trzasków") {
    GainPlugin gain;
    gain.initialize(context());
    gain.setParameter("gain", -6.0f);

    AudioBuffer buffer(2, 512);
    fillSine(buffer, 440.0, 0.5f);

    // Rampa gainu trwa 15 ms — mierzymy dopiero po jej zakończeniu.
    runBlocks(gain, buffer, 4);
    fillSine(buffer, 440.0, 0.5f);
    AudioBufferView view = buffer.view();
    gain.process(view);

    CHECK_NEAR(peakOf(buffer), 0.5f * dbToGain(-6.0f), 0.01);
}

TEST("dsp/gain — rampa nie tworzy skoku między blokami") {
    GainPlugin gain;
    gain.initialize(context(256));

    AudioBuffer buffer(1, 256);
    for (int i = 0; i < buffer.frames(); ++i) buffer.channel(0)[i] = 1.0f;

    gain.setParameter("gain", -40.0f);
    AudioBufferView view = buffer.view();
    gain.process(view);

    // Sąsiednie próbki nie mogą różnić się gwałtownie — to definicja braku trzasku.
    for (int i = 1; i < buffer.frames(); ++i) {
        const float step = std::fabs(buffer.channel(0)[i] - buffer.channel(0)[i - 1]);
        CHECK_MSG(step < 0.05f, "skok wzmocnienia: " + std::to_string(step));
    }
}

TEST("dsp/equalizer — pasmo bell podbija sygnał") {
    EqualizerPlugin eq;
    eq.initialize(context());
    eq.setParameter("band4.enabled", 1.0f);
    eq.setParameter("band4.type", static_cast<float>(FilterType::Bell));
    eq.setParameter("band4.freq", 1000.0f);
    eq.setParameter("band4.gain", 12.0f);
    eq.setParameter("band4.q", 1.0f);

    AudioBuffer buffer(2, 4096);
    fillSine(buffer, 1000.0, 0.2f);
    AudioBufferView view = buffer.view();
    eq.process(view);

    // Po ustabilizowaniu filtra amplituda ma wzrosnąć o ~12 dB.
    const float measured = rmsOf(buffer, 0, 2048);
    const float expected = 0.2f / std::sqrt(2.0f) * dbToGain(12.0f);
    CHECK_NEAR(measured, expected, expected * 0.1f);
}

TEST("dsp/equalizer — charakterystyka dla wykresu GUI") {
    EqualizerPlugin eq;
    eq.initialize(context());
    eq.setParameter("band4.enabled", 1.0f);
    eq.setParameter("band4.gain", 6.0f);
    eq.setParameter("band4.freq", 1000.0f);
    eq.setParameter("band4.q", 1.0f);

    const std::vector<double> frequencies{100.0, 1000.0, 10000.0};
    const auto response = eq.magnitudeResponseDb(frequencies);

    CHECK_EQ(response.size(), std::size_t{3});
    CHECK_NEAR(response[1], 6.0, 0.2);
    CHECK_NEAR(response[0], 0.0, 1.0);
    CHECK_NEAR(response[2], 0.0, 1.0);
}

TEST("dsp/equalizer — wyłączone pasma nie zmieniają sygnału") {
    EqualizerPlugin eq;
    eq.initialize(context());

    AudioBuffer buffer(2, 512);
    fillSine(buffer, 440.0, 0.4f);
    const float before = peakOf(buffer);

    AudioBufferView view = buffer.view();
    eq.process(view);
    CHECK_NEAR(peakOf(buffer), before, 1e-6);
}

TEST("dsp/kompresor — redukuje sygnał powyżej progu") {
    CompressorPlugin compressor;
    compressor.initialize(context());
    compressor.setParameter("threshold", -20.0f);
    compressor.setParameter("ratio", 4.0f);
    compressor.setParameter("attack", 1.0f);
    compressor.setParameter("release", 50.0f);
    compressor.setParameter("knee", 0.0f);
    compressor.setParameter("makeup", 0.0f);

    AudioBuffer buffer(2, 4096);
    fillSine(buffer, 200.0, 0.5f);   // ≈ -6 dBFS peak
    runBlocks(compressor, buffer, 1);

    fillSine(buffer, 200.0, 0.5f);
    AudioBufferView view = buffer.view();
    compressor.process(view);

    // -6 dBFS przy progu -20 dB i ratio 4:1 → ~14 dB ponad progiem, redukcja ~10,5 dB.
    CHECK_NEAR(compressor.gainReductionDb(), 10.5, 1.5);
    CHECK(peakOf(buffer) < 0.5f);
}

TEST("dsp/kompresor — sygnał poniżej progu przechodzi bez zmian") {
    CompressorPlugin compressor;
    compressor.initialize(context());
    compressor.setParameter("threshold", -6.0f);
    compressor.setParameter("ratio", 4.0f);
    compressor.setParameter("makeup", 0.0f);
    compressor.setParameter("knee", 0.0f);

    AudioBuffer buffer(2, 2048);
    fillSine(buffer, 200.0, 0.05f);   // ≈ -26 dBFS
    runBlocks(compressor, buffer, 2);

    fillSine(buffer, 200.0, 0.05f);
    AudioBufferView view = buffer.view();
    compressor.process(view);

    CHECK_NEAR(peakOf(buffer), 0.05f, 0.002f);
    CHECK_NEAR(compressor.gainReductionDb(), 0.0, 0.2);
}

TEST("dsp/limiter — nic nie przekracza sufitu") {
    LimiterPlugin limiter;
    limiter.initialize(context(1024));
    limiter.setParameter("ceiling", -1.0f);
    limiter.setParameter("lookahead", 2.0f);
    limiter.setParameter("release", 50.0f);

    const float ceiling = dbToGain(-1.0f);

    AudioBuffer buffer(2, 1024);
    for (int block = 0; block < 8; ++block) {
        fillSine(buffer, 120.0, 2.5f, block * 0.37);   // mocno przesterowany sygnał
        AudioBufferView view = buffer.view();
        limiter.process(view);
        CHECK_MSG(peakOf(buffer) <= ceiling + 1e-5f,
                  "przekroczony sufit: " + std::to_string(peakOf(buffer)));
    }
    CHECK(limiter.gainReductionDb() > 5.0f);
}

TEST("dsp/limiter — zgłasza opóźnienie look-ahead") {
    LimiterPlugin limiter;
    limiter.initialize(context(512));
    limiter.setParameter("lookahead", 2.0f);

    AudioBuffer buffer(2, 512);
    AudioBufferView view = buffer.view();
    limiter.process(view);   // przeliczenie współczynników

    const int expected = static_cast<int>(0.002 * kSampleRate);
    CHECK_NEAR(limiter.latencyFrames(), expected, 2);
}

TEST("dsp/limiter — cichy sygnał przechodzi bez tłumienia") {
    LimiterPlugin limiter;
    limiter.initialize(context(512));
    limiter.setParameter("ceiling", -1.0f);
    limiter.setParameter("lookahead", 1.0f);

    AudioBuffer buffer(2, 512);
    fillSine(buffer, 500.0, 0.2f);
    runBlocks(limiter, buffer, 4);

    fillSine(buffer, 500.0, 0.2f);
    AudioBufferView view = buffer.view();
    limiter.process(view);

    CHECK_NEAR(peakOf(buffer), 0.2f, 0.005f);
    CHECK_NEAR(limiter.gainReductionDb(), 0.0, 0.1);
}

TEST("dsp/bramka szumów — tłumi ciszę i otwiera się na sygnale") {
    NoiseGatePlugin gate;
    gate.initialize(context(1024));
    gate.setParameter("threshold", -30.0f);
    gate.setParameter("attack", 1.0f);
    gate.setParameter("hold", 5.0f);
    gate.setParameter("release", 10.0f);
    gate.setParameter("range", -60.0f);

    AudioBuffer buffer(2, 1024);

    // Szum na poziomie -50 dBFS — poniżej progu.
    for (int block = 0; block < 20; ++block) {
        fillSine(buffer, 300.0, 0.003f, block * 0.11);
        AudioBufferView view = buffer.view();
        gate.process(view);
    }
    CHECK_MSG(peakOf(buffer) < 0.0005f, "bramka nie zamknęła się: " + std::to_string(peakOf(buffer)));

    // Głośny sygnał — bramka musi się otworzyć.
    for (int block = 0; block < 10; ++block) {
        fillSine(buffer, 300.0, 0.4f, block * 0.11);
        AudioBufferView view = buffer.view();
        gate.process(view);
    }
    CHECK(peakOf(buffer) > 0.3f);
}

TEST("dsp/de-esser — tłumi pasmo wysokie, zostawia niskie") {
    DeEsserPlugin deesser;
    deesser.initialize(context(2048));
    deesser.setParameter("frequency", 6000.0f);
    deesser.setParameter("threshold", -40.0f);
    deesser.setParameter("ratio", 10.0f);

    AudioBuffer high(2, 2048);
    for (int block = 0; block < 6; ++block) {
        fillSine(high, 9000.0, 0.5f, block * 0.21);
        AudioBufferView view = high.view();
        deesser.process(view);
    }
    const float highLevel = rmsOf(high, 0, 1024);

    DeEsserPlugin deesserLow;
    deesserLow.initialize(context(2048));
    deesserLow.setParameter("frequency", 6000.0f);
    deesserLow.setParameter("threshold", -40.0f);
    deesserLow.setParameter("ratio", 10.0f);

    AudioBuffer low(2, 2048);
    for (int block = 0; block < 6; ++block) {
        fillSine(low, 300.0, 0.5f, block * 0.21);
        AudioBufferView view = low.view();
        deesserLow.process(view);
    }
    const float lowLevel = rmsOf(low, 0, 1024);

    CHECK_MSG(highLevel < lowLevel * 0.5f,
              "pasmo wysokie nie zostało stłumione (" + std::to_string(highLevel) + " vs " +
                  std::to_string(lowLevel) + ")");
    CHECK(deesser.gainReductionDb() > 5.0f);
}

TEST("dsp/redukcja szumów — uczy się tła i przepuszcza sygnał użyteczny") {
    NoiseSuppressionPlugin suppressor;
    suppressor.initialize(context(512, 1));
    suppressor.setParameter("strength", 1.0f);
    suppressor.setParameter("quality", 0.0f);

    // Deterministyczny „szum” — prosty generator liniowy, powtarzalny w CI.
    std::uint32_t seed = 12345u;
    auto noiseSample = [&seed] {
        seed = seed * 1664525u + 1013904223u;
        return (static_cast<float>(seed >> 8) / 8388608.0f - 1.0f) * 0.02f;   // ≈ -40 dBFS
    };

    AudioBuffer buffer(1, 512);
    float noiseIn = 0.0f;
    float noiseOut = 0.0f;

    // ~1,7 s samego szumu — tyle wystarcza na zebranie pełnego okna obserwacji.
    for (int block = 0; block < 160; ++block) {
        for (int i = 0; i < buffer.frames(); ++i) buffer.channel(0)[i] = noiseSample();
        if (block >= 150) noiseIn = std::max(noiseIn, rmsOf(buffer, 0));

        AudioBufferView view = buffer.view();
        suppressor.process(view);
        if (block >= 150) noiseOut = std::max(noiseOut, rmsOf(buffer, 0));
    }

    CHECK(noiseIn > 0.005f);
    CHECK_MSG(noiseOut < noiseIn * 0.2f,
              "szum stłumiony za słabo: wejście " + std::to_string(noiseIn) +
                  ", wyjście " + std::to_string(noiseOut));

    // Głośny sygnał użyteczny musi przejść bez zauważalnej utraty poziomu.
    float signalOut = 0.0f;
    for (int block = 0; block < 12; ++block) {
        fillSine(buffer, 500.0, 0.5f, block * 0.17);
        AudioBufferView view = buffer.view();
        suppressor.process(view);
        if (block >= 6) signalOut = std::max(signalOut, rmsOf(buffer, 0));
    }

    const float expected = 0.5f / std::sqrt(2.0f);
    CHECK_MSG(signalOut > expected * 0.8f,
              "sygnał użyteczny stłumiony: " + std::to_string(signalOut) +
                  " (oczekiwano ≈ " + std::to_string(expected) + ")");

    CHECK(suppressor.latencyFrames() > 0);
}

TEST("dsp/redukcja szumów — rejestr backendów przyjmuje nowe algorytmy") {
    auto& registry = NoiseSuppressorRegistry::instance();
    registry.registerBackend("test-passthrough", [] {
        class Passthrough final : public NoiseSuppressorBackend {
        public:
            [[nodiscard]] const char* name() const noexcept override { return "test-passthrough"; }
            void prepare(double, int, int) override {}
            void reset() noexcept override {}
            void setStrength(float) noexcept override {}
            [[nodiscard]] int latencyFrames() const noexcept override { return 0; }
            void process(AudioBufferView&) noexcept override {}
        };
        return std::unique_ptr<NoiseSuppressorBackend>(new Passthrough());
    });

    NoiseSuppressionPlugin suppressor;
    suppressor.initialize(context(256, 1));
    CHECK(suppressor.selectBackend("test-passthrough"));
    CHECK_STR_EQ(suppressor.backendId(), "test-passthrough");

    AudioBuffer buffer(1, 256);
    fillSine(buffer, 1000.0, 0.3f);
    AudioBufferView view = buffer.view();
    suppressor.process(view);   // podmiana backendu następuje w tym wywołaniu
    view = buffer.view();
    suppressor.process(view);

    CHECK_EQ(suppressor.latencyFrames(), 0);
    suppressor.collectRetired();
}

TEST("dsp/resampler — zachowuje częstotliwość przy 44.1 → 48 kHz") {
    Resampler resampler;
    resampler.prepare(1, 44100.0, 48000.0, 512);

    AudioBuffer input(1, 512);
    AudioBuffer output(1, 512);

    constexpr double kFrequency = 1000.0;
    double phase = 0.0;
    std::vector<float> collected;

    for (int block = 0; block < 20; ++block) {
        for (int i = 0; i < input.frames(); ++i) {
            input.channel(0)[i] = static_cast<float>(std::sin(phase));
            phase += kTwoPi * kFrequency / 44100.0;
        }

        Sample* inputs[1] = {input.channel(0)};
        resampler.pushInput(inputs, input.frames());

        Sample* outputs[1] = {output.channel(0)};
        const int produced = resampler.pullOutput(outputs, output.frames());
        for (int i = 0; i < produced; ++i) collected.push_back(output.channel(0)[i]);
    }

    CHECK(collected.size() > 8000);

    // Liczba przejść przez zero określa częstotliwość sygnału wyjściowego.
    int crossings = 0;
    for (std::size_t i = 1001; i < collected.size(); ++i)
        if ((collected[i - 1] < 0.0f) != (collected[i] < 0.0f)) ++crossings;

    const double duration = static_cast<double>(collected.size() - 1001) / 48000.0;
    const double measured = crossings / (2.0 * duration);
    CHECK_NEAR(measured, kFrequency, 15.0);

    // Amplituda nie może zauważalnie spaść.
    float peak = 0.0f;
    for (std::size_t i = 1001; i < collected.size(); ++i) peak = std::max(peak, std::fabs(collected[i]));
    CHECK_NEAR(peak, 1.0, 0.03);
}

TEST("dsp/resampler — tryb 1:1 przepuszcza próbki bez zmian") {
    Resampler resampler;
    resampler.prepare(1, 48000.0, 48000.0, 256);
    CHECK(resampler.isPassthrough());

    AudioBuffer input(1, 256);
    for (int i = 0; i < input.frames(); ++i)
        input.channel(0)[i] = static_cast<float>(std::sin(kTwoPi * 440.0 * i / kSampleRate));

    Sample* inputs[1] = {input.channel(0)};
    resampler.pushInput(inputs, input.frames());

    AudioBuffer output(1, 256);
    Sample* outputs[1] = {output.channel(0)};
    const int produced = resampler.pullOutput(outputs, 200);

    CHECK_EQ(produced, 200);
    // Pierwsza ramka bufora to historia interpolatora, więc wyjście startuje
    // dokładnie od pierwszej wepchniętej próbki.
    for (int i = 0; i < produced; ++i)
        CHECK_NEAR(output.channel(0)[i], input.channel(0)[i], 1e-5);
}

TEST("dsp/łańcuch efektów — kolejność i włączanie") {
    EffectChain chain;
    chain.setContext(context(256));

    auto first = PluginRegistry::instance().create("gain");
    auto second = PluginRegistry::instance().create("limiter");
    CHECK(first != nullptr);
    CHECK(second != nullptr);

    first->setInstanceId(1);
    second->setInstanceId(2);

    CHECK(chain.insert(std::move(first)));
    CHECK(chain.insert(std::move(second)));
    CHECK_EQ(chain.size(), 2);

    auto ordered = chain.ordered();
    CHECK_STR_EQ(ordered[0]->typeId(), "gain");
    CHECK_STR_EQ(ordered[1]->typeId(), "limiter");

    CHECK(chain.reorder({2, 1}));
    ordered = chain.ordered();
    CHECK_STR_EQ(ordered[0]->typeId(), "limiter");
    CHECK_STR_EQ(ordered[1]->typeId(), "gain");

    CHECK(chain.findByType("gain") != nullptr);
    CHECK(chain.find(2) != nullptr);

    auto detached = chain.detach(1);
    CHECK(detached != nullptr);
    CHECK_EQ(chain.size(), 1);
    CHECK(chain.find(1) == nullptr);
}

TEST("dsp/łańcuch efektów — wyłączony plugin nie zmienia sygnału") {
    EffectChain chain;
    chain.setContext(context(256));

    auto gain = PluginRegistry::instance().create("gain");
    gain->setInstanceId(1);
    gain->setParameter("gain", -20.0f);
    gain->setEnabled(false);
    CHECK(chain.insert(std::move(gain)));

    AudioBuffer buffer(2, 256);
    fillSine(buffer, 440.0, 0.5f);
    const float before = peakOf(buffer);

    AudioBufferView view = buffer.view();
    chain.process(view);
    CHECK_NEAR(peakOf(buffer), before, 1e-6);
}

TEST("dsp/każdy plugin mieści wszystkie swoje parametry") {
    // Regresja: equalizer deklaruje 50 parametrów. Gdy limit był mniejszy,
    // nadmiarowe pasma czytały pamięć spoza tablicy.
    auto& registry = PluginRegistry::instance();

    for (const auto& descriptor : registry.descriptors()) {
        auto plugin = registry.create(descriptor.typeId);
        CHECK_MSG(plugin != nullptr, "nie można utworzyć: " + descriptor.typeId);

        const auto parameters = plugin->getParameters();
        CHECK_MSG(parameters.size() <= kMaxPluginParameters,
                  descriptor.typeId + ": przekroczony limit parametrów");

        // Każdy zadeklarowany parametr musi być odczytywalny i zapisywalny.
        for (const auto& parameter : parameters) {
            bool found = false;
            plugin->getParameter(parameter.id, &found);
            CHECK_MSG(found, descriptor.typeId + ": brak parametru " + parameter.id);
            CHECK_MSG(plugin->setParameter(parameter.id, parameter.defaultValue),
                      descriptor.typeId + ": nie można ustawić " + parameter.id);
        }
    }

    // Equalizer musi mieć komplet: 10 pasm × 5 parametrów.
    auto equalizer = registry.create("equalizer");
    CHECK_EQ(equalizer->getParameters().size(),
             static_cast<std::size_t>(kEqBands * kEqParametersPerBand));

    bool found = false;
    equalizer->getParameter("band9.freq", &found);
    CHECK_MSG(found, "ostatnie pasmo equalizera nie zostało zarejestrowane");
}

TEST("dsp/equalizer — najwyższe pasmo działa jak każde inne") {
    EqualizerPlugin eq;
    eq.initialize(context());

    // Pasmo 9 leży na końcu tablicy parametrów — dawniej wypadało poza nią.
    eq.setParameter("band9.enabled", 1.0f);
    eq.setParameter("band9.type", static_cast<float>(FilterType::HighShelf));
    eq.setParameter("band9.freq", 8000.0f);
    eq.setParameter("band9.gain", -12.0f);
    eq.setParameter("band9.q", 0.707f);

    const auto response = eq.magnitudeResponseDb({100.0, 16000.0});
    CHECK_NEAR(response[0], 0.0, 0.6);
    CHECK_NEAR(response[1], -12.0, 1.0);

    AudioBuffer buffer(2, 4096);
    fillSine(buffer, 16000.0, 0.3f);
    AudioBufferView view = buffer.view();
    eq.process(view);

    const float measured = rmsOf(buffer, 0, 2048);
    const float expected = 0.3f / std::sqrt(2.0f) * dbToGain(-12.0f);
    CHECK_NEAR(measured, expected, expected * 0.15f);
}

TEST("dsp/rejestr pluginów — zna wszystkie efekty ze specyfikacji") {
    auto& registry = PluginRegistry::instance();
    for (const char* type : {"gain", "noisegate", "noisesuppression", "equalizer",
                             "compressor", "deesser", "limiter"}) {
        CHECK_MSG(registry.contains(type), std::string("brak efektu: ") + type);
        CHECK_MSG(registry.create(type) != nullptr, std::string("nie można utworzyć: ") + type);
    }
    CHECK_EQ(PluginRegistry::defaultChainOrder().size(), std::size_t{7});
}
