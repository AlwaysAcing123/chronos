#pragma once

// libpq helpers and the repository's private state, shared by the
// repository's translation units. Not a public interface.

#include "chronos/postgresql_paper_order_repository.hpp"

#include <libpq-fe.h>

#include <charconv>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace chronos {
namespace postgres_support {

class PostgresResult final {
public:
    explicit PostgresResult(PGresult* result) : result_(result) {
        if (result_ == nullptr) {
            throw std::runtime_error("PostgreSQL returned no result object");
        }
    }

    ~PostgresResult() { PQclear(result_); }

    PostgresResult(const PostgresResult&) = delete;
    PostgresResult& operator=(const PostgresResult&) = delete;
    PostgresResult(PostgresResult&& other) noexcept
        : result_(std::exchange(other.result_, nullptr)) {}
    PostgresResult& operator=(PostgresResult&& other) noexcept {
        if (this != &other) {
            PQclear(result_);
            result_ = std::exchange(other.result_, nullptr);
        }
        return *this;
    }

    [[nodiscard]] ExecStatusType status() const {
        return PQresultStatus(result_);
    }

    [[nodiscard]] int row_count() const { return PQntuples(result_); }

    // The rows an INSERT, UPDATE, or DELETE affected.
    [[nodiscard]] std::uint64_t affected_rows() const {
        const std::string_view count = PQcmdTuples(result_);
        std::uint64_t rows = 0;
        const auto parsed = std::from_chars(count.data(), count.data() + count.size(), rows);
        if (count.empty() || parsed.ec != std::errc{} || parsed.ptr != count.data() + count.size()) {
            throw std::runtime_error("PostgreSQL reported no affected row count");
        }
        return rows;
    }

    [[nodiscard]] bool is_null(int row, std::string_view column) const {
        return PQgetisnull(result_, row, column_index(column)) != 0;
    }

    [[nodiscard]] std::string value(
        int row,
        std::string_view column
    ) const {
        const int index = column_index(column);
        if (PQgetisnull(result_, row, index) != 0) {
            throw std::runtime_error(
                "unexpected NULL PostgreSQL column: " + std::string(column)
            );
        }
        return std::string(PQgetvalue(result_, row, index));
    }

    [[nodiscard]] std::optional<std::string> optional_value(
        int row,
        std::string_view column
    ) const {
        return is_null(row, column)
            ? std::nullopt
            : std::optional<std::string>(value(row, column));
    }

    [[nodiscard]] std::string error_message() const {
        const char* message = PQresultErrorMessage(result_);
        return message == nullptr ? "unknown PostgreSQL error" : message;
    }

    [[nodiscard]] std::string sql_state() const {
        const char* state = PQresultErrorField(result_, PG_DIAG_SQLSTATE);
        return state == nullptr ? "" : state;
    }

    [[nodiscard]] std::string constraint_name() const {
        const char* name = PQresultErrorField(
            result_,
            PG_DIAG_CONSTRAINT_NAME
        );
        return name == nullptr ? "" : name;
    }

private:
    [[nodiscard]] int column_index(std::string_view column) const {
        const std::string name(column);
        const int index = PQfnumber(result_, name.c_str());
        if (index < 0) {
            throw std::runtime_error(
                "PostgreSQL result is missing column: " + name
            );
        }
        return index;
    }

    PGresult* result_;
};

class PostgresParameters final {
public:
    void add(std::string value) {
        values_.push_back(checked_text(std::move(value)));
    }

    void add(std::string_view value) { add(std::string(value)); }

    void add(const char* value) { add(std::string(value)); }

    void add(std::int64_t value) { add(std::to_string(value)); }
    void add(std::uint64_t value) { add(std::to_string(value)); }
    void add(std::uint32_t value) { add(std::to_string(value)); }
    void add(bool value) { add(value ? "true" : "false"); }

    void add(const std::optional<std::string>& value) {
        values_.push_back(
            value ? std::optional<std::string>(checked_text(*value))
                  : std::nullopt
        );
    }

    void add_optional(std::optional<std::int64_t> value) {
        values_.push_back(value ? std::optional<std::string>(
            std::to_string(*value)
        ) : std::nullopt);
    }

    void add_optional(std::optional<std::uint64_t> value) {
        values_.push_back(value ? std::optional<std::string>(
            std::to_string(*value)
        ) : std::nullopt);
    }

    [[nodiscard]] const std::vector<const char*>& pointers() {
        pointers_.clear();
        pointers_.reserve(values_.size());
        for (const auto& value : values_) {
            pointers_.push_back(value ? value->c_str() : nullptr);
        }
        return pointers_;
    }

    [[nodiscard]] int size() const {
        if (values_.size() > static_cast<std::size_t>(
                std::numeric_limits<int>::max()
            )) {
            throw std::overflow_error("too many PostgreSQL parameters");
        }
        return static_cast<int>(values_.size());
    }

private:
    // libpq text parameters end at the first NUL byte. Persisting a silently
    // truncated identifier would make the journal disagree with the
    // in-memory state that was checked for idempotency.
    [[nodiscard]] static std::string checked_text(std::string value) {
        if (value.find('\0') != std::string::npos) {
            throw std::invalid_argument(
                "PostgreSQL text parameters must not contain NUL bytes"
            );
        }
        return value;
    }

    std::vector<std::optional<std::string>> values_;
    std::vector<const char*> pointers_;
};

// libpq returns no result, or marks the connection bad, when the server
// cannot be reached. Both are availability failures rather than query errors.
inline PostgresResult checked_result(PGconn* connection, PGresult* result) {
    if (result == nullptr || PQstatus(connection) == CONNECTION_BAD) {
        PQclear(result);
        throw PaperPersistenceUnavailable(
            "PostgreSQL connection is unavailable: " +
            std::string(PQerrorMessage(connection))
        );
    }
    return PostgresResult(result);
}

inline PostgresResult execute(PGconn* connection, std::string_view sql) {
    const std::string statement(sql);
    return checked_result(connection, PQexec(connection, statement.c_str()));
}

inline PostgresResult execute(
    PGconn* connection,
    std::string_view sql,
    PostgresParameters& parameters
) {
    const std::string statement(sql);
    const auto& pointers = parameters.pointers();
    return checked_result(connection, PQexecParams(
        connection,
        statement.c_str(),
        parameters.size(),
        nullptr,
        pointers.data(),
        nullptr,
        nullptr,
        0
    ));
}

inline void require_status(
    const PostgresResult& result,
    ExecStatusType expected,
    std::string_view operation
) {
    if (result.status() != expected) {
        throw std::runtime_error(
            std::string(operation) + " failed: " + result.error_message()
        );
    }
}

class PostgresTransaction final {
public:
    explicit PostgresTransaction(PGconn* connection)
        : connection_(connection) {
        const auto result = execute(connection_, "BEGIN");
        require_status(result, PGRES_COMMAND_OK, "begin transaction");
    }

    // A destructor must not throw, and it may run while an availability
    // exception is propagating. A lost connection has no transaction left to
    // roll back; the next repository operation resets it.
    ~PostgresTransaction() {
        if (!finished_ && PQstatus(connection_) == CONNECTION_OK) {
            PQclear(PQexec(connection_, "ROLLBACK"));
        }
    }

    PostgresTransaction(const PostgresTransaction&) = delete;
    PostgresTransaction& operator=(const PostgresTransaction&) = delete;

    void commit() {
        const auto result = execute(connection_, "COMMIT");
        require_status(result, PGRES_COMMAND_OK, "commit transaction");
        finished_ = true;
    }

private:
    PGconn* connection_;
    bool finished_{false};
};

template <typename Integer>
Integer parse_integer(std::string_view value, std::string_view field) {
    Integer parsed{};
    const auto result = std::from_chars(
        value.data(),
        value.data() + value.size(),
        parsed
    );
    if (result.ec != std::errc{} ||
        result.ptr != value.data() + value.size()) {
        throw std::runtime_error(
            "invalid integer in persisted field: " + std::string(field)
        );
    }
    return parsed;
}

inline bool parse_boolean(std::string_view value, std::string_view field) {
    if (value == "t") {
        return true;
    }
    if (value == "f") {
        return false;
    }
    throw std::runtime_error(
        "invalid boolean in persisted field: " + std::string(field)
    );
}

inline std::optional<std::int64_t> optional_int64(
    const PostgresResult& result,
    int row,
    std::string_view field
) {
    const auto value = result.optional_value(row, field);
    return value ? std::optional<std::int64_t>(
        parse_integer<std::int64_t>(*value, field)
    ) : std::nullopt;
}

inline std::optional<std::uint64_t> optional_uint64(
    const PostgresResult& result,
    int row,
    std::string_view field
) {
    const auto value = result.optional_value(row, field);
    return value ? std::optional<std::uint64_t>(
        parse_integer<std::uint64_t>(*value, field)
    ) : std::nullopt;
}

// The live-stream writer fence (ADR 0020, migration 007). A change that
// records live ingest sequences of a stream asks, in its own transaction,
// whether they belong to the stream's current writer generation: a writer
// holds the stream, and each sequence lies from writer_first_sequence up to
// next_sequence. With `lock`, the stream's row stays share-locked until the
// transaction ends. A takeover, a release, and a reservation update that row,
// so they wait: the change commits before the next generation begins, or
// sees it and is refused. Without `lock` it is a plain read, for a valuation,
// which answers for a generation that was current when it was read.
//
// A stream without a row is not live; replays never create one. A database
// without migration 007 has no generations, and no current server can take
// a live stream there, because acquisition needs them; nothing is refused.
// Whether the column exists is looked up until it is found, then
// remembered: it is never dropped.
class LiveStreamFence final {
public:
    // Why sequences `first` to `last` of `stream` may not be recorded, or
    // nullopt. Throws PaperPersistenceUnavailable when the check itself
    // fails, such as on a lock timeout.
    [[nodiscard]] std::optional<std::string> refusal(
        PGconn* connection,
        std::string_view stream,
        std::uint64_t first,
        std::uint64_t last,
        bool lock
    ) {
        if (first > last) {
            throw std::invalid_argument("a fenced range must not be empty");
        }
        if (!present_) {
            const auto found = execute(
                connection,
                "SELECT EXISTS (SELECT 1 FROM pg_attribute WHERE attrelid = "
                "to_regclass('live_stream_sequences') AND attname = 'writer_first_sequence' "
                "AND NOT attisdropped) AS present"
            );
            checked(found, "look up the live-stream fence");
            if (!parse_boolean(found.value(0, "present"), "present")) {
                return std::nullopt;
            }
            present_ = true;
        }
        PostgresParameters parameters;
        parameters.add(stream);
        parameters.add(first);
        parameters.add(last);
        const auto row = execute(
            connection,
            std::string(
                "SELECT writer_id IS NOT NULL AS held, writer_first_sequence::text AS generation, "
                "next_sequence::text AS next_sequence, "
                "coalesce($2::numeric < writer_first_sequence, false) AS earlier, "
                "(writer_id IS NOT NULL AND $2::numeric >= writer_first_sequence AND "
                "$3::numeric < next_sequence) AS current "
                "FROM live_stream_sequences WHERE stream_id = $1"
            ) + (lock ? " FOR SHARE" : ""),
            parameters
        );
        checked(row, "check the live-stream fence");
        if (row.row_count() == 0) {
            return std::nullopt;
        }
        if (parse_boolean(row.value(0, "current"), "current")) {
            return std::nullopt;
        }
        const std::string range = first == last
            ? "ingest sequence " + std::to_string(first)
            : "ingest sequences " + std::to_string(first) + " to " + std::to_string(last);
        if (!parse_boolean(row.value(0, "held"), "held")) {
            return range + " of live stream " + std::string(stream) +
                " cannot be recorded: no writer holds the stream now";
        }
        const std::string generation = row.optional_value(0, "generation").value_or("an unknown sequence");
        if (parse_boolean(row.value(0, "earlier"), "earlier")) {
            return range + " of live stream " + std::string(stream) +
                " belongs to an earlier writer; the stream's current writer began at " + generation +
                ", so this process no longer writes it";
        }
        return range + " of live stream " + std::string(stream) +
            " was never reserved by its current writer, whose sequences run from " + generation +
            " to below " + row.value(0, "next_sequence");
    }

private:
    static void checked(const PostgresResult& result, std::string_view operation) {
        if (result.status() != PGRES_TUPLES_OK) {
            throw PaperPersistenceUnavailable(
                std::string(operation) + " failed: " + result.error_message()
            );
        }
    }

    bool present_{false};
};

// Resolves a pending deployment decision as recorded, with its order's
// outcome, and counts the order against its run, in the caller's
// transaction (postgresql_model_deployments.cpp). Throws unless exactly one
// pending decision was resolved.
void resolve_deployment_order(
    PGconn* connection,
    std::string_view deployment_id,
    std::int64_t decision_sequence,
    const DeploymentOrderResolution& resolution
);

}  // namespace postgres_support

using postgres_support::PostgresTransaction;

struct PostgresPaperOrderRepository::Impl {
    Impl(
        std::string connection_string,
        std::optional<std::chrono::milliseconds> lock_timeout_
    )
        : connection(PQconnectdb(connection_string.c_str())),
          lock_timeout(lock_timeout_) {
        if (connection == nullptr) {
            throw std::runtime_error(
                "could not allocate PostgreSQL connection"
            );
        }
        if (PQstatus(connection) != CONNECTION_OK) {
            const std::string message = PQerrorMessage(connection);
            PQfinish(connection);
            connection = nullptr;
            throw PaperPersistenceUnavailable(
                "could not connect to PostgreSQL: " + message
            );
        }
        try {
            configure_session();
        } catch (...) {
            PQfinish(connection);
            connection = nullptr;
            throw;
        }
    }

    // Session settings do not survive a reconnect, so they are applied to
    // every new session.
    void configure_session() {
        if (!lock_timeout) {
            return;
        }
        if (lock_timeout->count() <= 0) {
            throw std::invalid_argument("lock timeout must be positive");
        }
        const auto result = postgres_support::execute(
            connection,
            "SET lock_timeout = '" + std::to_string(lock_timeout->count()) +
                "ms'"
        );
        postgres_support::require_status(result, PGRES_COMMAND_OK, "set lock timeout");
    }

    ~Impl() { PQfinish(connection); }

    // A connection lost by an earlier operation is reset once before the next
    // operation starts. The repository's only session state on the
    // connection is prepared statements, which are prepared again after a
    // reset; libpq reconnects with the original parameters.
    void ensure_connected() {
        if (PQstatus(connection) == CONNECTION_OK) {
            return;
        }
        candidates_prepared = false;
        PQreset(connection);
        if (PQstatus(connection) != CONNECTION_OK) {
            throw PaperPersistenceUnavailable(
                "could not reconnect to PostgreSQL: " +
                std::string(PQerrorMessage(connection))
            );
        }
        configure_session();
    }

    PGconn* connection;
    std::optional<std::chrono::milliseconds> lock_timeout;
    // The matcher runs accounts_to_match for every top of book, so it is
    // planned once per connection.
    bool candidates_prepared{false};
    postgres_support::LiveStreamFence fence;
};

struct PaperAccountLock::State {
    State(PGconn* connection_, std::string account_id_, postgres_support::LiveStreamFence& fence_)
        : connection(connection_),
          account_id(std::move(account_id_)),
          fence(fence_),
          transaction(std::in_place, connection_) {}

    PGconn* connection;
    std::string account_id;
    postgres_support::LiveStreamFence& fence;
    // Engaged until the lock commits or is released; resetting it rolls
    // back.
    std::optional<PostgresTransaction> transaction;
    std::optional<PaperOrderProcessor> processor;
    std::optional<PaperOrderProcessorRecoveryState> before;
    std::int64_t version{0};
};

}  // namespace chronos
