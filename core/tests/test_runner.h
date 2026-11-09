#ifndef DSM_TESTS_TEST_RUNNER_H
#define DSM_TESTS_TEST_RUNNER_H

#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace dsm::testing {

struct TestCase {
    std::string name;
    std::function<void()> body;
};

std::vector<TestCase>& registeredTests();

struct TestRegistration {
    TestRegistration(const char* name, std::function<void()> body) {
        registeredTests().push_back({name, std::move(body)});
    }
};

struct AssertionFailure {
    std::string message;
};

[[noreturn]] void failAssertion(const char* expression, const char* file, int line);

}  // namespace dsm::testing

#define DSM_TEST(testName)                                                              \
    static void testName();                                                             \
    static ::dsm::testing::TestRegistration registration_##testName(#testName, testName); \
    static void testName()

#define EXPECT_TRUE(expression)                                                \
    do {                                                                       \
        if (!(expression)) {                                                   \
            ::dsm::testing::failAssertion(#expression, __FILE__, __LINE__);    \
        }                                                                      \
    } while (false)

#define EXPECT_EQ(expected, actual) EXPECT_TRUE((expected) == (actual))

#endif
