// Minimal self-registering test harness. No third-party dependency, so the
// suite builds anywhere the libraries build.
//
//   TEST_CASE("name") { CHECK(x == 1); REQUIRE(ptr); CHECK_EQ(a, b); }
//
// CHECK records a failure and continues; REQUIRE stops the test case.
// A test binary accepts an optional substring filter as its first argument.
#pragma once

#include <cstdio>
#include <exception>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace vcc_test {

struct Case {
    const char* name;
    std::function<void()> fn;
    const char* file;
    int line;
};

inline std::vector<Case>& registry()
{
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn, const char* file, int line)
    {
        registry().push_back({name, std::move(fn), file, line});
    }
};

struct State {
    int failures = 0;
    int checks = 0;
};

inline State& state()
{
    static State s;
    return s;
}

struct RequireFailed {};

template <typename T>
std::string show(const T& v)
{
    std::ostringstream ss;
    if constexpr (std::is_same_v<T, bool>) ss << (v ? "true" : "false");
    else if constexpr (std::is_enum_v<T>) ss << static_cast<long long>(v);
    else ss << v;
    return ss.str();
}

inline void fail(const char* file, int line, const std::string& what)
{
    ++state().failures;
    std::fprintf(stderr, "  %s:%d: FAILED: %s\n", file, line, what.c_str());
}

}  // namespace vcc_test

#define VCC_CAT2(a, b) a##b
#define VCC_CAT(a, b) VCC_CAT2(a, b)

#define TEST_CASE(name)                                                                    \
    static void VCC_CAT(vcc_test_fn_, __LINE__)();                                         \
    static ::vcc_test::Registrar VCC_CAT(vcc_test_reg_, __LINE__)(                         \
        name, &VCC_CAT(vcc_test_fn_, __LINE__), __FILE__, __LINE__);                       \
    static void VCC_CAT(vcc_test_fn_, __LINE__)()

#define CHECK(expr)                                                                        \
    do {                                                                                   \
        ++::vcc_test::state().checks;                                                      \
        if (!(expr)) ::vcc_test::fail(__FILE__, __LINE__, #expr);                          \
    } while (0)

#define REQUIRE(expr)                                                                      \
    do {                                                                                   \
        ++::vcc_test::state().checks;                                                      \
        if (!(expr)) {                                                                     \
            ::vcc_test::fail(__FILE__, __LINE__, "REQUIRE " #expr);                        \
            throw ::vcc_test::RequireFailed{};                                             \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b)                                                                     \
    do {                                                                                   \
        ++::vcc_test::state().checks;                                                      \
        const auto vcc_a_ = (a);                                                          \
        const auto vcc_b_ = (b);                                                          \
        if (!(vcc_a_ == vcc_b_))                                                           \
            ::vcc_test::fail(__FILE__, __LINE__,                                           \
                             std::string(#a " == " #b "  (") + ::vcc_test::show(vcc_a_) +  \
                                 " vs " + ::vcc_test::show(vcc_b_) + ")");                 \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                              \
    do {                                                                                   \
        ++::vcc_test::state().checks;                                                      \
        double vcc_d_ = static_cast<double>(a) - static_cast<double>(b);                   \
        if (vcc_d_ < -(eps) || vcc_d_ > (eps))                                             \
            ::vcc_test::fail(__FILE__, __LINE__,                                           \
                             std::string(#a " ~= " #b "  (") + ::vcc_test::show(a) +       \
                                 " vs " + ::vcc_test::show(b) + ")");                      \
    } while (0)
