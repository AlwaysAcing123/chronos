#include "chronos/databento/equities_mini_mbp1_event_reader.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace chronos::databento_adapter {
namespace {

// The reader's message names the problem without echoing record bytes; the
// record number locates it in the recording.
std::string describe(const EquitiesMiniMbp1FileError& error) {
    if (!error.source_record_number) {
        return error.message;
    }
    return "record " + std::to_string(*error.source_record_number) + ": " +
        error.message;
}

}  // namespace

EquitiesMiniMbp1EventReader::EquitiesMiniMbp1EventReader(
    std::unique_ptr<EquitiesMiniMbp1FileReader> reader
)
    : reader_(std::move(reader)) {}

std::variant<
    std::unique_ptr<EquitiesMiniMbp1EventReader>,
    EquitiesMiniMbp1FileError
>
EquitiesMiniMbp1EventReader::open(
    const std::filesystem::path& source_path,
    MarketDataStreamId stream
) {
    auto opened = EquitiesMiniMbp1FileReader::open(source_path, std::move(stream));
    if (auto* error = std::get_if<EquitiesMiniMbp1FileError>(&opened)) {
        return std::move(*error);
    }
    return std::unique_ptr<EquitiesMiniMbp1EventReader>(
        new EquitiesMiniMbp1EventReader(std::get<
            std::unique_ptr<EquitiesMiniMbp1FileReader>
        >(std::move(opened)))
    );
}

MarketEventRead EquitiesMiniMbp1EventReader::read_next() {
    EquitiesMiniMbp1FileReadResult next = reader_->read_next();
    if (auto* event = std::get_if<ValidatedMarketEvent>(&next)) {
        return std::move(*event);
    }
    if (std::holds_alternative<EndOfRecording>(next)) {
        const auto& manifest = reader_->manifest();
        return EndOfMarketEvents{
            .source_sha256 = manifest
                ? std::optional<std::string>(std::string(manifest->source_sha256()))
                : std::nullopt,
        };
    }
    return MarketEventReadError{
        .message = describe(std::get<EquitiesMiniMbp1FileError>(next)),
    };
}

}  // namespace chronos::databento_adapter
