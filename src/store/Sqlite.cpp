#include "store/Sqlite.h"

#include <cstring>
#include <format>
#include <utility>

#include <sqlite3.h>

#include "core/Errors.h"

namespace morpheus::store {

// ---- Statement ----------------------------------------------------------

Statement::Statement(sqlite3* pDb, std::string_view sql)
    : _pDb(pDb)
{
    MORPHEUS_REQUIRE(pDb != nullptr, "Statement: database is closed or moved-from");
    if (sqlite3_prepare_v2(pDb, sql.data(), static_cast<int>(sql.size()), &_pStmt, nullptr) != SQLITE_OK) {
        throw StoreError(std::format("cannot prepare SQL: {} | {}", sqlite3_errmsg(pDb), sql));
    }
}

Statement::Statement(Statement&& other) noexcept
    : _pDb(other._pDb)
    , _pStmt(std::exchange(other._pStmt, nullptr))
{
}

Statement::~Statement()
{
    sqlite3_finalize(_pStmt); // Harmless no-op on nullptr.
}

Statement& Statement::Bind(int index, std::int64_t value)
{
    Check(sqlite3_bind_int64(_pStmt, index, value));
    return *this;
}

Statement& Statement::Bind(int index, std::string_view text)
{
    // A default-constructed string_view has a null data pointer, which SQLite would store as NULL, not "".
    const char* pText = text.data() != nullptr ? text.data() : "";
    Check(sqlite3_bind_text(_pStmt, index, pText, static_cast<int>(text.size()), SQLITE_TRANSIENT));
    return *this;
}

Statement& Statement::Bind(int index, const std::vector<float>& values)
{
    MORPHEUS_REQUIRE(!values.empty(), "Statement::Bind: float blob must not be empty");
    Check(sqlite3_bind_blob(_pStmt, index, values.data(), static_cast<int>(values.size() * sizeof(float)),
                            SQLITE_TRANSIENT));
    return *this;
}

bool Statement::Step()
{
    const int rc = sqlite3_step(_pStmt);
    if (rc == SQLITE_ROW) {
        return true;
    }
    if (rc == SQLITE_DONE) {
        return false;
    }
    throw StoreError(std::format("SQL step failed: {}", sqlite3_errmsg(_pDb)));
}

void Statement::Run()
{
    const bool hasRow = Step();
    MORPHEUS_REQUIRE(!hasRow, "Statement::Run used on a statement that returns rows");
}

std::int64_t Statement::ColumnInt(int index) const
{
    return sqlite3_column_int64(_pStmt, index);
}

std::string Statement::ColumnText(int index) const
{
    const unsigned char* pText = sqlite3_column_text(_pStmt, index);
    const int size = sqlite3_column_bytes(_pStmt, index);
    if (pText == nullptr) {
        return {};
    }
    return std::string(reinterpret_cast<const char*>(pText), static_cast<std::size_t>(size));
}

std::vector<float> Statement::ColumnFloats(int index) const
{
    // SQLite requires reading the blob pointer before its size.
    const void* pBlob = sqlite3_column_blob(_pStmt, index);
    const int size = sqlite3_column_bytes(_pStmt, index);
    // A ragged size means the stored data is damaged, not that the engine misused the API.
    if (size % static_cast<int>(sizeof(float)) != 0) {
        throw StoreError(std::format("stored vector blob is {} bytes, not a whole number of floats", size));
    }

    std::vector<float> values(static_cast<std::size_t>(size) / sizeof(float));
    if (size > 0) {
        std::memcpy(values.data(), pBlob, static_cast<std::size_t>(size));
    }
    return values;
}

void Statement::Check(int rc) const
{
    if (rc != SQLITE_OK) {
        throw StoreError(std::format("SQL bind failed: {}", sqlite3_errmsg(_pDb)));
    }
}

// ---- Database -----------------------------------------------------------

Database Database::Open(const std::filesystem::path& file, bool create)
{
    sqlite3* pDb = nullptr;
    const int flags = SQLITE_OPEN_READWRITE | (create ? SQLITE_OPEN_CREATE : 0);
    // SQLite expects UTF-8 file names; u8string keeps non-ASCII Windows paths intact.
    const std::u8string utf8Path = file.u8string();
    const int rc = sqlite3_open_v2(reinterpret_cast<const char*>(utf8Path.c_str()), &pDb, flags, nullptr);
    if (rc != SQLITE_OK) {
        const std::string message = pDb != nullptr ? sqlite3_errmsg(pDb) : sqlite3_errstr(rc);
        sqlite3_close(pDb);
        throw StoreError(std::format("cannot open '{}': {}", file.generic_string(), message));
    }
    return Database(pDb);
}

Database::Database(sqlite3* pDb)
    : _pDb(pDb)
{
}

Database::Database(Database&& other) noexcept
    : _pDb(std::exchange(other._pDb, nullptr))
{
}

Database& Database::operator=(Database&& other) noexcept
{
    if (this != &other) {
        sqlite3_close(_pDb);
        _pDb = std::exchange(other._pDb, nullptr);
    }
    return *this;
}

Database::~Database()
{
    sqlite3_close(_pDb); // Harmless no-op on nullptr.
}

void Database::Exec(std::string_view sql)
{
    MORPHEUS_REQUIRE(_pDb != nullptr, "Database::Exec on a closed or moved-from database");
    const std::string text(sql); // sqlite3_exec needs a NUL-terminated string.
    char* pError = nullptr;
    if (sqlite3_exec(_pDb, text.c_str(), nullptr, nullptr, &pError) != SQLITE_OK) {
        const std::string message = pError != nullptr ? pError : "unknown error";
        sqlite3_free(pError);
        throw StoreError(std::format("SQL failed: {} | {}", message, text));
    }
}

Statement Database::Prepare(std::string_view sql)
{
    return Statement(_pDb, sql);
}

void Database::RollbackNoThrow() noexcept
{
    sqlite3_exec(_pDb, "ROLLBACK", nullptr, nullptr, nullptr);
}

} // namespace morpheus::store
