#include <cstdio>
#include <string>

#include "TestFramework.h"
#include "helix/Log.h"

int main(int argc, char** argv) {
    // Logi silnika zaśmiecałyby wynik testów.
    helix::Log::setLevel(helix::LogLevel::Error);

    const std::string filter = (argc > 1) ? argv[1] : std::string();
    std::printf("Helix — testy%s\n\n", filter.empty() ? "" : (" [" + filter + "]").c_str());
    return helix::test::runAll(filter);
}
