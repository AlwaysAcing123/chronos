#pragma once

#include "test_support.hpp"

#include <libpq-fe.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace chronos::test {

// A raw libpq connection used by integration tests to observe and interfere
// with the connection under test independently of the repository.
class PostgresTestConnection final {
public:
    explicit PostgresTestConnection(const std::string& connection_string)
        : connection_(PQconnectdb(connection_string.c_str())) {
        if (connection_ == nullptr || PQstatus(connection_) != CONNECTION_OK) {
            const std::string message = connection_ == nullptr
                ? "allocation failed"
                : PQerrorMessage(connection_);
            PQfinish(connection_);
            throw TestFailure("test connection failed: " + message);
        }
    }

    ~PostgresTestConnection() { PQfinish(connection_); }

    PostgresTestConnection(const PostgresTestConnection&) = delete;
    PostgresTestConnection& operator=(const PostgresTestConnection&) = delete;

    // Executes one statement and returns its rows as text, NULL as "".
    std::vector<std::vector<std::string>> query(
        std::string_view sql,
        std::initializer_list<std::string> parameters = {}
    ) {
        const std::string statement(sql);
        std::vector<const char*> values;
        values.reserve(parameters.size());
        for (const auto& parameter : parameters) {
            values.push_back(parameter.c_str());
        }
        PGresult* result = PQexecParams(
            connection_,
            statement.c_str(),
            static_cast<int>(values.size()),
            nullptr,
            values.data(),
            nullptr,
            nullptr,
            0
        );
        const ExecStatusType status = PQresultStatus(result);
        if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
            const std::string message = PQresultErrorMessage(result);
            PQclear(result);
            throw TestFailure("test query failed: " + message);
        }
        std::vector<std::vector<std::string>> rows;
        for (int row = 0; row < PQntuples(result); ++row) {
            auto& columns = rows.emplace_back();
            for (int column = 0; column < PQnfields(result); ++column) {
                columns.emplace_back(PQgetvalue(result, row, column));
            }
        }
        PQclear(result);
        return rows;
    }

    std::string scalar(
        std::string_view sql,
        std::initializer_list<std::string> parameters = {}
    ) {
        const auto rows = query(sql, parameters);
        if (rows.size() != 1 || rows.front().size() != 1) {
            throw TestFailure("test query did not return one value");
        }
        return rows.front().front();
    }

private:
    PGconn* connection_;
};

inline std::string with_application_name(
    std::string connection_string,
    std::string_view application_name
) {
    const bool is_uri = connection_string.starts_with("postgresql://") ||
        connection_string.starts_with("postgres://");
    if (is_uri) {
        connection_string +=
            connection_string.find('?') == std::string::npos ? '?' : '&';
        connection_string += "application_name=";
    } else {
        connection_string += " application_name=";
    }
    connection_string += application_name;
    return connection_string;
}

inline std::string unique_test_id(std::string_view prefix) {
    static std::atomic<std::uint64_t> counter{0};
    const auto now = std::chrono::system_clock::now()
        .time_since_epoch()
        .count();
    return std::string(prefix) + "-" + std::to_string(now) + "-" +
        std::to_string(counter.fetch_add(1));
}

// Every migration in lexical file order, as a fresh Compose volume applies
// them. Each file is idempotent and owns its transaction.
inline std::string all_migrations_sql() {
    const std::filesystem::path directory =
        std::filesystem::path(CHRONOS_SOURCE_DIR) / "migrations";
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().extension() == ".sql") {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    if (files.empty()) {
        throw TestFailure("no migrations found in " + directory.string());
    }

    std::ostringstream contents;
    for (const auto& file : files) {
        std::ifstream input(file);
        if (!input) {
            throw TestFailure("could not open migration: " + file.string());
        }
        contents << input.rdbuf() << '\n';
    }
    return contents.str();
}

template <typename Condition>
void wait_until(Condition condition, std::string_view description) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            throw TestFailure(
                "timed out waiting for " + std::string(description)
            );
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// Blocks until a backend with the given application name is waiting for a
// heavyweight lock, which makes lock-interleaving tests deterministic.
inline void wait_until_blocked_on_lock(
    PostgresTestConnection& observer,
    const std::string& application_name
) {
    wait_until(
        [&] {
            return observer.scalar(
                "SELECT count(*) FROM pg_stat_activity "
                "WHERE application_name = $1 AND wait_event_type = 'Lock'",
                {application_name}
            ) == "1";
        },
        "a backend to block on a row lock"
    );
}

// Terminates every backend with the given application name and waits until
// the server has closed them, so the next client operation observes the loss.
inline void terminate_backends(
    PostgresTestConnection& observer,
    const std::string& application_name
) {
    static_cast<void>(observer.query(
        "SELECT pg_terminate_backend(pid) FROM pg_stat_activity "
        "WHERE application_name = $1",
        {application_name}
    ));
    wait_until(
        [&] {
            return observer.scalar(
                "SELECT count(*) FROM pg_stat_activity "
                "WHERE application_name = $1",
                {application_name}
            ) == "0";
        },
        "terminated backends to exit"
    );
}

}  // namespace chronos::test
