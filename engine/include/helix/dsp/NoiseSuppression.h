// Redukcja szumów (spec §12).
//
// Moduł jest celowo odseparowany od reszty łańcucha: `NoiseSuppressorBackend`
// to punkt wymiany algorytmu. Wbudowany backend "spectral" to klasyczne
// bramkowanie widmowe; backend oparty o model AI można dorejestrować później
// bez żadnych zmian w silniku ani w GUI.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "helix/Denormal.h"
#include "helix/Parameter.h"
#include "helix/dsp/AudioPlugin.h"
#include "helix/dsp/Fft.h"

namespace helix::dsp {

/// Kontrakt backendu redukcji szumów.
class NoiseSuppressorBackend {
public:
    virtual ~NoiseSuppressorBackend() = default;
    [[nodiscard]] virtual const char* name() const noexcept = 0;

    /// Alokacje dozwolone — wołane poza wątkiem audio.
    virtual void prepare(double sampleRate, int maxBlockFrames, int channels) = 0;

    virtual void reset() noexcept = 0;

    /// 0.0 = brak redukcji, 1.0 = maksymalna.
    virtual void setStrength(float strength) noexcept = 0;

    [[nodiscard]] virtual int latencyFrames() const noexcept = 0;

    /// RT: przetwarzanie in-place, bez alokacji.
    virtual void process(AudioBufferView& buffer) noexcept = 0;
};

/// Rejestr backendów — pozwala dołożyć implementację AI z zewnętrznego modułu.
class NoiseSuppressorRegistry {
public:
    using Factory = std::function<std::unique_ptr<NoiseSuppressorBackend>()>;

    static NoiseSuppressorRegistry& instance() {
        static NoiseSuppressorRegistry registry;
        return registry;
    }

    void registerBackend(std::string id, Factory factory) {
        std::lock_guard<std::mutex> lock(mutex_);
        factories_[std::move(id)] = std::move(factory);
    }

    [[nodiscard]] std::unique_ptr<NoiseSuppressorBackend> create(const std::string& id) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto it = factories_.find(id);
        if (it == factories_.end()) return nullptr;
        return it->second();
    }

    [[nodiscard]] std::vector<std::string> available() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> ids;
        ids.reserve(factories_.size());
        for (const auto& [id, _] : factories_) ids.push_back(id);
        return ids;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, Factory> factories_;
};

/// Spektralne bramkowanie szumu: STFT + estymacja podłogi szumowej metodą
/// minimum tracking + odejmowanie widmowe z wygładzaniem wzmocnień.
class SpectralNoiseSuppressor final : public NoiseSuppressorBackend {
public:
    explicit SpectralNoiseSuppressor(int fftOrder = 8) : order_(std::clamp(fftOrder, 6, 11)) {}

    [[nodiscard]] const char* name() const noexcept override { return "spectral"; }

    void prepare(double sampleRate, int /*maxBlockFrames*/, int channels) override {
        sampleRate_ = sampleRate > 0.0 ? sampleRate : kDefaultSampleRate;
        channels_   = std::max(1, channels);
        fft_.prepare(order_);
        size_ = fft_.size();
        hop_  = size_ / 4;
        bins_ = size_ / 2 + 1;

        window_.resize(static_cast<std::size_t>(size_));
        constexpr double kTwoPi = 6.283185307179586476925286766559;
        for (int i = 0; i < size_; ++i)
            window_[static_cast<std::size_t>(i)] = static_cast<float>(
                0.5 * (1.0 - std::cos(kTwoPi * static_cast<double>(i) / static_cast<double>(size_))));

        // Hann z hopem N/4 sumuje się do 1.5 — kompensujemy przy syntezie.
        olaScale_ = 1.0f / 1.5f;

        real_.assign(static_cast<std::size_t>(size_), 0.0f);
        imag_.assign(static_cast<std::size_t>(size_), 0.0f);

        // Estymacja szumu metodą minimum kroczącego: okno ~600 ms podzielone
        // na 5 podokien. Dzięki podziałowi estymata nadąża, gdy tło rośnie,
        // a nie „przykleja się” do jednorazowego minimum.
        subWindowFrames_ = std::max(4, static_cast<int>(0.12 * sampleRate_ / hop_));
        subWindows_      = 5;

        states_.clear();
        states_.resize(static_cast<std::size_t>(channels_));
        for (auto& s : states_) {
            s.inFifo.assign(static_cast<std::size_t>(size_), 0.0f);
            s.outAccum.assign(static_cast<std::size_t>(size_ + hop_), 0.0f);
            s.outFifo.assign(static_cast<std::size_t>(hop_), 0.0f);
            s.power.assign(static_cast<std::size_t>(bins_), 0.0f);
            s.noise.assign(static_cast<std::size_t>(bins_), 0.0f);
            s.subMin.assign(static_cast<std::size_t>(bins_), 0.0f);
            s.history.assign(static_cast<std::size_t>(bins_) * static_cast<std::size_t>(subWindows_), 0.0f);
            s.gain.assign(static_cast<std::size_t>(bins_), 1.0f);
            s.inPos = size_ - hop_;
            s.frameCounter = 0;
            s.historyWrite = 0;
            s.warmedUp = false;
        }

        // Stałe czasowe wygładzania.
        powerSmoothing_ = 0.9f;
        gainSmoothing_  = 0.5f;
        reset();
    }

    void reset() noexcept override {
        for (auto& s : states_) {
            std::fill(s.inFifo.begin(), s.inFifo.end(), 0.0f);
            std::fill(s.outAccum.begin(), s.outAccum.end(), 0.0f);
            std::fill(s.outFifo.begin(), s.outFifo.end(), 0.0f);
            std::fill(s.power.begin(), s.power.end(), 0.0f);
            std::fill(s.noise.begin(), s.noise.end(), 0.0f);
            std::fill(s.subMin.begin(), s.subMin.end(), 0.0f);
            std::fill(s.history.begin(), s.history.end(), 0.0f);
            std::fill(s.gain.begin(), s.gain.end(), 1.0f);
            s.inPos = size_ - hop_;
            s.frameCounter = 0;
            s.historyWrite = 0;
            s.warmedUp = false;
        }
    }

    void setStrength(float strength) noexcept override {
        strength_.store(std::clamp(strength, 0.0f, 1.0f), std::memory_order_relaxed);
    }

    [[nodiscard]] int latencyFrames() const noexcept override { return size_ - hop_; }

    void process(AudioBufferView& buffer) noexcept override {
        if (states_.empty()) return;

        const float strength    = strength_.load(std::memory_order_relaxed);
        const float oversub     = 1.0f + 2.0f * strength;   // współczynnik nadodejmowania
        const float floorGain   = 1.0f - 0.95f * strength;  // podłoga widmowa

        const int frames   = buffer.frames();
        const int channels = std::min(buffer.channels(), channels_);

        for (int c = 0; c < channels; ++c) {
            State& s = states_[static_cast<std::size_t>(c)];
            Sample* data = buffer.channel(c);

            const int latency = size_ - hop_;
            for (int i = 0; i < frames; ++i) {
                s.inFifo[static_cast<std::size_t>(s.inPos)] = data[i];
                // Wyjście jest opóźnione o (size_ - hop_) próbek — to koszt analizy STFT.
                data[i] = s.outFifo[static_cast<std::size_t>(s.inPos - latency)];
                ++s.inPos;

                if (s.inPos >= size_) {
                    processFrame(s, oversub, floorGain);
                    s.inPos = latency;
                }
            }
        }
    }

private:
    struct State {
        std::vector<float> inFifo;    ///< okno analizy (size_)
        std::vector<float> outAccum;  ///< akumulator overlap-add (size_ + hop_)
        std::vector<float> outFifo;   ///< gotowe próbki wyjściowe (hop_)
        std::vector<float> power;    ///< wygładzona moc widmowa
        std::vector<float> noise;    ///< estymata podłogi szumowej
        std::vector<float> subMin;   ///< minimum bieżącego podokna
        std::vector<float> history;  ///< minima poprzednich podokien (bins × subWindows)
        std::vector<float> gain;
        int  inPos = 0;
        int  frameCounter = 0;
        int  historyWrite = 0;
        bool warmedUp = false;       ///< czy zebrano pełne okno obserwacji
    };

    void processFrame(State& s, float oversub, float floorGain) noexcept {
        // 1. Okno analizy.
        for (int i = 0; i < size_; ++i) {
            real_[static_cast<std::size_t>(i)] =
                s.inFifo[static_cast<std::size_t>(i)] * window_[static_cast<std::size_t>(i)];
            imag_[static_cast<std::size_t>(i)] = 0.0f;
        }

        fft_.forward(real_.data(), imag_.data());

        // 2. Estymacja szumu (minimum kroczące) i wyliczenie wzmocnień.
        const bool rotate = (++s.frameCounter >= subWindowFrames_);

        for (int k = 0; k < bins_; ++k) {
            const auto ki = static_cast<std::size_t>(k);
            const float re = real_[ki];
            const float im = imag_[ki];
            const float p  = re * re + im * im;

            s.power[ki] = powerSmoothing_ * s.power[ki] + (1.0f - powerSmoothing_) * p;
            const float ps = s.power[ki];

            // Minimum bieżącego podokna.
            if (s.frameCounter == 1 || ps < s.subMin[ki]) s.subMin[ki] = ps;

            if (rotate) {
                s.history[ki * static_cast<std::size_t>(subWindows_) +
                          static_cast<std::size_t>(s.historyWrite)] = s.subMin[ki];
            }

            // Podłoga szumu = najmniejsze z minimów w całym oknie obserwacji.
            float minimum = s.subMin[ki];
            if (s.warmedUp || rotate) {
                for (int w = 0; w < subWindows_; ++w) {
                    const float value = s.history[ki * static_cast<std::size_t>(subWindows_) +
                                                  static_cast<std::size_t>(w)];
                    if (value > 0.0f && value < minimum) minimum = value;
                }
            }
            // Metoda minimum zaniża estymatę — korekta obciążenia.
            s.noise[ki] = minimum * kMinimumBias;

            const float subtracted = ps - oversub * s.noise[ki];
            float g = (ps > 1.0e-18f) ? std::sqrt(std::max(subtracted, 0.0f) / ps) : 1.0f;
            g = std::max(g, floorGain);

            // Wygładzanie wzmocnień w czasie — ogranicza artefakty „musical noise”.
            s.gain[ki] = gainSmoothing_ * s.gain[ki] + (1.0f - gainSmoothing_) * g;
        }

        if (rotate) {
            s.historyWrite = (s.historyWrite + 1) % subWindows_;
            s.frameCounter = 0;
            if (s.historyWrite == 0) s.warmedUp = true;
        }

        // 3. Aplikacja wzmocnień z zachowaniem symetrii hermitowskiej.
        for (int k = 0; k < bins_; ++k) {
            const auto ki = static_cast<std::size_t>(k);
            const float g = s.gain[ki];
            real_[ki] *= g;
            imag_[ki] *= g;
            if (k > 0 && k < size_ / 2) {
                const auto mirror = static_cast<std::size_t>(size_ - k);
                real_[mirror] =  real_[ki];
                imag_[mirror] = -imag_[ki];
            }
        }

        fft_.inverse(real_.data(), imag_.data());

        // 4. Overlap-add: okno syntezy + akumulacja.
        for (int i = 0; i < size_; ++i)
            s.outAccum[static_cast<std::size_t>(i)] +=
                real_[static_cast<std::size_t>(i)] * window_[static_cast<std::size_t>(i)] * olaScale_;

        // Pierwsze `hop_` próbek akumulatora jest już w pełni zsumowanych — wydajemy je.
        for (int i = 0; i < hop_; ++i)
            s.outFifo[static_cast<std::size_t>(i)] = s.outAccum[static_cast<std::size_t>(i)];

        for (int i = 0; i < size_; ++i)
            s.outAccum[static_cast<std::size_t>(i)] = s.outAccum[static_cast<std::size_t>(i + hop_)];
        for (int i = size_; i < size_ + hop_; ++i)
            s.outAccum[static_cast<std::size_t>(i)] = 0.0f;

        // 5. Przesunięcie okna wejściowego o hop.
        for (int i = 0; i < size_ - hop_; ++i)
            s.inFifo[static_cast<std::size_t>(i)] = s.inFifo[static_cast<std::size_t>(i + hop_)];
    }

    int    order_ = 8;
    int    size_  = 256;
    int    hop_   = 64;
    int    bins_  = 129;
    int    channels_ = 1;
    double sampleRate_ = kDefaultSampleRate;

    Fft                fft_;
    std::vector<float> window_;
    std::vector<float> real_;
    std::vector<float> imag_;
    std::vector<State> states_;

    static constexpr float kMinimumBias = 2.5f;

    int   subWindowFrames_ = 90;
    int   subWindows_      = 5;
    float olaScale_        = 1.0f / 1.5f;
    float powerSmoothing_  = 0.9f;
    float gainSmoothing_   = 0.5f;

    std::atomic<float> strength_{0.6f};
};

/// Plugin opakowujący backend. Podmiana backendu odbywa się przez wskaźnik
/// przekazywany do wątku audio — zwolnienie starego robi wątek sterujący.
class NoiseSuppressionPlugin final : public ParameterizedPlugin {
public:
    NoiseSuppressionPlugin() {
        addParameter({"strength", "Strength", "", 0.0f, 1.0f, 0.6f, ParamScale::Linear, {}});
        addParameter({"quality",  "Window",   "", 0.0f, 2.0f, 0.0f, ParamScale::Choice,
                      {"low-latency", "balanced", "quality"}});
    }

    [[nodiscard]] const char* typeId() const noexcept override { return "noisesuppression"; }
    [[nodiscard]] const char* displayName() const noexcept override { return "Noise Suppression"; }

    void initialize(const ProcessContext& context) override {
        context_ = context;
        installBackend(createBackend(backendId_, static_cast<int>(raw(1))));
        markDirty();
    }

    void reset() noexcept override {
        if (auto* b = active_.load(std::memory_order_acquire)) b->reset();
    }

    [[nodiscard]] int latencyFrames() const noexcept override {
        auto* b = active_.load(std::memory_order_acquire);
        return b ? b->latencyFrames() : 0;
    }

    void process(AudioBufferView& buffer) noexcept override {
        // Podmiana backendu przygotowanego wcześniej przez wątek sterujący.
        if (auto* pending = pending_.exchange(nullptr, std::memory_order_acq_rel)) {
            retired_.store(active_.exchange(pending, std::memory_order_acq_rel),
                           std::memory_order_release);
        }

        auto* backend = active_.load(std::memory_order_acquire);
        if (backend == nullptr) return;

        if (consumeDirty()) backend->setStrength(raw(0));
        backend->process(buffer);
    }

    /// Wybór algorytmu — wołane z wątku sterującego (np. po zmianie profilu).
    /// `id` musi istnieć w rejestrze; "spectral" jest wbudowany.
    bool selectBackend(const std::string& id) {
        auto backend = createBackend(id, static_cast<int>(raw(1)));
        if (!backend) return false;
        backendId_ = id;
        installBackend(std::move(backend));
        return true;
    }

    [[nodiscard]] std::string backendId() const { return backendId_; }

    /// Zwalnia backend odstawiony przez wątek audio. Wołane cyklicznie
    /// przez wątek sterujący (garbage collector silnika).
    void collectRetired() {
        if (auto* old = retired_.exchange(nullptr, std::memory_order_acq_rel))
            delete old;
    }

    ~NoiseSuppressionPlugin() override {
        delete active_.load(std::memory_order_acquire);
        delete pending_.load(std::memory_order_acquire);
        delete retired_.load(std::memory_order_acquire);
    }

    NoiseSuppressionPlugin(const NoiseSuppressionPlugin&) = delete;
    NoiseSuppressionPlugin& operator=(const NoiseSuppressionPlugin&) = delete;

private:
    static int orderForQuality(int quality) noexcept {
        switch (quality) {
            case 0:  return 8;   // 256 próbek → ~4 ms opóźnienia @48 kHz
            case 1:  return 9;   // 512 próbek → ~8 ms
            default: return 10;  // 1024 próbki → ~16 ms
        }
    }

    std::unique_ptr<NoiseSuppressorBackend> createBackend(const std::string& id, int quality) {
        std::unique_ptr<NoiseSuppressorBackend> backend;
        if (id == "spectral" || id.empty())
            backend = std::make_unique<SpectralNoiseSuppressor>(orderForQuality(quality));
        else
            backend = NoiseSuppressorRegistry::instance().create(id);

        if (!backend) return nullptr;
        backend->prepare(context_.sampleRate, context_.maxBlockSize, context_.channels);
        backend->setStrength(raw(0));
        return backend;
    }

    void installBackend(std::unique_ptr<NoiseSuppressorBackend> backend) {
        if (!backend) return;
        if (active_.load(std::memory_order_acquire) == nullptr) {
            // Pierwsza instalacja — wątek audio jeszcze nie pracuje na tym pluginie.
            active_.store(backend.release(), std::memory_order_release);
            return;
        }
        // Wymiana w locie: stary egzemplarz odbierze `collectRetired()`.
        collectRetired();
        auto* previousPending = pending_.exchange(backend.release(), std::memory_order_acq_rel);
        delete previousPending;
    }

    ProcessContext context_{};
    std::string    backendId_ = "spectral";

    std::atomic<NoiseSuppressorBackend*> active_{nullptr};
    std::atomic<NoiseSuppressorBackend*> pending_{nullptr};
    std::atomic<NoiseSuppressorBackend*> retired_{nullptr};
};

} // namespace helix::dsp
