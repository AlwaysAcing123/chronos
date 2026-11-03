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

option(
    CHRONOS_ENABLE_ALPACA_ADAPTER
    "Build the optional Alpaca IEX live market-data adapter"
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

# Content hashing and the strict JSON used by persisted artifacts. No
# third-party dependency, so the default build keeps none.
add_library(chronos_canonical_data
    core/src/canonical_json.cpp
    core/src/sha256.cpp
)

target_include_directories(chronos_canonical_data
    PUBLIC core/include
)

target_link_libraries(chronos_canonical_data
    PRIVATE chronos_build_options
)

# Provider-independent normalized market datasets. See ADR 0012.
add_library(chronos_datasets
    datasets/src/artifact_directory.cpp
    datasets/src/dataset_catalog.cpp
    datasets/src/dataset_error.cpp
    datasets/src/dataset_manifest.cpp
    datasets/src/dataset_quality.cpp
    datasets/src/dataset_reader.cpp
    datasets/src/dataset_writer.cpp
    datasets/src/derived_artifact.cpp
    datasets/src/event_table.cpp
    datasets/src/json_fields.cpp
    datasets/src/market_event_codec.cpp
    datasets/src/ndjson_file.cpp
)

target_include_directories(chronos_datasets
    PUBLIC datasets/include
)

target_link_libraries(chronos_datasets
    PUBLIC
        chronos_canonical_data
        chronos_market_data
    PRIVATE chronos_build_options
)

# Point-in-time features computed from validated market events.
add_library(chronos_features
    features/src/feature_artifact.cpp
    features/src/feature_row_codec.cpp
    features/src/top_of_book_features.cpp
)

target_include_directories(chronos_features
    PUBLIC features/include
)

target_link_libraries(chronos_features
    PUBLIC
        chronos_datasets
        chronos_market_data
    PRIVATE chronos_build_options
)

# Future-return labels computed from feature artifacts. See ADR 0013.
add_library(chronos_labels
    labels/src/future_midpoint_return.cpp
    labels/src/label_artifact.cpp
    labels/src/label_row_codec.cpp
)

target_include_directories(chronos_labels
    PUBLIC labels/include
)

target_link_libraries(chronos_labels
    PUBLIC chronos_features
    PRIVATE chronos_build_options
)

# Chronological, purged splits of label artifacts. See ADR 0014.
add_library(chronos_splits
    splits/src/chronological_split.cpp
    splits/src/split_artifact.cpp
    splits/src/split_row_codec.cpp
)

target_include_directories(chronos_splits
    PUBLIC splits/include
)

target_link_libraries(chronos_splits
    PUBLIC chronos_labels
    PRIVATE chronos_build_options
)

add_executable(chronos_dataset
    tools/dataset_cli.cpp
)

target_link_libraries(chronos_dataset
    PRIVATE
        chronos_splits
        chronos_build_options
)

add_library(chronos_market_replay
    core/src/current_market_view.cpp
    core/src/market_replay.cpp
    core/src/replay_clock.cpp
    core/src/replay_schedule.cpp
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

# Provider-independent market-data sources: the paced replay runner, the
# source lifecycle, and browser change notifications. Provider adapters plug
# in through core interfaces, so this library needs none of them.
add_library(chronos_market_data_sources
    services/market_data/src/dataset_replay.cpp
    services/market_data/src/leased_ingest_sequences.cpp
    services/market_data/src/market_data_notifier.cpp
    services/market_data/src/market_data_source.cpp
    services/market_data/src/market_state_feed.cpp
    services/market_data/src/monotonic_clock.cpp
    services/market_data/src/monotonic_utc_clock.cpp
    services/market_data/src/paced_replay_source.cpp
    services/market_data/src/reconnect_backoff.cpp
)

target_include_directories(chronos_market_data_sources
    PUBLIC services/market_data/include
)

# A normalized dataset replays through the same paced source (ADR 0025);
# the dataset library depends on nothing here, so there is no cycle.
target_link_libraries(chronos_market_data_sources
    PUBLIC
        chronos_datasets
        chronos_market_state
    PRIVATE chronos_build_options
)

add_library(chronos_paper_execution
    core/src/paper_order_codes.cpp
    core/src/paper_order_processor.cpp
    core/src/portfolio_valuation.cpp
    core/src/pre_trade_risk.cpp
    core/src/top_of_book_execution.cpp
    core/src/working_order.cpp
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

# Deterministic historical strategy evaluation of inference schedules
# through the paper-execution engine. See ADR 0016.
add_library(chronos_strategy
    strategy/src/inference_schedule.cpp
    strategy/src/long_flat_policy.cpp
    strategy/src/strategy_run_artifact.cpp
    strategy/src/strategy_simulation.cpp
)

target_include_directories(chronos_strategy
    PUBLIC strategy/include
)

target_link_libraries(chronos_strategy
    PUBLIC
        chronos_features
        chronos_paper_execution
    PRIVATE chronos_build_options
)

add_executable(chronos_strategy_cli
    tools/strategy_cli.cpp
)

# The executable is named chronos_strategy; the library owns that target name.
set_target_properties(chronos_strategy_cli
    PROPERTIES OUTPUT_NAME chronos_strategy
)

target_include_directories(chronos_strategy_cli
    PRIVATE tools
)

target_link_libraries(chronos_strategy_cli
    PRIVATE
        chronos_strategy
        chronos_build_options
)

if(CHRONOS_ENABLE_POSTGRESQL_ADAPTER)
    find_package(PostgreSQL REQUIRED)

    add_library(chronos_postgresql_persistence
        adapters/postgresql/src/postgresql_live_stream_sequences.cpp
        adapters/postgresql/src/postgresql_paper_order_repository.cpp
        adapters/postgresql/src/postgresql_model_deployments.cpp
    )

    target_include_directories(chronos_postgresql_persistence
        PUBLIC adapters/postgresql/include
    )

    target_link_libraries(chronos_postgresql_persistence
        PUBLIC
            chronos_paper_execution
            chronos_market_data_sources
            PostgreSQL::PostgreSQL
        PRIVATE chronos_build_options
    )
endif()

if(CHRONOS_ENABLE_PAPER_TRADING_API OR CHRONOS_ENABLE_ALPACA_ADAPTER)
    include(FetchContent)

    # These dependency names match the Databento SDK's declarations. The first
    # declaration wins, so a build that also enables the Databento adapter
    # uses these hash-verified copies. See ADR 0004 and ADR 0009.
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
    # and the Alpaca WebSocket stream require OpenSSL; the API server needs
    # neither TLS nor compression.
    if(CHRONOS_ENABLE_DATABENTO_ADAPTER OR CHRONOS_ENABLE_ALPACA_ADAPTER)
        set(HTTPLIB_REQUIRE_OPENSSL ON CACHE BOOL "" FORCE)
    else()
        set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
    endif()
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
endif()

if(CHRONOS_ENABLE_ALPACA_ADAPTER)
    add_library(chronos_alpaca_adapter
        adapters/alpaca/src/alpaca_credentials.cpp
        adapters/alpaca/src/alpaca_live_source.cpp
        adapters/alpaca/src/alpaca_normalizer.cpp
        adapters/alpaca/src/alpaca_protocol.cpp
        adapters/alpaca/src/alpaca_subscription.cpp
        adapters/alpaca/src/alpaca_timestamp.cpp
        adapters/alpaca/src/alpaca_websocket_transport.cpp
    )

    target_include_directories(chronos_alpaca_adapter
        PUBLIC adapters/alpaca/include
    )

    target_link_libraries(chronos_alpaca_adapter
        PUBLIC chronos_market_data_sources
        PRIVATE
            chronos_build_options
            httplib::httplib
            nlohmann_json::nlohmann_json
    )
endif()

if(CHRONOS_ENABLE_PAPER_TRADING_API)
    add_library(chronos_paper_trading_service
        services/paper_trading/src/history_cursor.cpp
        services/paper_trading/src/paper_trading_service.cpp
        services/paper_trading/src/working_order_matcher.cpp
    )

    # Model deployments (ADR 0018): the inference worker client and the
    # runtime that turns market events into decisions and orders.
    add_library(chronos_paper_trading_deployments
        services/paper_trading/src/deployment_inference.cpp
        services/paper_trading/src/deployment_runtime.cpp
    )

    target_link_libraries(chronos_paper_trading_deployments
        PUBLIC
            chronos_paper_trading_service
            chronos_features
            chronos_strategy
        PRIVATE
            chronos_build_options
            nlohmann_json::nlohmann_json
            Threads::Threads
    )

    target_include_directories(chronos_paper_trading_service
        PUBLIC services/paper_trading/include
    )

    target_link_libraries(chronos_paper_trading_service
        PUBLIC
            chronos_market_data_sources
            chronos_market_state
            chronos_postgresql_persistence
        PRIVATE chronos_build_options
    )

    add_library(chronos_paper_trading_http
        services/paper_trading/src/deployment_http.cpp
        services/paper_trading/src/paper_trading_http_api.cpp
        services/paper_trading/src/paper_trading_http_contract.cpp
        services/paper_trading/src/paper_trading_http_server.cpp
        services/paper_trading/src/server_arguments.cpp
    )

    target_link_libraries(chronos_paper_trading_http
        PUBLIC
            chronos_market_data_sources
            chronos_paper_trading_service
            chronos_paper_trading_deployments
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

    if(CHRONOS_ENABLE_ALPACA_ADAPTER)
        target_compile_definitions(chronos_paper_trading_server
            PRIVATE CHRONOS_PAPER_TRADING_SERVER_WITH_ALPACA_LIVE
        )
        target_link_libraries(chronos_paper_trading_server
            PRIVATE chronos_alpaca_adapter
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
        adapters/databento/src/dataset_conversion.cpp
        adapters/databento/src/equities_mini_mbp1_event_reader.cpp
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
            chronos_datasets
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

    add_executable(chronos_databento_normalize
        tools/databento_normalize.cpp
    )

    target_link_libraries(chronos_databento_normalize
        PRIVATE
            chronos_databento_adapter
            chronos_build_options
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

    add_executable(chronos_canonical_data_tests
        tests/canonical_data_tests.cpp
    )

    target_link_libraries(chronos_canonical_data_tests
        PRIVATE
            chronos_canonical_data
            chronos_build_options
    )

    add_test(
        NAME chronos_canonical_data_tests
        COMMAND chronos_canonical_data_tests
    )

    add_executable(chronos_normalized_dataset_tests
        tests/normalized_dataset_tests.cpp
    )

    # The market-data sources bring the dataset library with them.
    target_link_libraries(chronos_normalized_dataset_tests
        PRIVATE
            chronos_market_data_sources
            chronos_build_options
    )

    add_test(
        NAME chronos_normalized_dataset_tests
        COMMAND chronos_normalized_dataset_tests
    )

    # The server's normalized-dataset replay (ADR 0025).
    add_executable(chronos_dataset_replay_tests
        tests/dataset_replay_tests.cpp
    )

    target_link_libraries(chronos_dataset_replay_tests
        PRIVATE
            chronos_market_data_sources
            chronos_build_options
    )

    add_test(
        NAME chronos_dataset_replay_tests
        COMMAND chronos_dataset_replay_tests
    )

    add_executable(chronos_dataset_cli_tests
        tests/dataset_cli_tests.cpp
    )

    target_include_directories(chronos_dataset_cli_tests
        PRIVATE tools
    )

    target_link_libraries(chronos_dataset_cli_tests
        PRIVATE
            chronos_datasets
            chronos_build_options
    )

    add_test(
        NAME chronos_dataset_cli_tests
        COMMAND chronos_dataset_cli_tests
    )

    add_executable(chronos_feature_engine_tests
        tests/feature_engine_tests.cpp
    )

    target_link_libraries(chronos_feature_engine_tests
        PRIVATE
            chronos_features
            chronos_build_options
    )

    add_test(
        NAME chronos_feature_engine_tests
        COMMAND chronos_feature_engine_tests
    )

    add_executable(chronos_label_artifact_tests
        tests/label_artifact_tests.cpp
    )

    target_link_libraries(chronos_label_artifact_tests
        PRIVATE
            chronos_labels
            chronos_build_options
    )

    add_test(
        NAME chronos_label_artifact_tests
        COMMAND chronos_label_artifact_tests
    )

    add_executable(chronos_split_artifact_tests
        tests/split_artifact_tests.cpp
    )

    target_link_libraries(chronos_split_artifact_tests
        PRIVATE
            chronos_splits
            chronos_build_options
    )

    add_test(
        NAME chronos_split_artifact_tests
        COMMAND chronos_split_artifact_tests
    )

    add_executable(chronos_strategy_tests
        tests/strategy_tests.cpp
    )

    target_link_libraries(chronos_strategy_tests
        PRIVATE
            chronos_strategy
            chronos_build_options
    )

    add_test(
        NAME chronos_strategy_tests
        COMMAND chronos_strategy_tests
    )

    # Writes synthetic datasets for tests outside C++, such as the Python
    # research package's.
    add_executable(chronos_synthetic_dataset
        tests/tools/synthetic_dataset.cpp
    )

    target_link_libraries(chronos_synthetic_dataset
        PRIVATE
            chronos_datasets
            chronos_build_options
    )

    add_executable(chronos_dataset_command_tests
        tests/dataset_command_tests.cpp
    )

    target_link_libraries(chronos_dataset_command_tests
        PRIVATE
            chronos_splits
            chronos_build_options
    )

    add_test(
        NAME chronos_dataset_command_tests
        COMMAND chronos_dataset_command_tests $<TARGET_FILE:chronos_dataset>
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

    add_executable(chronos_replay_schedule_tests
        tests/replay_schedule_tests.cpp
    )

    target_link_libraries(chronos_replay_schedule_tests
        PRIVATE
            chronos_market_replay
            chronos_build_options
    )

    add_test(
        NAME chronos_replay_schedule_tests
        COMMAND chronos_replay_schedule_tests
    )

    add_executable(chronos_market_data_source_tests
        tests/market_data_source_tests.cpp
    )

    target_link_libraries(chronos_market_data_source_tests
        PRIVATE
            chronos_market_data_sources
            chronos_build_options
    )

    add_test(
        NAME chronos_market_data_source_tests
        COMMAND chronos_market_data_source_tests
    )

    # A live stream's sequences under a writer lease (ADR 0020), with an
    # in-memory store: no PostgreSQL.
    add_executable(chronos_leased_ingest_sequences_tests
        tests/leased_ingest_sequences_tests.cpp
    )

    target_link_libraries(chronos_leased_ingest_sequences_tests
        PRIVATE
            chronos_market_data_sources
            chronos_build_options
            Threads::Threads
    )

    add_test(
        NAME chronos_leased_ingest_sequences_tests
        COMMAND chronos_leased_ingest_sequences_tests
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

    add_executable(chronos_portfolio_valuation_tests
        tests/portfolio_valuation_tests.cpp
    )

    target_link_libraries(chronos_portfolio_valuation_tests
        PRIVATE
            chronos_paper_execution
            chronos_build_options
    )

    add_test(
        NAME chronos_portfolio_valuation_tests
        COMMAND chronos_portfolio_valuation_tests
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

    add_executable(chronos_working_order_tests
        tests/working_order_tests.cpp
    )

    target_link_libraries(chronos_working_order_tests
        PRIVATE
            chronos_paper_execution
            chronos_build_options
    )

    add_test(
        NAME chronos_working_order_tests
        COMMAND chronos_working_order_tests
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

        add_executable(chronos_postgresql_model_deployment_tests
            tests/postgresql_model_deployment_tests.cpp
        )

        target_compile_definitions(chronos_postgresql_model_deployment_tests
            PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(chronos_postgresql_model_deployment_tests
            PRIVATE
                chronos_postgresql_persistence
                chronos_build_options
                Threads::Threads
        )

        add_test(
            NAME chronos_postgresql_model_deployment_tests
            COMMAND chronos_postgresql_model_deployment_tests
        )

        set_tests_properties(chronos_postgresql_model_deployment_tests
            PROPERTIES SKIP_RETURN_CODE 77
        )

        add_executable(chronos_postgresql_working_order_tests
            tests/postgresql_working_order_tests.cpp
        )

        target_compile_definitions(chronos_postgresql_working_order_tests
            PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(chronos_postgresql_working_order_tests
            PRIVATE
                chronos_postgresql_persistence
                chronos_build_options
                Threads::Threads
        )

        add_test(
            NAME chronos_postgresql_working_order_tests
            COMMAND chronos_postgresql_working_order_tests
        )

        set_tests_properties(chronos_postgresql_working_order_tests
            PROPERTIES SKIP_RETURN_CODE 77
        )

        add_executable(chronos_postgresql_migration_tests
            tests/postgresql_migration_tests.cpp
        )

        target_compile_definitions(chronos_postgresql_migration_tests
            PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(chronos_postgresql_migration_tests
            PRIVATE
                chronos_postgresql_persistence
                chronos_build_options
                Threads::Threads
        )

        add_test(
            NAME chronos_postgresql_migration_tests
            COMMAND chronos_postgresql_migration_tests
        )

        set_tests_properties(chronos_postgresql_migration_tests
            PROPERTIES SKIP_RETURN_CODE 77
        )
    endif()

    if(CHRONOS_ENABLE_PAPER_TRADING_API)
        # Durable live-stream sequences (ADR 0020) against real PostgreSQL,
        # with history made by the real service and deployment repository.
        add_executable(chronos_postgresql_live_stream_sequences_tests
            tests/postgresql_live_stream_sequences_tests.cpp
        )

        target_compile_definitions(chronos_postgresql_live_stream_sequences_tests
            PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(chronos_postgresql_live_stream_sequences_tests
            PRIVATE
                chronos_paper_trading_service
                chronos_build_options
                Threads::Threads
        )

        add_test(
            NAME chronos_postgresql_live_stream_sequences_tests
            COMMAND chronos_postgresql_live_stream_sequences_tests
        )

        set_tests_properties(chronos_postgresql_live_stream_sequences_tests
            PROPERTIES SKIP_RETURN_CODE 77
        )

        add_executable(chronos_paper_trading_server_arguments_tests
            tests/paper_trading_server_arguments_tests.cpp
        )

        target_link_libraries(chronos_paper_trading_server_arguments_tests
            PRIVATE
                chronos_paper_trading_http
                chronos_build_options
        )

        add_test(
            NAME chronos_paper_trading_server_arguments_tests
            COMMAND chronos_paper_trading_server_arguments_tests
        )

        add_executable(chronos_paper_trading_history_cursor_tests
            tests/paper_trading_history_cursor_tests.cpp
        )

        target_link_libraries(chronos_paper_trading_history_cursor_tests
            PRIVATE
                chronos_paper_trading_service
                chronos_build_options
        )

        add_test(
            NAME chronos_paper_trading_history_cursor_tests
            COMMAND chronos_paper_trading_history_cursor_tests
        )

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

        add_executable(chronos_paper_trading_working_order_tests
            tests/paper_trading_working_order_tests.cpp
        )

        target_compile_definitions(chronos_paper_trading_working_order_tests
            PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(chronos_paper_trading_working_order_tests
            PRIVATE
                chronos_paper_trading_service
                chronos_build_options
                Threads::Threads
        )

        add_test(
            NAME chronos_paper_trading_working_order_tests
            COMMAND chronos_paper_trading_working_order_tests
        )

        set_tests_properties(chronos_paper_trading_working_order_tests
            PROPERTIES SKIP_RETURN_CODE 77
        )

        add_executable(chronos_deployment_condition_tests
            tests/deployment_condition_tests.cpp
        )

        target_link_libraries(chronos_deployment_condition_tests
            PRIVATE
                chronos_paper_trading_deployments
                chronos_build_options
        )

        add_test(
            NAME chronos_deployment_condition_tests
            COMMAND chronos_deployment_condition_tests
        )

        add_executable(chronos_paper_trading_inference_client_tests
            tests/paper_trading_inference_client_tests.cpp
        )

        target_link_libraries(chronos_paper_trading_inference_client_tests
            PRIVATE
                chronos_paper_trading_deployments
                chronos_build_options
                Threads::Threads
        )

        add_test(
            NAME chronos_paper_trading_inference_client_tests
            COMMAND chronos_paper_trading_inference_client_tests
        )

        add_executable(chronos_paper_trading_deployment_tests
            tests/paper_trading_deployment_tests.cpp
        )

        target_compile_definitions(chronos_paper_trading_deployment_tests
            PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(chronos_paper_trading_deployment_tests
            PRIVATE
                chronos_paper_trading_http
                chronos_build_options
                nlohmann_json::nlohmann_json
                Threads::Threads
        )

        add_test(
            NAME chronos_paper_trading_deployment_tests
            COMMAND chronos_paper_trading_deployment_tests
        )

        set_tests_properties(chronos_paper_trading_deployment_tests
            PROPERTIES SKIP_RETURN_CODE 77
        )

        add_executable(chronos_paper_trading_gtc_http_api_tests
            tests/paper_trading_gtc_http_api_tests.cpp
        )

        target_compile_definitions(chronos_paper_trading_gtc_http_api_tests
            PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        )

        target_link_libraries(chronos_paper_trading_gtc_http_api_tests
            PRIVATE
                chronos_paper_trading_http
                chronos_build_options
                nlohmann_json::nlohmann_json
                Threads::Threads
        )

        add_test(
            NAME chronos_paper_trading_gtc_http_api_tests
            COMMAND chronos_paper_trading_gtc_http_api_tests
        )

        set_tests_properties(chronos_paper_trading_gtc_http_api_tests
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

        add_executable(chronos_databento_dataset_conversion_tests
            tests/databento_dataset_conversion_tests.cpp
        )

        target_link_libraries(chronos_databento_dataset_conversion_tests
            PRIVATE
                chronos_databento_adapter
                chronos_market_replay
                chronos_build_options
        )

        add_test(
            NAME chronos_databento_dataset_conversion_tests
            COMMAND chronos_databento_dataset_conversion_tests
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

    if(CHRONOS_ENABLE_ALPACA_ADAPTER)
        add_executable(chronos_alpaca_adapter_tests
            tests/alpaca_adapter_tests.cpp
        )

        target_link_libraries(chronos_alpaca_adapter_tests
            PRIVATE
                chronos_alpaca_adapter
                chronos_build_options
        )

        add_test(
            NAME chronos_alpaca_adapter_tests
            COMMAND chronos_alpaca_adapter_tests
        )

        add_executable(chronos_alpaca_live_source_tests
            tests/alpaca_live_source_tests.cpp
        )

        target_link_libraries(chronos_alpaca_live_source_tests
            PRIVATE
                chronos_alpaca_adapter
                chronos_build_options
        )

        add_test(
            NAME chronos_alpaca_live_source_tests
            COMMAND chronos_alpaca_live_source_tests
        )

        # Deployments on the live source, through its fake transport.
        if(CHRONOS_ENABLE_PAPER_TRADING_API AND CHRONOS_ENABLE_POSTGRESQL_ADAPTER)
            add_executable(chronos_alpaca_deployment_tests
                tests/alpaca_deployment_tests.cpp
            )

            target_compile_definitions(chronos_alpaca_deployment_tests
                PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            )

            target_link_libraries(chronos_alpaca_deployment_tests
                PRIVATE
                    chronos_alpaca_adapter
                    chronos_paper_trading_deployments
                    chronos_build_options
                    Threads::Threads
            )

            add_test(
                NAME chronos_alpaca_deployment_tests
                COMMAND chronos_alpaca_deployment_tests
            )

            set_tests_properties(chronos_alpaca_deployment_tests
                PROPERTIES SKIP_RETURN_CODE 77
            )

            # A live stream continued across a server restart (ADR 0020).
            add_executable(chronos_live_stream_restart_tests
                tests/live_stream_restart_tests.cpp
            )

            target_compile_definitions(chronos_live_stream_restart_tests
                PRIVATE CHRONOS_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
            )

            target_link_libraries(chronos_live_stream_restart_tests
                PRIVATE
                    chronos_alpaca_adapter
                    chronos_paper_trading_deployments
                    chronos_build_options
                    Threads::Threads
            )

            add_test(
                NAME chronos_live_stream_restart_tests
                COMMAND chronos_live_stream_restart_tests
            )

            set_tests_properties(chronos_live_stream_restart_tests
                PROPERTIES SKIP_RETURN_CODE 77
            )
        endif()

        # A local WebSocket server exercises the real transport; nothing
        # leaves the loopback interface.
        add_executable(chronos_alpaca_websocket_transport_tests
            tests/alpaca_websocket_transport_tests.cpp
        )

        target_link_libraries(chronos_alpaca_websocket_transport_tests
            PRIVATE
                chronos_alpaca_adapter
                chronos_build_options
                httplib::httplib
        )

        add_test(
            NAME chronos_alpaca_websocket_transport_tests
            COMMAND chronos_alpaca_websocket_transport_tests
        )
    endif()
endif()
