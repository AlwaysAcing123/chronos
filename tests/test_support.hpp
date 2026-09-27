#pragma once

#include <cstddef>
#include <exception>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace chronos::test {

class TestFailure final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

inline void require(bool condition, std::string_view expression, int line) {
    if (!condition) {
        throw TestFailure(
            "line " + std::to_string(line) + ": requirement failed: " +
            std::string(expression)
        );
    }
}

template <typename Exception, typename Function>
void require_throws(Function&& function, int line) {
    try {
        std::invoke(std::forward<Function>(function));
    } catch (const Exception&) {
        return;
    } catch (const std::exception& error) {
        throw TestFailure(
            "line " + std::to_string(line) +
            ": expected a different exception; got: " + error.what()
        );
    }
    throw TestFailure(
        "line " + std::to_string(line) + ": expected an exception"
    );
}

struct TestCase {
    std::string_view name;
    void (*function)();
};

inline int run_tests(std::initializer_list<TestCase> tests) {
    int failed = 0;
    for (const auto& test : tests) {
        try {
            test.function();
            std::cout << "[PASS] " << test.name << '\n';
        } catch (const std::exception& failure) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": "
                      << failure.what() << '\n';
        } catch (...) {
            ++failed;
            std::cerr << "[FAIL] " << test.name << ": unknown exception\n";
        }
    }

    std::cout << tests.size() - static_cast<std::size_t>(failed) << "/"
              << tests.size() << " tests passed\n";
    return failed == 0 ? 0 : 1;
}

}  // namespace chronos::test

#define REQUIRE(expression) \
    ::chronos::test::require((expression), #expression, __LINE__)

#define REQUIRE_THROWS_AS(expression, exception_type) \
    ::chronos::test::require_throws<exception_type>( \
        [&] { static_cast<void>(expression); }, \
        __LINE__ \
    )
