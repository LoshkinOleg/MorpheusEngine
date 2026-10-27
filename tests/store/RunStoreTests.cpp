#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <format>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

TEST_CASE("A failed COMMIT does not leave the transaction open", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_failed_commit");
    Database a = Database::Open(file, true);
    a.Exec("CREATE TABLE t (x INTEGER)");
    Database b = Database::Open(file, false);

    // B holds a SHARED lock: a deferred transaction that has read keeps it until COMMIT (rollback journal mode).
    b.Exec("BEGIN; SELECT COUNT(*) FROM t;");

    // A can write into its cache (RESERVED lock) but COMMIT needs an EXCLUSIVE lock, which B blocks.
    // The busy timeout is 0, so COMMIT fails immediately with SQLITE_BUSY.
    CHECK_THROWS_AS(a.Transaction([&] { a.Exec("INSERT INTO t VALUES (1)"); }), StoreError);

    b.Exec("COMMIT");

    // A must be back in autocommit mode: a new transaction starts and commits, and the failed insert is gone.
    a.Transaction([&] { a.Exec("INSERT INTO t VALUES (2)"); });

    Statement rows = a.Prepare("SELECT x FROM t ORDER BY x");
    REQUIRE(rows.Step());
    CHECK(rows.ColumnInt(0) == 2);
    CHECK_FALSE(rows.Step());
}

TEST_CASE("RunStore::Open rejects a corrupt rng_seed", "[store]")
{
    for (const char* badSeed : {"12abc", "-1", "", "99999999999999999999"}) {
        const std::filesystem::path file = FreshDbPath("store_bad_seed");
        std::filesystem::remove(file);
        {
            RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
        }
        {
            Database db = Database::Open(file, false);
            db.Prepare("UPDATE run_meta SET value = ?1 WHERE key = 'rng_seed'").Bind(1, badSeed).Run();
        }
        INFO("seed value: '" << badSeed << "'");
        CHECK_THROWS_AS(RunStore::Open(file), StoreError);
        CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("rng_seed"));
        CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("run.db"));
    }
}

TEST_CASE("RunStore::Open rejects unparseable settings", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_bad_settings");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
    }
    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE run_meta SET value = '{bad' WHERE key = 'settings'");
    }
    CHECK_THROWS_AS(RunStore::Open(file), StoreError);
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("settings"));
    CHECK_THROWS_WITH(RunStore::Open(file), ContainsSubstring("run.db"));
}

TEST_CASE("RunStore::StateAt rejects an unparseable snapshot", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_bad_snapshot");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
    }
    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE state_snapshots SET state = '{bad' WHERE turn = 0");
    }
    RunStore store = RunStore::Open(file);
    CHECK_THROWS_AS(store.StateAt(0), StoreError);
    CHECK_THROWS_WITH(store.StateAt(0), ContainsSubstring("turn 0"));
    CHECK_THROWS_WITH(store.StateAt(0), ContainsSubstring("run.db"));
}

TEST_CASE("Statement::ColumnFloats treats a ragged blob as bad stored data", "[store]")
{
    Database db = Database::Open(FreshDbPath("store_ragged_blob"), true);
    db.Exec("CREATE TABLE v (b BLOB)");
    db.Exec("INSERT INTO v VALUES (x'010203')"); // 3 bytes: not a whole number of floats.

    Statement query = db.Prepare("SELECT b FROM v");
    REQUIRE(query.Step());
    CHECK_THROWS_AS(query.ColumnFloats(0), StoreError);
}

TEST_CASE("A failed RunStore::Create leaves no file behind", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_create_cleanup");
    // A directory where SQLite wants its rollback journal makes the schema transaction fail after the file exists.
    std::filesystem::create_directories(file.parent_path() / "run.db-journal");

    CHECK_THROWS_AS(RunStore::Create(file, TestMeta(), TestInitialState()), StoreError);
    CHECK_FALSE(std::filesystem::exists(file));

    // With the obstacle gone, a retry succeeds instead of reporting "already exists".
    std::filesystem::remove(file.parent_path() / "run.db-journal");
    RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
    CHECK(store.LatestTurn() == 0);
}

namespace {

TurnRecord TestTurn(int number, std::vector<std::string> notes = {})
{
    TurnRecord record;
    record.number = number;
    record.notes = std::move(notes);
    record.input = std::format("input {}", number);
    record.resolve = {{"intent", "climb"}};
    record.rolls = nlohmann::json::array({nlohmann::json{{"total", 14}}});
    record.applied = nlohmann::json::array({nlohmann::json{{"op", "set"}}});
    record.rejected = nlohmann::json::array();
    record.narration = std::format("narration {}", number);
    record.timings = {{"resolve_ms", 120}};
    return record;
}

nlohmann::json StateWithHealth(int health)
{
    return {{"player", {{"type", "character"}, {"fields", {{"health", health}}}}}};
}

} // namespace

TEST_CASE("RunStore commits turns with their resulting state", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_turns"), TestMeta(), TestInitialState());
    store.CommitTurn(TestTurn(1, {"first note", "second note"}), StateWithHealth(9));
    store.CommitTurn(TestTurn(2), StateWithHealth(8));

    CHECK(store.LatestTurn() == 2);
    CHECK(store.StateAt(1) == StateWithHealth(9));
    CHECK(store.StateAt(2) == StateWithHealth(8));

    const std::optional<TurnRecord> first = store.Turn(1);
    REQUIRE(first.has_value());
    CHECK(first->input == "input 1");
    CHECK(first->resolve == nlohmann::json({{"intent", "climb"}}));
    CHECK(first->rolls.at(0).at("total") == 14);
    CHECK(first->applied.size() == 1);
    CHECK(first->rejected.empty());
    CHECK(first->narration == "narration 1");
    CHECK(first->timings.at("resolve_ms") == 120);
    CHECK(first->notes == std::vector<std::string>{"first note", "second note"});

    const std::vector<TurnRecord> both = store.Turns(1, 2);
    REQUIRE(both.size() == 2);
    CHECK(both[0].number == 1);
    CHECK(both[0].notes == std::vector<std::string>{"first note", "second note"});
    CHECK(both[1].number == 2);
    CHECK(both[1].notes.empty());
    CHECK_FALSE(store.Turn(3).has_value());
}

TEST_CASE("RunStore only accepts the next turn number", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_turn_order"), TestMeta(), TestInitialState());
    CHECK_THROWS_AS(store.CommitTurn(TestTurn(2), StateWithHealth(9)), ContractError);
    CHECK(store.LatestTurn() == 0);

    store.CommitTurn(TestTurn(1), StateWithHealth(9));
    CHECK_THROWS_AS(store.CommitTurn(TestTurn(1), StateWithHealth(9)), ContractError);
    CHECK(store.LatestTurn() == 1);
}

TEST_CASE("A failed CommitTurn leaves no notes behind", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_failed_commit_notes"), TestMeta(), TestInitialState());

    // Wrong turn number: the commit throws and the note written for that turn must not exist.
    CHECK_THROWS_AS(store.CommitTurn(TestTurn(2, {"orphan note"}), StateWithHealth(9)), ContractError);
    CHECK(store.Notes().empty());

    store.CommitTurn(TestTurn(1, {"real note"}), StateWithHealth(9));
    CHECK_THROWS_AS(store.CommitTurn(TestTurn(1, {"duplicate note"}), StateWithHealth(8)), ContractError);
    REQUIRE(store.Notes().size() == 1);
    CHECK(store.Notes()[0].text == "real note");
}

TEST_CASE("TruncateAfter undoes later turns and snapshots", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_truncate"), TestMeta(), TestInitialState());
    store.CommitTurn(TestTurn(1), StateWithHealth(9));
    store.CommitTurn(TestTurn(2), StateWithHealth(8));
    store.CommitTurn(TestTurn(3), StateWithHealth(7));

    store.TruncateAfter(1);
    CHECK(store.LatestTurn() == 1);
    CHECK_FALSE(store.Turn(2).has_value());
    CHECK_THROWS_AS(store.StateAt(2), ContractError);
    CHECK(store.StateAt(1) == StateWithHealth(9));

    // The run continues from turn 1 as if turns 2 and 3 never happened.
    store.CommitTurn(TestTurn(2), StateWithHealth(5));
    CHECK(store.StateAt(2) == StateWithHealth(5));
}

TEST_CASE("RunStore::Turn rejects an unparseable stored column", "[store]")
{
    const std::filesystem::path file = FreshDbPath("store_bad_turn");
    {
        RunStore store = RunStore::Create(file, TestMeta(), TestInitialState());
        store.CommitTurn(TestTurn(1), StateWithHealth(9));
    }
    {
        Database db = Database::Open(file, false);
        db.Exec("UPDATE turns SET resolve = '{bad' WHERE number = 1");
    }
    RunStore store = RunStore::Open(file);
    CHECK_THROWS_AS(store.Turn(1), StoreError);
    CHECK_THROWS_WITH(store.Turn(1), ContainsSubstring("resolve"));
    CHECK_THROWS_WITH(store.Turn(1), ContainsSubstring("turn 1"));
    CHECK_THROWS_WITH(store.Turn(1), ContainsSubstring("run.db"));
}

TEST_CASE("RunStore keeps summaries and notes in order", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_summaries"), TestMeta(), TestInitialState());
    store.AddSummary({5, 8, "second fold"});
    store.AddSummary({1, 4, "first fold"});
    store.CommitTurn(TestTurn(1, {"storm season"}), StateWithHealth(9));
    store.CommitTurn(TestTurn(2, {"the captain distrusts Marta", "Marta limps"}), StateWithHealth(8));

    const std::vector<Summary> summaries = store.Summaries();
    REQUIRE(summaries.size() == 2);
    CHECK(summaries[0].text == "first fold");
    CHECK(summaries[1].fromTurn == 5);
    CHECK(summaries[1].toTurn == 8);

    const std::vector<Note> notes = store.Notes();
    REQUIRE(notes.size() == 3);
    CHECK(notes[0].turn == 1);
    CHECK(notes[0].text == "storm season");
    CHECK(notes[1].turn == 2);
    CHECK(notes[1].text == "the captain distrusts Marta");
    CHECK(notes[2].text == "Marta limps");

    CHECK_THROWS_AS(store.AddSummary({4, 2, "backwards"}), ContractError);
}

TEST_CASE("RunStore stores embeddings per kind and model", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_embeddings"), TestMeta(), TestInitialState());
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-a", {0.25f, -1.5f, 3.0f}});
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-a", {1.0f}}); // Replaces the previous vector.
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-b", {2.0f}});
    store.PutEmbedding({EmbeddingKind::Turn, "1", "embed-a", {0.5f, -0.125f}});

    const std::vector<EmbeddingRow> lore = store.Embeddings(EmbeddingKind::Lore, "embed-a");
    REQUIRE(lore.size() == 1);
    CHECK(lore[0].refId == "boarding");
    CHECK(lore[0].vector == std::vector<float>{1.0f});

    const std::vector<EmbeddingRow> turns = store.Embeddings(EmbeddingKind::Turn, "embed-a");
    REQUIRE(turns.size() == 1);
    CHECK(turns[0].kind == EmbeddingKind::Turn);
    CHECK(turns[0].vector == std::vector<float>{0.5f, -0.125f});

    CHECK_THROWS_AS(store.PutEmbedding({EmbeddingKind::Turn, "2", "embed-a", {}}), ContractError);
}

TEST_CASE("TruncateAfter also undoes later summaries, notes and turn embeddings", "[store]")
{
    RunStore store = RunStore::Create(FreshDbPath("store_truncate_memory"), TestMeta(), TestInitialState());
    for (int turn = 1; turn <= 5; ++turn) {
        std::vector<std::string> notes;
        if (turn == 3) {
            notes.push_back("kept");
        } else if (turn == 4) {
            notes.push_back("dropped");
        }
        store.CommitTurn(TestTurn(turn, notes), StateWithHealth(10 - turn));
    }
    store.AddSummary({1, 3, "kept"});
    store.AddSummary({4, 5, "dropped"});
    store.PutEmbedding({EmbeddingKind::Turn, "3", "embed-a", {1.0f}});
    store.PutEmbedding({EmbeddingKind::Turn, "4", "embed-a", {1.0f}});
    store.PutEmbedding({EmbeddingKind::Lore, "boarding", "embed-a", {1.0f}});

    store.TruncateAfter(3);

    REQUIRE(store.Summaries().size() == 1);
    CHECK(store.Summaries()[0].text == "kept");
    REQUIRE(store.Notes().size() == 1);
    CHECK(store.Notes()[0].text == "kept");

    std::set<std::string> turnRefs;
    for (const EmbeddingRow& row : store.Embeddings(EmbeddingKind::Turn, "embed-a")) {
        turnRefs.insert(row.refId);
    }
    CHECK(turnRefs == std::set<std::string>{"3"});
    CHECK(store.Embeddings(EmbeddingKind::Lore, "embed-a").size() == 1);
}
