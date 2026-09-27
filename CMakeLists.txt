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

option(
    CHRONOS_ENABLE_DATABENTO_ADAPTER
    "Build the optional Databento provider adapter"
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

add_library(chronos_market_data
    core/src/market_data_stream_validator.cpp
    core/src/market_data_validation.cpp
)

target_include_directories(chronos_market_data
    PUBLIC core/include
)

target_link_libraries(chronos_market_data
    PRIVATE chronos_build_options
)

add_library(chronos_market_replay
    core/src/current_market_view.cpp
    core/src/market_replay.cpp
    core/src/replay_clock.cpp
)

target_include_directories(chronos_market_replay
    PUBLIC core/include
)

target_link_libraries(chronos_market_replay
    PUBLIC chronos_market_data
    PRIVATE chronos_build_options
)

if(CHRONOS_ENABLE_DATABENTO_ADAPTER)
    include(FetchContent)

    set(DATABENTO_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
    set(DATABENTO_ENABLE_UNIT_TESTING OFF CACHE BOOL "" FORCE)
    set(DATABENTO_ENABLE_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(DATABENTO_ENABLE_CCACHE OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        databento
        URL
            https://github.com/databento/databento-cpp/archive/4e0cc2eded5ee3cff280ca5949f0ba251010afbf.tar.gz
        URL_HASH
            SHA256=9a10684c8925c850b22e5895fcdc2eccf791044e9ad364b8ab1a77029c18010c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(databento)

    add_library(chronos_databento_adapter
        adapters/databento/src/databento_historical_recording_provider.cpp
        adapters/databento/src/equities_mini_mbp1_file_reader.cpp
        adapters/databento/src/equities_mini_mbp1_normalizer.cpp
        adapters/databento/src/historical_recording_acquisition.cpp
        adapters/databento/src/recording_manifest.cpp
    )

    target_include_directories(chronos_databento_adapter
        PUBLIC adapters/databento/include
    )

    target_link_libraries(chronos_databento_adapter
        PUBLIC
            chronos_market_data
            databento::databento
        PRIVATE chronos_build_options
    )

    add_executable(chronos_databento_historical
        tools/databento_historical.cpp
    )

    target_link_libraries(chronos_databento_historical
        PRIVATE
            chronos_databento_adapter
            chronos_build_options
    )
endif()

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

    add_executable(chronos_market_data_tests
        tests/market_data_validation_tests.cpp
    )

    target_link_libraries(chronos_market_data_tests
        PRIVATE
            chronos_market_data
            chronos_build_options
    )

    add_test(
        NAME chronos_market_data_tests
        COMMAND chronos_market_data_tests
    )

    add_executable(chronos_market_data_stream_tests
        tests/market_data_stream_validator_tests.cpp
    )

    target_link_libraries(chronos_market_data_stream_tests
        PRIVATE
            chronos_market_data
            chronos_build_options
    )

    add_test(
        NAME chronos_market_data_stream_tests
        COMMAND chronos_market_data_stream_tests
    )

    add_executable(chronos_market_replay_tests
        tests/market_replay_tests.cpp
    )

    target_link_libraries(chronos_market_replay_tests
        PRIVATE
            chronos_market_replay
            chronos_build_options
    )

    add_test(
        NAME chronos_market_replay_tests
        COMMAND chronos_market_replay_tests
    )

    if(CHRONOS_ENABLE_DATABENTO_ADAPTER)
        add_executable(chronos_databento_adapter_tests
            tests/databento_equities_mini_mbp1_tests.cpp
        )

        target_link_libraries(chronos_databento_adapter_tests
            PRIVATE
                chronos_databento_adapter
                chronos_build_options
        )

        add_test(
            NAME chronos_databento_adapter_tests
            COMMAND chronos_databento_adapter_tests
        )

        add_executable(chronos_databento_file_reader_tests
            tests/databento_equities_mini_file_reader_tests.cpp
        )

        target_link_libraries(chronos_databento_file_reader_tests
            PRIVATE
                chronos_databento_adapter
                chronos_market_replay
                chronos_build_options
        )

        add_test(
            NAME chronos_databento_file_reader_tests
            COMMAND chronos_databento_file_reader_tests
        )
    endif()
endif()
