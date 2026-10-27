#pragma once

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

struct sqlite3;
struct sqlite3_stmt;

namespace morpheus::store {

// Thrown when SQLite fails or a run database is missing, already exists, or has the wrong schema version.
class StoreError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// One prepared statement. Bind parameters (1-based), then Step() through rows or Run() a statement without rows.
class Statement {
private:
    sqlite3* _pDb = nullptr;
    sqlite3_stmt* _pStmt = nullptr;

public:
    Statement(sqlite3* pDb, std::string_view sql);
    Statement(Statement&& other) noexcept;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement& operator=(Statement&&) = delete;
    ~Statement();

    Statement& Bind(int index, std::int64_t value);
    Statement& Bind(int index, std::string_view text);
    // Stored as the raw bytes of the floats (little-endian on every supported platform). Must not be empty.
    Statement& Bind(int index, const std::vector<float>& values);

    // True when a row is available; false when the statement finished. Throws StoreError on failure.
    bool Step();

    // Executes a statement that must not return rows (INSERT, UPDATE, DELETE).
    void Run();

    std::int64_t ColumnInt(int index) const;
    std::string ColumnText(int index) const;
    std::vector<float> ColumnFloats(int index) const;

private:
    // Throws StoreError with SQLite's message unless rc is SQLITE_OK.
    void Check(int rc) const;
};

// Owns one SQLite connection.
class Database {
private:
    sqlite3* _pDb = nullptr;

public:
    // Opens a database file read-write; `create` allows creating it. Throws StoreError on failure.
    static Database Open(const std::filesystem::path& file, bool create);

    Database(Database&& other) noexcept;
    Database& operator=(Database&& other) noexcept;
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;
    ~Database();

    // Runs one or more SQL statements that return no rows.
    void Exec(std::string_view sql);

    Statement Prepare(std::string_view sql);

    // Runs body() between BEGIN IMMEDIATE and COMMIT. If body throws, rolls back and rethrows.
    template <class F>
    void Transaction(F&& body)
    {
        Exec("BEGIN IMMEDIATE");
        try {
            body();
        } catch (...) {
            RollbackNoThrow();
            throw;
        }
        Exec("COMMIT");
    }

private:
    explicit Database(sqlite3* pDb);

    // Used on the error path of Transaction, where a second exception would hide the first.
    void RollbackNoThrow() noexcept;
};

} // namespace morpheus::store
