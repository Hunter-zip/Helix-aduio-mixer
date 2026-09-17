// Globalne skróty klawiszowe przez RegisterHotKey (spec §17).
#pragma once

#include <memory>

#include "helix/app/HotkeyManager.h"

namespace helix::app {

[[nodiscard]] std::unique_ptr<HotkeyBackend> createWindowsHotkeyBackend();

} // namespace helix::app
