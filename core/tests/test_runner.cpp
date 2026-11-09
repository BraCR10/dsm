#include "test_runner.h"

namespace dsm::testing {

std::vector<TestCase>& registeredTests() {
    static std::vector<TestCase> tests;
    return tests;
}

void failAssertion(const char* expression, const char* file, int line) {
    throw AssertionFailure{std::string(file) + ":" + std::to_string(line) + " expected " + expression};
}

}  // namespace dsm::testing

int main() {
    int failedCount = 0;
    for (const auto& test : dsm::testing::registeredTests()) {
        try {
            test.body();
            std::printf("[PASS] %s\n", test.name.c_str());
        } catch (const dsm::testing::AssertionFailure& failure) {
            ++failedCount;
            std::printf("[FAIL] %s: %s\n", test.name.c_str(), failure.message.c_str());
        }
    }
    std::printf("%zu tests, %d failed\n", dsm::testing::registeredTests().size(), failedCount);
    return failedCount == 0 ? 0 : 1;
}
