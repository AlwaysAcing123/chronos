cmake_minimum_required(VERSION 3.24)

project(chronos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(chronos_accounting
    core/src/paper_account.cpp
)

target_include_directories(chronos_accounting
    PUBLIC core/include
)

target_compile_options(chronos_accounting
    PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Werror
)

include(CTest)

if(BUILD_TESTING)
    add_executable(chronos_core_tests
        tests/core_tests.cpp
    )

    target_link_libraries(chronos_core_tests
        PRIVATE chronos_accounting
    )

    target_compile_options(chronos_core_tests
        PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Werror
    )

    add_test(
        NAME chronos_core_tests
        COMMAND chronos_core_tests
    )
endif()
