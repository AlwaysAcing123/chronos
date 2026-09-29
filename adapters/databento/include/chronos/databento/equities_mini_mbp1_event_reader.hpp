#pragma once

#include "chronos/databento/equities_mini_mbp1_file_reader.hpp"
#include "chronos/market_event_reader.hpp"

#include <filesystem>
#include <memory>
#include <variant>

namespace chronos::databento_adapter {

// Presents a local EQUS.MINI MBP-1 recording as a provider-independent
// MarketEventReader, so a replay driver never sees Databento types. Events
// are streamed from the file in normalized order; the end of the recording
// carries its verified SHA-256.
class EquitiesMiniMbp1EventReader final : public MarketEventReader {
public:
    [[nodiscard]] static std::variant<
        std::unique_ptr<EquitiesMiniMbp1EventReader>,
        EquitiesMiniMbp1FileError
    > open(
        const std::filesystem::path& source_path,
        MarketDataStreamId stream
    );

    [[nodiscard]] MarketEventRead read_next() override;

private:
    explicit EquitiesMiniMbp1EventReader(
        std::unique_ptr<EquitiesMiniMbp1FileReader> reader
    );

    std::unique_ptr<EquitiesMiniMbp1FileReader> reader_;
};

}  // namespace chronos::databento_adapter
