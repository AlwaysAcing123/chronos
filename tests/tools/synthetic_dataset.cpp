#include "chronos/canonical_json.hpp"
#include "chronos/datasets/dataset_writer.hpp"
#include "chronos/market_data_validation.hpp"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

// A test-only tool that writes a normalized dataset from a small CSV of
// synthetic quotes and statuses, so tests outside C++ (the Python research
// package) can build the whole pipeline from data that is neither licensed
// nor downloaded. Each line is one event, in ingest order:
//
//     quote,<instrument>,<receive_ns>,<bid>,<bid_size>,<ask>,<ask_size>
//     status,<instrument>,<receive_ns>,<open|halted|closed|pre_open|unknown>
//
// Prices and sizes are decimals; an empty bid or ask pair is a missing side.
// Every event happens when it is received. Blank lines and lines starting
// with '#' are ignored.
namespace {

namespace json = chronos::canonical_json;
namespace ds = chronos::datasets;

std::vector<std::string> fields_of(const std::string& line) {
    std::vector<std::string> fields;
    std::stringstream stream(line);
    std::string field;
    while (std::getline(stream, field, ',')) {
        fields.push_back(field);
    }
    if (!line.empty() && line.back() == ',') {
        fields.emplace_back();
    }
    return fields;
}

std::optional<chronos::BookLevel> side(const std::string& price, const std::string& size) {
    if (price.empty() && size.empty()) {
        return std::nullopt;
    }
    return chronos::BookLevel{.price = chronos::Price::parse(price), .quantity = chronos::Quantity::parse(size)};
}

chronos::TradingStatus status_of(const std::string& name) {
    using chronos::TradingStatus;
    for (const auto& [text, status] : std::vector<std::pair<std::string_view, TradingStatus>>{
             {"open", TradingStatus::Open},
             {"halted", TradingStatus::Halted},
             {"closed", TradingStatus::Closed},
             {"pre_open", TradingStatus::PreOpen},
             {"unknown", TradingStatus::Unknown},
         }) {
        if (name == text) {
            return status;
        }
    }
    throw std::invalid_argument("unknown status " + name);
}

chronos::MarketEvent event_of(const std::vector<std::string>& fields, std::uint64_t sequence, const std::string& stream) {
    if (fields.size() < 4) {
        throw std::invalid_argument("too few fields");
    }
    const std::int64_t at = std::stoll(fields[2]);
    const chronos::MarketEventHeader header{
        .instrument = chronos::InstrumentId(fields[1]),
        .stream = chronos::MarketDataStreamId(stream),
        .venue = std::nullopt,
        .event_timestamp = chronos::Timestamp::from_nanoseconds(at),
        .receive_timestamp = chronos::Timestamp::from_nanoseconds(at),
        .ingest_sequence = sequence,
        .venue_sequence = std::nullopt,
        .quality = chronos::MarketDataQuality{}.with(chronos::MarketDataQualityFlag::Replay),
    };
    if (fields[0] == "quote" && fields.size() == 7) {
        return chronos::TopOfBookEvent{.header = header, .bid = side(fields[3], fields[4]), .ask = side(fields[5], fields[6])};
    }
    if (fields[0] == "status" && fields.size() == 4) {
        return chronos::MarketStatusEvent{.header = header, .status = status_of(fields[3]), .reason = "synthetic"};
    }
    throw std::invalid_argument("expected a quote with 7 fields or a status with 4");
}

int run(const std::string& input, const std::string& catalog, const std::string& stream) {
    std::ifstream file(input);
    if (!file) {
        std::cerr << "cannot read " << input << '\n';
        return 1;
    }
    auto created = ds::NormalizedMarketDatasetWriter::create(catalog, chronos::MarketDataStreamId(stream));
    if (auto* error = std::get_if<ds::DatasetError>(&created)) {
        std::cerr << error->field << ": " << error->message << '\n';
        return 1;
    }
    auto& writer = std::get<std::unique_ptr<ds::NormalizedMarketDatasetWriter>>(created);
    std::string line;
    std::uint64_t sequence = 0;
    std::uint64_t number = 0;
    while (std::getline(file, line)) {
        ++number;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        try {
            auto validated = chronos::validate_market_event(event_of(fields_of(line), sequence, stream));
            if (const auto* rejected = std::get_if<chronos::MarketEventValidationFailure>(&validated)) {
                for (const auto& error : rejected->errors) {
                    std::cerr << "line " << number << ": " << error.field << ": " << error.message << '\n';
                }
                return 1;
            }
            if (auto error = writer->append(std::get<chronos::ValidatedMarketEvent>(validated))) {
                std::cerr << "line " << number << ": " << error->field << ": " << error->message << '\n';
                return 1;
            }
        } catch (const std::exception& error) {
            std::cerr << "line " << number << ": " << error.what() << '\n';
            return 1;
        }
        ++sequence;
    }
    auto finished = writer->finish(ds::DatasetSource{
        .provider = "synthetic",
        .provider_dataset = "SYNTHETIC",
        .provider_schema = "top-of-book",
        .description = "synthetic events for tests",
        .recording_sha256 = std::string(64, '0'),
        .coverage = std::nullopt,
        .normalizer = "chronos.test.synthetic_dataset",
        .normalizer_version = "1",
    });
    if (auto* error = std::get_if<ds::DatasetError>(&finished)) {
        std::cerr << error->field << ": " << error->message << '\n';
        return 1;
    }
    const auto& dataset = std::get<ds::PublishedDataset>(finished);
    std::cout << json::serialize(json::Value(json::Object{
        {"dataset_id", dataset.manifest.dataset_id},
        {"directory", dataset.directory.string()},
    })) << '\n';
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc != 4) {
        std::cerr << "Usage: chronos_synthetic_dataset <events.csv> <catalog> <stream-id>\n";
        return 2;
    }
    return run(argv[1], argv[2], argv[3]);
}
