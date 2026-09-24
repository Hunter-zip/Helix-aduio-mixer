// Minimalny framework testowy — bez zależności zewnętrznych, żeby build
// działał wszędzie tak samo (także w CI bez dostępu do sieci).
#pragma once

#include <cmath>
#include <cstdio>
#include <exception>
#include <functional>
#include <string>
#include <vector>

namespace helix::test {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body) {
        registry().push_back(TestCase{name, std::move(body)});
    }
};

class AssertionFailure : public std::exception {
public:
    explicit AssertionFailure(std::string message) : message_(std::move(message)) {}
    [[nodiscard]] const char* what() const noexcept override { return message_.c_str(); }

private:
    std::string message_;
};

inline void fail(const char* file, int line, const std::string& message) {
    throw AssertionFailure(std::string(file) + ":" + std::to_string(line) + " — " + message);
}

inline bool nearlyEqual(double a, double b, double tolerance) {
    return std::fabs(a - b) <= tolerance;
}

inline int runAll(const std::string& filter) {
    int passed = 0;
    int failed = 0;

    for (const auto& test : registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) continue;
        try {
            test.body();
            std::printf("  \033[32mOK\033[0m   %s\n", test.name.c_str());
            ++passed;
        } catch (const std::exception& error) {
            std::printf("  \033[31mFAIL\033[0m %s\n       %s\n", test.name.c_str(), error.what());
            ++failed;
        } catch (...) {
            std::printf("  \033[31mFAIL\033[0m %s\n       nieznany wyjątek\n", test.name.c_str());
            ++failed;
        }
    }

    std::printf("\n%d testów przeszło, %d nie przeszło\n", passed, failed);
    return failed == 0 ? 0 : 1;
}

} // namespace helix::test

#define HELIX_CONCAT_INNER(a, b) a##b
#define HELIX_CONCAT(a, b) HELIX_CONCAT_INNER(a, b)

#define TEST(name)                                                                    \
    static void HELIX_CONCAT(helixTest_, __LINE__)();                                 \
    static const ::helix::test::Registrar HELIX_CONCAT(helixRegistrar_, __LINE__)(    \
        name, []() { HELIX_CONCAT(helixTest_, __LINE__)(); });                        \
    static void HELIX_CONCAT(helixTest_, __LINE__)()

#define CHECK(condition)                                                              \
    do {                                                                              \
        if (!(condition))                                                             \
            ::helix::test::fail(__FILE__, __LINE__, "warunek nieprawdziwy: " #condition); \
    } while (false)

#define CHECK_MSG(condition, message)                                                 \
    do {                                                                              \
        if (!(condition))                                                             \
            ::helix::test::fail(__FILE__, __LINE__, std::string(message));            \
    } while (false)

#define CHECK_EQ(actual, expected)                                                    \
    do {                                                                              \
        const auto helixActual = (actual);                                            \
        const auto helixExpected = (expected);                                        \
        if (!(helixActual == helixExpected))                                          \
            ::helix::test::fail(__FILE__, __LINE__,                                   \
                                std::string("oczekiwano ") + std::to_string(helixExpected) + \
                                    ", otrzymano " + std::to_string(helixActual));    \
    } while (false)

#define CHECK_STR_EQ(actual, expected)                                                \
    do {                                                                              \
        const std::string helixActual = (actual);                                     \
        const std::string helixExpected = (expected);                                 \
        if (helixActual != helixExpected)                                             \
            ::helix::test::fail(__FILE__, __LINE__,                                   \
                                "oczekiwano \"" + helixExpected + "\", otrzymano \"" + \
                                    helixActual + "\"");                              \
    } while (false)

#define CHECK_NEAR(actual, expected, tolerance)                                       \
    do {                                                                              \
        const double helixActual = static_cast<double>(actual);                       \
        const double helixExpected = static_cast<double>(expected);                   \
        if (!::helix::test::nearlyEqual(helixActual, helixExpected, (tolerance)))      \
            ::helix::test::fail(__FILE__, __LINE__,                                   \
                                "oczekiwano " + std::to_string(helixExpected) +       \
                                    " ±" + std::to_string(static_cast<double>(tolerance)) + \
                                    ", otrzymano " + std::to_string(helixActual));    \
    } while (false)
