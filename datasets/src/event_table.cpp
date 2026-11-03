#include "chronos/datasets/event_table.hpp"

#include "chronos/market_data_validation.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace chronos::datasets {
namespace {

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

std::optional<BookLevel> side(const std::string& price, const std::string& size) {
    if (price.empty() && size.empty()) {
        return std::nullopt;
    }
    return BookLevel{.price = Price::parse(price), .quantity = Quantity::parse(size)};
}

TradingStatus status_of(const std::string& name) {
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

MarketEvent event_of(const std::vector<std::string>& fields, std::uint64_t sequence, const MarketDataStreamId& stream) {
    if (fields.size() < 4) {
        throw std::invalid_argument("too few fields");
    }
    std::size_t parsed = 0;
    const std::int64_t at = std::stoll(fields[2], &parsed);
    if (parsed != fields[2].size()) {
        throw std::invalid_argument("the receive time is not a whole number of nanoseconds");
    }
    const MarketEventHeader header{
        .instrument = InstrumentId(fields[1]),
        .stream = stream,
        .venue = std::nullopt,
        .event_timestamp = Timestamp::from_nanoseconds(at),
        .receive_timestamp = Timestamp::from_nanoseconds(at),
        .ingest_sequence = sequence,
        .venue_sequence = std::nullopt,
        .quality = MarketDataQuality{}.with(MarketDataQualityFlag::Replay),
    };
    if (fields[0] == "quote" && fields.size() == 7) {
        return TopOfBookEvent{.header = header, .bid = side(fields[3], fields[4]), .ask = side(fields[5], fields[6])};
    }
    if (fields[0] == "status" && fields.size() == 4) {
        return MarketStatusEvent{.header = header, .status = status_of(fields[3]), .reason = "synthetic"};
    }
    throw std::invalid_argument("expected a quote with 7 fields or a status with 4");
}

DatasetError line_error(std::uint64_t number, std::string field, std::string message) {
    return dataset_error(DatasetErrorCode::InvalidValue, std::move(field), std::move(message), number);
}

}  // namespace

std::variant<PublishedDataset, DatasetError> write_event_table_dataset(
    std::istream& table,
    const std::filesystem::path& catalog,
    const MarketDataStreamId& stream,
    DatasetSource source
) {
    auto created = NormalizedMarketDatasetWriter::create(catalog, stream);
    if (auto* error = std::get_if<DatasetError>(&created)) {
        return std::move(*error);
    }
    auto& writer = std::get<std::unique_ptr<NormalizedMarketDatasetWriter>>(created);
    std::string line;
    std::uint64_t sequence = 0;
    std::uint64_t number = 0;
    while (std::getline(table, line)) {
        ++number;
        if (line.empty() || line.front() == '#') {
            continue;
        }
        MarketEventValidationResult validated = MarketEventValidationFailure{};
        try {
            validated = validate_market_event(event_of(fields_of(line), sequence, stream));
        } catch (const std::exception& error) {
            return line_error(number, "event_table", error.what());
        }
        if (const auto* rejected = std::get_if<MarketEventValidationFailure>(&validated)) {
            std::string message;
            for (const auto& error : rejected->errors) {
                message += (message.empty() ? "" : "; ") + error.field + ": " + error.message;
            }
            return line_error(number, "event_table", message);
        }
        if (auto error = writer->append(std::get<ValidatedMarketEvent>(validated))) {
            error->line = number;
            return std::move(*error);
        }
        ++sequence;
    }
    if (table.bad()) {
        return dataset_error(DatasetErrorCode::ReadFailed, "event_table", "the event table could not be read");
    }
    return writer->finish(std::move(source));
}

DatasetSource synthetic_scenario_source(std::string_view scenario, std::string_view version, std::string table_sha256) {
    return DatasetSource{
        .provider = std::string(kSyntheticProvider),
        .provider_dataset = std::string(scenario),
        .provider_schema = "event_table",
        .description = "invented events of scenario " + std::string(scenario) + " version " + std::string(version) +
            ", not market data",
        .recording_sha256 = std::move(table_sha256),
        .coverage = std::nullopt,
        .normalizer = std::string(kEventTableNormalizer),
        .normalizer_version = std::string(kEventTableNormalizerVersion),
    };
}

}  // namespace chronos::datasets
