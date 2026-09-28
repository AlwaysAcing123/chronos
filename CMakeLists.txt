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

option(
    CHRONOS_ENABLE_POSTGRESQL_ADAPTER
    "Build the optional PostgreSQL paper-trading persistence adapter"
    OFF
)

option(
    CHRONOS_ENABLE_PAPER_TRADING_API
    "Build the optional paper-trading HTTP API (requires PostgreSQL)"
    OFF
)

if(CHRONOS_ENABLE_PAPER_TRADING_API AND
   NOT CHRONOS_ENABLE_POSTGRESQL_ADAPTER)
    message(
        FATAL_ERROR
        "CHRONOS_ENABLE_PAPER_TRADING_API requires "
        "CHRONOS_ENABLE_POSTGRESQL_ADAPTER"
    )
endif()

find_package(Threads REQUIRED)

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

add_library(chronos_market_state
    core/src/market_state.cpp
)

target_include_directories(chronos_market_state
    PUBLIC core/include
)

target_link_libraries(chronos_market_state
    PUBLIC
        chronos_market_replay
        Threads::Threads
    PRIVATE chronos_build_options
)

add_library(chronos_paper_execution
    core/src/paper_order_codes.cpp
    core/src/paper_order_processor.cpp
    core/src/pre_trade_risk.cpp
    core/src/top_of_book_execution.cpp
)

target_include_directories(chronos_paper_execution
    PUBLIC core/include
)

target_link_libraries(chronos_paper_execution
    PUBLIC
        chronos_accounting
        chronos_market_data
    PRIVATE chronos_build_options
)

if(CHRONOS_ENABLE_POSTGRESQL_ADAPTER)
    find_package(PostgreSQL REQUIRED)

    add_library(chronos_postgresql_persistence
        adapters/postgresql/src/postgresql_paper_order_repository.cpp
    )

    target_include_directories(chronos_postgresql_persistence
        PUBLIC adapters/postgresql/include
    )

    target_link_libraries(chronos_postgresql_persistence
        PUBLIC
            chronos_paper_execution
            PostgreSQL::PostgreSQL
        PRIVATE chronos_build_options
    )
endif()

if(CHRONOS_ENABLE_PAPER_TRADING_API)
    include(FetchContent)

    # These dependency names match the Databento SDK's declarations. The first
    # declaration wins, so a build that also enables the Databento adapter
    # uses these hash-verified copies. See ADR 0004.
    FetchContent_Declare(
        json
        URL
            https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        URL_HASH
            SHA256=42f6e95cad6ec532fd372391373363b62a14af6d771056dbfc86160e6dfff7aa
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )

    # cpp-httplib is configured by preprocessor macros that its target
    # publishes to every consumer. Translation units compiled with different
    # macros in one binary violate the one-definition rule, so this is the
    # only httplib configuration in the build. The Databento historical client
    # requires OpenSSL; the API server needs neither TLS nor compression.
    set(
        HTTPLIB_REQUIRE_OPENSSL
        ${CHRONOS_ENABLE_DATABENTO_ADAPTER}
        CACHE BOOL "" FORCE
    )
    set(HTTPLIB_USE_OPENSSL_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_ZLIB_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    set(HTTPLIB_USE_ZSTD_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        httplib
        URL
            https://github.com/yhirose/cpp-httplib/archive/4f3f9ef19be83ae97a5d9a059432dc00e445b7ab.tar.gz
        URL_HASH
            SHA256=415e8d638f6e2390f51565d4fdc86577dc0ef09f56da7ea74d7731d9da8349a6
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(json httplib)

    # Third-party headers must not be subject to Chronos's -Werror policy.
    foreach(dependency IN ITEMS nlohmann_json httplib)
        get_target_property(
            dependency_includes
            ${dependency}
            INTERFACE_INCLUDE_DIRECTORIES
        )
        set_target_properties(${dependency}
            PROPERTIES
                INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${dependency_includes}"
        )
    endforeach()

    add_library(chronos_paper_trading_service
        services/paper_trading/src/paper_trading_service.cpp
    )

    target_include_directories(chronos_paper_trading_service
        PUBLIC services/paper_trading/include
    )

    target_link_libraries(chronos_paper_trading_service
        PUBLIC
            chronos_market_state
            chronos_postgresql_persistence
        PRIVATE chronos_build_options
    )

    add_library(chronos_paper_trading_http
        services/paper_trading/src/paper_trading_http_api.cpp
        services/paper_trading/src/paper_trading_http_contract.cpp
        services/paper_trading/src/paper_trading_http_server.cpp
    )

    target_link_libraries(chronos_paper_trading_http
        PUBLIC chronos_paper_trading_service
        PRIVATE
            chronos_build_options
            httplib::httplib
            nlohmann_json::nlohmann_json
    )

    add_executable(chronos_paper_trading_server
        services/paper_trading/src/paper_trading_server_main.cpp
    )

    target_link_libraries(chronos_paper_trading_server
        PRIVATE
            chronos_paper_trading_http
            chronos_build_options
    )

    # Replaying a local DBN recording into the server's market state needs
    # the optional Databento adapter, which is defined later in this file.
    if(CHRONOS_ENABLE_DATABENTO_ADAPTER)
        target_compile_definitions(chronos_paper_trading_server
            PRIVATE CHRONOS_PAPER_TRADING_SERVER_WITH_DATABENTO_REPLAY
        )
        target_link_libraries(chronos_paper_trading_server
            PRIVATE chronos_databento_adapter
        )
    endif()
endif()

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
        adapters/databento/src/secret_redaction.cpp
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

    add_executable(chronos_databento_replay
        tools/databento_replay.cpp
    )

    target_link_libraries(chronos_databento_replay
        PRIVATE
            chronos_databento_adapter
            chronos_market_replay
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

    add_executable(chronos_market_state_tests
        tests/market_state_tests.cpp
    )

    target_link_libraries(chronos_market_state_tests
        PRIVATE
            chronos_market_state
            chronos_build_options
    )

    add_test(
        NAME chronos_market_state_tests
        COMMAND chronos_market_state_tests
    )

    add_executable(chronos_paper_execution_tests
        tests/paper_execution_tests.cpp
    )

    target_link_libraries(chronos_paper_execution_tests
        PRIVATE
            chronos_paper_execution
            chronos_build_options
    )

    add_test(
        NAME chronos_paper_execution_tests
        COMMAND chronos_paper_execution_tests
    )

    add_executable(chronos_paper_order_codes_tests
        tests/paper_order_codes_tests.cpp
    )

    target_link_libraries(chronos_paper_order_codes_tests
        PRIVATE
            chronos_paper_execution
            chronos_build_options
    )

    add_test(
        NAME chronos_paper_order_codes_tests
        COMMAND chronos_paper_order_codes_tests
    )

    add_executable(chronos_paper_order_processor_tests
        tests/paper_order_processor_tests.cpp
    )

    target_link_libraries(chronos_paper_order_processor_tests
        PRIVATE
            chronos_paper_execution
            chronos_build_options
    )

    add_test(
        NAME chronos_paper_order_processor_tests
        COMMAND chronos_paper_order_processor_tests
    )

    if(CHRONOS_ENABLE_POSTGRESQL_ADAPTER)
        add_executable(chronos_postgresql_persistence_tests
            tests/postgresql_paper_order_repository_tests.cpp
        )

        target_compile_definitions(chronos_postgresql_persistence_tests
            PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(chronos_postgresql_persistence_tests
            PRIVATE
                chronos_postgresql_persistence
                chronos_build_options
                Threads::Threads
        )

        add_test(
            NAME chronos_postgresql_persistence_tests
            COMMAND chronos_postgresql_persistence_tests
        )

        set_tests_properties(chronos_postgresql_persistence_tests
            PROPERTIES SKIP_RETURN_CODE 77
        )
    endif()

    if(CHRONOS_ENABLE_PAPER_TRADING_API)
        add_executable(chronos_paper_trading_http_contract_tests
            tests/paper_trading_http_contract_tests.cpp
        )

        target_link_libraries(chronos_paper_trading_http_contract_tests
            PRIVATE
                chronos_paper_trading_http
                chronos_build_options
                nlohmann_json::nlohmann_json
        )

        add_test(
            NAME chronos_paper_trading_http_contract_tests
            COMMAND chronos_paper_trading_http_contract_tests
        )

        add_executable(chronos_paper_trading_service_tests
            tests/paper_trading_service_tests.cpp
        )

        target_compile_definitions(chronos_paper_trading_service_tests
            PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(chronos_paper_trading_service_tests
            PRIVATE
                chronos_paper_trading_service
                chronos_build_options
        )

        add_test(
            NAME chronos_paper_trading_service_tests
            COMMAND chronos_paper_trading_service_tests
        )

        set_tests_properties(chronos_paper_trading_service_tests
            PROPERTIES SKIP_RETURN_CODE 77
        )

        add_executable(chronos_paper_trading_http_api_tests
            tests/paper_trading_http_api_tests.cpp
        )

        target_compile_definitions(chronos_paper_trading_http_api_tests
            PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(chronos_paper_trading_http_api_tests
            PRIVATE
                chronos_paper_trading_http
                chronos_build_options
                httplib::httplib
                nlohmann_json::nlohmann_json
        )

        add_test(
            NAME chronos_paper_trading_http_api_tests
            COMMAND chronos_paper_trading_http_api_tests
        )

        set_tests_properties(chronos_paper_trading_http_api_tests
            PROPERTIES SKIP_RETURN_CODE 77
        )
    endif()

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

        add_executable(chronos_databento_secret_redaction_tests
            tests/databento_secret_redaction_tests.cpp
        )

        target_link_libraries(chronos_databento_secret_redaction_tests
            PRIVATE
                chronos_databento_adapter
                chronos_build_options
        )

        add_test(
            NAME chronos_databento_secret_redaction_tests
            COMMAND chronos_databento_secret_redaction_tests
        )
    endif()
endif()
