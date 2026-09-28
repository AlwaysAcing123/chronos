#include "chronos/databento/equities_mini_mbp1_file_reader.hpp"
#include "chronos/market_replay.hpp"
#include "databento_cli_json.hpp"

#include <nlohmann/json.hpp>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace {

namespace adapter = chronos::databento_adapter;

struct Arguments {
    std::filesystem::path input;
    chronos::MarketDataStreamId stream;
};

struct EventCounts {
    std::uint64_t total{0};
    std::uint64_t trades{0};
    std::uint64_t top_of_book_updates{0};
    std::uint64_t market_status_updates{0};
    std::uint64_t instrument_definitions{0};
};

class UsageError final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::string usage() {
    return
        "Usage:\n"
        "  chronos_databento_replay --input <FILE.dbn.zst> "
        "--stream-id <ID>\n\n"
        "Replays a local EQUS.MINI MBP-1 recording without network access.\n";
}

std::string require_value(
    int argc,
    char* argv[],
    int& index,
    std::string_view option
) {
    if (index + 1 >= argc) {
        throw UsageError(std::string(option) + " requires a value");
    }
    ++index;
    return argv[index];
}

void assign_once(
    std::optional<std::string>& destination,
    std::string value,
    std::string_view option
) {
    if (destination) {
        throw UsageError(std::string(option) + " may be provided only once");
    }
    destination = std::move(value);
}

Arguments parse_arguments(int argc, char* argv[]) {
    std::optional<std::string> input;
    std::optional<std::string> stream_id;
    for (int index = 1; index < argc; ++index) {
        const std::string_view option = argv[index];
        if (option == "--input") {
            assign_once(
                input,
                require_value(argc, argv, index, option),
                option
            );
        } else if (option == "--stream-id") {
            assign_once(
                stream_id,
                require_value(argc, argv, index, option),
                option
            );
        } else if (option == "--help" || option == "-h") {
            throw UsageError("");
        } else {
            throw UsageError("unknown option: " + std::string(option));
        }
    }
    if (!input || !stream_id) {
        throw UsageError("--input and --stream-id are required");
    }
    return Arguments{
        .input = std::move(*input),
        .stream = chronos::MarketDataStreamId(std::move(*stream_id)),
    };
}

nlohmann::json replay_error_json(const chronos::MarketReplayError& error) {
    nlohmann::json document = {
        {"code", "market_replay_failed"},
        {"field", error.field},
        {"message", error.message},
    };
    if (error.stream_error) {
        document["stream_error"] = {
            {"field", error.stream_error->field},
            {"message", error.stream_error->message},
        };
    }
    if (error.clock_error) {
        document["clock_error"] = {
            {"message", error.clock_error->message},
            {"requested_time_ns", std::to_string(
                error.clock_error->requested_time.nanoseconds()
            )},
        };
        if (error.clock_error->current_time) {
            document["clock_error"]["current_time_ns"] = std::to_string(
                error.clock_error->current_time->nanoseconds()
            );
        }
    }
    return document;
}

nlohmann::json event_counts_json(const EventCounts& counts) {
    return {
        {"instrument_definitions", counts.instrument_definitions},
        {"market_status_updates", counts.market_status_updates},
        {"top_of_book_updates", counts.top_of_book_updates},
        {"total", counts.total},
        {"trades", counts.trades},
    };
}

bool increment(std::uint64_t& value) {
    if (value == std::numeric_limits<std::uint64_t>::max()) {
        return false;
    }
    ++value;
    return true;
}

bool count_event(
    const chronos::ValidatedMarketEvent& event,
    EventCounts& counts
) {
    if (!increment(counts.total)) {
        return false;
    }
    return std::visit(
        [&counts](const auto& concrete_event) {
            using Event = std::decay_t<decltype(concrete_event)>;
            if constexpr (std::is_same_v<Event, chronos::TradeEvent>) {
                return increment(counts.trades);
            } else if constexpr (
                std::is_same_v<Event, chronos::TopOfBookEvent>
            ) {
                return increment(counts.top_of_book_updates);
            } else if constexpr (
                std::is_same_v<Event, chronos::MarketStatusEvent>
            ) {
                return increment(counts.market_status_updates);
            } else {
                static_assert(std::is_same_v<
                    Event,
                    chronos::InstrumentDefinitionEvent
                >);
                return increment(counts.instrument_definitions);
            }
        },
        event.value()
    );
}

nlohmann::json quality_json(const chronos::MarketDataQuality& quality) {
    return {
        {"potentially_invalid_book", quality.contains(
            chronos::MarketDataQualityFlag::PotentiallyInvalidBook
        )},
        {"questionable_receive_timestamp", quality.contains(
            chronos::MarketDataQualityFlag::QuestionableReceiveTimestamp
        )},
        {"raw", quality.raw()},
        {"replay", quality.contains(chronos::MarketDataQualityFlag::Replay)},
        {"snapshot", quality.contains(
            chronos::MarketDataQualityFlag::Snapshot
        )},
        {"source_specific", quality.contains(
            chronos::MarketDataQualityFlag::SourceSpecific
        )},
    };
}

nlohmann::json header_json(const chronos::MarketEventHeader& header) {
    return {
        {"event_timestamp_ns", std::to_string(
            header.event_timestamp.nanoseconds()
        )},
        {"ingest_sequence", header.ingest_sequence},
        {"quality", quality_json(header.quality)},
        {"receive_timestamp_ns", std::to_string(
            header.receive_timestamp.nanoseconds()
        )},
    };
}

nlohmann::json book_level_json(
    const std::optional<chronos::BookLevel>& level
) {
    if (!level) {
        return nullptr;
    }
    return {
        {"price", level->price.to_string()},
        {"quantity", level->quantity.to_string()},
    };
}

std::string_view aggressor_side_text(chronos::AggressorSide side) {
    switch (side) {
        case chronos::AggressorSide::Buy:
            return "buy";
        case chronos::AggressorSide::Sell:
            return "sell";
        case chronos::AggressorSide::Unknown:
            return "unknown";
    }
    return "unknown";
}

nlohmann::json trade_json(const std::optional<chronos::TradeEvent>& trade) {
    if (!trade) {
        return nullptr;
    }
    return {
        {"aggressor_side", aggressor_side_text(trade->aggressor_side)},
        {"header", header_json(trade->header)},
        {"price", trade->price.to_string()},
        {"quantity", trade->quantity.to_string()},
    };
}

nlohmann::json top_of_book_json(
    const std::optional<chronos::TopOfBookEvent>& top_of_book
) {
    if (!top_of_book) {
        return nullptr;
    }
    return {
        {"ask", book_level_json(top_of_book->ask)},
        {"bid", book_level_json(top_of_book->bid)},
        {"header", header_json(top_of_book->header)},
    };
}

nlohmann::json instrument_views_json(
    const chronos::CurrentMarketView& view,
    const std::map<std::string, EventCounts>& counts_by_instrument
) {
    nlohmann::json document = nlohmann::json::object();
    for (const auto& [symbol, market] : view.instruments()) {
        const auto counts = counts_by_instrument.find(symbol);
        document[symbol] = {
            {"counts", counts == counts_by_instrument.end()
                ? event_counts_json(EventCounts{})
                : event_counts_json(counts->second)},
            {"last_trade", trade_json(market.last_trade)},
            {"top_of_book", top_of_book_json(market.top_of_book)},
        };
    }
    return document;
}

int report_error(nlohmann::json error) {
    std::cerr << nlohmann::json{{"error", std::move(error)}}.dump(2) << '\n';
    return 1;
}

nlohmann::json recording_read_error_json(
    const adapter::EquitiesMiniMbp1FileError& error
) {
    auto document = chronos::databento_cli::recording_error_json(error);
    document["code"] = "recording_read_failed";
    return document;
}

int run(const Arguments& arguments) {
    auto open_result = adapter::EquitiesMiniMbp1FileReader::open(
        arguments.input,
        arguments.stream
    );
    if (std::holds_alternative<adapter::EquitiesMiniMbp1FileError>(
            open_result
        )) {
        return report_error(recording_read_error_json(
            std::get<adapter::EquitiesMiniMbp1FileError>(
                std::move(open_result)
            )
        ));
    }

    auto reader = std::get<std::unique_ptr<
        adapter::EquitiesMiniMbp1FileReader
    >>(std::move(open_result));
    chronos::MarketReplay replay(arguments.stream);
    EventCounts total_counts;
    std::map<std::string, EventCounts> counts_by_instrument;
    std::optional<chronos::Timestamp> first_replay_time;

    while (true) {
        auto read_result = reader->read_next();
        if (std::holds_alternative<adapter::EndOfRecording>(read_result)) {
            break;
        }
        if (std::holds_alternative<adapter::EquitiesMiniMbp1FileError>(
                read_result
            )) {
            return report_error(recording_read_error_json(
                std::get<adapter::EquitiesMiniMbp1FileError>(
                    std::move(read_result)
                )
            ));
        }

        const auto& event = std::get<chronos::ValidatedMarketEvent>(
            read_result
        );
        auto replay_result = replay.accept(event);
        if (std::holds_alternative<chronos::MarketReplayError>(replay_result)) {
            return report_error(replay_error_json(
                std::get<chronos::MarketReplayError>(
                    std::move(replay_result)
                )
            ));
        }
        const auto& step = std::get<chronos::MarketReplayStep>(replay_result);
        if (!first_replay_time) {
            first_replay_time = step.replay_time;
        }
        auto& instrument_counts = counts_by_instrument[
            step.instrument.value()
        ];
        EventCounts next_total_counts = total_counts;
        EventCounts next_instrument_counts = instrument_counts;
        if (!count_event(event, next_total_counts) ||
            !count_event(event, next_instrument_counts)) {
            return report_error({
                {"code", "counter_overflow"},
                {"field", "event_counts"},
                {"message", "replay event count cannot be represented"},
            });
        }
        total_counts = next_total_counts;
        instrument_counts = next_instrument_counts;
    }

    if (!reader->manifest()) {
        return report_error({
            {"code", "missing_manifest"},
            {"field", "manifest"},
            {"message", "completed recording did not produce a manifest"},
        });
    }
    if (reader->manifest()->normalized_event_count() != total_counts.total) {
        return report_error({
            {"code", "event_count_mismatch"},
            {"field", "counts.total"},
            {"message", "replay count does not match the recording manifest"},
        });
    }

    const auto last_replay_time = replay.clock().current_time();
    const nlohmann::json replay_document = {
        {"counts", event_counts_json(total_counts)},
        {"first_receive_timestamp_ns", first_replay_time
            ? nlohmann::json(std::to_string(
                first_replay_time->nanoseconds()
            ))
            : nlohmann::json(nullptr)},
        {"instruments", instrument_views_json(
            replay.market_view(),
            counts_by_instrument
        )},
        {"last_ingest_sequence", replay.last_accepted_ingest_sequence()
            ? nlohmann::json(*replay.last_accepted_ingest_sequence())
            : nlohmann::json(nullptr)},
        {"last_receive_timestamp_ns", last_replay_time
            ? nlohmann::json(std::to_string(
                last_replay_time->nanoseconds()
            ))
            : nlohmann::json(nullptr)},
    };
    std::cout << nlohmann::json{
        {"manifest", nlohmann::json::parse(reader->manifest()->to_json())},
        {"recording_path", arguments.input.string()},
        {"replay", replay_document},
    }.dump(2) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        return run(parse_arguments(argc, argv));
    } catch (const UsageError& error) {
        if (*error.what() != '\0') {
            std::cerr << "Error: " << error.what() << "\n\n";
        }
        std::cerr << usage();
        return 2;
    } catch (const std::exception& exception) {
        std::cerr << "Unexpected error: " << exception.what() << '\n';
        return 1;
    }
}
