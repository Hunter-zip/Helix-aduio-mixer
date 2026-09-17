cmake_minimum_required(VERSION 3.20)
project(HelixAudioMixer VERSION 0.2.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(HELIX_BUILD_TESTS "Buduj testy silnika" ON)
option(HELIX_WERROR      "Traktuj ostrzeżenia jako błędy" ON)

if (NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(helix_warnings INTERFACE)
if (MSVC)
    target_compile_options(helix_warnings INTERFACE /W4 /permissive- $<$<BOOL:${HELIX_WERROR}>:/WX>)
else()
    target_compile_options(helix_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
        -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual
        $<$<BOOL:${HELIX_WERROR}>:-Werror>)
endif()

# ── Rdzeń silnika ────────────────────────────────────────────────────────────
set(HELIX_CORE_SOURCES
    src/Log.cpp
    src/app/AppDetection.cpp
    src/app/AutomationEngine.cpp
    src/app/ControlServer.cpp
    src/app/EngineCommands.cpp
    src/app/EngineController.cpp
    src/app/EngineProfile.cpp
    src/app/HotkeyManager.cpp
    src/app/Json.cpp
    src/app/ProfileManager.cpp
    src/core/AudioEngine.cpp
    src/core/ChannelManager.cpp
    src/core/DeviceManager.cpp
    src/core/Bus.cpp
    src/core/Channel.cpp
    src/core/InputSource.cpp
    src/core/MasterBus.cpp
    src/core/NullBackend.cpp
    src/core/PlatformBackend.cpp
    src/dsp/PluginRegistry.cpp
    src/virtualaudio/DriverInterface.cpp
    src/virtualaudio/SharedRingBuffer.cpp
    src/virtualaudio/VirtualDeviceManager.cpp
)

if (WIN32)
    list(APPEND HELIX_CORE_SOURCES
        platform/windows/WasapiCommon.cpp
        platform/windows/WasapiBackend.cpp
        platform/windows/WindowsAppDetector.cpp
        platform/windows/WindowsHotkeys.cpp
        platform/windows/WindowsDriverInterface.cpp
    )
endif()

add_library(helix_engine STATIC ${HELIX_CORE_SOURCES})
target_include_directories(helix_engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(helix_engine PRIVATE helix_warnings)

find_package(Threads REQUIRED)
target_link_libraries(helix_engine PUBLIC Threads::Threads)

if (UNIX AND NOT APPLE)
    target_link_libraries(helix_engine PUBLIC rt)
endif()

if (WIN32)
    target_link_libraries(helix_engine PUBLIC ole32 oleaut32 uuid winmm avrt ksuser mfplat mmdevapi ws2_32
                                        propsys setupapi advapi32 psapi user32 shell32)
    target_compile_definitions(helix_engine PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# ── Aplikacje ────────────────────────────────────────────────────────────────
add_executable(helix-engine apps/helix-engine/main.cpp)
target_link_libraries(helix-engine PRIVATE helix_engine helix_warnings)

add_executable(helix-cli apps/helix-cli/main.cpp)
target_link_libraries(helix-cli PRIVATE helix_engine helix_warnings)

if (WIN32)
    target_link_libraries(helix-cli PRIVATE ws2_32)
endif()

install(TARGETS helix-engine helix-cli RUNTIME DESTINATION bin)

if (HELIX_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
