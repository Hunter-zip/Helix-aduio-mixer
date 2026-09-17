#include "WasapiCommon.h"

#include <cmath>
#include <cstring>
#include <vector>

namespace helix::platform {

// {a45c254e-df1c-4efd-8020-67d146a850e0}, PID 14 — przyjazna nazwa urządzenia.
const PROPERTYKEY kPropertyDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// {f19f064d-082c-4e27-bc73-6882a1bb8e4c}, PID 0 — format silnika audio urządzenia.
const PROPERTYKEY kPropertyAudioEngineDeviceFormat = {
    {0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

std::string toUtf8(const wchar_t* text) {
    if (text == nullptr) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (length <= 1) return {};
    std::string result(static_cast<std::size_t>(length - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, result.data(), length, nullptr, nullptr);
    return result;
}

std::wstring toUtf16(const std::string& text) {
    if (text.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        result.data(), length);
    return result;
}

std::string describeHresult(HRESULT result) {
    switch (result) {
        case AUDCLNT_E_DEVICE_INVALIDATED:     return "urządzenie zostało odłączone";
        case AUDCLNT_E_DEVICE_IN_USE:          return "urządzenie jest zajęte (tryb wyłączny)";
        case AUDCLNT_E_UNSUPPORTED_FORMAT:     return "urządzenie nie obsługuje tego formatu";
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED: return "tryb wyłączny jest zablokowany w systemie";
        case AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED: return "nieprawidłowe wyrównanie bufora";
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED: return "nie udało się utworzyć punktu końcowego";
        case AUDCLNT_E_SERVICE_NOT_RUNNING:    return "usługa Windows Audio nie działa";
        case AUDCLNT_E_INVALID_DEVICE_PERIOD:  return "nieobsługiwany okres bufora";
        case E_ACCESSDENIED:                   return "brak uprawnień do urządzenia";
        case E_OUTOFMEMORY:                    return "brak pamięci";
        case E_INVALIDARG:                     return "nieprawidłowy argument";
        default: break;
    }

    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "błąd HRESULT 0x%08lX",
                  static_cast<unsigned long>(result));
    return buffer;
}

SampleFormat detectSampleFormat(const WAVEFORMATEX* format) noexcept {
    if (format == nullptr) return SampleFormat::Unsupported;

    WORD tag = format->wFormatTag;
    WORD bits = format->wBitsPerSample;
    WORD validBits = bits;

    if (tag == WAVE_FORMAT_EXTENSIBLE && format->cbSize >= 22) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        validBits = extensible->Samples.wValidBitsPerSample;
        if (IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT))
            tag = WAVE_FORMAT_IEEE_FLOAT;
        else if (IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM))
            tag = WAVE_FORMAT_PCM;
        else
            return SampleFormat::Unsupported;
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32) return SampleFormat::Float32;
    if (tag == WAVE_FORMAT_PCM) {
        if (bits == 16) return SampleFormat::Int16;
        if (bits == 24) return SampleFormat::Int24;
        if (bits == 32) return (validBits == 24) ? SampleFormat::Int32 : SampleFormat::Int32;
    }
    return SampleFormat::Unsupported;
}

void convertToFloat(const void* source, float* destination, int frames, int channels,
                    SampleFormat format) noexcept {
    const std::size_t count = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);

    switch (format) {
        case SampleFormat::Float32:
            std::memcpy(destination, source, count * sizeof(float));
            break;

        case SampleFormat::Int16: {
            const auto* data = static_cast<const std::int16_t*>(source);
            constexpr float scale = 1.0f / 32768.0f;
            for (std::size_t i = 0; i < count; ++i) destination[i] = static_cast<float>(data[i]) * scale;
            break;
        }

        case SampleFormat::Int24: {
            const auto* data = static_cast<const std::uint8_t*>(source);
            constexpr float scale = 1.0f / 8388608.0f;
            for (std::size_t i = 0; i < count; ++i) {
                const std::size_t offset = i * 3u;
                std::int32_t value = static_cast<std::int32_t>(data[offset]) |
                                     (static_cast<std::int32_t>(data[offset + 1]) << 8) |
                                     (static_cast<std::int32_t>(data[offset + 2]) << 16);
                if (value & 0x800000) value |= ~0xFFFFFF;   // rozszerzenie znaku
                destination[i] = static_cast<float>(value) * scale;
            }
            break;
        }

        case SampleFormat::Int32: {
            const auto* data = static_cast<const std::int32_t*>(source);
            constexpr float scale = 1.0f / 2147483648.0f;
            for (std::size_t i = 0; i < count; ++i) destination[i] = static_cast<float>(data[i]) * scale;
            break;
        }

        case SampleFormat::Unsupported:
            std::memset(destination, 0, count * sizeof(float));
            break;
    }
}

void convertFromFloat(const float* source, void* destination, int frames, int channels,
                      SampleFormat format) noexcept {
    const std::size_t count = static_cast<std::size_t>(frames) * static_cast<std::size_t>(channels);

    auto clamp = [](float value) {
        return value < -1.0f ? -1.0f : (value > 1.0f ? 1.0f : value);
    };

    switch (format) {
        case SampleFormat::Float32:
            std::memcpy(destination, source, count * sizeof(float));
            break;

        case SampleFormat::Int16: {
            auto* data = static_cast<std::int16_t*>(destination);
            for (std::size_t i = 0; i < count; ++i)
                data[i] = static_cast<std::int16_t>(clamp(source[i]) * 32767.0f);
            break;
        }

        case SampleFormat::Int24: {
            auto* data = static_cast<std::uint8_t*>(destination);
            for (std::size_t i = 0; i < count; ++i) {
                const auto value = static_cast<std::int32_t>(clamp(source[i]) * 8388607.0f);
                const std::size_t offset = i * 3u;
                data[offset]     = static_cast<std::uint8_t>(value & 0xFF);
                data[offset + 1] = static_cast<std::uint8_t>((value >> 8) & 0xFF);
                data[offset + 2] = static_cast<std::uint8_t>((value >> 16) & 0xFF);
            }
            break;
        }

        case SampleFormat::Int32: {
            auto* data = static_cast<std::int32_t*>(destination);
            for (std::size_t i = 0; i < count; ++i)
                data[i] = static_cast<std::int32_t>(
                    static_cast<double>(clamp(source[i])) * 2147483647.0);
            break;
        }

        case SampleFormat::Unsupported:
            break;
    }
}

} // namespace helix::platform
