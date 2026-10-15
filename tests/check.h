#pragma once

// Tiny self-registering test framework, to avoid an external dependency.

#include <cstdio>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace check {

struct Case {
    const char* name;
    std::function<void()> fn;
};

inline std::vector<Case>& registry() {
    static std::vector<Case> cases;
    return cases;
}

inline int& failures() {
    static int n = 0;
    return n;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) { registry().push_back({name, std::move(fn)}); }
};

template <typename T>
std::string describe(const T& v) {
    if constexpr (std::is_arithmetic_v<T> || std::is_enum_v<T>) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "0x%llX", static_cast<unsigned long long>(v));
        return buf;
    } else {
        return "\"" + std::string(v) + "\"";
    }
}

inline int runAll() {
    int failedCases = 0;
    for (auto& c : registry()) {
        int before = failures();
        c.fn();
        if (failures() != before) {
            ++failedCases;
            std::printf("FAIL %s\n", c.name);
        }
    }
    std::printf("%zu tests, %d failed\n", registry().size(), failedCases);
    return failedCases ? 1 : 0;
}

} // namespace check

#define CHECK_CAT2(a, b) a##b
#define CHECK_CAT(a, b) CHECK_CAT2(a, b)

#define TEST_CASE(name)                                                                  \
    static void CHECK_CAT(test_fn_, __LINE__)();                                         \
    static check::Registrar CHECK_CAT(test_reg_, __LINE__)(name, CHECK_CAT(test_fn_, __LINE__)); \
    static void CHECK_CAT(test_fn_, __LINE__)()

#define CHECK_EQ(a, b)                                                                   \
    do {                                                                                 \
        auto va_ = (a);                                                                  \
        auto vb_ = (b);                                                                  \
        if (!(va_ == vb_)) {                                                             \
            std::printf("  %s:%d: CHECK_EQ(%s, %s) failed: %s != %s\n", __FILE__, __LINE__, \
                        #a, #b, check::describe(va_).c_str(), check::describe(vb_).c_str()); \
            ++check::failures();                                                         \
        }                                                                                \
    } while (0)
