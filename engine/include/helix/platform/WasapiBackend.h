// Backend WASAPI (Windows 10/11) — spec §2, §5, §20.
#pragma once

#include <memory>

#include "helix/core/AudioBackend.h"

namespace helix::core {

/// Tworzy backend WASAPI. Zwraca nullptr, gdy COM lub API audio są niedostępne.
[[nodiscard]] std::unique_ptr<AudioBackend> createWasapiBackend();

} // namespace helix::core
