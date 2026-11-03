#include "chronos/market_data/dataset_replay.hpp"

#include "chronos/datasets/dataset_error.hpp"
#include "chronos/datasets/dataset_reader.hpp"

#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace chronos::market_data {
namespace {

std::string describe(const datasets::DatasetError& error) {
    std::string text = std::string(datasets::error_code_name(error.code)) + " (" + error.field;
    if (error.line) {
        text += " line " + std::to_string(*error.line);
    }
    return text + "): " + error.message;
}

std::string joined(const std::set<std::string>& names) {
    std::string text;
    for (const auto& name : names) {
        text += text.empty() ? name : ", " + name;
    }
    return text.empty() ? "none" : text;
}

std::string normalizer_of(const datasets::DatasetSource& source) {
    return source.normalizer + " version " + source.normalizer_version;
}

}  // namespace

MarketDataSourceDescription dataset_replay_description(
    const datasets::DatasetManifest& manifest,
    std::vector<InstrumentId> instruments
) {
    const datasets::DatasetSource& source = manifest.source;
    const std::string feed = source.provider_dataset + " " + source.provider_schema;
    if (source.provider == datasets::kSyntheticProvider) {
        return MarketDataSourceDescription{
            .mode = MarketDataMode::HistoricalReplay,
            .synthetic = true,
            .provider = source.provider,
            .feed = feed,
            .label = "Synthetic replay: invented prices, not market data",
            .coverage_notice =
                "These events are synthetic: " + source.description +
                ". Their prices, sizes, and times were generated, not observed on any "
                "exchange (" + normalizer_of(source) + "), so nothing here describes a "
                "real market.",
            .coverage_label = "Synthetic, not market data",
            .instruments = std::move(instruments),
        };
    }
    const std::string window = source.coverage
        ? "Its provider query window is in the dataset manifest (" + source.coverage->semantics + ")"
        : "No provider query window was recorded";
    return MarketDataSourceDescription{
        .mode = MarketDataMode::HistoricalReplay,
        .synthetic = false,
        .provider = source.provider,
        .feed = feed,
        .label = source.provider + " " + feed + " recording (normalized dataset replay)",
        .coverage_notice =
            "A recorded replay: " + source.description + ", normalized by " + normalizer_of(source) +
            ". " + window + ", and the replay holds only the events the recording contains.",
        .coverage_label = "Recorded, one query window",
        .instruments = std::move(instruments),
    };
}

std::variant<DatasetReplay, DatasetReplayRefusal> open_dataset_replay(
    const std::filesystem::path& directory,
    const MarketDataStreamId& stream,
    const std::vector<InstrumentId>& instruments
) {
    auto verified = datasets::verify_dataset(directory);
    if (const auto* error = std::get_if<datasets::DatasetError>(&verified)) {
        return DatasetReplayRefusal{"the dataset does not verify: " + describe(*error)};
    }
    const auto& manifest = std::get<datasets::DatasetManifest>(verified);
    if (manifest.stream != stream) {
        return DatasetReplayRefusal{
            "--stream-id " + stream.value() + " is not the dataset's stream, " + manifest.stream.value(),
        };
    }
    std::set<std::string> configured;
    for (const auto& instrument : instruments) {
        configured.insert(instrument.value());
    }
    std::set<std::string> recorded;
    for (const auto& entry : manifest.summary.by_instrument) {
        recorded.insert(entry.first);
    }
    if (configured != recorded) {
        return DatasetReplayRefusal{
            "--instrument must list exactly the dataset's instruments (" + joined(recorded) + "), not " +
            joined(configured) + ": every event of the dataset is replayed",
        };
    }
    auto opened = datasets::NormalizedMarketEventReader::open(directory);
    if (const auto* error = std::get_if<datasets::DatasetError>(&opened)) {
        return DatasetReplayRefusal{"the dataset cannot be opened for playback: " + describe(*error)};
    }
    auto reader = std::get<std::unique_ptr<datasets::NormalizedMarketEventReader>>(std::move(opened));
    if (reader->manifest() != manifest) {
        return DatasetReplayRefusal{"the dataset changed after it was verified"};
    }
    return DatasetReplay{
        .reader = std::move(reader),
        .description = dataset_replay_description(manifest, instruments),
        .dataset_id = manifest.dataset_id,
    };
}

}  // namespace chronos::market_data
