#pragma once

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

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

// Synthetic EQUS.MINI MBP-1 recordings for tests: no network, no API key,
// and no licensed data.
namespace chronos::test::databento_fixtures {

namespace db = ::databento;

inline constexpr std::uint32_t kAaplInstrumentId = 38;
inline constexpr std::uint32_t kSpyInstrumentId = 39;

inline std::uint64_t recording_day_nanoseconds() {
    const auto day = date::sys_days{date::year{2024} / 1 / 2};
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            day.time_since_epoch()
        ).count()
    );
}

inline db::UnixNanos unix_nanos(std::uint64_t value) {
    return db::UnixNanos{db::UnixNanos::duration{value}};
}

inline db::Metadata valid_metadata() {
    const std::uint64_t start = recording_day_nanoseconds();
    return db::Metadata{
        db::kDbnVersion,
        "EQUS.MINI",
        db::Schema::Mbp1,
        unix_nanos(start),
        unix_nanos(start + 1'000'000'000),
        0,
        db::SType::RawSymbol,
        db::SType::InstrumentId,
        false,
        db::kSymbolCstrLen,
        {"AAPL", "SPY"},
        {"SPY"},
        {"MISSING"},
        {
            {"AAPL",
             {{
                 date::year{2024} / 1 / 2,
                 date::year{2024} / 1 / 3,
                 std::to_string(kAaplInstrumentId),
             }}},
            {"SPY",
             {{
                 date::year{2024} / 1 / 2,
                 date::year{2024} / 1 / 3,
                 std::to_string(kSpyInstrumentId),
             }}},
        },
    };
}

inline db::Mbp1Msg mbp1_record(
    std::uint32_t instrument_id,
    std::uint64_t timestamp_offset
) {
    const std::uint64_t start = recording_day_nanoseconds();
    return db::Mbp1Msg{
        .hd = db::RecordHeader{
            .length = static_cast<std::uint8_t>(
                sizeof(db::Mbp1Msg) / db::RecordHeader::kLengthMultiplier
            ),
            .rtype = db::r_type::Mbp1,
            .publisher_id = static_cast<std::uint16_t>(
                db::Publisher::EqusMiniEqus
            ),
            .instrument_id = instrument_id,
            .ts_event = unix_nanos(start + timestamp_offset),
        },
        .price = db::kUndefPrice,
        .size = 0,
        .action = db::action::Modify,
        .side = db::side::Bid,
        .flags = db::FlagSet{},
        .depth = 0,
        .ts_recv = unix_nanos(start + timestamp_offset + 100),
        .ts_in_delta = db::TimeDeltaNanos{0},
        .sequence = 0,
        .levels = std::array<db::BidAskPair, 1>{db::BidAskPair{
            .bid_px = 100'000'000'000,
            .ask_px = 100'010'000'000,
            .bid_sz = 5,
            .ask_sz = 6,
            .bid_ct = 0,
            .ask_ct = 0,
        }},
    };
}

inline std::vector<db::Mbp1Msg> valid_records() {
    db::Mbp1Msg aapl = mbp1_record(kAaplInstrumentId, 1'000);
    db::Mbp1Msg spy = mbp1_record(kSpyInstrumentId, 2'000);
    spy.action = db::action::Trade;
    spy.side = db::side::Ask;
    spy.price = 100'005'000'000;
    spy.size = 2;
    return {aapl, spy};
}

class TemporaryDbnFile final {
public:
    TemporaryDbnFile() {
        static std::uint64_t next_id = 0;
        const auto timestamp = std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count();
        path_ = std::filesystem::temp_directory_path() /
            ("chronos-dbn-" + std::to_string(timestamp) + "-" +
             std::to_string(next_id++) + ".dbn");
    }

    ~TemporaryDbnFile() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(manifest_path(), ignored);
    }

    TemporaryDbnFile(const TemporaryDbnFile&) = delete;
    TemporaryDbnFile& operator=(const TemporaryDbnFile&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::filesystem::path manifest_path() const {
        return path_.string() + ".manifest.json";
    }

    void write(
        const db::Metadata& metadata,
        const std::vector<db::Mbp1Msg>& records
    ) const {
        db::OutFileStream output(path_);
        db::DbnEncoder encoder(metadata, &output);
        for (const auto& record : records) {
            encoder.EncodeRecord(record);
        }
    }

    void write_trade_record(
        const db::Metadata& metadata,
        const db::TradeMsg& record
    ) const {
        db::OutFileStream output(path_);
        db::DbnEncoder encoder(metadata, &output);
        encoder.EncodeRecord(record);
    }

    void write_compressed(
        const db::Metadata& metadata,
        const std::vector<db::Mbp1Msg>& records
    ) const {
        db::OutFileStream output(path_);
        db::detail::ZstdCompressStream compression(&output);
        db::DbnEncoder encoder(metadata, &compression);
        for (const auto& record : records) {
            encoder.EncodeRecord(record);
        }
    }

    void write_with_send_timestamp(
        db::Metadata metadata,
        const db::Mbp1Msg& record,
        db::UnixNanos send_timestamp
    ) const {
        metadata.ts_out = true;
        db::OutFileStream output(path_);
        db::DbnEncoder encoder(metadata, &output);
        encoder.EncodeRecord(
            db::WithTsOut<db::Mbp1Msg>(record, send_timestamp)
        );
    }

private:
    std::filesystem::path path_;
};


}  // namespace chronos::test::databento_fixtures
