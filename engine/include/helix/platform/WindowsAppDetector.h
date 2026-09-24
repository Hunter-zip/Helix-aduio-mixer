// Wykrywanie sesji audio aplikacji przez WASAPI (spec §5).
#pragma once

#include <memory>

#include "helix/app/AppDetection.h"

namespace helix::app {

[[nodiscard]] std::unique_ptr<AppDetector> createWindowsAppDetector();

} // namespace helix::app
