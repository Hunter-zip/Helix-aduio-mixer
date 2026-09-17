# Testy silnika — jeden plik wykonywalny na obszar, plus osobny dla RT-safety
# (nadpisuje globalny operator new, więc musi żyć w izolacji).

add_executable(helix_tests
    TestMain.cpp
    JsonTests.cpp
    DspTests.cpp
    EngineTests.cpp
    ConcurrencyTests.cpp
    AppTests.cpp
    VirtualAudioTests.cpp
)
target_include_directories(helix_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(helix_tests PRIVATE helix_engine helix_warnings)

add_test(NAME helix_tests COMMAND helix_tests)

add_executable(helix_rt_tests RealtimeSafetyTests.cpp)
target_include_directories(helix_rt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(helix_rt_tests PRIVATE helix_engine helix_warnings)

add_test(NAME helix_rt_tests COMMAND helix_rt_tests)
