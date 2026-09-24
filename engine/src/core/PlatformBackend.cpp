#include "helix/core/AudioBackend.h"
#include "helix/core/NullBackend.h"

#if defined(_WIN32)
#include "helix/platform/WasapiBackend.h"
#endif

#include "helix/Log.h"

namespace helix::core {

std::unique_ptr<AudioBackend> createPlatformBackend() {
#if defined(_WIN32)
    auto backend = createWasapiBackend();
    if (backend) return backend;
    Log::warn("Backend", "WASAPI niedostępne — przełączam na backend programowy");
#endif
    return createNullBackend(true);
}

} // namespace helix::core
