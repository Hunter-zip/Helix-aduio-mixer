// Silnik audio (spec §3, §27 etap 1) — serce programu.
//
// Zasady:
//  * przetwarzanie odbywa się w wątku urządzenia, bez alokacji i blokad;
//  * struktura grafu (kanały, magistrale, źródła) jest publikowana atomowo,
//    a stare wersje zwalniane przez wątek sterujący po barierze bloków;
//  * GUI nigdy nie dotyka buforów — komunikuje się wyłącznie przez to API.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "helix/AudioBuffer.h"
#include "helix/Types.h"
#include "helix/core/AudioBackend.h"
#include "helix/core/Bus.h"
#include "helix/core/Channel.h"
#include "helix/core/InputSource.h"
#include "helix/core/MasterBus.h"
#include "helix/core/RoutingMatrix.h"
#include "helix/dsp/AudioPlugin.h"

namespace helix::core {

/// Statystyki pracy silnika (diagnostyka, spec §23/§27 etap 7).
struct EngineStats {
    std::uint64_t blocksProcessed = 0;
    std::uint64_t xruns = 0;
    double cpuLoad = 0.0;          ///< 0..1 — udział czasu bloku zużyty na DSP
    double peakCpuLoad = 0.0;
    double engineLatencyMs = 0.0;  ///< opóźnienie wnoszone przez sam silnik
    double sampleRate = kDefaultSampleRate;
    int    blockFrames = kDefaultBlockFrames;
    bool   running = false;
};

/// Konfiguracja formatu pracy (spec §21).
struct EngineFormat {
    double sampleRate  = kDefaultSampleRate;
    int    blockFrames = kDefaultBlockFrames;
    int    channels    = 2;
};

class AudioEngine final : public RenderCallback {
public:
    AudioEngine();
    ~AudioEngine() override;

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    // ── Cykl życia (wątek sterujący) ────────────────────────────────────────

    /// Ustawia format pracy. Gdy silnik pracuje, graf zostaje przygotowany na nowo.
    Status configure(const EngineFormat& format);
    [[nodiscard]] EngineFormat format() const;

    /// Włącza przetwarzanie. Od tej chwili renderBlock() może być wołane.
    void setRunning(bool running) noexcept;
    [[nodiscard]] bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    // ── Budowa grafu (wątek sterujący; wołane przez ChannelManager) ──────────

    Channel* addChannel(std::string name);
    bool     removeChannel(ChannelId id);
    [[nodiscard]] Channel* findChannel(ChannelId id) const;
    [[nodiscard]] std::vector<Channel*> channels() const;

    Bus* addBus(std::string name, std::string label, BusKind kind);
    bool removeBus(BusId id);
    [[nodiscard]] Bus* findBus(BusId id) const;
    [[nodiscard]] std::vector<Bus*> buses() const;

    InputSource* addSource(SourceKind kind, std::string name);
    bool         removeSource(SourceId id);
    [[nodiscard]] InputSource* findSource(SourceId id) const;
    [[nodiscard]] std::vector<InputSource*> sources() const;

    /// Przypisuje źródło do kanału (spec §5). kInvalidChannel odpina źródło.
    bool assignSource(SourceId source, ChannelId channel);

    [[nodiscard]] RoutingMatrix& routing() noexcept { return routing_; }
    [[nodiscard]] const RoutingMatrix& routing() const noexcept { return routing_; }
    [[nodiscard]] MasterBus& master() noexcept { return master_; }

    /// Wskazuje magistralę, która jest bezpośrednio wpięta w zegar urządzenia.
    /// Jej sygnał trafia do karty bez dodatkowego bufora (spec §14).
    void setPrimaryBus(BusId bus);
    [[nodiscard]] BusId primaryBus() const noexcept { return master_.primaryBus(); }

    /// Publikuje zmiany struktury do wątku audio. Wołane po serii modyfikacji.
    void commitGraph();

    /// Zwalnia obiekty odstawione przez wątek audio. Wołane cyklicznie
    /// przez wątek sterujący (nigdy z callbacku audio).
    void collectGarbage();

    /// Odstawia obiekt do zwolnienia po barierze dwóch bloków audio.
    /// Używane przy wymianie efektów i innych struktur widocznych dla RT.
    void retire(std::function<void()> deleter);

    // ── Wątek audio ─────────────────────────────────────────────────────────

    /// Callback urządzenia podstawowego. Przetwarza cały graf i wypełnia bufor.
    void renderBlock(Sample* interleaved, int frames, int channels) noexcept override;

    /// Przetwarza jeden blok bez kopiowania do urządzenia (tryb offline/testy).
    void processBlock(int frames) noexcept;

    // ── Diagnostyka ─────────────────────────────────────────────────────────

    [[nodiscard]] EngineStats stats() const noexcept;
    void resetStats() noexcept;

    /// Sumaryczna latencja: urządzenie + blok silnika + look-ahead efektów.
    [[nodiscard]] double estimatedLatencyMs(double deviceLatencyMs) const noexcept;

    /// Licznik przetworzonych bloków — bariera dla bezpiecznego zwalniania pamięci.
    [[nodiscard]] std::uint64_t blockCounter() const noexcept {
        return blockCounter_.load(std::memory_order_acquire);
    }

private:
    /// Migawka struktury widziana przez wątek audio.
    struct Graph {
        std::vector<Channel*>     channels;
        std::vector<Bus*>         buses;
        std::vector<InputSource*> sources;
        std::vector<int>          sourceChannelIndex;  ///< indeks kanału dla każdego źródła (-1 = brak)
        bool anySolo = false;
        int  primaryBusIndex = -1;
    };

    struct RetiredObject {
        std::uint64_t stamp = 0;
        std::function<void()> deleter;
    };

    void rebuildGraphLocked();
    void prepareObjects();
    void retireLocked(std::function<void()> deleter);  ///< wymaga trzymania structureMutex_
    [[nodiscard]] int allocateChannelIndex();
    [[nodiscard]] int allocateBusIndex();

    void processBlockInternal(const Graph& graph, int frames) noexcept;
    void mixToBuses(const Graph& graph, int frames) noexcept;

    mutable std::mutex structureMutex_;   ///< serializuje wyłącznie wątek sterujący

    EngineFormat format_{};
    std::atomic<bool> running_{false};

    std::vector<std::unique_ptr<Channel>>     channelStore_;
    std::vector<std::unique_ptr<Bus>>         busStore_;
    std::vector<std::unique_ptr<InputSource>> sourceStore_;

    std::array<bool, kMaxChannels> channelIndexUsed_{};
    std::array<bool, kMaxBuses>    busIndexUsed_{};

    RoutingMatrix routing_;
    MasterBus     master_;

    std::atomic<const Graph*> activeGraph_{nullptr};
    std::unique_ptr<Graph>    pendingGraph_;

    std::vector<RetiredObject> retired_;

    ChannelId nextChannelId_ = 1;
    BusId     nextBusId_     = 1;
    SourceId  nextSourceId_  = 1;
    PluginId  nextPluginId_  = 1;

    // Bufory robocze wątku audio — prealokowane, nigdy nie realokowane w RT.
    AudioBuffer sourceScratch_;

    // Kopie formatu czytane w wątku audio bez mutexa (zmieniane tylko przy stopie).
    std::atomic<int>    blockLimit_{kDefaultBlockFrames};
    std::atomic<double> sampleRateCached_{kDefaultSampleRate};

    std::atomic<std::uint64_t> blockCounter_{0};
    std::atomic<std::uint64_t> xruns_{0};
    std::atomic<double>        cpuLoad_{0.0};
    std::atomic<double>        peakCpuLoad_{0.0};

public:
    /// Nadaje kolejny identyfikator instancji pluginu (wątek sterujący).
    [[nodiscard]] PluginId nextPluginId() noexcept { return nextPluginId_++; }
};

} // namespace helix::core
