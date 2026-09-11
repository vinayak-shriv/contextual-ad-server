#pragma once

// A deliberately tiny test harness (no external dependency to install).
// TEST(name) registers a function; CHECK* macros record failures without aborting,
// so one run reports every broken assertion.

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace mini_test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failures() {
    static int count = 0;
    return count;
}

struct Registrar {
    Registrar(const char* name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

inline void report_failure(const char* file, int line, const std::string& msg) {
    ++failures();
    std::cerr << "    FAIL " << file << ":" << line << "  " << msg << "\n";
}

inline int run_all() {
    int failed_tests = 0;
    for (const auto& t : registry()) {
        const int before = failures();
        t.fn();
        const bool ok = failures() == before;
        if (!ok) ++failed_tests;
        std::cout << (ok ? "[ PASS ] " : "[ FAIL ] ") << t.name << "\n";
    }
    std::cout << "\n" << registry().size() - failed_tests << "/" << registry().size()
              << " tests passed\n";
    return failed_tests == 0 ? 0 : 1;
}

}  // namespace mini_test

#define MT_CONCAT_INNER(a, b) a##b
#define MT_CONCAT(a, b) MT_CONCAT_INNER(a, b)

#define TEST(name)                                                              \
    static void name();                                                         \
    static ::mini_test::Registrar MT_CONCAT(registrar_, name)(#name, &name);    \
    static void name()

#define CHECK(cond)                                                             \
    do {                                                                        \
        if (!(cond)) ::mini_test::report_failure(__FILE__, __LINE__, #cond);    \
    } while (0)

#define CHECK_EQ(a, b)                                                          \
    do {                                                                        \
        const auto& va_ = (a);                                                  \
        const auto& vb_ = (b);                                                  \
        if (!(va_ == vb_)) {                                                    \
            std::ostringstream os_;                                             \
            os_ << #a " == " #b "  (" << va_ << " vs " << vb_ << ")";           \
            ::mini_test::report_failure(__FILE__, __LINE__, os_.str());         \
        }                                                                       \
    } while (0)

#define CHECK_NEAR(a, b, eps)                                                   \
    do {                                                                        \
        const double va_ = (a);                                                 \
        const double vb_ = (b);                                                 \
        if (std::fabs(va_ - vb_) > (eps)) {                                     \
            std::ostringstream os_;                                             \
            os_ << #a " ~= " #b "  (" << va_ << " vs " << vb_ << ")";           \
            ::mini_test::report_failure(__FILE__, __LINE__, os_.str());         \
        }                                                                       \
    } while (0)
