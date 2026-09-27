cmake_minimum_required(VERSION 3.24)

project(chronos LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(
    CHRONOS_ENABLE_SANITIZERS
    "Enable AddressSanitizer and UndefinedBehaviorSanitizer"
    OFF
)

add_library(chronos_build_options INTERFACE)

target_compile_options(chronos_build_options
    INTERFACE
        "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-Wall;-Wextra;-Wpedantic;-Werror>"
)

if(CHRONOS_ENABLE_SANITIZERS)
    if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(
            FATAL_ERROR
            "Sanitizer builds on macOS require AppleClang or Clang"
        )
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(chronos_build_options
            INTERFACE
                -fsanitize=address,undefined
                -fno-omit-frame-pointer
        )

        target_link_options(chronos_build_options
            INTERFACE
                -fsanitize=address,undefined
        )
    else()
        message(
            FATAL_ERROR
            "CHRONOS_ENABLE_SANITIZERS requires GCC, Clang, or AppleClang"
        )
    endif()
endif()

add_library(chronos_accounting
    core/src/paper_account.cpp
)

target_include_directories(chronos_accounting
    PUBLIC core/include
)

target_link_libraries(chronos_accounting
    PRIVATE chronos_build_options
)

include(CTest)

if(BUILD_TESTING)
    add_executable(chronos_core_tests
        tests/core_tests.cpp
    )

    target_link_libraries(chronos_core_tests
        PRIVATE
            chronos_accounting
            chronos_build_options
    )

    add_test(
        NAME chronos_core_tests
        COMMAND chronos_core_tests
    )
endif()
