// Moduł instalacji sterownika wirtualnego audio na Windows (spec §7).
#pragma once

#include <memory>

#include "helix/virtualaudio/DriverInterface.h"

namespace helix::virtualaudio {

[[nodiscard]] std::unique_ptr<DriverInterface> createWindowsDriverInterface();

} // namespace helix::virtualaudio
