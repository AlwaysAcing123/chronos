#pragma once

#include "chronos/market_data/leased_ingest_sequences.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace chronos {

// Live streams' writer leases and sequence reservations in PostgreSQL
// (live_stream_sequences, migration 006; ADR 0020).
//
// It has a connection of its own, opened on first use and again after a
// failure, with short connect, statement, and lock timeouts and TCP
// keepalives, so a lost or blocked database fails a call within seconds
// instead of holding the keeper. Lease expiry is computed and compared with
// the database's clock_timestamp() only. Every call is one transaction; a
// call that throws may or may not have committed. A missing table, legacy
// history without adoption, history beyond the reservations, and an
// exhausted stream are refusals, not errors.
//
// Used by one thread at a time: the sequences' keeper.
class PostgresIngestSequenceLeaseStore final : public market_data::IngestSequenceLeaseStore {
public:
    explicit PostgresIngestSequenceLeaseStore(
        std::string connection_string,
        std::chrono::milliseconds statement_timeout = std::chrono::milliseconds(5'000)
    );
    ~PostgresIngestSequenceLeaseStore() override;

    PostgresIngestSequenceLeaseStore(const PostgresIngestSequenceLeaseStore&) = delete;
    PostgresIngestSequenceLeaseStore& operator=(const PostgresIngestSequenceLeaseStore&) = delete;

    [[nodiscard]] market_data::LeaseAcquisition acquire(
        const MarketDataStreamId& stream,
        const market_data::StreamWriter& writer,
        std::chrono::milliseconds lease,
        std::uint64_t block_size,
        bool adopt_history
    ) override;

    [[nodiscard]] market_data::LeaseRenewal renew(
        const MarketDataStreamId& stream,
        const market_data::StreamWriter& writer,
        std::chrono::milliseconds lease,
        std::uint64_t block_size
    ) override;

    void release(const MarketDataStreamId& stream, const market_data::StreamWriter& writer) override;

    // The largest sequence of `stream` that any durable account execution
    // state, order, fill, working order, deployment watermark, or decision
    // references, or nullopt if none does. Read in its own transaction.
    [[nodiscard]] std::optional<std::uint64_t> durable_maximum(const MarketDataStreamId& stream);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace chronos
