// Copyright (c) VC Composer. All rights reserved.
//
// Minimal test harness.
//
// Deliberately dependency-free: a build machine that has to fetch a test
// framework is a build machine that can fail for reasons unrelated to the
// code, and the signing/release machines in particular should be able to run
// the suite offline.
#pragma once

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

namespace vcc::test {

struct TestCase {
    const char* name;
    std::function<void()> body;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> body)
    {
        registry().push_back(TestCase{name, std::move(body)});
    }
};

/// Thrown by a failed assertion; caught by the runner so one failure does not
/// abort the remaining cases.
struct Failure {
    std::string message;
};

inline void fail(const std::string& message)
{
    throw Failure{message};
}

inline std::string location(const char* file, int line)
{
    return std::string(file) + ":" + std::to_string(line);
}

inline bool nearly_equal(double a, double b, double tolerance)
{
    return std::fabs(a - b) <= tolerance;
}

int run_all(const char* suite_name);

}  // namespace vcc::test

// ---------------------------------------------------------------------------
// Macros
// ---------------------------------------------------------------------------

#define VCC_TEST(name)                                                                 \
    static void name();                                                                \
    static ::vcc::test::Registrar registrar_##name(#name, name);                       \
    static void name()

// Variadic: an assertion whose expression contains a brace-initializer with
// commas (VCC_CHECK(TimeSignature{4, 4}.is_valid())) would otherwise look
// like two macro arguments.
#define VCC_CHECK(...)                                                                 \
    do {                                                                               \
        if (!(__VA_ARGS__)) {                                                          \
            ::vcc::test::fail(::vcc::test::location(__FILE__, __LINE__)                \
                              + ": expected " #__VA_ARGS__);                           \
        }                                                                              \
    } while (false)

#define VCC_CHECK_MSG(condition, message)                                              \
    do {                                                                               \
        if (!(condition)) {                                                            \
            ::vcc::test::fail(::vcc::test::location(__FILE__, __LINE__) + ": "          \
                              + std::string(message));                                 \
        }                                                                              \
    } while (false)

#define VCC_CHECK_EQ(actual, expected)                                                 \
    do {                                                                               \
        const auto vcc_actual = (actual);                                              \
        const auto vcc_expected = (expected);                                          \
        if (!(vcc_actual == vcc_expected)) {                                           \
            ::vcc::test::fail(::vcc::test::location(__FILE__, __LINE__)                \
                              + ": " #actual " == " #expected " failed");              \
        }                                                                              \
    } while (false)

#define VCC_CHECK_NEAR(actual, expected, tolerance)                                    \
    do {                                                                               \
        if (!::vcc::test::nearly_equal((actual), (expected), (tolerance))) {            \
            ::vcc::test::fail(::vcc::test::location(__FILE__, __LINE__)                \
                              + ": " #actual " not within " #tolerance " of " #expected); \
        }                                                                              \
    } while (false)

/// Defines main() for a test binary.
#define VCC_TEST_MAIN(suite_name)                                                      \
    int main()                                                                         \
    {                                                                                  \
        return ::vcc::test::run_all(suite_name);                                       \
    }                                                                                  \
    namespace vcc::test {                                                              \
    int run_all(const char* name)                                                      \
    {                                                                                  \
        int failures = 0;                                                              \
        std::printf("[ suite ] %s (%zu cases)\n", name, registry().size());            \
        for (const TestCase& test_case : registry()) {                                 \
            try {                                                                      \
                test_case.body();                                                      \
                std::printf("[   ok  ] %s\n", test_case.name);                          \
            } catch (const Failure& failure) {                                          \
                ++failures;                                                             \
                std::printf("[ FAIL  ] %s\n           %s\n", test_case.name,            \
                            failure.message.c_str());                                   \
            } catch (const std::exception& error) {                                      \
                ++failures;                                                             \
                std::printf("[ FAIL  ] %s\n           unexpected exception: %s\n",       \
                            test_case.name, error.what());                              \
            } catch (...) {                                                             \
                ++failures;                                                             \
                std::printf("[ FAIL  ] %s\n           unknown exception\n",              \
                            test_case.name);                                            \
            }                                                                           \
        }                                                                               \
        if (failures == 0)                                                              \
            std::printf("[ passed] %s: all %zu cases\n", name, registry().size());       \
        else                                                                            \
            std::printf("[ failed] %s: %d of %zu cases\n", name, failures,               \
                        registry().size());                                             \
        return failures == 0 ? 0 : 1;                                                   \
    }                                                                                   \
    }
