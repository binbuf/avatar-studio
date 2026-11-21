#pragma once

// A deliberately tiny, zero-dependency test registry. There is no external test
// framework yet; T01 only needs deterministic unit tests that build everywhere.
//
//   TEST_CASE(name) { CHECK(expr); }
//
// Registration is static; main() calls run_all(). A failed CHECK does not abort
// the test case, so one run reports every failing check in that case.

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace avatar::test {

struct TestCase {
    const char* name;
    void (*function)();
};

inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> cases;
    return cases;
}

inline int& failure_count() {
    static int count = 0;
    return count;
}

class Registrar {
public:
    Registrar(const char* name, void (*function)()) {
        registry().push_back(TestCase{name, function});
    }
};

inline void report_failure(const char* file, int line, const std::string& message) {
    ++failure_count();
    std::fprintf(stderr, "  %s(%d): %s\n", file, line, message.c_str());
}

template <typename Actual, typename Expected>
void report_eq_failure(const char* file,
                       int line,
                       const char* expression,
                       const Actual& actual,
                       const Expected& expected) {
    std::ostringstream stream;
    stream << expression << " (actual: " << actual << ", expected: " << expected << ')';
    report_failure(file, line, stream.str());
}

inline int run_all() {
    int failed_tests = 0;
    for (const TestCase& test : registry()) {
        const int before = failure_count();
        std::printf("[ RUN      ] %s\n", test.name);
        test.function();
        if (failure_count() == before) {
            std::printf("[       OK ] %s\n", test.name);
        } else {
            ++failed_tests;
            std::printf("[  FAILED  ] %s\n", test.name);
        }
    }
    std::printf("%zu test case(s), %d failed, %d check failure(s)\n",
                registry().size(),
                failed_tests,
                failure_count());
    return failed_tests == 0 ? 0 : 1;
}

} // namespace avatar::test

#define TEST_CASE(name)                                                     \
    static void name();                                                     \
    static ::avatar::test::Registrar name##_registrar{#name, &name};        \
    static void name()

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            ::avatar::test::report_failure(__FILE__, __LINE__,              \
                                           "CHECK failed: " #expr);         \
        }                                                                   \
    } while (false)

#define CHECK_EQ(actual, expected)                                          \
    do {                                                                    \
        const auto& avatar_test_actual = (actual);                          \
        const auto& avatar_test_expected = (expected);                      \
        if (!(avatar_test_actual == avatar_test_expected)) {                \
            ::avatar::test::report_eq_failure(__FILE__, __LINE__,           \
                                              #actual " == " #expected,     \
                                              avatar_test_actual,           \
                                              avatar_test_expected);        \
        }                                                                   \
    } while (false)
