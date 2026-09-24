// Wirtualne urządzenia audio Helixa (spec §7).
//
// Każde wirtualne urządzenie to nazwany kanał pamięci współdzielonej:
//  * wyjście — silnik zapisuje, klient (sterownik/aplikacja) czyta,
//  * wejście — klient zapisuje, silnik czyta.
//
// Widoczność w systemie jako zwykłe urządzenie audio zapewnia pakiet sterownika
// obsługiwany przez DriverInterface; transport danych jest od niego niezależny.
#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "helix/Types.h"
#include "helix/core/AudioEngine.h"
#include "helix/virtualaudio/DriverInterface.h"
#include "helix/virtualaudio/SharedRingBuffer.h"

namespace helix::virtualaudio {

enum class VirtualEndpointKind : std::uint8_t {
    Output,  ///< magistrala miksera → aplikacja zewnętrzna
    Input    ///< aplikacja zewnętrzna → kanał miksera
};

/// Definicja wirtualnego urządzenia.
struct VirtualEndpointSpec {
    std::string id;       ///< identyfikator transportu (nazwa segmentu)
    std::string name;     ///< nazwa widoczna dla użytkownika
    VirtualEndpointKind kind = VirtualEndpointKind::Output;
    int channels = 2;
};

/// Stan wirtualnego urządzenia dla GUI.
struct VirtualEndpointState {
    std::string id;
    std::string name;
    VirtualEndpointKind kind = VirtualEndpointKind::Output;
    bool        enabled = false;
    bool        transportReady = false;
    bool        clientConnected = false;
    BusId       bus = kInvalidBus;
    SourceId    source = kInvalidSource;
    int         channels = 2;
    double      sampleRate = kDefaultSampleRate;
    std::uint64_t overruns = 0;
    std::uint64_t underruns = 0;
};

/// Domyślny zestaw urządzeń wg specyfikacji §7.
[[nodiscard]] const std::vector<VirtualEndpointSpec>& defaultVirtualEndpoints();

class VirtualDeviceManager {
public:
    VirtualDeviceManager(core::AudioEngine& engine, std::unique_ptr<DriverInterface> driver);
    ~VirtualDeviceManager();

    VirtualDeviceManager(const VirtualDeviceManager&) = delete;
    VirtualDeviceManager& operator=(const VirtualDeviceManager&) = delete;

    /// Tworzy komplet domyślnych punktów końcowych (bez uruchamiania transportu).
    void createDefaultEndpoints();

    Status addEndpoint(const VirtualEndpointSpec& spec);
    bool   removeEndpoint(const std::string& id);

    /// Włącza/wyłącza transport danego punktu końcowego.
    Status setEndpointEnabled(const std::string& id, bool enabled);

    /// Podpina wyjście wirtualne pod magistralę silnika.
    Status bindOutput(const std::string& id, BusId bus);

    /// Podpina wejście wirtualne pod źródło silnika.
    Status bindInput(const std::string& id, SourceId source);

    [[nodiscard]] std::vector<VirtualEndpointState> states() const;

    /// Uruchamia wątek transportowy przenoszący audio między silnikiem a klientami.
    Status start(double sampleRate, int blockFrames);
    void   stop();
    [[nodiscard]] bool isRunning() const noexcept { return running_.load(std::memory_order_acquire); }

    [[nodiscard]] DriverInterface& driver() noexcept { return *driver_; }
    [[nodiscard]] DriverStatus driverStatus() const { return driver_->query(); }

    /// Jeden cykl transportu — wywoływany przez wątek wewnętrzny, a w testach ręcznie.
    void pump(int frames);

private:
    struct Endpoint {
        VirtualEndpointSpec spec;
        SharedRingBuffer    transport;
        BusId               bus = kInvalidBus;
        SourceId            source = kInvalidSource;
        bool                enabled = false;
        std::vector<Sample> scratch;
    };

    [[nodiscard]] Endpoint* findLocked(const std::string& id);
    void transportLoop();

    core::AudioEngine&               engine_;
    std::unique_ptr<DriverInterface> driver_;

    mutable std::mutex                      mutex_;
    std::vector<std::unique_ptr<Endpoint>>  endpoints_;

    std::atomic<bool>            running_{false};
    std::unique_ptr<std::thread> worker_;
    double                       sampleRate_ = kDefaultSampleRate;
    int                          blockFrames_ = kDefaultBlockFrames;
};

} // namespace helix::virtualaudio
