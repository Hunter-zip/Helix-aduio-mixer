// Interfejs pluginu audio (spec §25) — wspólny kontrakt dla wszystkich efektów.
// Dzięki niemu dokładanie kolejnych efektów nie wymaga zmian w silniku.
#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "helix/AudioBuffer.h"
#include "helix/Types.h"

namespace helix::dsp {

/// Maksymalna liczba parametrów jednego pluginu.
/// Najbardziej pojemny wbudowany efekt to equalizer: 10 pasm × 5 parametrów.
inline constexpr std::size_t kMaxPluginParameters = 64;

/// Sposób odwzorowania parametru w GUI.
enum class ParamScale : std::uint8_t {
    Linear,       ///< liniowy suwak
    Logarithmic,  ///< skala logarytmiczna (częstotliwości)
    Decibels,     ///< wartość w dB
    Boolean,      ///< 0/1
    Choice        ///< indeks na liście `choices`
};

/// Opis pojedynczego parametru — GUI buduje z tego kontrolki bez wiedzy o typie efektu.
struct PluginParameter {
    std::string id;
    std::string name;
    std::string unit;
    float       minValue     = 0.0f;
    float       maxValue     = 1.0f;
    float       defaultValue = 0.0f;
    ParamScale  scale        = ParamScale::Linear;
    std::vector<std::string> choices;
};

/// Kontekst przygotowania toru.
struct ProcessContext {
    double sampleRate   = kDefaultSampleRate;
    int    maxBlockSize = kDefaultBlockFrames;
    int    channels     = 2;
};

/// Bazowy interfejs efektu.
///
/// Kontrakt czasu rzeczywistego:
///  * `process()` — wołane z wątku audio: bez alokacji, bez blokad, bez I/O;
///  * `initialize()` — wątek sterujący, alokacje dozwolone;
///  * `setParameter()` — dowolny wątek, zapis atomowy; przeliczenie współczynników
///    następuje na początku najbliższego bloku w `process()`.
class AudioPlugin {
public:
    virtual ~AudioPlugin() = default;

    /// Stabilny identyfikator typu (zapisywany w profilu).
    [[nodiscard]] virtual const char* typeId() const noexcept = 0;

    /// Nazwa pokazywana w GUI.
    [[nodiscard]] virtual const char* displayName() const noexcept = 0;

    /// Przygotowanie do pracy. Wywoływane poza wątkiem audio.
    virtual void initialize(const ProcessContext& context) = 0;

    /// Czyści stan wewnętrzny (RT-safe).
    virtual void reset() noexcept = 0;

    /// Przetwarza blok in-place.
    virtual void process(AudioBufferView& buffer) noexcept = 0;

    /// Opisy parametrów.
    [[nodiscard]] virtual std::vector<PluginParameter> getParameters() const = 0;

    /// Ustawia parametr. Zwraca false dla nieznanego identyfikatora.
    virtual bool setParameter(std::string_view id, float value) noexcept = 0;

    /// Odczyt parametru; `found` informuje, czy identyfikator istnieje.
    [[nodiscard]] virtual float getParameter(std::string_view id, bool* found = nullptr) const noexcept = 0;

    /// Opóźnienie wnoszone przez efekt (w próbkach). Domyślnie zerowe —
    /// wszystkie wbudowane efekty Helixa pracują bez look-ahead poza limiterem.
    [[nodiscard]] virtual int latencyFrames() const noexcept { return 0; }

    /// Bypass jest wspólny dla wszystkich efektów i obsługiwany przez łańcuch.
    void setEnabled(bool enabled) noexcept { enabled_.store(enabled, std::memory_order_relaxed); }
    [[nodiscard]] bool isEnabled() const noexcept { return enabled_.load(std::memory_order_relaxed); }

    /// Ustawiane przez ChannelManager — stabilny identyfikator instancji.
    void setInstanceId(PluginId id) noexcept { instanceId_ = id; }
    [[nodiscard]] PluginId instanceId() const noexcept { return instanceId_; }

private:
    std::atomic<bool> enabled_{true};
    PluginId          instanceId_ = 0;
};

/// Pomocnicza baza: trzyma wartości parametrów w atomikach i sygnalizuje,
/// kiedy trzeba przeliczyć współczynniki. Dzięki temu `process()` nie bierze mutexa.
class ParameterizedPlugin : public AudioPlugin {
public:
    bool setParameter(std::string_view id, float value) noexcept override {
        for (std::size_t i = 0; i < count_; ++i) {
            if (descriptors_[i].id == id) {
                const auto& d = descriptors_[i];
                const float clamped = value < d.minValue ? d.minValue
                                    : (value > d.maxValue ? d.maxValue : value);
                values_[i].store(clamped, std::memory_order_relaxed);
                dirty_.store(true, std::memory_order_release);
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] float getParameter(std::string_view id, bool* found = nullptr) const noexcept override {
        for (std::size_t i = 0; i < count_; ++i) {
            if (descriptors_[i].id == id) {
                if (found) *found = true;
                return values_[i].load(std::memory_order_relaxed);
            }
        }
        if (found) *found = false;
        return 0.0f;
    }

    [[nodiscard]] std::vector<PluginParameter> getParameters() const override {
        return std::vector<PluginParameter>(descriptors_.begin(), descriptors_.begin() + static_cast<long>(count_));
    }

protected:
    /// Rejestracja parametru w konstruktorze pochodnej klasy.
    /// Zwraca indeks parametru albo `kMaxPluginParameters`, gdy zabrakło miejsca.
    std::size_t addParameter(PluginParameter param) {
        const std::size_t index = count_;
        if (index >= kMaxPluginParameters) {
            // Przekroczenie limitu jest błędem programisty, nie sytuacją runtime —
            // w trybie debug przerwie budowę pluginu zamiast po cichu gubić parametry.
            assert(false && "przekroczono limit parametrów pluginu");
            return kMaxPluginParameters;
        }
        values_[index].store(param.defaultValue, std::memory_order_relaxed);
        descriptors_[index] = std::move(param);
        ++count_;
        dirty_.store(true, std::memory_order_release);
        return index;
    }

    /// Odczyt parametru po indeksie. Indeks spoza zakresu zwraca 0 zamiast
    /// sięgać poza tablicę.
    [[nodiscard]] float raw(std::size_t index) const noexcept {
        if (index >= count_) return 0.0f;
        return values_[index].load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t parameterCount() const noexcept { return count_; }

    /// Wołane na początku `process()`: zwraca true dokładnie raz po zmianie parametrów.
    [[nodiscard]] bool consumeDirty() noexcept {
        return dirty_.exchange(false, std::memory_order_acquire);
    }

    void markDirty() noexcept { dirty_.store(true, std::memory_order_release); }

private:
    std::array<PluginParameter, kMaxPluginParameters> descriptors_{};
    std::array<std::atomic<float>, kMaxPluginParameters> values_{};
    std::size_t count_ = 0;
    std::atomic<bool> dirty_{true};
};

using PluginPtr = std::unique_ptr<AudioPlugin>;

} // namespace helix::dsp
