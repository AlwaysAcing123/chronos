#include "chronos/databento/equities_mini_mbp1_event_reader.hpp"
#include "chronos/databento/equities_mini_mbp1_file_reader.hpp"
#include "chronos/databento/historical_recording_acquisition.hpp"
#include "chronos/market_replay.hpp"
#include "databento_test_support.hpp"
#include "test_support.hpp"

#include <databento/constants.hpp>
#include <databento/datetime.hpp>
#include <databento/dbn.hpp>
#include <databento/dbn_encoder.hpp>
#include <databento/detail/zstd_stream.hpp>
#include <databento/enums.hpp>
#include <databento/file_stream.hpp>
#include <databento/flag_set.hpp>
#include <databento/publishers.hpp>
#include <databento/record.hpp>
#include <databento/with_ts_out.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace adapter = chronos::databento_adapter;
using namespace chronos::test::databento_fixtures;
namespace db = ::databento;

using adapter::EndOfRecording;
using adapter::EquitiesMiniMbp1EventReader;
using adapter::EquitiesMiniMbp1FileError;
using adapter::EquitiesMiniMbp1FileErrorCode;
using adapter::EquitiesMiniMbp1FileOpenResult;
using adapter::EquitiesMiniMbp1FileReadResult;
using adapter::EquitiesMiniMbp1FileReader;
using adapter::HistoricalRecordingAcquisitionRequest;
using adapter::HistoricalRecordingAcquisitionResult;
using adapter::HistoricalRecordingAcquisitionSuccess;
using adapter::HistoricalRecordingDownloadResult;
using adapter::HistoricalRecordingError;
using adapter::HistoricalRecordingErrorCode;
using adapter::HistoricalRecordingProvider;
using adapter::HistoricalRecordingQuery;
using adapter::HistoricalRecordingQuote;
using adapter::HistoricalRecordingQuoteResult;
using adapter::RecordingManifestWriteError;
using adapter::RecordingManifestWriteErrorCode;
using adapter::RecordingManifestWriteSuccess;
using chronos::MarketDataQualityFlag;
using chronos::MarketDataStreamId;
using chronos::MarketReplay;
using chronos::MarketReplayStep;
using chronos::TopOfBookEvent;
using chronos::TradeEvent;
using chronos::ValidatedMarketEvent;

static_assert(!std::is_copy_constructible_v<EquitiesMiniMbp1FileReader>);
static_assert(!std::is_move_constructible_v<EquitiesMiniMbp1FileReader>);

class TemporaryRecordingDestination final {
public:
    TemporaryRecordingDestination() {
        static std::uint64_t next_id = 0;
        const auto timestamp = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
        path_ = std::filesystem::temp_directory_path() /
            ("chronos-recording-" + std::to_string(timestamp) + "-" +
             std::to_string(next_id++) + ".dbn.zst");
    }

    ~TemporaryRecordingDestination() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(manifest_path(), ignored);
    }

    TemporaryRecordingDestination(
        const TemporaryRecordingDestination&
    ) = delete;
    TemporaryRecordingDestination& operator=(
        const TemporaryRecordingDestination&
    ) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::filesystem::path manifest_path() const {
        return path_.string() + ".manifest.json";
    }

private:
    std::filesystem::path path_;
};

HistoricalRecordingError historical_error(
    HistoricalRecordingErrorCode code,
    std::string message
) {
    return HistoricalRecordingError{
        .code = code,
        .field = "fake_provider",
        .message = std::move(message),
        .recording_error = std::nullopt,
        .manifest_error = std::nullopt,
    };
}

class FakeHistoricalRecordingProvider final
    : public HistoricalRecordingProvider {
public:
    explicit FakeHistoricalRecordingProvider(
        std::filesystem::path source_recording,
        HistoricalRecordingQuote quote = HistoricalRecordingQuote{
            .estimated_record_count = 2,
            .estimated_billable_size_bytes = 1'024,
            .estimated_cost_usd = 0.01,
        }
    ) : source_recording_(std::move(source_recording)), quote_(quote) {}

    [[nodiscard]] HistoricalRecordingQuoteResult quote(
        const HistoricalRecordingQuery&
    ) override {
        ++quote_calls;
        if (quote_error) {
            return *quote_error;
        }
        return quote_;
    }

    [[nodiscard]] HistoricalRecordingDownloadResult download_to_file(
        const HistoricalRecordingQuery&,
        const std::filesystem::path& destination
    ) override {
        ++download_calls;
        if (download_error) {
            return *download_error;
        }
        std::error_code copy_error;
        std::filesystem::copy_file(
            source_recording_,
            destination,
            std::filesystem::copy_options::overwrite_existing,
            copy_error
        );
        if (copy_error) {
            return historical_error(
                HistoricalRecordingErrorCode::ProviderDownloadFailed,
                copy_error.message()
            );
        }
        return std::monostate{};
    }

    std::optional<HistoricalRecordingError> quote_error;
    std::optional<HistoricalRecordingError> download_error;
    std::uint64_t quote_calls{0};
    std::uint64_t download_calls{0};

private:
    std::filesystem::path source_recording_;
    HistoricalRecordingQuote quote_;
};

HistoricalRecordingQuery valid_historical_query() {
    return HistoricalRecordingQuery{
        .start = "2024-01-02T14:30:00Z",
        .end = "2024-01-02T14:31:00Z",
        .symbols = {"AAPL", "SPY"},
    };
}

HistoricalRecordingAcquisitionSuccess require_acquisition_success(
    HistoricalRecordingAcquisitionResult result
) {
    REQUIRE(std::holds_alternative<HistoricalRecordingAcquisitionSuccess>(
        result
    ));
    return std::get<HistoricalRecordingAcquisitionSuccess>(
        std::move(result)
    );
}

HistoricalRecordingError require_acquisition_error(
    HistoricalRecordingAcquisitionResult result,
    HistoricalRecordingErrorCode code
) {
    REQUIRE(std::holds_alternative<HistoricalRecordingError>(result));
    auto error = std::get<HistoricalRecordingError>(std::move(result));
    REQUIRE(error.code == code);
    REQUIRE(!error.field.empty());
    REQUIRE(!error.message.empty());
    return error;
}

std::unique_ptr<EquitiesMiniMbp1FileReader> require_reader(
    EquitiesMiniMbp1FileOpenResult result
) {
    REQUIRE(std::holds_alternative<
        std::unique_ptr<EquitiesMiniMbp1FileReader>
    >(result));
    return std::get<std::unique_ptr<EquitiesMiniMbp1FileReader>>(
        std::move(result)
    );
}

EquitiesMiniMbp1FileError require_open_error(
    EquitiesMiniMbp1FileOpenResult result,
    EquitiesMiniMbp1FileErrorCode code
) {
    REQUIRE(std::holds_alternative<EquitiesMiniMbp1FileError>(result));
    auto error = std::get<EquitiesMiniMbp1FileError>(std::move(result));
    REQUIRE(error.code == code);
    REQUIRE(!error.field.empty());
    REQUIRE(!error.message.empty());
    return error;
}

ValidatedMarketEvent require_event(EquitiesMiniMbp1FileReadResult result) {
    REQUIRE(std::holds_alternative<ValidatedMarketEvent>(result));
    return std::get<ValidatedMarketEvent>(std::move(result));
}

EquitiesMiniMbp1FileError require_read_error(
    EquitiesMiniMbp1FileReadResult result,
    EquitiesMiniMbp1FileErrorCode code
) {
    REQUIRE(std::holds_alternative<EquitiesMiniMbp1FileError>(result));
    auto error = std::get<EquitiesMiniMbp1FileError>(std::move(result));
    REQUIRE(error.code == code);
    REQUIRE(!error.field.empty());
    REQUIRE(!error.message.empty());
    return error;
}

void require_end(EquitiesMiniMbp1FileReadResult result) {
    REQUIRE(std::holds_alternative<EndOfRecording>(result));
}

void test_file_reader_streams_records_in_deterministic_order() {
    TemporaryDbnFile file;
    file.write(valid_metadata(), valid_records());
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("aapl-spy-session"),
        10
    ));

    REQUIRE(!reader->manifest().has_value());
    const auto aapl_top = require_event(reader->read_next());
    const auto spy_trade = require_event(reader->read_next());
    const auto spy_top = require_event(reader->read_next());
    require_end(reader->read_next());
    require_end(reader->read_next());

    const auto& first_top = std::get<TopOfBookEvent>(aapl_top.value());
    const auto& trade = std::get<TradeEvent>(spy_trade.value());
    const auto& second_top = std::get<TopOfBookEvent>(spy_top.value());
    REQUIRE(first_top.header.instrument.value() == "AAPL");
    REQUIRE(first_top.header.ingest_sequence == 10);
    REQUIRE(trade.header.instrument.value() == "SPY");
    REQUIRE(trade.header.ingest_sequence == 11);
    REQUIRE(second_top.header.instrument.value() == "SPY");
    REQUIRE(second_top.header.ingest_sequence == 12);
    REQUIRE(first_top.header.quality.contains(MarketDataQualityFlag::Replay));
    REQUIRE(trade.header.quality.contains(MarketDataQualityFlag::Replay));
}

void test_completed_reader_exposes_checksum_backed_manifest() {
    TemporaryDbnFile file;
    file.write(valid_metadata(), valid_records());
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("manifest-stream"),
        7
    ));
    while (std::holds_alternative<ValidatedMarketEvent>(
        reader->read_next()
    )) {
    }

    REQUIRE(reader->manifest().has_value());
    const auto& manifest = *reader->manifest();
    REQUIRE(manifest.manifest_version() == 1);
    REQUIRE(manifest.source_file_name() == file.path().filename().string());
    REQUIRE(manifest.source_size_bytes() == std::filesystem::file_size(
        file.path()
    ));
    REQUIRE(manifest.source_sha256().size() == 64);
    REQUIRE(
        manifest.source_sha256() ==
        "ca6d2363a203cc7de58a3a67357206bae6936f9f8cf3e6a1d11063ba3a94f94e"
    );
    REQUIRE(manifest.dataset() == "EQUS.MINI");
    REQUIRE(manifest.schema() == "mbp-1");
    REQUIRE(manifest.input_symbology() == "raw_symbol");
    REQUIRE(manifest.output_symbology() == "instrument_id");
    REQUIRE(manifest.requested_symbols() ==
        std::vector<std::string>({"AAPL", "SPY"}));
    REQUIRE(manifest.partially_resolved_symbols() ==
        std::vector<std::string>({"SPY"}));
    REQUIRE(manifest.unresolved_symbols() ==
        std::vector<std::string>({"MISSING"}));
    REQUIRE(manifest.stream() == MarketDataStreamId("manifest-stream"));
    REQUIRE(manifest.first_ingest_sequence() == 7);
    REQUIRE(manifest.last_ingest_sequence() == 9);
    REQUIRE(manifest.provider_record_count() == 2);
    REQUIRE(manifest.normalized_event_count() == 3);

    const std::string first_json = manifest.to_json();
    REQUIRE(first_json == manifest.to_json());
    const nlohmann::json document = nlohmann::json::parse(first_json);
    REQUIRE(document.at("provider").at("sdk_version") == "0.68.0");
    REQUIRE(document.at("counts").at("provider_records") == 2);
    REQUIRE(document.at("normalization").at("last_ingest_sequence") == 9);
    REQUIRE(document.at("source").at("file_name") ==
        file.path().filename().string());
    REQUIRE(first_json.find(file.path().parent_path().string()) ==
        std::string::npos);
}

void test_compressed_dbn_is_streamed_transparently() {
    TemporaryDbnFile file;
    file.write_compressed(valid_metadata(), valid_records());
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("compressed")
    ));

    require_event(reader->read_next());
    require_event(reader->read_next());
    require_event(reader->read_next());
    require_end(reader->read_next());
    REQUIRE(reader->manifest().has_value());
    REQUIRE(reader->manifest()->provider_record_count() == 2);
    REQUIRE(reader->manifest()->normalized_event_count() == 3);
}

void test_appended_send_timestamp_is_recorded_and_ignored() {
    TemporaryDbnFile file;
    const auto record = mbp1_record(kAaplInstrumentId, 1'000);
    file.write_with_send_timestamp(
        valid_metadata(),
        record,
        unix_nanos(recording_day_nanoseconds() + 2'000)
    );
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("ts-out")
    ));

    const auto event = require_event(reader->read_next());
    REQUIRE(
        std::get<TopOfBookEvent>(event.value()).header.instrument.value() ==
        "AAPL"
    );
    require_end(reader->read_next());
    REQUIRE(reader->manifest()->includes_send_timestamps());
}

void test_empty_valid_recording_has_complete_manifest() {
    TemporaryDbnFile file;
    file.write(valid_metadata(), {});
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("empty-valid"),
        42
    ));

    require_end(reader->read_next());
    REQUIRE(reader->manifest().has_value());
    REQUIRE(reader->manifest()->provider_record_count() == 0);
    REQUIRE(reader->manifest()->normalized_event_count() == 0);
    REQUIRE(reader->manifest()->first_ingest_sequence() == 42);
    REQUIRE(!reader->manifest()->last_ingest_sequence().has_value());
}

void test_manifest_is_published_once_without_overwrite() {
    TemporaryDbnFile file;
    file.write(valid_metadata(), valid_records());
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("published-manifest")
    ));
    while (std::holds_alternative<ValidatedMarketEvent>(
        reader->read_next()
    )) {
    }
    REQUIRE(reader->manifest().has_value());

    auto first_write = adapter::write_recording_manifest(
        file.manifest_path(),
        *reader->manifest()
    );
    REQUIRE(std::holds_alternative<RecordingManifestWriteSuccess>(
        first_write
    ));
    REQUIRE(
        std::get<RecordingManifestWriteSuccess>(first_write).bytes_written ==
        reader->manifest()->to_json().size()
    );

    std::ifstream input(file.manifest_path(), std::ios::binary);
    const std::string written_contents{
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>()
    };
    REQUIRE(written_contents == reader->manifest()->to_json());
    const auto permissions = std::filesystem::status(
        file.manifest_path()
    ).permissions();
    REQUIRE(
        (permissions & std::filesystem::perms::owner_write) ==
        std::filesystem::perms::none
    );

    auto second_write = adapter::write_recording_manifest(
        file.manifest_path(),
        *reader->manifest()
    );
    REQUIRE(std::holds_alternative<RecordingManifestWriteError>(
        second_write
    ));
    REQUIRE(
        std::get<RecordingManifestWriteError>(second_write).code ==
        RecordingManifestWriteErrorCode::DestinationAlreadyExists
    );

    std::ifstream unchanged_input(file.manifest_path(), std::ios::binary);
    const std::string unchanged_contents{
        std::istreambuf_iterator<char>(unchanged_input),
        std::istreambuf_iterator<char>()
    };
    REQUIRE(unchanged_contents == written_contents);
}

void test_invalid_source_paths_and_files_are_rejected() {
    require_open_error(
        EquitiesMiniMbp1FileReader::open(
            std::filesystem::temp_directory_path(),
            MarketDataStreamId("directory")
        ),
        EquitiesMiniMbp1FileErrorCode::SourcePathNotRegularFile
    );

    TemporaryDbnFile empty_file;
    std::ofstream(empty_file.path(), std::ios::binary);
    require_open_error(
        EquitiesMiniMbp1FileReader::open(
            empty_file.path(),
            MarketDataStreamId("empty")
        ),
        EquitiesMiniMbp1FileErrorCode::SourceFileOpenFailed
    );
}

void test_metadata_contract_is_enforced() {
    TemporaryDbnFile file;
    db::Metadata metadata = valid_metadata();
    metadata.dataset = "XNAS.ITCH";
    file.write(metadata, {});
    require_open_error(
        EquitiesMiniMbp1FileReader::open(
            file.path(),
            MarketDataStreamId("wrong-dataset")
        ),
        EquitiesMiniMbp1FileErrorCode::UnexpectedDataset
    );

    metadata = valid_metadata();
    metadata.schema = db::Schema::Mbp10;
    file.write(metadata, {});
    require_open_error(
        EquitiesMiniMbp1FileReader::open(
            file.path(),
            MarketDataStreamId("wrong-schema")
        ),
        EquitiesMiniMbp1FileErrorCode::UnexpectedSchema
    );

    metadata = valid_metadata();
    metadata.schema = std::nullopt;
    file.write(metadata, {});
    require_open_error(
        EquitiesMiniMbp1FileReader::open(
            file.path(),
            MarketDataStreamId("missing-schema")
        ),
        EquitiesMiniMbp1FileErrorCode::MissingSchema
    );

    metadata = valid_metadata();
    metadata.stype_out = db::SType::RawSymbol;
    file.write(metadata, {});
    require_open_error(
        EquitiesMiniMbp1FileReader::open(
            file.path(),
            MarketDataStreamId("wrong-stype")
        ),
        EquitiesMiniMbp1FileErrorCode::UnexpectedOutputSymbology
    );
}

void test_unmapped_instrument_is_a_structured_terminal_error() {
    TemporaryDbnFile file;
    db::Metadata metadata = valid_metadata();
    metadata.mappings.resize(1);
    file.write(metadata, {mbp1_record(kSpyInstrumentId, 1'000)});
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("unmapped")
    ));

    const auto error = require_read_error(
        reader->read_next(),
        EquitiesMiniMbp1FileErrorCode::SymbolResolutionFailed
    );
    REQUIRE(error.source_record_number == 1);
    REQUIRE(!reader->manifest().has_value());
    require_read_error(
        reader->read_next(),
        EquitiesMiniMbp1FileErrorCode::SymbolResolutionFailed
    );
}

// The provider-independent view streams the same events and ends with the
// verified checksum, so a replay driver needs no Databento types.
void test_event_reader_streams_events_and_ends_with_the_checksum() {
    TemporaryDbnFile file;
    file.write(valid_metadata(), valid_records());
    auto opened = EquitiesMiniMbp1EventReader::open(
        file.path(),
        MarketDataStreamId("event-reader")
    );
    REQUIRE(std::holds_alternative<std::unique_ptr<EquitiesMiniMbp1EventReader>>(opened));
    auto& reader = *std::get<std::unique_ptr<EquitiesMiniMbp1EventReader>>(opened);
    std::vector<std::uint64_t> sequences;
    while (true) {
        auto next = reader.read_next();
        if (auto* event = std::get_if<ValidatedMarketEvent>(&next)) {
            sequences.push_back(chronos::market_event_header(event->value()).ingest_sequence);
            continue;
        }
        const auto* end = std::get_if<chronos::EndOfMarketEvents>(&next);
        REQUIRE(end != nullptr);
        REQUIRE(end->source_sha256.has_value());
        REQUIRE(end->source_sha256->size() == 64);
        break;
    }
    REQUIRE((sequences == std::vector<std::uint64_t>{0, 1, 2}));
}

void test_event_reader_reports_errors_with_their_record() {
    TemporaryDbnFile file;
    db::Metadata metadata = valid_metadata();
    metadata.mappings.resize(1);
    file.write(metadata, {mbp1_record(kSpyInstrumentId, 1'000)});
    auto opened = EquitiesMiniMbp1EventReader::open(file.path(), MarketDataStreamId("unmapped"));
    auto& reader = *std::get<std::unique_ptr<EquitiesMiniMbp1EventReader>>(opened);
    const auto next = reader.read_next();
    const auto* error = std::get_if<chronos::MarketEventReadError>(&next);
    REQUIRE(error != nullptr);
    REQUIRE(error->message.starts_with("record 1: "));

    const auto missing = EquitiesMiniMbp1EventReader::open(
        file.path().parent_path() / "missing.dbn.zst",
        MarketDataStreamId("missing")
    );
    REQUIRE(std::holds_alternative<EquitiesMiniMbp1FileError>(missing));
}

void test_normalization_error_preserves_nested_detail() {
    TemporaryDbnFile file;
    db::Mbp1Msg invalid = mbp1_record(kAaplInstrumentId, 1'000);
    invalid.sequence = 1;
    file.write(valid_metadata(), {invalid});
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("invalid-record")
    ));

    const auto error = require_read_error(
        reader->read_next(),
        EquitiesMiniMbp1FileErrorCode::RecordNormalizationFailed
    );
    REQUIRE(error.source_record_number == 1);
    REQUIRE(error.normalization_error.has_value());
    REQUIRE(
        error.normalization_error->code ==
        adapter::EquitiesMiniMbp1ErrorCode::UnexpectedVenueSequence
    );
}

void test_record_type_mismatch_is_rejected() {
    TemporaryDbnFile file;
    const std::uint64_t timestamp = recording_day_nanoseconds() + 1'000;
    const db::TradeMsg trade{
        .hd = db::RecordHeader{
            .length = static_cast<std::uint8_t>(
                sizeof(db::TradeMsg) / db::RecordHeader::kLengthMultiplier
            ),
            .rtype = db::r_type::Mbp0,
            .publisher_id = static_cast<std::uint16_t>(
                db::Publisher::EqusMiniEqus
            ),
            .instrument_id = kAaplInstrumentId,
            .ts_event = unix_nanos(timestamp),
        },
        .price = 100'000'000'000,
        .size = 1,
        .action = db::action::Trade,
        .side = db::side::Bid,
        .flags = db::FlagSet{},
        .depth = 0,
        .ts_recv = unix_nanos(timestamp + 100),
        .ts_in_delta = db::TimeDeltaNanos{0},
        .sequence = 0,
    };
    file.write_trade_record(valid_metadata(), trade);
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("wrong-record")
    ));
    require_read_error(
        reader->read_next(),
        EquitiesMiniMbp1FileErrorCode::UnexpectedRecordType
    );
}

void test_source_mutation_prevents_manifest_creation() {
    TemporaryDbnFile file;
    file.write(valid_metadata(), valid_records());
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("mutated")
    ));
    require_event(reader->read_next());

    std::ofstream output(file.path(), std::ios::binary | std::ios::app);
    output.put('\0');
    output.close();

    require_event(reader->read_next());
    require_event(reader->read_next());
    require_read_error(
        reader->read_next(),
        EquitiesMiniMbp1FileErrorCode::SourceFileChanged
    );
    REQUIRE(!reader->manifest().has_value());
}

void test_stable_trailing_partial_record_is_rejected() {
    TemporaryDbnFile file;
    file.write(valid_metadata(), valid_records());
    std::ofstream output(file.path(), std::ios::binary | std::ios::app);
    output.put('\0');
    output.close();

    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("trailing-partial")
    ));
    require_event(reader->read_next());
    require_event(reader->read_next());
    require_event(reader->read_next());
    require_read_error(
        reader->read_next(),
        EquitiesMiniMbp1FileErrorCode::RecordDecodeFailed
    );
    REQUIRE(!reader->manifest().has_value());
}

void test_ingest_sequence_exhaustion_stays_structured() {
    TemporaryDbnFile file;
    auto records = valid_records();
    file.write(valid_metadata(), {records[1]});
    auto reader = require_reader(EquitiesMiniMbp1FileReader::open(
        file.path(),
        MarketDataStreamId("exhausted"),
        std::numeric_limits<std::uint64_t>::max()
    ));

    const auto error = require_read_error(
        reader->read_next(),
        EquitiesMiniMbp1FileErrorCode::RecordNormalizationFailed
    );
    REQUIRE(error.normalization_error.has_value());
    REQUIRE(
        error.normalization_error->code ==
        adapter::EquitiesMiniMbp1ErrorCode::IngestSequenceExhausted
    );
}

struct ReplayOutcome {
    chronos::CurrentMarketView market_view;
    std::optional<chronos::Timestamp> replay_time;
    std::vector<MarketReplayStep> steps;

    friend bool operator==(const ReplayOutcome&, const ReplayOutcome&) =
        default;
};

ReplayOutcome replay_recording(const std::filesystem::path& path) {
    const MarketDataStreamId stream("dbn-replay");
    auto reader = require_reader(
        EquitiesMiniMbp1FileReader::open(path, stream)
    );
    MarketReplay replay(stream);
    std::vector<MarketReplayStep> steps;

    while (true) {
        auto read_result = reader->read_next();
        if (std::holds_alternative<EndOfRecording>(read_result)) {
            break;
        }
        REQUIRE(std::holds_alternative<ValidatedMarketEvent>(read_result));
        auto replay_result = replay.accept(
            std::get<ValidatedMarketEvent>(read_result)
        );
        REQUIRE(std::holds_alternative<MarketReplayStep>(replay_result));
        steps.push_back(
            std::get<MarketReplayStep>(std::move(replay_result))
        );
    }

    return ReplayOutcome{
        .market_view = replay.market_view(),
        .replay_time = replay.clock().current_time(),
        .steps = std::move(steps),
    };
}

void test_dbn_recording_replays_identically_into_market_view() {
    TemporaryDbnFile file;
    file.write_compressed(valid_metadata(), valid_records());

    const ReplayOutcome first = replay_recording(file.path());
    const ReplayOutcome second = replay_recording(file.path());
    REQUIRE(second == first);
    REQUIRE(first.steps.size() == 3);

    const auto* aapl = first.market_view.find(
        chronos::InstrumentId("AAPL")
    );
    REQUIRE(aapl != nullptr);
    REQUIRE(aapl->top_of_book.has_value());
    REQUIRE(!aapl->last_trade.has_value());

    const auto* spy = first.market_view.find(chronos::InstrumentId("SPY"));
    REQUIRE(spy != nullptr);
    REQUIRE(spy->last_trade.has_value());
    REQUIRE(spy->top_of_book.has_value());
    REQUIRE(first.replay_time == spy->top_of_book->header.receive_timestamp);
}

void test_historical_acquisition_is_cost_bounded_and_write_once() {
    TemporaryDbnFile source;
    source.write_compressed(valid_metadata(), valid_records());
    TemporaryRecordingDestination destination;
    FakeHistoricalRecordingProvider provider(source.path());

    const auto success = require_acquisition_success(
        adapter::acquire_historical_recording(
            provider,
            HistoricalRecordingAcquisitionRequest{
                .query = valid_historical_query(),
                .destination = destination.path(),
                .stream = MarketDataStreamId("historical-acquisition"),
                .maximum_cost_usd = 0.02,
            }
        )
    );

    REQUIRE(provider.quote_calls == 1);
    REQUIRE(provider.download_calls == 1);
    REQUIRE(success.quote.estimated_cost_usd == 0.01);
    REQUIRE(success.recording_path == destination.path());
    REQUIRE(success.manifest_path == destination.manifest_path());
    REQUIRE(std::filesystem::is_regular_file(destination.path()));
    REQUIRE(std::filesystem::is_regular_file(destination.manifest_path()));
    REQUIRE(success.manifest.source_file_name() ==
        destination.path().filename().string());
    REQUIRE(success.manifest.provider_record_count() == 2);
    REQUIRE(success.manifest.normalized_event_count() == 3);

    const auto permissions =
        std::filesystem::status(destination.path()).permissions();
    REQUIRE(
        (permissions & std::filesystem::perms::owner_write) ==
        std::filesystem::perms::none
    );

    require_acquisition_error(
        adapter::acquire_historical_recording(
            provider,
            HistoricalRecordingAcquisitionRequest{
                .query = valid_historical_query(),
                .destination = destination.path(),
                .stream = MarketDataStreamId("historical-acquisition"),
                .maximum_cost_usd = 0.02,
            }
        ),
        HistoricalRecordingErrorCode::DestinationAlreadyExists
    );
    REQUIRE(provider.quote_calls == 1);
    REQUIRE(provider.download_calls == 1);
}

void test_historical_acquisition_refuses_cost_above_limit() {
    TemporaryDbnFile source;
    source.write(valid_metadata(), valid_records());
    TemporaryRecordingDestination destination;
    FakeHistoricalRecordingProvider provider(
        source.path(),
        HistoricalRecordingQuote{
            .estimated_record_count = 2,
            .estimated_billable_size_bytes = 1'024,
            .estimated_cost_usd = 0.25,
        }
    );

    require_acquisition_error(
        adapter::acquire_historical_recording(
            provider,
            HistoricalRecordingAcquisitionRequest{
                .query = valid_historical_query(),
                .destination = destination.path(),
                .stream = MarketDataStreamId("cost-limit"),
                .maximum_cost_usd = 0.10,
            }
        ),
        HistoricalRecordingErrorCode::CostLimitExceeded
    );

    REQUIRE(provider.quote_calls == 1);
    REQUIRE(provider.download_calls == 0);
    REQUIRE(!std::filesystem::exists(destination.path()));
    REQUIRE(!std::filesystem::exists(destination.manifest_path()));
}

void test_historical_acquisition_cleans_failed_download() {
    TemporaryDbnFile source;
    source.write(valid_metadata(), valid_records());
    TemporaryRecordingDestination destination;
    FakeHistoricalRecordingProvider provider(source.path());
    provider.download_error = historical_error(
        HistoricalRecordingErrorCode::ProviderDownloadFailed,
        "simulated download failure"
    );

    require_acquisition_error(
        adapter::acquire_historical_recording(
            provider,
            HistoricalRecordingAcquisitionRequest{
                .query = valid_historical_query(),
                .destination = destination.path(),
                .stream = MarketDataStreamId("download-failure"),
                .maximum_cost_usd = 0.02,
            }
        ),
        HistoricalRecordingErrorCode::ProviderDownloadFailed
    );

    REQUIRE(provider.download_calls == 1);
    REQUIRE(!std::filesystem::exists(destination.path()));
    REQUIRE(!std::filesystem::exists(destination.manifest_path()));
}

void test_historical_acquisition_removes_invalid_recording() {
    TemporaryDbnFile invalid_source;
    std::ofstream(invalid_source.path(), std::ios::binary) << "not dbn";
    TemporaryRecordingDestination destination;
    FakeHistoricalRecordingProvider provider(invalid_source.path());

    const auto error = require_acquisition_error(
        adapter::acquire_historical_recording(
            provider,
            HistoricalRecordingAcquisitionRequest{
                .query = valid_historical_query(),
                .destination = destination.path(),
                .stream = MarketDataStreamId("invalid-download"),
                .maximum_cost_usd = 0.02,
            }
        ),
        HistoricalRecordingErrorCode::RecordingValidationFailed
    );

    REQUIRE(error.recording_error.has_value());
    REQUIRE(!std::filesystem::exists(destination.path()));
    REQUIRE(!std::filesystem::exists(destination.manifest_path()));
}

}  // namespace

int main() {
    return chronos::test::run_tests({
        {
            "deterministic file stream",
            test_file_reader_streams_records_in_deterministic_order,
        },
        {
            "recording manifest",
            test_completed_reader_exposes_checksum_backed_manifest,
        },
        {
            "compressed DBN",
            test_compressed_dbn_is_streamed_transparently,
        },
        {
            "send timestamp",
            test_appended_send_timestamp_is_recorded_and_ignored,
        },
        {
            "empty recording",
            test_empty_valid_recording_has_complete_manifest,
        },
        {
            "immutable manifest publication",
            test_manifest_is_published_once_without_overwrite,
        },
        {
            "invalid source",
            test_invalid_source_paths_and_files_are_rejected,
        },
        {"metadata contract", test_metadata_contract_is_enforced},
        {
            "unmapped instrument",
            test_unmapped_instrument_is_a_structured_terminal_error,
        },
        {
            "event reader streams events and ends with the checksum",
            test_event_reader_streams_events_and_ends_with_the_checksum,
        },
        {
            "event reader reports errors with their record",
            test_event_reader_reports_errors_with_their_record,
        },
        {
            "nested normalization error",
            test_normalization_error_preserves_nested_detail,
        },
        {"record type mismatch", test_record_type_mismatch_is_rejected},
        {
            "source mutation",
            test_source_mutation_prevents_manifest_creation,
        },
        {
            "trailing partial record",
            test_stable_trailing_partial_record_is_rejected,
        },
        {
            "sequence exhaustion",
            test_ingest_sequence_exhaustion_stays_structured,
        },
        {
            "deterministic DBN replay",
            test_dbn_recording_replays_identically_into_market_view,
        },
        {
            "historical acquisition",
            test_historical_acquisition_is_cost_bounded_and_write_once,
        },
        {
            "historical cost limit",
            test_historical_acquisition_refuses_cost_above_limit,
        },
        {
            "historical download cleanup",
            test_historical_acquisition_cleans_failed_download,
        },
        {
            "invalid historical recording cleanup",
            test_historical_acquisition_removes_invalid_recording,
        },
    });
}
