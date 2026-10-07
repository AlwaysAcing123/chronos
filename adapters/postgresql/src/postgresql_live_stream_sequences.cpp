#include "chronos/postgresql_live_stream_sequences.hpp"

#include "postgres_support.hpp"

#include <libpq-fe.h>

#include <array>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace chronos {
namespace {

using postgres_support::PostgresParameters;
using postgres_support::PostgresResult;

constexpr std::uint64_t kLastSequence = std::numeric_limits<std::uint64_t>::max();
// next_sequence once every sequence has been reserved: 2^64.
constexpr std::string_view kExhausted = "18446744073709551616";
constexpr std::string_view kUndefinedTable = "42P01";
constexpr std::string_view kUndefinedColumn = "42703";
constexpr std::size_t kMessageLimit = 240;

std::string bounded(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) {
        text.pop_back();
    }
    if (text.size() > kMessageLimit) {
        text.resize(kMessageLimit - 3);
        text += "...";
    }
    return text;
}

// next_sequence as read: a sequence, or nullopt for 2^64.
std::optional<std::uint64_t> parse_next(const std::string& text) {
    if (text == kExhausted) {
        return std::nullopt;
    }
    return postgres_support::parse_integer<std::uint64_t>(text, "live_stream_sequences.next_sequence");
}

std::string next_text(std::uint64_t last) {
    return last == kLastSequence ? std::string(kExhausted) : std::to_string(last + 1);
}

market_data::SequenceBlock reserve_from(std::uint64_t first, std::uint64_t block_size) {
    const std::uint64_t last = block_size - 1 > kLastSequence - first ? kLastSequence : first + (block_size - 1);
    return market_data::SequenceBlock{.first = first, .last = last};
}

// Thrown for a statement that did not do what was required; the caller
// rolls back and reports the store as unavailable.
[[noreturn]] void fail(const PostgresResult& result, std::string_view operation) {
    throw PaperPersistenceUnavailable(
        bounded("live_stream_sequences " + std::string(operation) + " failed: " + result.error_message())
    );
}

class MissingTable final : public std::runtime_error {
public:
    MissingTable()
        : std::runtime_error(
              "the database has no live_stream_sequences table; apply "
              "migrations/006_live_stream_sequences.sql"
          ) {}
};

// The fence's writer generations (ADR 0020): without them no writer can
// take a stream, so a deposed one could never be refused.
class MissingFence final : public std::runtime_error {
public:
    MissingFence()
        : std::runtime_error(
              "the database has no live-stream writer generations; apply "
              "migrations/007_live_stream_writer_fence.sql"
          ) {}
};

void require(const PostgresResult& result, ExecStatusType expected, std::string_view operation) {
    if (result.status() == expected) {
        return;
    }
    if (result.sql_state() == kUndefinedTable) {
        throw MissingTable();
    }
    if (result.sql_state() == kUndefinedColumn) {
        throw MissingFence();
    }
    fail(result, operation);
}

constexpr std::string_view kDurableMaximum = R"SQL(
    SELECT max(reference)::text AS maximum FROM (
        SELECT max(market_ingest_sequence) AS reference
        FROM paper_execution_states WHERE market_stream_id = $1
        UNION ALL
        SELECT max(market_ingest_sequence)
        FROM paper_fills WHERE market_stream_id = $1
        UNION ALL
        SELECT max(greatest(market_ingest_sequence, execution_market_ingest_sequence))
        FROM paper_order_records WHERE market_stream_id = $1
        UNION ALL
        SELECT max(accepted_market_ingest_sequence)
        FROM paper_orders WHERE market_stream_id = $1
        UNION ALL
        SELECT max(watermark_ingest_sequence)
        FROM model_deployments WHERE stream_id = $1
        UNION ALL
        SELECT max(greatest(observation_ingest_sequence, execution_ingest_sequence))
        FROM model_deployment_decisions WHERE stream_id = $1
    ) AS durable
)SQL";

// writer_first_sequence is read only so that a database without the
// fence is refused before anything changes.
constexpr std::string_view kLockRow = R"SQL(
    SELECT next_sequence::text AS next_sequence, writer_first_sequence::text AS writer_first_sequence,
           writer_id, writer_label,
           coalesce(lease_expires_at > clock_timestamp(), false) AS held,
           to_char(lease_expires_at AT TIME ZONE 'UTC', 'YYYY-MM-DD"T"HH24:MI:SS.MS"Z"') AS expires_at
    FROM live_stream_sequences WHERE stream_id = $1 FOR UPDATE
)SQL";

}  // namespace

struct PostgresIngestSequenceLeaseStore::Impl {
    Impl(std::string connection_string_, std::chrono::milliseconds statement_timeout_)
        : connection_string(std::move(connection_string_)),
          statement_timeout(statement_timeout_) {
        if (statement_timeout.count() <= 0) {
            throw std::invalid_argument("the statement timeout must be positive");
        }
    }

    ~Impl() { disconnect(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    void disconnect() noexcept {
        if (connection != nullptr) {
            PQfinish(connection);
            connection = nullptr;
        }
    }

    // A fresh session after any failure: settings never survive a reset,
    // and an aborted transaction is never reused.
    PGconn* connected() {
        if (connection != nullptr && PQstatus(connection) == CONNECTION_OK &&
            PQtransactionStatus(connection) == PQTRANS_IDLE) {
            return connection;
        }
        disconnect();
        const std::array<const char*, 8> keywords{
            "dbname", "connect_timeout", "application_name", "keepalives",
            "keepalives_idle", "keepalives_interval", "keepalives_count", nullptr,
        };
        const std::array<const char*, 8> values{
            connection_string.c_str(), "5", "chronos live stream sequences", "1", "5", "2", "2", nullptr,
        };
        connection = PQconnectdbParams(keywords.data(), values.data(), 1);
        if (connection == nullptr) {
            throw PaperPersistenceUnavailable("could not allocate a PostgreSQL connection");
        }
        if (PQstatus(connection) != CONNECTION_OK) {
            const std::string message = PQerrorMessage(connection);
            disconnect();
            throw PaperPersistenceUnavailable(bounded("could not connect to PostgreSQL: " + message));
        }
        const std::string timeout = std::to_string(statement_timeout.count());
        const auto configured = postgres_support::execute(
            connection,
            "SET statement_timeout = '" + timeout + "ms'; SET lock_timeout = '" + timeout +
                "ms'; SET idle_in_transaction_session_timeout = '" + timeout + "ms'"
        );
        if (configured.status() != PGRES_COMMAND_OK) {
            const std::string message = configured.error_message();
            disconnect();
            throw PaperPersistenceUnavailable(bounded("could not configure the PostgreSQL session: " + message));
        }
        return connection;
    }

    // Runs `work` in one transaction on a healthy connection. Any exception
    // drops the connection, so the next call starts a clean session and no
    // half-finished transaction survives.
    template <typename Work>
    auto transaction(Work&& work) {
        PGconn* session = connected();
        try {
            postgres_support::PostgresTransaction transaction(session);
            auto result = work(session);
            if (!result.second) {
                return std::move(result.first);  // rolled back by the destructor
            }
            transaction.commit();
            return std::move(result.first);
        } catch (...) {
            disconnect();
            throw;
        }
    }

    std::optional<std::uint64_t> maximum(PGconn* session, const MarketDataStreamId& stream) {
        PostgresParameters parameters;
        parameters.add(stream.value());
        const auto result = postgres_support::execute(session, kDurableMaximum, parameters);
        require(result, PGRES_TUPLES_OK, "durable history check");
        const auto value = result.optional_value(0, "maximum");
        return value ? std::optional<std::uint64_t>(
                           postgres_support::parse_integer<std::uint64_t>(*value, "durable ingest sequence")
                       )
                     : std::nullopt;
    }

    std::string connection_string;
    std::chrono::milliseconds statement_timeout;
    PGconn* connection{nullptr};
};

PostgresIngestSequenceLeaseStore::PostgresIngestSequenceLeaseStore(
    std::string connection_string,
    std::chrono::milliseconds statement_timeout
)
    : impl_(std::make_unique<Impl>(std::move(connection_string), statement_timeout)) {}

PostgresIngestSequenceLeaseStore::~PostgresIngestSequenceLeaseStore() = default;

market_data::LeaseAcquisition PostgresIngestSequenceLeaseStore::acquire(
    const MarketDataStreamId& stream,
    const market_data::StreamWriter& writer,
    std::chrono::milliseconds lease,
    std::uint64_t block_size,
    bool adopt_history
) {
    if (block_size == 0 || lease.count() <= 0) {
        throw std::invalid_argument("a lease and a block must not be empty");
    }
    using Outcome = std::pair<market_data::LeaseAcquisition, bool>;
    try {
        return impl_->transaction([&](PGconn* session) -> Outcome {
            PostgresParameters lock;
            lock.add(stream.value());
            auto row = postgres_support::execute(session, kLockRow, lock);
            require(row, PGRES_TUPLES_OK, "lock");
            std::optional<std::uint64_t> adopted;
            if (row.row_count() == 0) {
                const auto referenced = impl_->maximum(session, stream);
                if (referenced && !adopt_history) {
                    return {
                        market_data::LeaseRefused{
                            "live stream " + stream.value() + " has durable history up to ingest sequence " +
                            std::to_string(*referenced) +
                            " from before durable live sequences; start the server once with "
                            "--adopt-live-stream-history to continue after it, or use a new stream ID",
                        },
                        false,
                    };
                }
                PostgresParameters created;
                created.add(stream.value());
                created.add(referenced ? next_text(*referenced) : std::string("0"));
                created.add(referenced ? std::optional<std::string>(std::to_string(*referenced)) : std::nullopt);
                const auto inserted = postgres_support::execute(
                    session,
                    "INSERT INTO live_stream_sequences (stream_id, next_sequence, origin, adopted_maximum) "
                    "VALUES ($1, $2::numeric, CASE WHEN $3::numeric IS NULL THEN 'new' ELSE 'adopted' END, "
                    "$3::numeric) ON CONFLICT (stream_id) DO NOTHING",
                    created
                );
                require(inserted, PGRES_COMMAND_OK, "creation");
                if (inserted.affected_rows() == 1) {
                    adopted = referenced;
                }
                // Another process may have created it first: read what is there.
                row = postgres_support::execute(session, kLockRow, lock);
                require(row, PGRES_TUPLES_OK, "lock");
                if (row.row_count() != 1) {
                    throw PaperPersistenceUnavailable("live_stream_sequences lost the row it created");
                }
            }
            const auto holder = row.optional_value(0, "writer_id");
            const bool held = postgres_support::parse_boolean(row.value(0, "held"), "held");
            if (holder && *holder != writer.id && held) {
                return {
                    market_data::LeaseHeld{
                        .writer_label = row.optional_value(0, "writer_label").value_or("another process"),
                        .expires_at = row.optional_value(0, "expires_at").value_or("unknown"),
                    },
                    false,
                };
            }
            const std::string next_value = row.value(0, "next_sequence");
            const std::optional<std::uint64_t> next = parse_next(next_value);
            // Every reference must be behind what was ever reserved.
            if (const auto referenced = impl_->maximum(session, stream); referenced && next && *referenced >= *next) {
                return {
                    market_data::LeaseRefused{
                        "live stream " + stream.value() + " has durable history up to ingest sequence " +
                        std::to_string(*referenced) + ", at or beyond its next reservable sequence " +
                        std::to_string(*next) +
                        ": a server without durable live sequences, or a replay, wrote to it; "
                        "use a new stream ID",
                    },
                    false,
                };
            }
            if (!next) {
                return {
                    market_data::LeaseRefused{
                        "live stream " + stream.value() + " has issued every ingest sequence; use a new stream ID",
                    },
                    false,
                };
            }
            const market_data::SequenceBlock block = reserve_from(*next, block_size);
            PostgresParameters update;
            update.add(stream.value());
            update.add(next_text(block.last));
            update.add(writer.id);
            update.add(writer.label);
            update.add(static_cast<std::int64_t>(lease.count()));
            update.add(next_value);
            // A new generation begins with this block: every sequence any
            // earlier writer, this one included, could have numbered is
            // below it, so the fence refuses them from now on.
            const auto updated = postgres_support::execute(
                session,
                "UPDATE live_stream_sequences SET next_sequence = $2::numeric, writer_id = $3, "
                "writer_label = $4, lease_expires_at = clock_timestamp() + $5::bigint * interval '1 millisecond', "
                "writer_first_sequence = $6::numeric, acquisitions = acquisitions + 1, "
                "updated_at = clock_timestamp() "
                "WHERE stream_id = $1 AND next_sequence = $6::numeric",
                update
            );
            require(updated, PGRES_COMMAND_OK, "acquisition");
            if (updated.affected_rows() != 1) {
                throw PaperPersistenceUnavailable("live_stream_sequences changed under its row lock");
            }
            return {market_data::LeaseGranted{.block = block, .adopted_maximum = adopted}, true};
        });
    } catch (const MissingTable& missing) {
        return market_data::LeaseRefused{missing.what()};
    } catch (const MissingFence& missing) {
        return market_data::LeaseRefused{missing.what()};
    }
}

market_data::LeaseRenewal PostgresIngestSequenceLeaseStore::renew(
    const MarketDataStreamId& stream,
    const market_data::StreamWriter& writer,
    std::chrono::milliseconds lease,
    std::uint64_t block_size
) {
    using Outcome = std::pair<market_data::LeaseRenewal, bool>;
    return impl_->transaction([&](PGconn* session) -> Outcome {
        PostgresParameters lock;
        lock.add(stream.value());
        const auto row = postgres_support::execute(session, kLockRow, lock);
        require(row, PGRES_TUPLES_OK, "lock");
        if (row.row_count() == 0) {
            return {market_data::LeaseLost{"the stream has no record any more"}, false};
        }
        const auto holder = row.optional_value(0, "writer_id");
        if (holder != writer.id) {
            return {
                market_data::LeaseLost{
                    holder ? "it is now held by " + row.optional_value(0, "writer_label").value_or("another process")
                           : std::string("it was released"),
                },
                false,
            };
        }
        market_data::LeaseRenewed renewed;
        std::optional<std::string> next_after;
        if (block_size != 0) {
            if (const auto next = parse_next(row.value(0, "next_sequence"))) {
                renewed.block = reserve_from(*next, block_size);
                next_after = next_text(renewed.block->last);
            } else {
                renewed.exhausted = true;
            }
        }
        PostgresParameters update;
        update.add(stream.value());
        update.add(writer.id);
        update.add(static_cast<std::int64_t>(lease.count()));
        update.add(next_after);
        const auto updated = postgres_support::execute(
            session,
            "UPDATE live_stream_sequences SET lease_expires_at = clock_timestamp() + $3::bigint * interval "
            "'1 millisecond', next_sequence = coalesce($4::numeric, next_sequence), updated_at = clock_timestamp() "
            "WHERE stream_id = $1 AND writer_id = $2",
            update
        );
        require(updated, PGRES_COMMAND_OK, "renewal");
        if (updated.affected_rows() != 1) {
            throw PaperPersistenceUnavailable("live_stream_sequences changed under its row lock");
        }
        return {renewed, true};
    });
}

void PostgresIngestSequenceLeaseStore::release(
    const MarketDataStreamId& stream,
    const market_data::StreamWriter& writer
) {
    PGconn* session = impl_->connected();
    PostgresParameters parameters;
    parameters.add(stream.value());
    parameters.add(writer.id);
    try {
        const auto released = postgres_support::execute(
            session,
            "UPDATE live_stream_sequences SET writer_id = NULL, writer_label = NULL, lease_expires_at = NULL, "
            "writer_first_sequence = NULL, updated_at = clock_timestamp() "
            "WHERE stream_id = $1 AND writer_id = $2",
            parameters
        );
        require(released, PGRES_COMMAND_OK, "release");
    } catch (...) {
        impl_->disconnect();
        throw;
    }
}

std::optional<std::uint64_t> PostgresIngestSequenceLeaseStore::durable_maximum(const MarketDataStreamId& stream) {
    using Outcome = std::pair<std::optional<std::uint64_t>, bool>;
    return impl_->transaction([&](PGconn* session) -> Outcome { return {impl_->maximum(session, stream), true}; });
}

}  // namespace chronos
