### Task 8: SQLite wrapper and run database creation

**Files:**
- Modify: `CMakeLists.txt` (SQLite dependency, `src/store` subdirectory)
- Modify: `tests/CMakeLists.txt` (link `morpheus_store`)
- Create: `src/store/CMakeLists.txt`, `src/store/Sqlite.h`, `src/store/Sqlite.cpp`, `src/store/RunStore.h`, `src/store/RunStore.cpp`, `tests/store/RunStoreTests.cpp`

**Interfaces:**
- Consumes: `MORPHEUS_REQUIRE`, `ContractError` (Task 1).
- Produces (namespace `morpheus::store`):
  - `class StoreError : std::runtime_error`
  - `class Database`:
    - `static Database Open(const std::filesystem::path&, bool create)`
    - `void Exec(std::string_view)`
    - `Statement Prepare(std::string_view)`
    - `template<class F> void Transaction(F&&)`
  - `class Statement`:
    - `Bind(int, std::int64_t)`, `Bind(int, std::string_view)`, `Bind(int, const std::vector<float>&)` (each returns `Statement&`)
    - `bool Step()`, `void Run()`
    - `std::int64_t ColumnInt(int) const`, `std::string ColumnText(int) const`, `std::vector<float> ColumnFloats(int) const`
  - `struct RunMeta { std::string worldId, worldHash, modelId; std::uint64_t rngSeed; nlohmann::json settings; std::string createdAt; }`
  - `class RunStore`:
    - `static constexpr int SCHEMA_VERSION = 1`
    - `static RunStore Create(const std::filesystem::path&, RunMeta, const nlohmann::json& initialState)`
    - `static RunStore Open(const std::filesystem::path&)`
    - `const RunMeta& Meta() const`
    - `int LatestTurn()`
    - `nlohmann::json StateAt(int turn)`

- [ ] **Step 1: Add the SQLite dependency to `CMakeLists.txt`**

Insert this block directly after `FetchContent_MakeAvailable(nlohmann)`:

```cmake

# SQLite amalgamation (store). The archive has no CMakeLists.txt, so the library target is defined here.
FetchContent_Declare(
    sqlite
    URL      https://www.sqlite.org/2025/sqlite-amalgamation-3500400.zip   # SQLite 3.50.4
    URL_HASH SHA256=1d3049dd0f830a025a53105fc79fd2ab9431aea99e137809d064d8ee8356b032
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite)
add_library(sqlite3 STATIC ${sqlite_SOURCE_DIR}/sqlite3.c)
target_include_directories(sqlite3 SYSTEM PUBLIC ${sqlite_SOURCE_DIR})
target_compile_definitions(sqlite3 PRIVATE SQLITE_OMIT_LOAD_EXTENSION SQLITE_DQS=0)
```

Replace the line `add_subdirectory(src/core)` with:

```cmake
add_subdirectory(src/core)
add_subdirectory(src/store)
```

In `tests/CMakeLists.txt`, replace `target_link_libraries(morpheus_tests PRIVATE morpheus_core Catch2::Catch2WithMain)` with:

```cmake
target_link_libraries(morpheus_tests PRIVATE morpheus_core morpheus_store Catch2::Catch2WithMain)
```

Create `src/store/CMakeLists.txt`:

```cmake
add_library(morpheus_store STATIC
    Sqlite.cpp
    RunStore.cpp
)
target_include_directories(morpheus_store PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_store
    PUBLIC morpheus_core
    PRIVATE sqlite3
)
morpheus_target_defaults(morpheus_store)
```

- [ ] **Step 2: Write the failing test `tests/store/RunStoreTests.cpp`**

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <stdexcept>
#include <string_view>

#include "TestPaths.h"
#include "core/Errors.h"
#include "store/RunStore.h"
#include "store/Sqlite.h"

using namespace morpheus::store;
using morpheus::core::ContractError;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::Matches;

namespace {

RunMeta TestMeta()
{
    RunMeta meta;
    meta.worldId = "mini";
    meta.worldHash = "0123456789abcdef";
    meta.modelId = "test-model.gguf";
    meta.rngSeed = 18446744073709551615ULL; // Max value: proves the seed survives storage without sign issues.
    meta.settings = {{"n_ctx", 8192}};
    return meta;
}

nlohmann::json TestInitialState()
{
    return nlohmann::json::parse(R"({"player": {"type": "character", "fields": {"health": 10}}})");
}

std::filesystem::path FreshDbPath(std::string_view testName)
{
    return morpheus::test::FreshTempDir(testName) / "run.db";
}

} // namespace

TEST_CASE("RunStore::Create stores meta and the turn-0 snapshot", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_create");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
        CHECK(store.Meta().worldId == "mini");
        CHECK_THAT(store.Meta().createdAt, Matches(R"(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z)"));
        CHECK(store.LatestTurn() == 0);
        CHECK(store.StateAt(0) == TestInitialState());
    }

    RunStore reopened = RunStore::Open(file);
    CHECK(reopened.Meta().worldHash == "0123456789abcdef");
    CHECK(reopened.Meta().modelId == "test-model.gguf");
    CHECK(reopened.Meta().rngSeed == 18446744073709551615ULL);
    CHECK(reopened.Meta().settings == nlohmann::json({{"n_ctx", 8192}}));
    CHECK(reopened.StateAt(0) == TestInitialState());
    CHECK_THROWS_AS(reopened.StateAt(1), ContractError);
}

TEST_CASE("RunStore refuses to overwrite a run or open the wrong file", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_refuse");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
    }
    CHECK_THROWS_AS(RunStore::Create(file, TestMeta(), TestInitialState()), StoreError);
    CHECK_THROWS_AS(RunStore::Open(file.parent_path() / "missing.db"), StoreError);

    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE run_meta SET value = '999' WHERE key = 'schema_version'");
    }
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("schema version"));
}

TEST_CASE("Database transactions roll back when the body throws", "[store]")
{
    Database db = Database::Open(FreshDbPath("store_transaction"), true);
    db.Exec("CREATE TABLE t (x INTEGER)");

    // The condition keeps the compiler from treating the commit path as unreachable code.
    bool fail = true;
    CHECK_THROWS_AS(db.Transaction([&] {
        db.Exec("INSERT INTO t VALUES (1)");
        if (fail) {
            throw std::runtime_error("boom");
        }
    }), std::runtime_error);

    Statement count = db.Prepare("SELECT COUNT(*) FROM t");
    REQUIRE(count.Step());
    CHECK(count.ColumnInt(0) == 0);
}
```

- [ ] **Step 3: Configure and build. Expect failure.**

Run: `cmake --preset tests`
Expected: the SQLite archive downloads and passes its SHA256 check, then configuration FAILS with `Cannot find source file: Sqlite.cpp` (from `src/store/CMakeLists.txt`).

- [ ] **Step 4: Implement `src/store/Sqlite.h`**

```cpp
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
```

- [ ] **Step 5: Implement `src/store/Sqlite.cpp`**

```cpp
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
    MORPHEUS_REQUIRE(size % static_cast<int>(sizeof(float)) == 0, "ColumnFloats: blob is not a whole number of floats");

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
```

- [ ] **Step 6: Implement `src/store/RunStore.h`**

```cpp
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <nlohmann/json.hpp>

#include "store/Sqlite.h"

namespace morpheus::store {

struct RunMeta {
    std::string worldId = {};
    std::string worldHash = {}; // World content hash at run creation.
    std::string modelId = {};
    std::uint64_t rngSeed = 0;
    nlohmann::json settings = nlohmann::json::object();
    std::string createdAt = {}; // ISO-8601 UTC, set by RunStore::Create.
};

// One run's SQLite database (saves/<world>/<run>.db). The only writer of run data.
// Tables: run_meta, turns, state_snapshots, summaries, notes, embeddings.
class RunStore {
public:
    static constexpr int SCHEMA_VERSION = 1;

private:
    Database _db;
    RunMeta _meta = {};

public:
    // Creates a new run database holding the meta and the turn-0 snapshot. Throws StoreError if the file exists.
    static RunStore Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState);

    // Opens an existing run. Throws StoreError if it is missing or was written with another schema version.
    static RunStore Open(const std::filesystem::path& file);

    const RunMeta& Meta() const;

    // Highest committed turn number, 0 before the first turn.
    int LatestTurn();

    // State after `turn` (0 = initial state). Throws ContractError if there is no snapshot for it.
    nlohmann::json StateAt(int turn);

private:
    RunStore(Database db, RunMeta meta);
};

} // namespace morpheus::store
```

- [ ] **Step 7: Implement `src/store/RunStore.cpp`**

```cpp
#include "store/RunStore.h"

#include <array>
#include <chrono>
#include <format>
#include <map>
#include <utility>

#include "core/Errors.h"

namespace morpheus::store {

namespace {

constexpr const char* SCHEMA_SQL = R"sql(
CREATE TABLE run_meta (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
CREATE TABLE turns (
    number    INTEGER PRIMARY KEY,
    input     TEXT NOT NULL,
    resolve   TEXT NOT NULL,
    rolls     TEXT NOT NULL,
    applied   TEXT NOT NULL,
    rejected  TEXT NOT NULL,
    narration TEXT NOT NULL,
    timings   TEXT NOT NULL
);
CREATE TABLE state_snapshots (
    turn  INTEGER PRIMARY KEY,
    state TEXT NOT NULL
);
CREATE TABLE summaries (
    id        INTEGER PRIMARY KEY AUTOINCREMENT,
    from_turn INTEGER NOT NULL,
    to_turn   INTEGER NOT NULL,
    text      TEXT NOT NULL
);
CREATE TABLE notes (
    id   INTEGER PRIMARY KEY AUTOINCREMENT,
    turn INTEGER NOT NULL,
    text TEXT NOT NULL
);
CREATE TABLE embeddings (
    kind   TEXT NOT NULL,
    ref_id TEXT NOT NULL,
    model  TEXT NOT NULL,
    vector BLOB NOT NULL,
    PRIMARY KEY (kind, ref_id, model)
);
)sql";

} // namespace

RunStore RunStore::Create(const std::filesystem::path& file, RunMeta meta, const nlohmann::json& initialState)
{
    if (std::filesystem::exists(file)) {
        throw StoreError(std::format("run database '{}' already exists", file.generic_string()));
    }
    if (file.has_parent_path()) {
        std::filesystem::create_directories(file.parent_path());
    }
    meta.createdAt = std::format("{:%Y-%m-%dT%H:%M:%SZ}",
                                 std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()));

    // Schema, meta and the turn-0 snapshot land together or not at all.
    Database db = Database::Open(file, true);
    db.Transaction([&] {
        db.Exec(SCHEMA_SQL);

        const std::array<std::pair<const char*, std::string>, 7> metaRows = {{
            {"schema_version", std::to_string(SCHEMA_VERSION)},
            {"world_id", meta.worldId},
            {"world_hash", meta.worldHash},
            {"model_id", meta.modelId},
            {"rng_seed", std::to_string(meta.rngSeed)}, // Text: SQLite integers are signed 64-bit.
            {"settings", meta.settings.dump()},
            {"created_at", meta.createdAt},
        }};
        for (const auto& [key, value] : metaRows) {
            db.Prepare("INSERT INTO run_meta (key, value) VALUES (?1, ?2)").Bind(1, key).Bind(2, value).Run();
        }

        db.Prepare("INSERT INTO state_snapshots (turn, state) VALUES (0, ?1)").Bind(1, initialState.dump()).Run();
    });
    return RunStore(std::move(db), std::move(meta));
}

RunStore RunStore::Open(const std::filesystem::path& file)
{
    if (!std::filesystem::exists(file)) {
        throw StoreError(std::format("run database '{}' does not exist", file.generic_string()));
    }
    Database db = Database::Open(file, false);

    // Read the key/value meta table in one pass.
    std::map<std::string, std::string> values;
    {
        Statement query = db.Prepare("SELECT key, value FROM run_meta");
        while (query.Step()) {
            values.emplace(query.ColumnText(0), query.ColumnText(1));
        }
    }
    auto value = [&](const char* key) -> const std::string& {
        const auto it = values.find(key);
        if (it == values.end()) {
            throw StoreError(std::format("'{}': run_meta has no '{}'", file.generic_string(), key));
        }
        return it->second;
    };

    if (value("schema_version") != std::to_string(SCHEMA_VERSION)) {
        throw StoreError(std::format("'{}': schema version {} is not the supported version {}",
                                     file.generic_string(), value("schema_version"), SCHEMA_VERSION));
    }

    RunMeta meta;
    meta.worldId = value("world_id");
    meta.worldHash = value("world_hash");
    meta.modelId = value("model_id");
    meta.rngSeed = std::stoull(value("rng_seed"));
    meta.settings = nlohmann::json::parse(value("settings"));
    meta.createdAt = value("created_at");
    return RunStore(std::move(db), std::move(meta));
}

const RunMeta& RunStore::Meta() const
{
    return _meta;
}

int RunStore::LatestTurn()
{
    Statement query = _db.Prepare("SELECT COALESCE(MAX(number), 0) FROM turns");
    const bool hasRow = query.Step();
    MORPHEUS_REQUIRE(hasRow, "LatestTurn: aggregate query returned no row");
    return static_cast<int>(query.ColumnInt(0));
}

nlohmann::json RunStore::StateAt(int turn)
{
    Statement query = _db.Prepare("SELECT state FROM state_snapshots WHERE turn = ?1");
    query.Bind(1, turn);
    const bool found = query.Step();
    MORPHEUS_REQUIRE(found, std::format("StateAt: no snapshot for turn {}", turn));
    return nlohmann::json::parse(query.ColumnText(0));
}

RunStore::RunStore(Database db, RunMeta meta)
    : _db(std::move(db))
    , _meta(std::move(meta))
{
}

} // namespace morpheus::store
```

- [ ] **Step 8: Build and run the tests. Expect them to pass.**

Run: `cmake --preset tests && cmake --build --preset tests && ctest --preset tests`
Expected: all tests pass, including the 3 `[store]` tests.

- [ ] **Step 9: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt src/store tests/store
git commit -m "Add SQLite wrapper and run database creation" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

---

