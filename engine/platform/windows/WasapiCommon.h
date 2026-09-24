// Wspólne definicje dla warstwy Windows: COM, konwersje formatów, uchwyty RAII.
#pragma once

#if !defined(_WIN32)
#error "Ten plik jest przeznaczony wyłącznie dla Windows"
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

// Parametry aktywacji klienta audio dla przechwytywania per proces (Windows 10 20H1+).
// Starsze zestawy nagłówków (np. mingw-w64) ich nie mają — definiujemy je lokalnie,
// zgodnie z ABI z Windows SDK.
#if __has_include(<audioclientactivationparams.h>)
  #include <audioclientactivationparams.h>
#else
typedef enum HELIX_AUDIOCLIENT_ACTIVATION_TYPE {
    HELIX_AUDIOCLIENT_ACTIVATION_TYPE_DEFAULT = 0,
    HELIX_AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK = 1
} HELIX_AUDIOCLIENT_ACTIVATION_TYPE;

typedef enum HELIX_PROCESS_LOOPBACK_MODE {
    HELIX_PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE = 0,
    HELIX_PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE = 1
} HELIX_PROCESS_LOOPBACK_MODE;

typedef struct HELIX_AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS {
    DWORD TargetProcessId;
    HELIX_PROCESS_LOOPBACK_MODE ProcessLoopbackMode;
} HELIX_AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS;

typedef struct HELIX_AUDIOCLIENT_ACTIVATION_PARAMS {
    HELIX_AUDIOCLIENT_ACTIVATION_TYPE ActivationType;
    union {
        HELIX_AUDIOCLIENT_PROCESS_LOOPBACK_PARAMS ProcessLoopbackParams;
    } DUMMYUNIONNAME;
} HELIX_AUDIOCLIENT_ACTIVATION_PARAMS;

#define AUDIOCLIENT_ACTIVATION_PARAMS HELIX_AUDIOCLIENT_ACTIVATION_PARAMS
#define AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK HELIX_AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK
#define PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE \
    HELIX_PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE
#define PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE \
    HELIX_PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE
#endif

#ifndef VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK
#define VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK L"VAD\\Process_Loopback"
#endif

namespace helix::platform {

// Podtypy formatu i maski kanałów definiujemy samodzielnie zamiast ciągnąć
// `ksmedia.h`. Te wartości są częścią stabilnego ABI Windows, a ich lokalizacja
// w nagłówkach różni się między Windows SDK a mingw-w64 — jedno źródło prawdy
// oszczędza kłopotów przy każdym z nich.

/// KSDATAFORMAT_SUBTYPE_PCM — {00000001-0000-0010-8000-00AA00389B71}
inline const GUID kSubFormatPcm = {
    0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

/// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT — {00000003-0000-0010-8000-00AA00389B71}
inline const GUID kSubFormatIeeeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

inline constexpr DWORD kSpeakerFrontLeft   = 0x1;
inline constexpr DWORD kSpeakerFrontRight  = 0x2;
inline constexpr DWORD kSpeakerFrontCenter = 0x4;

/// Prosty wskaźnik COM z licznikiem referencji.
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { reset(); }

    ComPtr(const ComPtr& other) : pointer_(other.pointer_) {
        if (pointer_) pointer_->AddRef();
    }

    ComPtr& operator=(const ComPtr& other) {
        if (this != &other) {
            reset();
            pointer_ = other.pointer_;
            if (pointer_) pointer_->AddRef();
        }
        return *this;
    }

    ComPtr(ComPtr&& other) noexcept : pointer_(other.pointer_) { other.pointer_ = nullptr; }

    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset();
            pointer_ = other.pointer_;
            other.pointer_ = nullptr;
        }
        return *this;
    }

    void reset() {
        if (pointer_) {
            pointer_->Release();
            pointer_ = nullptr;
        }
    }

    [[nodiscard]] T*  get() const noexcept { return pointer_; }
    [[nodiscard]] T** put() noexcept { reset(); return &pointer_; }
    [[nodiscard]] void** putVoid() noexcept { reset(); return reinterpret_cast<void**>(&pointer_); }

    T* operator->() const noexcept { return pointer_; }
    explicit operator bool() const noexcept { return pointer_ != nullptr; }

private:
    T* pointer_ = nullptr;
};

/// RAII dla pamięci zwracanej przez CoTaskMemAlloc.
template <typename T>
class CoMem {
public:
    CoMem() = default;
    ~CoMem() { if (pointer_) CoTaskMemFree(pointer_); }

    CoMem(const CoMem&) = delete;
    CoMem& operator=(const CoMem&) = delete;

    [[nodiscard]] T*  get() const noexcept { return pointer_; }
    [[nodiscard]] T** put() noexcept {
        if (pointer_) { CoTaskMemFree(pointer_); pointer_ = nullptr; }
        return &pointer_;
    }

private:
    T* pointer_ = nullptr;
};

/// RAII dla uchwytu zdarzenia.
class EventHandle {
public:
    EventHandle() = default;
    explicit EventHandle(HANDLE handle) : handle_(handle) {}
    ~EventHandle() { reset(); }

    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;

    EventHandle(EventHandle&& other) noexcept : handle_(other.handle_) { other.handle_ = nullptr; }

    EventHandle& operator=(EventHandle&& other) noexcept {
        if (this != &other) {
            reset();
            handle_ = other.handle_;
            other.handle_ = nullptr;
        }
        return *this;
    }

    bool create() {
        reset();
        handle_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return handle_ != nullptr;
    }

    void reset() {
        if (handle_) { CloseHandle(handle_); handle_ = nullptr; }
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    explicit operator bool() const noexcept { return handle_ != nullptr; }

private:
    HANDLE handle_ = nullptr;
};

/// Inicjalizacja COM w trybie wielowątkowym, per wątek.
class ComApartment {
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        initialized_ = SUCCEEDED(result);
        alreadyInitialized_ = (result == RPC_E_CHANGED_MODE);
    }

    ~ComApartment() {
        if (initialized_ && !alreadyInitialized_) CoUninitialize();
    }

    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;

    [[nodiscard]] bool ok() const noexcept { return initialized_ || alreadyInitialized_; }

private:
    bool initialized_ = false;
    bool alreadyInitialized_ = false;
};

/// Klucze właściwości urządzeń audio.
///
/// Definiujemy je lokalnie, bo nie każdy zestaw nagłówków (np. mingw-w64)
/// udostępnia komplet — a wartości GUID są częścią stabilnego API Windows.
extern const PROPERTYKEY kPropertyDeviceFriendlyName;
extern const PROPERTYKEY kPropertyAudioEngineDeviceFormat;

/// Konwersja UTF-16 → UTF-8.
[[nodiscard]] std::string toUtf8(const wchar_t* text);

/// Konwersja UTF-8 → UTF-16.
[[nodiscard]] std::wstring toUtf16(const std::string& text);

/// Czytelny opis kodu HRESULT.
[[nodiscard]] std::string describeHresult(HRESULT result);

/// Rozpoznany format próbek urządzenia.
enum class SampleFormat : std::uint8_t { Float32, Int16, Int24, Int32, Unsupported };

[[nodiscard]] SampleFormat detectSampleFormat(const WAVEFORMATEX* format) noexcept;

/// Konwersja bloku urządzenia → float32 (przeplot).
void convertToFloat(const void* source, float* destination, int frames, int channels,
                    SampleFormat format) noexcept;

/// Konwersja float32 → format urządzenia.
void convertFromFloat(const float* source, void* destination, int frames, int channels,
                      SampleFormat format) noexcept;

} // namespace helix::platform
