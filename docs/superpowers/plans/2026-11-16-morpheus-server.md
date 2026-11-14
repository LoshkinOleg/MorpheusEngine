# morpheus_server (sub-project D2) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `morpheus_server`, a localhost HTTP server that holds the Mantella host's hosted runs, plays their turns through `turn::HostedSession` with each turn's action and spoken line streamed as Server-Sent Events, keeps their memory folded in the background, and reaches the models through llama-server only.

**Architecture:** A new static library `src/server` (`morpheus_server_lib`) in three layers: run workers (one thread per run owning its `HostedSession` and its own `LlamaServerBackend`, with a shared `SlotPool` giving each conversation a llama-server slot), a transport-free `Api` (method, path and body in; status and JSON or a `TurnStream` out), and an `HttpServer` adapter on cpp-httplib. A thin executable `tools/server` (`morpheus_server`) wires them to settings.json and signals. Two small engine changes come first: the event overflow cure in `HostedSession` (stored in a new `hosted_turns.events_omitted` column, store version 4) and `LlamaServerBackend::SlotCount()` / `ChatModelId()`.

**Tech Stack:** C++20, CMake >= 3.25 (Visual Studio 2022 on Windows, clang + Ninja on Linux), cpp-httplib v0.59.0 (header-only, `morpheus_httplib`), nlohmann::json, SQLite (`store`), Catch2 v3.

**Spec:** `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`. Read it before Task 1.

## Global Constraints

- Every response, error or not, carries `"api_version": "1.0.0"`; for a stream, in its first event.
- The server listens on 127.0.0.1 only; port 4998 by default (`--port`); `--runs` defaults to `./runs`; `--settings` defaults to `./settings.json`.
- `run_id` is 1 to 64 characters of `[A-Za-z0-9_-]`. A run's file is `<runs dir>/<run_id>.morpheus`.
- Slot 0 is the upkeep slot (`Maintain`: folding, condensing). Conversations get the other slots, least recently used first. With one slot, upkeep and turns share slot 0.
- Statuses: 400 for `core::DataError`, a bad `run_id`, a malformed body or field, and `speaker: "decide"`; 404 for an unknown route or an unknown conversation of an opened run; 409 for a run not opened in this server process; 422 for `turn::TurnError`; 503 for `inference::InferenceError`; 500 for anything else.
- An error body is `{"api_version": "1.0.0", "error": {"status": <int>, "message": <string>}}`.
- Stream events, in order: `action` (`{api_version, speaker, action, refused?}`; `action` is `{id, params}` or null), `token` (`{text}`), `sentence` (`{text}`), then `done` (`{speaker, action, line, turn, events_omitted, timings}`) or `error` (`{status, message}`). SSE wire form: `event: <name>\ndata: <one line of JSON>\n\n`.
- A sentence ends at `.`, `!`, `?` or the ellipsis character (U+2026), then any closing quotes or brackets, then whitespace. Abbreviations are not special.
- The listening socket owns its port alone: `SO_EXCLUSIVEADDRUSE` on Windows, plain `SO_REUSEADDR` elsewhere.
- `RunStore` schema version 4: `hosted_turns` gains `events_omitted INTEGER NOT NULL DEFAULT 0`; version 3 and version 2 files upgrade in place in a transaction.
- `MORPHEUS_BUILD_SERVER` defaults to `PROJECT_IS_TOP_LEVEL`, needs `MORPHEUS_BUILD_LLAMA_SERVER`, never links llama.cpp. A vendored build defines no server target.
- Only `src/llamaserver/LlamaServerBackend.cpp`, `src/server/HttpServer.cpp` and the tests include `<httplib.h>`.
- CLAUDE.md coding rules apply to every line: fail fast (`MORPHEUS_REQUIRE` for engine bugs, `core::DataError` for bad data); method bodies commented enough to read top to bottom; a private helper only with more than one caller (local lambdas are fine); class layout order nested types, public data, private data, public methods, private methods; constants ALL_CAPS and grouped; types and methods PascalCase, locals camelCase, members `_camelCase`, raw pointers `pName` / `_pName`; members initialized at declaration; RAII, no naked new/delete, raw pointers never own; no legacy paths; ASCII only in source comments and docs.
- Warnings are errors: MSVC `/W4 /permissive-`, Linux `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Cast between `std::size_t` and `int` explicitly.
- Tests are written first (Catch2). Fast loop: `cmake --build --preset tests`, then `./build/tests/bin/Debug/morpheus_tests.exe "<tag or name>"`; full: `ctest --preset tests` and `ctest --preset asan` (build `asan` first with `cmake --build --preset asan`).
- Never stage `settings.json`, `models/`, `imgui.ini` or `docs/references/`. Commit messages end with the implementing model's trailer, e.g. `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` or `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- Before reporting, check every changed file with `LC_ALL=C grep -c $'[\x80-\xFF]' <file>` (must print 0) and leave no `morpheus_tests` or `ctest` process running (`tasklist | grep -i -E "morpheus|ctest"`).

## File map

| File | Task | Responsibility |
|---|---|---|
| `src/core/HostedRecord.h` | 1 | `HostedTurn::eventsOmitted` |
| `src/store/RunStore.h/.cpp` | 1 | Schema version 4, `events_omitted` column, upgrades from 3 and 2 |
| `src/turn/HostedSession.h/.cpp` | 2 | The event overflow cure; L3 entries leave out the omitted events |
| `src/llamaserver/LlamaServerBackend.h/.cpp` | 3 | `SlotCount()`, `ChatModelId()` from the chat server's `/props` |
| `CMakeLists.txt`, `src/server/CMakeLists.txt`, `tests/CMakeLists.txt` | 4 | `MORPHEUS_BUILD_SERVER`, `morpheus_server_lib`, the tests' link |
| `src/server/ServerLog.h` | 4 | `server::Log`, the line logger every layer takes |
| `src/server/SlotPool.h/.cpp` | 4 | Slot assignment, thread-safe |
| `src/server/SlotBackend.h/.cpp` | 4 | The `LlmBackend` wrapper that puts calls on the current slot |
| `src/server/SentenceSplitter.h/.cpp` | 5 | Streamed pieces in, finished sentences out |
| `src/server/RunWorker.h/.cpp` | 6 | One run: thread, job queue, session, backend, idle `Maintain`, reopen, stop |
| `src/server/RunRegistry.h/.cpp` | 7 | `run_id` rule and file, run workers by id, open, stop |
| `src/server/TurnStream.h/.cpp` | 8 | A turn's events from the worker to the HTTP handler |
| `src/server/Api.h/.cpp` | 8, 9 | Every route, validation and status mapping, without sockets |
| `src/server/HttpServer.h/.cpp` | 10 | cpp-httplib: routes to `Api`, SSE, stop on disconnect, the exclusive port |
| `tools/server/CMakeLists.txt`, `tools/server/main.cpp` | 11 | `morpheus_server`: flags, settings, backends, signals |
| `tests/store/HostedStoreTests.cpp` | 1 | Store version 4 |
| `tests/hosted/HostedTurnTests.cpp` | 2 | The overflow cure |
| `tests/llamaserver/LlamaServerBackendTests.cpp` | 3 | Slot count and chat model id |
| `tests/server/ServerTestSupport.h` | 4, 8, 10 | Shared helpers of the server tests |
| `tests/server/SlotPoolTests.cpp`, `SlotBackendTests.cpp`, `SentenceSplitterTests.cpp`, `RunWorkerTests.cpp`, `RunRegistryTests.cpp`, `ApiTests.cpp`, `ApiTurnTests.cpp`, `HttpServerTests.cpp`, `ServerLiveTests.cpp` | 4-11 | The server's tests |
| `README.md`, `CHANGELOG.md`, `CLAUDE.md`, the architecture and hosted-world specs | 12 | Documentation |

---

### Task 1: Store version 4: each hosted turn keeps how many events its prompt left out

**Files:**
- Modify: `src/core/HostedRecord.h` (struct `HostedTurn`)
- Modify: `src/store/RunStore.h:54` (`SCHEMA_VERSION`)
- Modify: `src/store/RunStore.cpp` (`HOSTED_SCHEMA_SQL`, the upgrade in `RunStore::Open`, `CommitHostedTurn`, `HostedTurns`)
- Test: `tests/store/HostedStoreTests.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `int core::HostedTurn::eventsOmitted = 0;` stored in `hosted_turns.events_omitted` and read back by `RunStore::HostedTurns`. `store::RunStore::SCHEMA_VERSION == 4`.

- [ ] **Step 1: Write the failing tests**

In `tests/store/HostedStoreTests.cpp`, rename the existing upgrade test and update its version check:

```cpp
TEST_CASE("A version 2 run is upgraded in place to an owned version 4 run, its data unchanged", "[store][hosted]")
```

and in its body:

```cpp
    // The file now says version 4 and owned, has the hosted tables, and opens again as it is.
    {
        store::Database db = store::Database::Open(file, false);
        store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
        REQUIRE(version.Step());
        CHECK(version.ColumnText(0) == "4");
```

(the rest of that block is unchanged). Then append these test cases at the end of the file. They use the file's existing helpers `HostedMeta()`, `TestHostData()`, `TwoParticipants()`, `TestHostedTurn(number, conversationId)`, `HostedFile(name)` (a hosted run with conversation c1, one turn and one event) and `Damage(file, sql)`:

```cpp
TEST_CASE("A hosted turn keeps how many events its prompt left out", "[store][hosted]")
{
    const std::filesystem::path file = test::FreshTempDir("store_events_omitted") / "run.db";
    std::string id;
    {
        store::RunStore store = store::RunStore::CreateHosted(file, HostedMeta(), TestHostData());
        id = store.CreateConversation(TwoParticipants());
        core::HostedTurn turn = TestHostedTurn(1, id);
        turn.eventsOmitted = 3;
        store.CommitHostedTurn(turn);
        store.CommitHostedTurn(TestHostedTurn(2, id));
    }
    store::RunStore reopened = store::RunStore::Open(file);
    const std::vector<core::HostedTurn> turns = reopened.HostedTurns(id);
    REQUIRE(turns.size() == 2);
    CHECK(turns[0].eventsOmitted == 3);
    CHECK(turns[1].eventsOmitted == 0);
}

TEST_CASE("A version 3 run is upgraded in place to version 4, its turns with no events left out", "[store][hosted]")
{
    // A version 3 file as RunStore wrote it: a version 4 file without the new column, marked version 3.
    const std::filesystem::path file = HostedFile("store_upgrade_v3");
    Damage(file, "ALTER TABLE hosted_turns DROP COLUMN events_omitted;"
                 "UPDATE run_meta SET value = '3' WHERE key = 'schema_version';");
    {
        store::RunStore store = store::RunStore::Open(file);
        CHECK(store.Meta().kind == store::RunKind::Hosted);
        const std::vector<core::HostedTurn> turns = store.HostedTurns("c1");
        REQUIRE(turns.size() == 1);
        CHECK(turns[0].eventsOmitted == 0);
        CHECK(store.ConversationEvents("c1").size() == 1);
    }
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "4");
}

TEST_CASE("A failed upgrade from version 3 names the file and leaves it a version 3 run", "[store][hosted]")
{
    // The column already exists, so adding it fails and the transaction rolls back.
    const std::filesystem::path file = HostedFile("store_upgrade_v3_fails");
    Damage(file, "UPDATE run_meta SET value = '3' WHERE key = 'schema_version'");
    CHECK_THROWS_AS(store::RunStore::Open(file), store::StoreError);
    CHECK_THROWS_WITH(store::RunStore::Open(file), ContainsSubstring("run.db") && ContainsSubstring("version 3"));
    store::Database db = store::Database::Open(file, false);
    store::Statement version = db.Prepare("SELECT value FROM run_meta WHERE key = 'schema_version'");
    REQUIRE(version.Step());
    CHECK(version.ColumnText(0) == "3");
}

TEST_CASE("A negative events_omitted is a StoreError naming the file and the column", "[store][hosted]")
{
    const std::filesystem::path file = HostedFile("store_damaged_omitted");
    Damage(file, "UPDATE hosted_turns SET events_omitted = -1");
    store::RunStore store = store::RunStore::Open(file);
    CHECK_THROWS_AS(store.HostedTurns("c1"), store::StoreError);
    CHECK_THROWS_WITH(store.HostedTurns("c1"), ContainsSubstring("run.db") && ContainsSubstring("events_omitted"));
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[store]"`
Expected: a compile error, `'eventsOmitted': is not a member of 'morpheus::core::HostedTurn'`.

- [ ] **Step 3: Add the field**

In `src/core/HostedRecord.h`, in `struct HostedTurn`, after `std::string line = {};`:

```cpp
    // How many of the events before the turn (recorded since the conversation's previous turn, then the request's)
    // its prompt left out, the oldest first, because the tail was over tail_allowance. The store keeps them all.
    int eventsOmitted = 0;
```

- [ ] **Step 4: Store it: schema version 4 and its upgrades**

In `src/store/RunStore.h`, change `static constexpr int SCHEMA_VERSION = 3;` to `static constexpr int SCHEMA_VERSION = 4;`.

In `src/store/RunStore.cpp`, replace the comment above `HOSTED_SCHEMA_SQL` and the `hosted_turns` table in it:

```cpp
// The tables version 3 adds, with version 4's events_omitted: a hosted run's host data, its conversations, its turns
// and the events recorded between them. Created in every file, so a version 2 file is upgraded by adding them.
constexpr const char* HOSTED_SCHEMA_SQL = R"sql(
```

```sql
CREATE TABLE hosted_turns (
    number          INTEGER PRIMARY KEY,
    conversation_id TEXT NOT NULL,
    speaker_id      TEXT NOT NULL,
    listener_ids    TEXT NOT NULL,
    player_text     TEXT NOT NULL,
    events          TEXT NOT NULL,
    observation     TEXT NOT NULL,
    resolve_output  TEXT NOT NULL,
    adjudication    TEXT NOT NULL,
    line            TEXT NOT NULL,
    timings         TEXT NOT NULL,
    events_omitted  INTEGER NOT NULL DEFAULT 0
);
```

Right after `HOSTED_SCHEMA_SQL`, add:

```cpp
// What version 4 adds to a version 3 file: each hosted turn's count of the events its prompt left out, 0 for the
// turns already played.
constexpr const char* EVENTS_OMITTED_SQL =
    "ALTER TABLE hosted_turns ADD COLUMN events_omitted INTEGER NOT NULL DEFAULT 0;";
```

In `RunStore::Open`, replace the whole `if (value("schema_version") == "2") { ... }` block with:

```cpp
    // An older run is upgraded in place, in one transaction, its data untouched: version 2 gains the hosted tables
    // (already with version 4's column) and is marked owned; version 3 gains hosted_turns.events_omitted, 0 for the
    // turns it holds. Any other version is refused below.
    const std::string version = value("schema_version"); // A copy: `values` changes below.
    if (version == "2" || version == "3") {
        try {
            db.Transaction([&] {
                if (version == "2") {
                    db.Exec(HOSTED_SCHEMA_SQL);
                    db.Prepare("INSERT INTO run_meta (key, value) VALUES ('kind', ?1)")
                        .Bind(1, RunKindName(RunKind::Owned))
                        .Run();
                } else {
                    db.Exec(EVENTS_OMITTED_SQL);
                }
                db.Prepare("UPDATE run_meta SET value = ?1 WHERE key = 'schema_version'")
                    .Bind(1, std::to_string(SCHEMA_VERSION))
                    .Run();
            });
        } catch (const StoreError& error) {
            // The transaction rolled back: the file is still at its old version.
            throw StoreError(std::format("'{}': cannot upgrade from schema version {} to {}: {}",
                                         core::PathText(file), version, SCHEMA_VERSION, error.what()));
        }
        values["schema_version"] = std::to_string(SCHEMA_VERSION);
        if (version == "2") {
            values["kind"] = RunKindName(RunKind::Owned);
        }
    }
```

In `RunStore::CommitHostedTurn`, add next to its other `MORPHEUS_REQUIRE` checks (before the insert):

```cpp
    MORPHEUS_REQUIRE(record.eventsOmitted >= 0, "RunStore::CommitHostedTurn: eventsOmitted must not be negative");
```

and replace the insert with:

```cpp
    _db.Prepare("INSERT INTO hosted_turns (number, conversation_id, speaker_id, listener_ids, player_text, events, "
                "observation, resolve_output, adjudication, line, timings, events_omitted) "
                "VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11, ?12)")
        .Bind(1, record.number)
        .Bind(2, record.conversationId)
        .Bind(3, record.speakerId)
        .Bind(4, DumpText(listeners))
        .Bind(5, DumpText(playerText))
        .Bind(6, DumpText(events))
        .Bind(7, DumpText(record.observation))
        .Bind(8, record.resolveOutput)
        .Bind(9, DumpText(adjudication))
        .Bind(10, record.line)
        .Bind(11, DumpText(record.timings))
        .Bind(12, record.eventsOmitted)
        .Run();
```

In `RunStore::HostedTurns`, select the column last:

```cpp
    Statement query = _db.Prepare("SELECT number, speaker_id, listener_ids, player_text, events, observation, "
                                  "resolve_output, adjudication, line, timings, events_omitted FROM hosted_turns "
                                  "WHERE conversation_id = ?1 ORDER BY number");
```

and after `turn.timings = parseColumn(9, "timings");`:

```cpp
        turn.eventsOmitted = static_cast<int>(query.ColumnInt(10));
        if (turn.eventsOmitted < 0) {
            throw damaged("events_omitted", "is negative");
        }
```

(`damaged` is the lambda `HostedTurns` already uses for its other columns. If `ColumnInt` returns `std::int64_t`, the `static_cast<int>` above is the conversion; keep it explicit.)

- [ ] **Step 5: Find every other place that names version 3**

Run: `grep -rn "version 3\|SCHEMA_VERSION\|\"3\"" src tests | grep -iv "api_version"`
Expected: only the places this task already changed, or tests that build version 2 files (leave those). Update any other test that checks the stored version is 3 so it expects 4, and any comment that calls the current version 3 (`src/store/RunStore.h`'s class comment, if it says so).

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[store]"`
Expected: `All tests passed`.

Then: `./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`
Expected: `All tests passed` (hosted sessions write and read the new column).

- [ ] **Step 7: Commit**

```bash
git add src/core/HostedRecord.h src/store/RunStore.h src/store/RunStore.cpp tests/store/HostedStoreTests.cpp
git commit -m "Store version 4: a hosted turn keeps how many events its prompt left out; version 3 and 2 files upgrade in place"
```

(Add the implementing model's `Co-Authored-By` trailer as the message's last line.)

---

### Task 2: The event overflow cure: the newest events that fit stay in the prompt

**Files:**
- Modify: `src/turn/HostedSession.cpp` (`RunTurn`, `Entries`)
- Modify: `src/turn/HostedSession.h` (the comments of `RecordEvents` and `RunTurn`)
- Test: `tests/hosted/HostedTurnTests.cpp`

**Interfaces:**
- Consumes: `core::HostedTurn::eventsOmitted` (Task 1).
- Produces: `RunTurn` no longer throws `TurnError` for events alone: when the tail is over `tail_allowance` with no retrieved snippet left, the oldest events leave the prompt until it fits, and `record.eventsOmitted` counts them. `TurnError` remains when the observation, the player text and the instruction alone do not fit. A turn's L3 entry leaves out the same events.

- [ ] **Step 1: Write the failing tests**

In `tests/hosted/HostedTurnTests.cpp`, delete the whole test case `"Events recorded between turns over the tail allowance make every later turn a TurnError until the conversation ends"` and put these two in its place (they use the file's `OpenRun`, `NONE`, and `test::Request`, `test::HULDA`, `test::AELA`, `test::FixtureObservation`, `test::HostedSettings` from `hosted/HostedTestSupport.h`):

```cpp
TEST_CASE("Events over the tail allowance: the oldest leave the prompt, the newest stay, and the turn counts them",
          "[hosted][turn]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"Welcome.", "Hm.", "Yes?"};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_events_overflow", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, {}));

    // Four events of 300 tokens each (the fake counts 4 bytes a token): with the observation and the player's line,
    // over tail_allowance 1024. Three are recorded between turns, the fourth comes with the request.
    const std::string a(1200, 'a');
    const std::string b(1200, 'b');
    const std::string c(1200, 'c');
    const std::string d(1200, 'd');
    session.RecordEvents(id, {nlohmann::json{{"text", a}}, nlohmann::json{{"text", b}}});
    session.RecordEvents(id, {nlohmann::json{{"text", c}}});
    turn::HostedTurnRequest request = test::Request(id, test::HULDA, "Still there?");
    request.events = {nlohmann::json{{"text", d}}};
    const std::optional<core::HostedTurn> record = session.RunTurn(request, {}, {});
    REQUIRE(record);

    // The oldest left the prompt; the newest and the player's line stayed, and the tail fits.
    const context::Assembly& assembly = session.LastAssembly();
    const std::string& tail = assembly.prompt.messages.back().content;
    CHECK(record->eventsOmitted >= 1);
    CHECK(record->eventsOmitted < 4);
    CHECK(assembly.tokens.tail <= test::HostedSettings().budget.tailAllowance);
    CHECK_THAT(tail, !ContainsSubstring(a));
    CHECK_THAT(tail, ContainsSubstring("(" + d + ")"));
    CHECK_THAT(tail, ContainsSubstring("Still there?"));
    // The store keeps every event, and the turn keeps its count.
    CHECK(session.Events(id).size() == 3);
    CHECK(session.Turns(id).back().events.size() == 1);
    CHECK(session.Turns(id).back().eventsOmitted == record->eventsOmitted);

    // The next turn's L3 shows that turn with the events its prompt showed: not the oldest, still the newest.
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Well?"), {}, {}));
    const inference::Prompt& next = session.LastAssembly().prompt;
    bool showsOldest = false;
    bool showsNewest = false;
    for (std::size_t k = 1; k < next.stableCount; ++k) {
        showsOldest = showsOldest || next.messages[k].content.find(a) != std::string::npos;
        showsNewest = showsNewest || next.messages[k].content.find(d) != std::string::npos;
    }
    CHECK_FALSE(showsOldest);
    CHECK(showsNewest);
}

TEST_CASE("A tail over the allowance even without its events is still a TurnError, before any model call",
          "[hosted][turn]")
{
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_tail_over", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    session.RecordEvents(id, {nlohmann::json{{"text", "A door opens."}}});
    const turn::HostedTurnRequest request = test::Request(id, test::HULDA, std::string(5000, 'x'));
    CHECK_THROWS_MATCHES(session.RunTurn(request, {}, {}), turn::TurnError,
                         MessageMatches(ContainsSubstring("the observation, the events and the player text use")));
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
    CHECK(session.Events(id).size() == 1);
}
```

Add `#include "context/PromptBuilder.h"` to the file's includes if `context::Assembly` is not already visible through `turn/HostedSession.h` (it is declared in `context/PromptBuilder.h`).

- [ ] **Step 2: Run the tests to see the first fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted][turn]"`
Expected: the first new test FAILS with a `TurnError` ("the observation, the events and the player text use ... over tail_allowance 1024"); the second passes already.

- [ ] **Step 3: Trim the oldest events in RunTurn**

In `src/turn/HostedSession.cpp`, `RunTurn`, replace the fit call:

```cpp
    std::optional<context::Assembly> assembly = Upkeep(conversation.id, turn).Fit(
        hits, [&](const std::vector<RetrievalHit>& kept) { return Pass(conversation.id, kept, tail); }, stop);
```

with:

```cpp
    // The fit loop sheds retrieved hits first. When the tail is still over tail_allowance with none left, the oldest
    // events leave the prompt one by one until it fits (the store keeps them all; the record counts them). The
    // observation, the player text and the instruction must fit on their own: with no event left, an over-long tail
    // is the fit loop's TurnError, before any model call.
    std::optional<context::Assembly> assembly = Upkeep(conversation.id, turn).Fit(
        hits,
        [&](const std::vector<RetrievalHit>& kept) {
            FitPass pass = Pass(conversation.id, kept, tail);
            while (pass.assembly.tailOverBudget && kept.empty() && !tail.events.empty()) {
                tail.events.erase(tail.events.begin());
                pass = Pass(conversation.id, kept, tail);
            }
            return pass;
        },
        stop);
```

In step 4 of `RunTurn` (the record), after `record.line = core::Trim(spoken.text);`:

```cpp
    record.eventsOmitted = static_cast<int>(newEvents.size() - tail.events.size());
```

- [ ] **Step 4: Leave the omitted events out of the turn's L3 entry**

In `HostedSession::Entries`, replace:

```cpp
            for (const nlohmann::json& event : turn.events) {
                before.push_back("(" + event.at("text").get<std::string>() + ")");
            }
            if (turn.playerText) {
```

with:

```cpp
            for (const nlohmann::json& event : turn.events) {
                before.push_back("(" + event.at("text").get<std::string>() + ")");
            }
            // The events its prompt left out (the oldest, when the tail was over tail_allowance) stay out of its
            // entry too, so one long burst of events does not overflow L3 later.
            before.erase(before.begin(),
                         before.begin() + std::min<std::ptrdiff_t>(turn.eventsOmitted, std::ssize(before)));
            if (turn.playerText) {
```

- [ ] **Step 5: Update the contract comments**

In `src/turn/HostedSession.h`, replace the comment of `RecordEvents` with:

```cpp
    // Adds events to an open conversation without a turn, in order (HostedWorld::CheckEvents). They come before the
    // conversation's next turn: every event recorded since its last turn joins that turn's tail. When the tail is over
    // tail_allowance, the oldest of them are left out of that turn's prompt (RunTurn); the store keeps them all.
    // Throws DataError naming the field.
```

and in the comment of `RunTurn`, replace the sentences from "Throws DataError naming the field for a malformed request" to the end of the paragraph with:

```cpp
    // Throws DataError naming the field for a malformed request (before any model call), TurnError when the
    // observation, the player text and the instruction alone do not fit tail_allowance (before the resolve call), and
    // InferenceError from the backend. The tail holds the observation, every event recorded since the conversation's
    // last turn (RecordEvents), the request's events and the player text: when it is over tail_allowance with no
    // retrieved snippet left to shed, the oldest events leave the prompt one by one until it fits. The record's
    // eventsOmitted counts them; the store keeps them, and the turn's L3 entry leaves them out as its prompt did.
    // Model mistakes are not exceptions: a refused action is recorded with its reasons and the turn goes on with no
    // action.
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`
Expected: `All tests passed`.

Then the owned game's tests, which share the fit loop: `./build/tests/bin/Debug/morpheus_tests.exe "[turn]"`
Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/HostedTurnTests.cpp
git commit -m "Hosted turns keep the newest events that fit: the oldest leave the prompt and its L3 entry, and the turn counts them"
```

---

### Task 3: LlamaServerBackend reads the chat server's slot count and model id

**Files:**
- Modify: `src/llamaserver/LlamaServerBackend.h` (members, two public methods)
- Modify: `src/llamaserver/LlamaServerBackend.cpp` (the constructor's chat `/props` read, a helper for model ids)
- Test: `tests/llamaserver/LlamaServerBackendTests.cpp`

**Interfaces:**
- Consumes: nothing new.
- Produces: `int llamaserver::LlamaServerBackend::SlotCount() const` (the chat server's `/props` `total_slots`) and `std::string llamaserver::LlamaServerBackend::ChatModelId() const` (its `/props` `model_path` file name without the extension). The constructor throws `InferenceError` naming the chat URL when `total_slots` is below 1 or the model path names no file.

- [ ] **Step 1: Write the failing tests**

Append to `tests/llamaserver/LlamaServerBackendTests.cpp`:

```cpp
TEST_CASE("LlamaServerBackend reads the chat server's slot count and model id from /props", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    {
        // The fake's default /props: 3 slots, and a model file the fake names for both servers.
        const llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
        CHECK(backend.SlotCount() == 3);
        CHECK(backend.ChatModelId() == "nomic-embed-text-v1.5.Q8_0");
    }
    {
        chat.SetProps(test::ThinkingProps());
        const llamaserver::LlamaServerBackend backend(test::ConfigFor(chat, embed));
        CHECK(backend.ChatModelId() == "Qwen3.5-0.8B-Q4_K_M");
    }
}

TEST_CASE("LlamaServerBackend refuses a chat server with no slot or no model file", "[llamaserver]")
{
    test::FakeLlamaServer chat;
    test::FakeLlamaServer embed;
    SECTION("no slot")
    {
        nlohmann::json props = test::ThinkingProps();
        props["total_slots"] = 0;
        chat.SetProps(props);
        CHECK_THROWS_AS(llamaserver::LlamaServerBackend(test::ConfigFor(chat, embed)), inference::InferenceError);
        CHECK_THROWS_WITH(llamaserver::LlamaServerBackend(test::ConfigFor(chat, embed)),
                          ContainsSubstring(chat.Url()) && ContainsSubstring("total_slots"));
    }
    SECTION("no model file")
    {
        nlohmann::json props = test::ThinkingProps();
        props["model_path"] = "";
        chat.SetProps(props);
        CHECK_THROWS_WITH(llamaserver::LlamaServerBackend(test::ConfigFor(chat, embed)),
                          ContainsSubstring(chat.Url()) && ContainsSubstring("model_path"));
    }
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver]"`
Expected: a compile error, `'SlotCount': is not a member of 'morpheus::llamaserver::LlamaServerBackend'`.

- [ ] **Step 3: Add the members and methods**

In `src/llamaserver/LlamaServerBackend.h`, after `std::string _embeddingModelId = {};`:

```cpp
    int _slotCount = 0;                            // The chat server's slots (/props total_slots).
    std::string _chatModelId = {};                 // The chat server's model file name, without its extension.
```

After `std::string EmbeddingModelId() const override;`:

```cpp
    // The chat server's slots, from its /props total_slots: what morpheus_server shares between conversations.
    int SlotCount() const;
    // The chat server's model: its /props model_path's file name without the extension, as EmbeddingModelId.
    std::string ChatModelId() const;
```

In the constructor's comment, change "the chat server's context per slot, which must hold config.contextSize, and its chat template (whether the model thinks); the embedding server's model file." to "the chat server's context per slot, which must hold config.contextSize, its chat template (whether the model thinks), its slot count and its model file; the embedding server's model file." and add "a chat server with no slot," to the list of `InferenceError` causes.

- [ ] **Step 4: Read them in the constructor**

In `src/llamaserver/LlamaServerBackend.cpp`, in the anonymous namespace, add:

```cpp
// A model's id as Morpheus names it: the file name in a server's /props model_path, without the extension, as the
// in-process backend names its models; "" when the path names no file.
std::string ModelIdFromPath(const std::string& modelPath)
{
    const std::size_t nameStart = modelPath.find_last_of("/\\");
    const std::string fileName = modelPath.substr(nameStart == std::string::npos ? 0 : nameStart + 1);
    const std::size_t dot = fileName.rfind('.');
    return dot == std::string::npos || dot == 0 ? fileName : fileName.substr(0, dot);
}
```

Replace the chat server's `/props` read:

```cpp
    const auto [slotContext, chatTemplate] =
        Ask(*_pChatClient, _config.chatUrl, "/props", std::nullopt, [](const nlohmann::json& props) {
            return std::pair(props.at("default_generation_settings").at("n_ctx").get<int>(),
                             props.at("chat_template").get<std::string>());
        });
```

with:

```cpp
    struct ChatProps {
        int slotContext = 0;
        std::string chatTemplate = {};
        int slots = 0;
        std::string modelPath = {};
    };
    const ChatProps chat = Ask(*_pChatClient, _config.chatUrl, "/props", std::nullopt, [](const nlohmann::json& props) {
        return ChatProps{props.at("default_generation_settings").at("n_ctx").get<int>(),
                         props.at("chat_template").get<std::string>(), props.at("total_slots").get<int>(),
                         props.at("model_path").get<std::string>()};
    });
    const int slotContext = chat.slotContext;
    const std::string& chatTemplate = chat.chatTemplate;
    // Its slots, which morpheus_server shares between conversations, and its model's id, for /health.
    if (chat.slots < 1) {
        throw InferenceError(
            std::format("the chat server at {} reports {} slots in /props (total_slots)", _config.chatUrl, chat.slots));
    }
    _slotCount = chat.slots;
    _chatModelId = ModelIdFromPath(chat.modelPath);
    if (_chatModelId.empty()) {
        throw InferenceError(std::format("the chat server at {} names no model file in /props (model_path '{}')",
                                         _config.chatUrl, chat.modelPath));
    }
```

and replace the embedding server's name stripping:

```cpp
    const std::size_t nameStart = modelPath.find_last_of("/\\");
    const std::string fileName = modelPath.substr(nameStart == std::string::npos ? 0 : nameStart + 1);
    const std::size_t dot = fileName.rfind('.');
    _embeddingModelId = dot == std::string::npos || dot == 0 ? fileName : fileName.substr(0, dot);
```

with:

```cpp
    _embeddingModelId = ModelIdFromPath(modelPath);
```

Add the two methods after `EmbeddingModelId()`:

```cpp
int LlamaServerBackend::SlotCount() const
{
    return _slotCount;
}

std::string LlamaServerBackend::ChatModelId() const
{
    return _chatModelId;
}
```

- [ ] **Step 5: Give every chat fake's custom /props its slots and model file**

The chat `/props` now needs `total_slots` and `model_path`. Run `grep -rn "SetProps(" tests/` and, for each call on a fake used as the chat server whose JSON lacks them, add `{"total_slots", 3}` and, if missing, `{"model_path", "/m/chat.gguf"}`. For example, in `"LlamaServerBackend refuses a chat server whose slots hold less than the context size"`:

```cpp
    chat.SetProps({{"default_generation_settings", {{"n_ctx", 4096}}},
                   {"total_slots", 3},
                   {"chat_template", ""},
                   {"model_path", "/m/chat.gguf"}});
```

Calls on a fake used only as the embedding server need nothing.

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[llamaserver],[fake-llama-server]"`
Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/llamaserver/LlamaServerBackend.h src/llamaserver/LlamaServerBackend.cpp tests/llamaserver/
git commit -m "LlamaServerBackend reads the chat server's slot count and model id from /props"
```

---

### Task 4: The server module: build option, `SlotPool` and `SlotBackend`

**Files:**
- Modify: `CMakeLists.txt` (the option, its check, the subdirectory)
- Create: `src/server/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt` (filter and link)
- Create: `src/server/ServerLog.h`, `src/server/SlotPool.h`, `src/server/SlotPool.cpp`, `src/server/SlotBackend.h`, `src/server/SlotBackend.cpp`
- Test: `tests/server/SlotPoolTests.cpp`, `tests/server/SlotBackendTests.cpp`

**Interfaces:**
- Consumes: `inference::LlmBackend` (`src/inference/Llm.h`), `core::ContractError` / `MORPHEUS_REQUIRE` (`src/core/Errors.h`).
- Produces:
  - `using server::Log = std::function<void(const std::string& line)>;` (`server/ServerLog.h`)
  - `class server::SlotPool` with `static constexpr int UPKEEP_SLOT = 0;`, `explicit SlotPool(int slotCount)`, `int SlotCount() const`, `int Acquire(const std::string& key)`, `void Release(const std::string& key)`, `std::string HolderOf(int slot) const`
  - `class server::SlotBackend final : public inference::LlmBackend` with `explicit SlotBackend(inference::LlmBackend& backend)`, `void SetSlot(int slot)`, `int Slot() const`, and every `LlmBackend` override
  - CMake target `morpheus_server_lib`; option `MORPHEUS_BUILD_SERVER`
  - Test tag `[hosted-server]` for every server test that needs no real llama-server. (`[server]` stays the tag of tests that need real llama-servers; never put it on these.)

- [ ] **Step 1: Add the build option and the module**

In `CMakeLists.txt`, after the `option(MORPHEUS_BUILD_LLAMA_SERVER ...)` line:

```cmake
option(MORPHEUS_BUILD_SERVER "Build morpheus_server, the HTTP server for hosted runs (needs MORPHEUS_BUILD_LLAMA_SERVER; no llama.cpp)" ${PROJECT_IS_TOP_LEVEL})
```

after the existing `if(MORPHEUS_BUILD_LLAMA AND NOT MORPHEUS_BUILD_LLAMA_SERVER) ... endif()` check:

```cmake
if(MORPHEUS_BUILD_SERVER AND NOT MORPHEUS_BUILD_LLAMA_SERVER)
    message(FATAL_ERROR "MORPHEUS_BUILD_SERVER (morpheus_server) needs MORPHEUS_BUILD_LLAMA_SERVER.")
endif()
```

and in the engine libraries section, after `if(MORPHEUS_BUILD_LLAMA_SERVER) add_subdirectory(src/llamaserver) endif()`:

```cmake
if(MORPHEUS_BUILD_SERVER)
    add_subdirectory(src/server)
endif()
```

Create `src/server/CMakeLists.txt`:

```cmake
# morpheus_server's library (morpheus_server spec, section 4): run workers, the transport-free API and the HTTP
# adapter. It is not one of the engine libraries a vendored build exposes, so it has no Morpheus:: alias.
add_library(morpheus_server_lib STATIC
    SlotBackend.cpp
    SlotPool.cpp
)
target_include_directories(morpheus_server_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(morpheus_server_lib
    PUBLIC morpheus_turn morpheus_llamaserver nlohmann_json::nlohmann_json
    PRIVATE morpheus_httplib
)
morpheus_target_defaults(morpheus_server_lib)
```

In `tests/CMakeLists.txt`, after the `/tests/llamaserver/` filter:

```cmake
# The server's tests need morpheus_server_lib; presets without it skip their sources too.
if(NOT MORPHEUS_BUILD_SERVER)
    list(FILTER MORPHEUS_TEST_SOURCES EXCLUDE REGEX "/tests/server/")
endif()
```

and after the `if(MORPHEUS_BUILD_LLAMA_SERVER) target_link_libraries(... morpheus_httplib) endif()` block:

```cmake
if(MORPHEUS_BUILD_SERVER)
    target_link_libraries(morpheus_tests PRIVATE morpheus_server_lib)
endif()
```

Create `src/server/ServerLog.h`:

```cpp
#pragma once

#include <functional>
#include <string>

namespace morpheus::server {

// Where the server's layers write a line for the operator: the executable writes them to stderr, tests keep them.
// Called from any thread, so the function must be thread-safe.
using Log = std::function<void(const std::string& line)>;

} // namespace morpheus::server
```

- [ ] **Step 2: Write the failing tests**

Create `tests/server/SlotPoolTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <thread>
#include <vector>

#include "core/Errors.h"
#include "server/SlotPool.h"

using namespace morpheus;

TEST_CASE("SlotPool keeps slot 0 for upkeep and gives conversations the others, least recently used first",
          "[hosted-server][slots]")
{
    server::SlotPool pool(3);
    CHECK(pool.SlotCount() == 3);
    CHECK(pool.Acquire("run/c1") == 1);
    CHECK(pool.Acquire("run/c2") == 2);
    CHECK(pool.Acquire("run/c1") == 1); // Its own slot again; c1 is now the most recently used.
    // A third conversation takes the slot of the one used least recently (c2), which loses it.
    CHECK(pool.Acquire("run/c3") == 2);
    CHECK(pool.HolderOf(2) == "run/c3");
    CHECK(pool.HolderOf(0) == "");
    // c2's next turn takes a slot again: c1's, now the least recently used.
    CHECK(pool.Acquire("run/c2") == 1);
    CHECK(pool.HolderOf(1) == "run/c2");
}

TEST_CASE("SlotPool frees a released conversation's slot for the next one", "[hosted-server][slots]")
{
    server::SlotPool pool(3);
    CHECK(pool.Acquire("a/c1") == 1);
    CHECK(pool.Acquire("a/c2") == 2);
    pool.Release("a/c1");
    CHECK(pool.HolderOf(1) == "");
    CHECK(pool.Acquire("b/c1") == 1);
    pool.Release("never/held"); // Nothing to free: nothing changes.
    CHECK(pool.HolderOf(1) == "b/c1");
    CHECK(pool.HolderOf(2) == "a/c2");
}

TEST_CASE("SlotPool with one slot shares slot 0 between upkeep and every conversation", "[hosted-server][slots]")
{
    server::SlotPool pool(1);
    CHECK(pool.Acquire("a/c1") == server::SlotPool::UPKEEP_SLOT);
    CHECK(pool.Acquire("a/c2") == server::SlotPool::UPKEEP_SLOT);
    CHECK(pool.HolderOf(0) == "");
}

TEST_CASE("SlotPool hands out conversation slots safely from several threads", "[hosted-server][slots]")
{
    server::SlotPool pool(3);
    std::vector<int> slots(400, -1);
    {
        std::vector<std::jthread> threads;
        for (int t = 0; t < 4; ++t) {
            threads.emplace_back([&pool, &slots, t] {
                for (int i = 0; i < 100; ++i) {
                    slots[static_cast<std::size_t>(t * 100 + i)] = pool.Acquire("run/c" + std::to_string(t * 100 + i));
                }
            });
        }
    }
    for (const int slot : slots) {
        CHECK((slot == 1 || slot == 2));
    }
}

TEST_CASE("SlotPool refuses no slot and an empty key", "[hosted-server][slots]")
{
    CHECK_THROWS_AS(server::SlotPool(0), core::ContractError);
    server::SlotPool pool(2);
    CHECK_THROWS_AS(pool.Acquire(""), core::ContractError);
    CHECK_THROWS_AS(pool.HolderOf(2), core::ContractError);
}
```

Create `tests/server/SlotBackendTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <string>

#include "core/Errors.h"
#include "fakes/ScriptedBackend.h"
#include "inference/Llm.h"
#include "server/SlotBackend.h"

using namespace morpheus;

namespace {

const inference::Prompt PROMPT = {{{inference::Role::System, "Be brief."}, {inference::Role::User, "Hi"}}, 1};

} // namespace

TEST_CASE("SlotBackend puts calls left on the default slot on its current slot, and keeps a slot a call chose",
          "[hosted-server][slots]")
{
    test::ScriptedBackend inner(4096,
                                [](const inference::Prompt&, const inference::GenerationSpec&) { return std::string("ok"); });
    server::SlotBackend backend(inner);
    CHECK(backend.Slot() == -1);
    inference::GenerationSpec spec;
    spec.maxTokens = 8;
    CHECK(backend.Complete(PROMPT, spec, {}, {}).text == "ok"); // No slot set yet: -1 stays.
    backend.SetSlot(2);
    backend.Complete(PROMPT, spec, {}, {});
    inference::GenerationSpec chosen = spec;
    chosen.slot = 1;
    backend.Complete(PROMPT, chosen, {}, {});
    REQUIRE(inner.Calls().size() == 3);
    CHECK(inner.Calls()[0].spec.slot == -1);
    CHECK(inner.Calls()[1].spec.slot == 2);
    CHECK(inner.Calls()[2].spec.slot == 1);
    CHECK(backend.Slot() == 2);
}

TEST_CASE("SlotBackend forwards counting, embedding and the model id, and refuses a slot below -1",
          "[hosted-server][slots]")
{
    test::ScriptedBackend inner(4096);
    server::SlotBackend backend(inner);
    CHECK(backend.CountTokens("abcdefgh") == inner.CountTokens("abcdefgh"));
    CHECK(backend.CountPromptTokens(PROMPT) == inner.CountPromptTokens(PROMPT));
    CHECK(backend.ContextSize() == 4096);
    CHECK(backend.Embed({"a lighthouse"}, inference::EmbedPurpose::Query) ==
          inner.Embed({"a lighthouse"}, inference::EmbedPurpose::Query));
    CHECK(backend.EmbeddingModelId() == inner.EmbeddingModelId());
    CHECK_THROWS_AS(backend.SetSlot(-2), core::ContractError);
}
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake --preset tests` (the option is new), `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/SlotPool.h'` (and `morpheus_server_lib` failing for want of its sources).

- [ ] **Step 4: Write `SlotPool`**

Create `src/server/SlotPool.h`:

```cpp
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace morpheus::server {

// The chat server's slots, shared by every run of morpheus_server (spec, section 2). Slot 0 is the upkeep slot: the
// calls of folding and condensing. The others go to conversations, the least recently used first: with more
// conversations than slots, the conversation used least recently loses its slot, and its next turn takes one again
// (its prompt is then read again instead of reused from the slot's cache). With a single slot, upkeep and every
// conversation share slot 0. Thread-safe.
class SlotPool {
public:
    static constexpr int UPKEEP_SLOT = 0;

private:
    // Who holds a conversation slot, and when it last used it.
    struct Seat {
        std::string key = {};      // The holding conversation's key; empty when the slot is free.
        std::uint64_t lastUse = 0; // _clock at its holder's last Acquire.
    };

    mutable std::mutex _mutex = {};
    int _slotCount = 0;
    std::vector<Seat> _seats = {}; // Indexed by slot. Seat 0 (upkeep) is never handed to a conversation when there are more.
    std::uint64_t _clock = 0;      // Counts Acquire calls: the order of use.

public:
    // Throws ContractError for a count below 1.
    explicit SlotPool(int slotCount);

    int SlotCount() const;

    // The slot of conversation `key` (its run's and its own id, e.g. "save_1/c2"), marked as used now: the slot it
    // holds, else a free conversation slot, else the slot of the conversation used least recently, which loses it.
    // With one slot: 0. Throws ContractError for an empty key.
    int Acquire(const std::string& key);

    // Frees conversation `key`'s slot, if it holds one.
    void Release(const std::string& key);

    // The key of the conversation holding `slot`; "" when it is free, and always for slot 0. Throws ContractError for
    // a slot out of range.
    std::string HolderOf(int slot) const;
};

} // namespace morpheus::server
```

Create `src/server/SlotPool.cpp`:

```cpp
#include "server/SlotPool.h"

#include <algorithm>
#include <cstddef>
#include <format>

#include "core/Errors.h"

namespace morpheus::server {

SlotPool::SlotPool(int slotCount)
    : _slotCount(slotCount)
    , _seats(static_cast<std::size_t>(std::max(slotCount, 0)))
{
    MORPHEUS_REQUIRE(slotCount >= 1, std::format("SlotPool: a server has at least one slot, not {}", slotCount));
}

int SlotPool::SlotCount() const
{
    return _slotCount;
}

int SlotPool::Acquire(const std::string& key)
{
    MORPHEUS_REQUIRE(!key.empty(), "SlotPool::Acquire: a conversation needs a key");
    std::scoped_lock lock(_mutex);
    // One slot: upkeep and every conversation share it.
    if (_slotCount == 1) {
        return UPKEEP_SLOT;
    }
    ++_clock;

    // The conversation's own slot, else a free one, else the one used least recently.
    int chosen = -1;
    for (int slot = 1; slot < _slotCount && chosen < 0; ++slot) {
        if (_seats[static_cast<std::size_t>(slot)].key == key) {
            chosen = slot;
        }
    }
    for (int slot = 1; slot < _slotCount && chosen < 0; ++slot) {
        if (_seats[static_cast<std::size_t>(slot)].key.empty()) {
            chosen = slot;
        }
    }
    if (chosen < 0) {
        chosen = 1;
        for (int slot = 2; slot < _slotCount; ++slot) {
            if (_seats[static_cast<std::size_t>(slot)].lastUse < _seats[static_cast<std::size_t>(chosen)].lastUse) {
                chosen = slot;
            }
        }
    }
    Seat& seat = _seats[static_cast<std::size_t>(chosen)];
    seat.key = key;
    seat.lastUse = _clock;
    return chosen;
}

void SlotPool::Release(const std::string& key)
{
    std::scoped_lock lock(_mutex);
    for (Seat& seat : _seats) {
        if (seat.key == key) {
            seat = Seat{};
        }
    }
}

std::string SlotPool::HolderOf(int slot) const
{
    MORPHEUS_REQUIRE(slot >= 0 && slot < _slotCount,
                     std::format("SlotPool::HolderOf: slot {} is not one of the {} slots", slot, _slotCount));
    std::scoped_lock lock(_mutex);
    return _seats[static_cast<std::size_t>(slot)].key;
}

} // namespace morpheus::server
```

- [ ] **Step 5: Write `SlotBackend`**

Create `src/server/SlotBackend.h`:

```cpp
#pragma once

#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"

namespace morpheus::server {

// An LlmBackend that forwards every call to another and puts its calls on a chosen server slot: Complete sets
// GenerationSpec::slot to the current slot when the call left it at -1 (the backend's default slot), and keeps a slot
// the call chose. A run's worker sets the slot before each job (the conversation's) and before folding (the upkeep
// slot), so HostedSession's calls land on the right slot without knowing about slots. Not thread-safe, like the
// backend it wraps.
class SlotBackend final : public inference::LlmBackend {
private:
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives this.
    int _slot = -1;                             // -1: the wrapped backend's default slot.

public:
    explicit SlotBackend(inference::LlmBackend& backend);

    // The slot of the calls that follow; -1 for the wrapped backend's default. Throws ContractError below -1.
    void SetSlot(int slot);
    int Slot() const;

    int CountTokens(std::string_view text) override;
    int CountPromptTokens(const inference::Prompt& prompt) override;
    int ContextSize() const override;
    inference::Completion Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                   const inference::TokenCallback& onToken, std::stop_token stop) override;
    std::vector<std::uint8_t> SaveState() override;
    void RestoreState(std::span<const std::uint8_t> state) override;
    std::vector<std::vector<float>> Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose) override;
    std::string EmbeddingModelId() const override;
};

} // namespace morpheus::server
```

Create `src/server/SlotBackend.cpp`:

```cpp
#include "server/SlotBackend.h"

#include <format>

#include "core/Errors.h"

namespace morpheus::server {

SlotBackend::SlotBackend(inference::LlmBackend& backend)
    : _pBackend(&backend)
{
}

void SlotBackend::SetSlot(int slot)
{
    MORPHEUS_REQUIRE(slot >= -1, std::format("SlotBackend::SetSlot: slot {} is below -1", slot));
    _slot = slot;
}

int SlotBackend::Slot() const
{
    return _slot;
}

int SlotBackend::CountTokens(std::string_view text)
{
    return _pBackend->CountTokens(text);
}

int SlotBackend::CountPromptTokens(const inference::Prompt& prompt)
{
    return _pBackend->CountPromptTokens(prompt);
}

int SlotBackend::ContextSize() const
{
    return _pBackend->ContextSize();
}

inference::Completion SlotBackend::Complete(const inference::Prompt& prompt, const inference::GenerationSpec& spec,
                                            const inference::TokenCallback& onToken, std::stop_token stop)
{
    // A call that chose no slot goes on the current one; a call that chose one keeps it.
    if (spec.slot != -1 || _slot == -1) {
        return _pBackend->Complete(prompt, spec, onToken, stop);
    }
    inference::GenerationSpec onSlot = spec;
    onSlot.slot = _slot;
    return _pBackend->Complete(prompt, onSlot, onToken, stop);
}

std::vector<std::uint8_t> SlotBackend::SaveState()
{
    return _pBackend->SaveState();
}

void SlotBackend::RestoreState(std::span<const std::uint8_t> state)
{
    _pBackend->RestoreState(state);
}

std::vector<std::vector<float>> SlotBackend::Embed(const std::vector<std::string>& texts, inference::EmbedPurpose purpose)
{
    return _pBackend->Embed(texts, purpose);
}

std::string SlotBackend::EmbeddingModelId() const
{
    return _pBackend->EmbeddingModelId();
}

} // namespace morpheus::server
```

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"`
Expected: `All tests passed`.

Then check that a vendored build still defines no server target: `cmake -S cmake/consumer-check -B build/consumer-check` and `cmake --build build/consumer-check` (README, "Using Morpheus from another project"). Expected: configure succeeds (it fails when the vendored build defines more than the engine libraries and `morpheus_httplib`).

- [ ] **Step 7: Commit**

```bash
git add CMakeLists.txt tests/CMakeLists.txt src/server/ tests/server/
git commit -m "morpheus_server_lib: the MORPHEUS_BUILD_SERVER option, SlotPool and SlotBackend"
```

---

### Task 5: `SentenceSplitter`

**Files:**
- Create: `src/server/SentenceSplitter.h`, `src/server/SentenceSplitter.cpp`
- Modify: `src/server/CMakeLists.txt` (add `SentenceSplitter.cpp`)
- Test: `tests/server/SentenceSplitterTests.cpp`

**Interfaces:**
- Consumes: `core::Trim(std::string_view) -> std::string` (`src/core/Text.h`).
- Produces: `class server::SentenceSplitter` with `std::vector<std::string> Add(std::string_view piece)` and `std::string Finish()`.

- [ ] **Step 1: Write the failing tests**

Create `tests/server/SentenceSplitterTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <string>
#include <vector>

#include "server/SentenceSplitter.h"

using namespace morpheus;

namespace {

// The sentences of `pieces` streamed in order: those Add completes, then what Finish returns when it is not empty.
std::vector<std::string> Split(const std::vector<std::string>& pieces)
{
    server::SentenceSplitter splitter;
    std::vector<std::string> sentences;
    for (const std::string& piece : pieces) {
        for (std::string& sentence : splitter.Add(piece)) {
            sentences.push_back(std::move(sentence));
        }
    }
    const std::string rest = splitter.Finish();
    if (!rest.empty()) {
        sentences.push_back(rest);
    }
    return sentences;
}

// The ellipsis character, U+2026, and the right double and single quotation marks, U+201D and U+2019, in UTF-8.
const std::string ELLIPSIS = "\xE2\x80\xA6";
const std::string RIGHT_DOUBLE = "\xE2\x80\x9D";
const std::string RIGHT_SINGLE = "\xE2\x80\x99";
const std::string LEFT_DOUBLE = "\xE2\x80\x9C";

} // namespace

TEST_CASE("SentenceSplitter ends a sentence at . ! ? and the ellipsis character, then whitespace",
          "[hosted-server][sentences]")
{
    CHECK(Split({"Lead the way. I will follow! Will you? Wait" + ELLIPSIS + " now."}) ==
          std::vector<std::string>{"Lead the way.", "I will follow!", "Will you?", "Wait" + ELLIPSIS, "now."});
}

TEST_CASE("SentenceSplitter keeps closing quotes and brackets with their sentence", "[hosted-server][sentences]")
{
    CHECK(Split({"\"Go home.\" She left. (Quietly.) Done"}) ==
          std::vector<std::string>{"\"Go home.\"", "She left.", "(Quietly.)", "Done"});
    CHECK(Split({LEFT_DOUBLE + "Run!" + RIGHT_DOUBLE + " he said. It" + RIGHT_SINGLE + "s late."}) ==
          std::vector<std::string>{LEFT_DOUBLE + "Run!" + RIGHT_DOUBLE, "he said.", "It" + RIGHT_SINGLE + "s late."});
}

TEST_CASE("SentenceSplitter takes pieces split anywhere, inside a word or a UTF-8 character", "[hosted-server][sentences]")
{
    CHECK(Split({"Hel", "lo", ".", " ", "Th", "ere\xE2", "\x80", "\xA6 ", "end"}) ==
          std::vector<std::string>{"Hello.", "There" + ELLIPSIS, "end"});
}

TEST_CASE("SentenceSplitter gives a sentence as soon as the whitespace after it arrives", "[hosted-server][sentences]")
{
    server::SentenceSplitter splitter;
    CHECK(splitter.Add("Hello.").empty()); // "Hello.5" would not end there: it waits for what follows.
    CHECK(splitter.Add(" Next") == std::vector<std::string>{"Hello."});
    CHECK(splitter.Finish() == "Next");
    // Finish starts it afresh.
    CHECK(splitter.Add("Again. ") == std::vector<std::string>{"Again."});
    CHECK(splitter.Finish().empty());
}

TEST_CASE("SentenceSplitter does not end at a decimal point, ends after dots, and splits abbreviations",
          "[hosted-server][sentences]")
{
    CHECK(Split({"It costs 3.5 septims... Fine. Ok"}) ==
          std::vector<std::string>{"It costs 3.5 septims...", "Fine.", "Ok"});
    // Abbreviations are not special (documented).
    CHECK(Split({"Ask Mr. Smith."}) == std::vector<std::string>{"Ask Mr.", "Smith."});
}

TEST_CASE("SentenceSplitter ends at a newline and gives no empty sentence", "[hosted-server][sentences]")
{
    CHECK(Split({"One.\nTwo.\r\n"}) == std::vector<std::string>{"One.", "Two."});
    CHECK(Split({"  ", "\n"}).empty());
    CHECK(Split({". . "}) == std::vector<std::string>{".", "."});
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/SentenceSplitter.h'`.

- [ ] **Step 3: Write the splitter**

Create `src/server/SentenceSplitter.h`:

```cpp
#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace morpheus::server {

// Turns a line streamed in pieces into its sentences as soon as each is complete (morpheus_server spec, section 1,
// "Sentences"), so the host's text-to-speech can start on the first one. A sentence ends at '.', '!', '?' or the
// ellipsis character (U+2026), followed by any closing quotes or brackets, then whitespace; the sentence is the text up
// to the closing characters, trimmed. A '.' between digits ends nothing (no whitespace follows it). Pieces may split
// anywhere, inside a word or a UTF-8 character included. Abbreviations ("Mr. Smith") are not special: they end a
// sentence.
class SentenceSplitter {
private:
    std::string _pending = {}; // The text not yet part of a finished sentence.
    std::size_t _scanned = 0;  // How much of _pending has been looked at for a sentence end.

public:
    // Adds the next piece; returns the sentences it completed, in order.
    std::vector<std::string> Add(std::string_view piece);

    // Ends the line: returns what is left, trimmed (the last sentence, or "" when nothing is left), and starts afresh.
    std::string Finish();
};

} // namespace morpheus::server
```

Create `src/server/SentenceSplitter.cpp`:

```cpp
#include "server/SentenceSplitter.h"

#include <algorithm>
#include <array>

#include "core/Text.h"

namespace morpheus::server {

namespace {

// What ends a sentence: '.', '!', '?' and the ellipsis character (U+2026 in UTF-8).
constexpr std::array<std::string_view, 4> ENDINGS = {".", "!", "?", "\xE2\x80\xA6"};
// What may close a sentence after its ending: quotes and brackets, ASCII and UTF-8 (U+201D, U+2019, U+00BB).
constexpr std::array<std::string_view, 8> CLOSERS = {"\"", "'", ")", "]", "}", "\xE2\x80\x9D", "\xE2\x80\x99", "\xC2\xBB"};

} // namespace

std::vector<std::string> SentenceSplitter::Add(std::string_view piece)
{
    _pending += piece;
    std::vector<std::string> sentences;

    // Each whitespace not looked at yet may end a sentence: it does when the text before it, once its closing
    // characters are set aside, ends with an ending. The text before a whitespace never changes, so each is looked at
    // once.
    std::size_t at = _scanned;
    while (at < _pending.size()) {
        const char c = _pending[at];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
            ++at;
            continue;
        }
        std::string_view before = std::string_view(_pending).substr(0, at);
        for (bool closed = true; closed;) {
            closed = false;
            for (const std::string_view closer : CLOSERS) {
                if (before.ends_with(closer)) {
                    before.remove_suffix(closer.size());
                    closed = true;
                    break;
                }
            }
        }
        const bool ends = std::any_of(ENDINGS.begin(), ENDINGS.end(),
                                      [&before](std::string_view ending) { return before.ends_with(ending); });
        if (!ends) {
            ++at;
            continue;
        }
        // A sentence: everything before the whitespace, trimmed. What follows the whitespace is looked at next.
        std::string sentence = core::Trim(std::string_view(_pending).substr(0, at));
        if (!sentence.empty()) {
            sentences.push_back(std::move(sentence));
        }
        _pending.erase(0, at + 1);
        at = 0;
    }
    _scanned = _pending.size();
    return sentences;
}

std::string SentenceSplitter::Finish()
{
    std::string rest = core::Trim(_pending);
    _pending.clear();
    _scanned = 0;
    return rest;
}

} // namespace morpheus::server
```

Add `SentenceSplitter.cpp` to `add_library(morpheus_server_lib STATIC ...)` in `src/server/CMakeLists.txt` (keep the list sorted).

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[sentences]"`
Expected: `All tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/server/SentenceSplitter.h src/server/SentenceSplitter.cpp src/server/CMakeLists.txt tests/server/SentenceSplitterTests.cpp
git commit -m "SentenceSplitter: a streamed line's sentences as soon as each ends"
```

---

### Task 6: `RunWorker`: one run's thread, queue, session and background folding

**Files:**
- Create: `src/server/RunWorker.h`, `src/server/RunWorker.cpp`
- Modify: `src/server/CMakeLists.txt` (add `RunWorker.cpp`)
- Create: `tests/server/ServerTestSupport.h`
- Test: `tests/server/RunWorkerTests.cpp`

**Interfaces:**
- Consumes: `turn::HostedSession` (`Open`, `Maintain`, `Meta`, readers), `turn::HostData`, `turn::HostedWorld(const HostData&)`, `turn::HostDataHash(const HostData&)` (`src/turn/HostedWorld.h`), `server::SlotBackend`, `server::SlotPool::UPKEEP_SLOT`, `server::Log` (Task 4), `inference::InferenceError`.
- Produces:
  - `class server::RunWorker` with `using Job = std::function<void(turn::HostedSession& session, SlotBackend& backend, std::stop_token stop)>;`, constructor `RunWorker(std::string runId, std::filesystem::path file, turn::HostData data, core::Settings settings, std::uint64_t seed, std::unique_ptr<inference::LlmBackend> backend, Log log)`, `const std::string& RunId() const`, `std::future<void> Post(Job job)`, `void Call(Job job)`, `void Reopen(turn::HostData data, std::uint64_t seed)`, `void RequestStop()`.
  - Jobs refused because the worker stops fail with `inference::InferenceError` whose message contains `"the server is stopping"`.
  - `tests/server/ServerTestSupport.h` with `test::WaitUntil` and `test::MakeWorker` (later tasks add to this file).

- [ ] **Step 1: Write the test helpers**

Create `tests/server/ServerTestSupport.h`:

```cpp
#pragma once

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "server/RunWorker.h"

namespace morpheus::test {

// Polls `condition` every 20 ms until it holds or `limit` passes; returns whether it held.
inline bool WaitUntil(const std::function<bool()>& condition,
                      std::chrono::milliseconds limit = std::chrono::milliseconds(10000))
{
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}

// A run worker named `runId` on run file `file`, with open.json's host data, HostedSettings and seed 42, on a
// scripted backend that answers with `responder`. `*ppBackend` gets that backend's address, so a test can reach it
// from a job (on the worker's thread, where the backend is used). Log lines are dropped.
inline std::unique_ptr<server::RunWorker> MakeWorker(const std::string& runId, const std::filesystem::path& file,
                                                     ScriptedBackend::Responder responder,
                                                     ScriptedBackend** ppBackend = nullptr)
{
    auto pBackend = std::make_unique<ScriptedBackend>(16384, std::move(responder));
    if (ppBackend != nullptr) {
        *ppBackend = pBackend.get();
    }
    return std::make_unique<server::RunWorker>(runId, file, FixtureHostData(), HostedSettings(), 42, std::move(pBackend),
                                               [](const std::string&) {});
}

} // namespace morpheus::test
```

- [ ] **Step 2: Write the failing tests**

Create `tests/server/RunWorkerTests.cpp`. Catch2's checks are not thread-safe: a job only records what it saw into variables, and the test checks them after the job's future (or `Call`) has returned.

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "server/RunWorker.h"
#include "server/ServerTestSupport.h"
#include "server/SlotBackend.h"
#include "turn/EngineText.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// A job that does nothing.
void Nothing(turn::HostedSession&, server::SlotBackend&, std::stop_token)
{
}

} // namespace

TEST_CASE("RunWorker opens its run and runs jobs one at a time, in arrival order, on its own thread",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    const std::filesystem::path file = test::HostedRunFile("worker_order");
    std::unique_ptr<server::RunWorker> worker = test::MakeWorker("worker_order", file, script.Responder());
    CHECK(worker->RunId() == "worker_order");
    CHECK(std::filesystem::exists(file));

    std::vector<int> order;
    std::thread::id jobThread;
    std::vector<std::future<void>> done;
    for (int i = 0; i < 5; ++i) {
        done.push_back(worker->Post([&order, &jobThread, i](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
            order.push_back(i);
            jobThread = std::this_thread::get_id();
        }));
    }
    for (std::future<void>& job : done) {
        job.get();
    }
    CHECK(order == std::vector<int>{0, 1, 2, 3, 4});
    CHECK(jobThread != std::this_thread::get_id());
}

TEST_CASE("A job's exception reaches its caller, and the worker goes on", "[hosted-server][worker]")
{
    test::HostedScript script;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_throw", test::HostedRunFile("worker_throw"), script.Responder());
    CHECK_THROWS_AS(worker->Call([](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
                        throw core::DataError("speaker: no such actor");
                    }),
                    core::DataError);
    bool ran = false;
    worker->Call([&ran](turn::HostedSession&, server::SlotBackend&, std::stop_token) { ran = true; });
    CHECK(ran);
}

TEST_CASE("A RunWorker that cannot open its run throws the open's error and leaves no file", "[hosted-server][worker]")
{
    test::HostedScript script;
    const std::filesystem::path file = test::HostedRunFile("worker_bad_data");
    turn::HostData bad = test::FixtureHostData();
    bad.catalog = 42; // Not an action catalog: HostedWorld's DataError, before the file is touched.
    CHECK_THROWS_AS(server::RunWorker("worker_bad_data", file, bad, test::HostedSettings(), 42,
                                      std::make_unique<test::ScriptedBackend>(16384, script.Responder()),
                                      [](const std::string&) {}),
                    core::DataError);
    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("Between jobs the worker folds ended conversations on the upkeep slot; a job's calls use the slot it sets",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Welcome."};
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_fold", test::HostedRunFile("worker_fold"), script.Responder(), &pBackend);

    std::string id;
    bool played = false;
    worker->Call([&](turn::HostedSession& session, server::SlotBackend& backend, std::stop_token stop) {
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        backend.SetSlot(2);
        played = session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, stop).has_value();
        session.EndConversation(id);
    });
    REQUIRE(played);

    // Folding happens in the background once the queue is empty: wait until the conversation's turn is folded.
    const bool folded = test::WaitUntil([&] {
        int through = 0;
        worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            through = session.Conversation(id)->foldedThrough;
        });
        return through == 1;
    });
    CHECK(folded);

    // The turn's two calls ran on slot 2, the fold's on the upkeep slot.
    std::vector<int> slots;
    std::vector<bool> folds;
    worker->Call([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
        for (const test::ScriptedBackend::Call& call : pBackend->Calls()) {
            slots.push_back(call.spec.slot);
            folds.push_back(call.prompt.messages.back().content == turn::SUMMARIZE_INSTRUCTION);
        }
    });
    REQUIRE(slots.size() >= 3);
    CHECK(slots[0] == 2);
    CHECK(slots[1] == 2);
    for (std::size_t i = 2; i < slots.size(); ++i) {
        CHECK(slots[i] == server::SlotPool::UPKEEP_SLOT);
        CHECK(folds[i]);
    }
}

TEST_CASE("A job stops a running fold, and the fold runs again after it", "[hosted-server][worker]")
{
    // The fold's summary streams slowly: 9 pieces, 100 ms each, once the delay is set.
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Welcome."};
    std::atomic<int> foldCalls = 0;
    test::ScriptedBackend::Responder responder = [&](const inference::Prompt& prompt,
                                                     const inference::GenerationSpec& spec) -> std::string {
        if (prompt.messages.back().content == turn::SUMMARIZE_INSTRUCTION) {
            ++foldCalls;
            return "Summary of a long talk at the inn.";
        }
        return script.Responder()(prompt, spec);
    };
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_fold_stop", test::HostedRunFile("worker_fold_stop"), responder, &pBackend);
    std::string id;
    worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token stop) {
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, stop);
        pBackend->SetPieceDelay(std::chrono::milliseconds(100));
        session.EndConversation(id);
    });

    // While the fold streams, a job arrives: the fold stops at its next piece, and the job runs at once.
    REQUIRE(test::WaitUntil([&] { return foldCalls.load() >= 1; }));
    const auto start = std::chrono::steady_clock::now();
    worker->Call([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
        pBackend->SetPieceDelay(std::chrono::milliseconds(0));
    });
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(600));

    // Then the fold runs again, now without the delay, and lands.
    const bool folded = test::WaitUntil([&] {
        int through = 0;
        worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            through = session.Conversation(id)->foldedThrough;
        });
        return through == 1;
    });
    CHECK(folded);
    CHECK(foldCalls.load() >= 2);
}

TEST_CASE("Stopping the worker stops a running turn, which commits nothing, and refuses later jobs",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"A long answer that streams slowly, piece by piece."};
    const std::filesystem::path file = test::HostedRunFile("worker_stop");
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker = test::MakeWorker("worker_stop", file, script.Responder(), &pBackend);

    std::atomic<bool> streaming = false;
    std::string id;
    std::optional<core::HostedTurn> result;
    std::future<void> turn = worker->Post([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token stop) {
        pBackend->SetPieceDelay(std::chrono::milliseconds(100));
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        turn::HostedCallbacks callbacks;
        callbacks.onToken = [&streaming](std::string_view) { streaming = true; };
        result = session.RunTurn(test::Request(id, test::HULDA, "Hello."), callbacks, stop);
    });
    REQUIRE(test::WaitUntil([&] { return streaming.load(); }));
    worker->RequestStop();
    turn.get(); // The job returned: the stop cancelled its turn.
    CHECK_FALSE(result.has_value());
    CHECK_THROWS_AS(worker->Call(Nothing), inference::InferenceError);
    CHECK_THROWS_WITH(worker->Call(Nothing), ContainsSubstring("the server is stopping"));
    worker.reset();

    // Nothing was committed.
    test::ScriptedBackend fresh(16384, script.Responder());
    turn::HostedSession reopened =
        turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, fresh);
    CHECK(reopened.Turns(id).empty());
}

TEST_CASE("Reopen: the same data changes nothing, changed data applies, and a failed reopen leaves the run as it was",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_reopen", test::HostedRunFile("worker_reopen"), script.Responder(), &pBackend);
    // What the run looks like now: its stored host data's hash, and whether conversation `id` is still there.
    std::string id;
    auto look = [&worker, &id] {
        std::pair<std::string, bool> seen;
        worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            seen = {session.Meta().worldHash, session.Conversation(id).has_value()};
        });
        return seen;
    };
    worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    });
    const std::string original = turn::HostDataHash(test::FixtureHostData());
    CHECK(look() == std::pair<std::string, bool>{original, true});

    // The same data: nothing changes.
    worker->Reopen(test::FixtureHostData(), 42);
    CHECK(look() == std::pair<std::string, bool>{original, true});

    // Changed data (a mod update): the run reopens with it and keeps its conversations.
    turn::HostData changed = test::FixtureHostData();
    changed.lore.push_back({"The mill", "The mill burned down last winter.", false});
    worker->Reopen(changed, 42);
    CHECK(look() == std::pair<std::string, bool>{turn::HostDataHash(changed), true});

    // Bad data: HostedWorld's DataError, and the open run is untouched.
    turn::HostData bad = changed;
    bad.catalog = 42;
    CHECK_THROWS_AS(worker->Reopen(bad, 42), core::DataError);
    CHECK(look() == std::pair<std::string, bool>{turn::HostDataHash(changed), true});

    // Data whose open fails (its new lore cannot be embedded): the error, and the run opens again as it was.
    worker->Call([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) { pBackend->SetEmbedLimit(64); });
    turn::HostData unembeddable = changed;
    unembeddable.lore.push_back({"The road", std::string(200, 'z'), false});
    CHECK_THROWS_AS(worker->Reopen(unembeddable, 42), inference::InferenceError);
    CHECK(look() == std::pair<std::string, bool>{turn::HostDataHash(changed), true});
}
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/RunWorker.h'`.

- [ ] **Step 4: Write `RunWorker`**

Create `src/server/RunWorker.h`:

```cpp
#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>

#include "core/Settings.h"
#include "inference/Llm.h"
#include "server/ServerLog.h"
#include "server/SlotBackend.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

namespace morpheus::server {

// One opened run of morpheus_server (spec, section 2): a thread of its own that owns the run's HostedSession and its
// own backend, and runs the run's jobs one at a time, in arrival order. While no job waits, it folds the run's ended
// conversations in the background (HostedSession::Maintain, on the upkeep slot); a new job stops that, and it resumes
// once the queue is empty again. HostedSession's contract (one caller at a time) holds because only this thread calls
// it. Every public method is thread-safe.
class RunWorker {
public:
    // A job: the run's session, the backend its calls go through (the job sets the slot), and a stop token that fires
    // when the worker stops.
    using Job = std::function<void(turn::HostedSession& session, SlotBackend& backend, std::stop_token stop)>;

private:
    // A queued piece of work and its completion. Post's jobs need the session; Reopen's work replaces it.
    struct Entry {
        std::function<void(std::stop_token)> work = {};
        std::promise<void> done = {};
    };

    std::string _runId = {};
    std::filesystem::path _file = {};
    core::Settings _settings = {};
    turn::HostData _data = {}; // The host data the session was last opened with.
    std::uint64_t _seed = 0;
    std::unique_ptr<inference::LlmBackend> _pModels = nullptr;
    SlotBackend _backend; // Wraps *_pModels; set by the constructor.
    Log _log = {};
    std::optional<turn::HostedSession> _session = {}; // Empty only while closed after a failed Reopen.
    std::mutex _mutex = {};                           // Guards the members below.
    std::condition_variable_any _wake = {};
    std::deque<Entry> _queue = {};
    bool _maintenanceDue = true;           // Folding may have work: set after each job, cleared once Maintain is done.
    std::stop_source _maintenanceStop = {}; // Stops the running Maintain; a new job and RequestStop request it.
    bool _stopping = false;
    std::jthread _thread = {}; // Last: joined first on destruction, before the session and the backend go.

public:
    // Opens the run on the worker's thread (HostedSession::Open on `file` with `data`, `settings` and `seed`, through
    // `backend`) and waits for it: Open's exception (DataError, StoreError, InferenceError) is rethrown here, and no
    // worker is left. `log` gets the worker's lines (folding failures). Throws ContractError for a null backend or an
    // empty log.
    RunWorker(std::string runId, std::filesystem::path file, turn::HostData data, core::Settings settings,
              std::uint64_t seed, std::unique_ptr<inference::LlmBackend> backend, Log log);
    // Stops (RequestStop) and joins the thread.
    ~RunWorker();
    RunWorker(const RunWorker&) = delete;
    RunWorker& operator=(const RunWorker&) = delete;

    const std::string& RunId() const;

    // Queues `job` after the jobs before it and returns its completion: the future rethrows what the job threw. A run
    // left closed by a failed Reopen fails the job with InferenceError naming the run. After RequestStop the job never
    // runs, and the future holds InferenceError "the server is stopping".
    std::future<void> Post(Job job);

    // Post, then wait: rethrows what the job threw.
    void Call(Job job);

    // Opens the run again with `data`, on the worker's thread after the queued jobs, and waits. The same host data
    // (equal content hash) changes nothing. Changed data is checked first (HostedWorld's DataError; the open run is
    // untouched); then the session closes and the run opens again (HostedSession::Open re-embeds the changed lore,
    // then replaces the stored host data). If that open fails, the run opens again with its previous data and the
    // error is rethrown; if even that fails, the run stays closed (logged) until a Reopen succeeds. `seed` matters only
    // when the file is missing.
    void Reopen(turn::HostData data, std::uint64_t seed);

    // Stops the worker without waiting: the running job's stop token and a running Maintain fire, and queued and later
    // jobs fail with InferenceError "the server is stopping". The destructor joins.
    void RequestStop();

private:
    // Queues `work` and returns its completion; refused (InferenceError "the server is stopping") after RequestStop.
    std::future<void> Enqueue(std::function<void(std::stop_token)> work);
};

} // namespace morpheus::server
```

Create `src/server/RunWorker.cpp`:

```cpp
#include "server/RunWorker.h"

#include <exception>
#include <format>
#include <string_view>
#include <utility>

#include "core/Errors.h"
#include "server/SlotPool.h"

namespace morpheus::server {

namespace {

// What a job refused by a stopping worker fails with.
constexpr std::string_view STOPPING_MESSAGE = "the server is stopping";

} // namespace

RunWorker::RunWorker(std::string runId, std::filesystem::path file, turn::HostData data, core::Settings settings,
                     std::uint64_t seed, std::unique_ptr<inference::LlmBackend> backend, Log log)
    : _runId(std::move(runId))
    , _file(std::move(file))
    , _settings(std::move(settings))
    , _data(std::move(data))
    , _seed(seed)
    , _pModels(std::move(backend))
    , _backend([this]() -> inference::LlmBackend& {
        MORPHEUS_REQUIRE(_pModels != nullptr, "RunWorker: a run needs a backend");
        return *_pModels;
    }())
    , _log(std::move(log))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_log), "RunWorker: a run needs a log");

    // The run opens on the worker's own thread, which then serves it; the constructor waits for the open.
    std::promise<void> opened;
    std::future<void> openedResult = opened.get_future();
    _thread = std::jthread([this, opened = std::move(opened)](std::stop_token stop) mutable {
        try {
            _session.emplace(turn::HostedSession::Open(_file, _data, _settings, _seed, _backend));
            opened.set_value();
        } catch (...) {
            opened.set_exception(std::current_exception());
            return;
        }

        for (;;) {
            // The next job, or folding when no job waits and folding may have work; nothing until one or the other.
            std::optional<Entry> entry;
            std::stop_token maintenanceToken;
            {
                std::unique_lock lock(_mutex);
                _wake.wait(lock, stop, [this] { return !_queue.empty() || _maintenanceDue; });
                if (stop.stop_requested()) {
                    return; // RequestStop failed the queued jobs; Post refuses new ones.
                }
                if (!_queue.empty()) {
                    entry.emplace(std::move(_queue.front()));
                    _queue.pop_front();
                } else {
                    _maintenanceStop = std::stop_source();
                    maintenanceToken = _maintenanceStop.get_token();
                }
            }

            if (!entry) {
                // Folding, on the upkeep slot. A new job or RequestStop stops it; it then runs again once the queue is
                // empty. A failure (the model unreachable, a summary cap too small) must not stop the run's turns: it
                // is logged, and folding is tried again after the next job.
                bool finished = true;
                if (_session) {
                    _backend.SetSlot(SlotPool::UPKEEP_SLOT);
                    try {
                        finished = _session->Maintain(maintenanceToken);
                    } catch (const std::exception& error) {
                        _log(std::format("run {}: background folding failed: {}", _runId, error.what()));
                    }
                }
                if (finished) {
                    std::scoped_lock lock(_mutex);
                    _maintenanceDue = false;
                }
                continue;
            }

            // A job: run it and report its end; afterwards folding looks again.
            try {
                entry->work(stop);
                entry->done.set_value();
            } catch (...) {
                entry->done.set_exception(std::current_exception());
            }
            std::scoped_lock lock(_mutex);
            _maintenanceDue = true;
        }
    });
    openedResult.get(); // Rethrows Open's exception; the thread has returned, and _thread's destructor joins it.
}

RunWorker::~RunWorker()
{
    RequestStop();
}

const std::string& RunWorker::RunId() const
{
    return _runId;
}

std::future<void> RunWorker::Post(Job job)
{
    MORPHEUS_REQUIRE(static_cast<bool>(job), "RunWorker::Post: no job");
    // A job needs the session; a run left closed by a failed Reopen has none.
    return Enqueue([this, job = std::move(job)](std::stop_token stop) {
        if (!_session) {
            throw inference::InferenceError(
                std::format("run '{}' is closed: it could not be opened again after a failed reopen", _runId));
        }
        job(*_session, _backend, stop);
    });
}

void RunWorker::Call(Job job)
{
    Post(std::move(job)).get();
}

void RunWorker::Reopen(turn::HostData data, std::uint64_t seed)
{
    Enqueue([this, data = std::move(data), seed](std::stop_token) mutable {
        // Changed data is checked before the open run is touched; the same data changes nothing.
        const turn::HostedWorld check(data);
        if (_session && turn::HostDataHash(data) == _session->Meta().worldHash) {
            return;
        }
        // One session per run file: the open one closes before the run opens again.
        _session.reset();
        try {
            _session.emplace(turn::HostedSession::Open(_file, data, _settings, seed, _backend));
            _data = std::move(data);
        } catch (...) {
            // The new data could not open the run: open it again as it was, and report the new data's error.
            try {
                _session.emplace(turn::HostedSession::Open(_file, _data, _settings, _seed, _backend));
            } catch (const std::exception& error) {
                _log(std::format("run {}: cannot open again with its previous host data: {}", _runId, error.what()));
            }
            throw;
        }
    }).get();
}

void RunWorker::RequestStop()
{
    // Refuse what is queued and what comes later, stop a running Maintain, then the thread: its stop token reaches the
    // running job.
    std::deque<Entry> refused;
    {
        std::scoped_lock lock(_mutex);
        _stopping = true;
        _maintenanceStop.request_stop();
        refused.swap(_queue);
    }
    for (Entry& entry : refused) {
        entry.done.set_exception(std::make_exception_ptr(inference::InferenceError(std::string(STOPPING_MESSAGE))));
    }
    _thread.request_stop();
}

std::future<void> RunWorker::Enqueue(std::function<void(std::stop_token)> work)
{
    Entry entry;
    entry.work = std::move(work);
    std::future<void> done = entry.done.get_future();
    {
        std::scoped_lock lock(_mutex);
        if (_stopping) {
            entry.done.set_exception(std::make_exception_ptr(inference::InferenceError(std::string(STOPPING_MESSAGE))));
            return done;
        }
        _queue.push_back(std::move(entry));
        // Work comes first: a running Maintain stops now, and runs again once the queue is empty.
        _maintenanceStop.request_stop();
    }
    _wake.notify_one();
    return done;
}

} // namespace morpheus::server
```

Notes for the implementer:
- `std::condition_variable_any::wait(lock, stop, predicate)` returns when the predicate holds or the stop is requested; `_thread.request_stop()` wakes it.
- The constructor's member initializer order must match the declaration order (`-Wreorder` is an error).
- If `HostedSession` has no move assignment, `std::optional::emplace` (used above) is still fine: it destroys and constructs in place. `HostedSession::Open` returns by value.

Add `RunWorker.cpp` to `src/server/CMakeLists.txt`.

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[worker]"`
Expected: `All tests passed`. Run it three more times to check that the timing test is steady: `for i in 1 2 3; do ./build/tests/bin/Debug/morpheus_tests.exe "[worker]" | tail -1; done`.

- [ ] **Step 6: Commit**

```bash
git add src/server/RunWorker.h src/server/RunWorker.cpp src/server/CMakeLists.txt tests/server/ServerTestSupport.h tests/server/RunWorkerTests.cpp
git commit -m "RunWorker: a run's own thread, job queue and session, folding in the background between jobs"
```

---

### Task 7: `RunRegistry`: run ids, run files and the workers by id

**Files:**
- Create: `src/server/RunRegistry.h`, `src/server/RunRegistry.cpp`
- Modify: `src/server/CMakeLists.txt` (add `RunRegistry.cpp`)
- Test: `tests/server/RunRegistryTests.cpp`

**Interfaces:**
- Consumes: `server::RunWorker` (Task 6), `server::Log`, `core::DataError`, `store::StoreError` (`src/store/Sqlite.h`), `core::PathText` (`src/core/Text.h`).
- Produces: `class server::RunRegistry` with `using BackendFactory = std::function<std::unique_ptr<inference::LlmBackend>()>;`, `static constexpr std::size_t MAX_RUN_ID_LENGTH = 64;`, `static constexpr std::string_view RUN_FILE_EXTENSION = ".morpheus";`, `RunRegistry(std::filesystem::path runsDir, core::Settings settings, BackendFactory makeBackend, Log log)`, `~RunRegistry()`, `static void CheckRunId(std::string_view runId)`, `std::filesystem::path RunFile(std::string_view runId) const`, `bool Open(const std::string& runId, turn::HostData data, std::uint64_t seed)`, `RunWorker* Find(std::string_view runId) const`, `void StopAll()`.

- [ ] **Step 1: Write the failing tests**

Create `tests/server/RunRegistryTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <memory>
#include <stop_token>
#include <string>

#include "TestPaths.h"
#include "core/Errors.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "server/RunRegistry.h"
#include "server/RunWorker.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// A registry of runs in `runsDir` on scripted backends answering with `script`.
server::RunRegistry MakeRegistry(const std::filesystem::path& runsDir, test::HostedScript& script)
{
    return server::RunRegistry(
        runsDir, test::HostedSettings(),
        [&script] { return std::make_unique<test::ScriptedBackend>(16384, script.Responder()); },
        [](const std::string&) {});
}

} // namespace

TEST_CASE("RunRegistry accepts run ids of 1 to 64 letters, digits, '_' and '-'", "[hosted-server][registry]")
{
    CHECK_NOTHROW(server::RunRegistry::CheckRunId("skyrim_save-01"));
    CHECK_NOTHROW(server::RunRegistry::CheckRunId(std::string(64, 'a')));
    for (const std::string bad : {std::string(), std::string(65, 'a'), std::string("a/b"), std::string("a.b"),
                                  std::string(".."), std::string("a b")}) {
        INFO("'" << bad << "'");
        CHECK_THROWS_AS(server::RunRegistry::CheckRunId(bad), core::DataError);
        CHECK_THROWS_WITH(server::RunRegistry::CheckRunId(bad), ContainsSubstring("run_id"));
    }
}

TEST_CASE("RunRegistry creates its runs folder and names each run's file after its id", "[hosted-server][registry]")
{
    test::HostedScript script;
    const std::filesystem::path runsDir = test::FreshTempDir("registry_dir") / "nested" / "runs";
    server::RunRegistry registry = MakeRegistry(runsDir, script);
    CHECK(std::filesystem::is_directory(runsDir));
    CHECK(registry.RunFile("save_1") == runsDir / "save_1.morpheus");
    CHECK_THROWS_AS(registry.RunFile("../x"), core::DataError);

    // A runs folder that is a file cannot be made.
    const std::filesystem::path file = test::FreshTempDir("registry_file") / "runs";
    test::WriteFile(file, "not a folder");
    CHECK_THROWS_AS(MakeRegistry(file, script), core::DataError);
    CHECK_THROWS_WITH(MakeRegistry(file, script), ContainsSubstring("runs"));
}

TEST_CASE("RunRegistry opens a run once per process and reopens it on its worker; a restart finds the file",
          "[hosted-server][registry]")
{
    test::HostedScript script;
    const std::filesystem::path runsDir = test::FreshTempDir("registry_open") / "runs";
    {
        server::RunRegistry registry = MakeRegistry(runsDir, script);
        CHECK(registry.Find("save_1") == nullptr);
        CHECK(registry.Open("save_1", test::FixtureHostData(), 7));
        CHECK(std::filesystem::exists(runsDir / "save_1.morpheus"));
        server::RunWorker* pWorker = registry.Find("save_1");
        REQUIRE(pWorker != nullptr);
        CHECK(pWorker->RunId() == "save_1");
        // Again: not created, the same worker.
        CHECK_FALSE(registry.Open("save_1", test::FixtureHostData(), 7));
        CHECK(registry.Find("save_1") == pWorker);
        // The seed went to the new run.
        std::uint64_t seed = 0;
        pWorker->Call([&seed](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            seed = session.Meta().rngSeed;
        });
        CHECK(seed == 7);
    }
    // A new process: the file is there, so it is opened, not created.
    server::RunRegistry restarted = MakeRegistry(runsDir, script);
    CHECK(restarted.Find("save_1") == nullptr);
    CHECK_FALSE(restarted.Open("save_1", test::FixtureHostData(), 7));
    CHECK(restarted.Find("save_1") != nullptr);
}

TEST_CASE("RunRegistry registers nothing when a run cannot open", "[hosted-server][registry]")
{
    test::HostedScript script;
    const std::filesystem::path runsDir = test::FreshTempDir("registry_fail") / "runs";
    SECTION("bad host data")
    {
        server::RunRegistry registry = MakeRegistry(runsDir, script);
        turn::HostData bad = test::FixtureHostData();
        bad.catalog = 42;
        CHECK_THROWS_AS(registry.Open("save_1", bad, 0), core::DataError);
        CHECK(registry.Find("save_1") == nullptr);
        CHECK_FALSE(std::filesystem::exists(runsDir / "save_1.morpheus"));
    }
    SECTION("no backend")
    {
        server::RunRegistry registry(
            runsDir, test::HostedSettings(),
            []() -> std::unique_ptr<inference::LlmBackend> {
                throw inference::InferenceError("cannot reach http://127.0.0.1:1/props");
            },
            [](const std::string&) {});
        CHECK_THROWS_AS(registry.Open("save_1", test::FixtureHostData(), 0), inference::InferenceError);
        CHECK(registry.Find("save_1") == nullptr);
    }
}

TEST_CASE("RunRegistry::StopAll stops every worker: their later jobs are refused", "[hosted-server][registry]")
{
    test::HostedScript script;
    server::RunRegistry registry = MakeRegistry(test::FreshTempDir("registry_stop") / "runs", script);
    REQUIRE(registry.Open("a", test::FixtureHostData(), 0));
    REQUIRE(registry.Open("b", test::FixtureHostData(), 0));
    registry.StopAll();
    for (const char* runId : {"a", "b"}) {
        server::RunWorker* pWorker = registry.Find(runId);
        REQUIRE(pWorker != nullptr);
        CHECK_THROWS_WITH(pWorker->Call([](turn::HostedSession&, server::SlotBackend&, std::stop_token) {}),
                          ContainsSubstring("the server is stopping"));
    }
}
```

`RunRegistry` holds mutexes, so it is not movable; `MakeRegistry` returning by value relies on guaranteed copy elision (C++17), which holds for a prvalue `return server::RunRegistry(...)`. If the compiler still objects, change `MakeRegistry` to return `std::unique_ptr<server::RunRegistry>` and adapt the tests.

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/RunRegistry.h'`.

- [ ] **Step 3: Write `RunRegistry`**

Create `src/server/RunRegistry.h`:

```cpp
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "core/Settings.h"
#include "inference/Llm.h"
#include "server/RunWorker.h"
#include "server/ServerLog.h"
#include "turn/HostedWorld.h"

namespace morpheus::server {

// The runs morpheus_server has opened (spec, section 2): each run id's file in the runs folder and its worker. A run
// stays open until the registry is destroyed. Thread-safe.
class RunRegistry {
public:
    // Makes each run's own backend (backends are not thread-safe, so runs do not share one).
    using BackendFactory = std::function<std::unique_ptr<inference::LlmBackend>()>;

    static constexpr std::size_t MAX_RUN_ID_LENGTH = 64;
    static constexpr std::string_view RUN_FILE_EXTENSION = ".morpheus";

private:
    std::filesystem::path _runsDir = {};
    core::Settings _settings = {};
    BackendFactory _makeBackend = {};
    Log _log = {};
    std::mutex _openMutex = {};     // One open at a time: two opens of one new run make one worker.
    mutable std::mutex _mutex = {}; // Guards _runs.
    std::map<std::string, std::unique_ptr<RunWorker>, std::less<>> _runs = {};

public:
    // Creates `runsDir` when it is missing; DataError naming it when it cannot. Throws ContractError for an empty
    // factory or log.
    RunRegistry(std::filesystem::path runsDir, core::Settings settings, BackendFactory makeBackend, Log log);
    // Stops every worker (StopAll), then joins them.
    ~RunRegistry();
    RunRegistry(const RunRegistry&) = delete;
    RunRegistry& operator=(const RunRegistry&) = delete;

    // Throws DataError "run_id: ..." unless `runId` is 1 to MAX_RUN_ID_LENGTH characters of A-Z, a-z, 0-9, '_' and '-'.
    static void CheckRunId(std::string_view runId);

    // "<runs dir>/<run_id>.morpheus". Checks the id first (CheckRunId).
    std::filesystem::path RunFile(std::string_view runId) const;

    // Opens run `runId` with `data`. The first open in this process makes its worker, which creates the file (with
    // `seed` for sampling) or reopens it; a later open reopens it on its worker (RunWorker::Reopen). Returns whether the
    // file was created. Throws what the open throws (DataError, StoreError, InferenceError, including the backend
    // factory's); nothing is registered then. Opens take turns, so a long open (lore to embed) delays other opens, not
    // other runs' requests.
    bool Open(const std::string& runId, turn::HostData data, std::uint64_t seed);

    // The worker of a run opened in this process, or nullptr. It lives until the registry is destroyed.
    RunWorker* Find(std::string_view runId) const;

    // Stops every worker without waiting (RunWorker::RequestStop): running turns and folds stop, and later jobs fail.
    void StopAll();
};

} // namespace morpheus::server
```

Create `src/server/RunRegistry.cpp`:

```cpp
#include "server/RunRegistry.h"

#include <algorithm>
#include <format>
#include <system_error>
#include <utility>

#include "core/Errors.h"
#include "core/Text.h"
#include "store/Sqlite.h"

namespace morpheus::server {

RunRegistry::RunRegistry(std::filesystem::path runsDir, core::Settings settings, BackendFactory makeBackend, Log log)
    : _runsDir(std::move(runsDir))
    , _settings(std::move(settings))
    , _makeBackend(std::move(makeBackend))
    , _log(std::move(log))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_makeBackend) && static_cast<bool>(_log),
                     "RunRegistry: a backend factory and a log are needed");
    // The runs folder is the operator's choice (--runs): one that cannot be made is a configuration error.
    std::error_code error;
    std::filesystem::create_directories(_runsDir, error);
    if (error || !std::filesystem::is_directory(_runsDir)) {
        throw core::DataError(std::format("runs folder '{}': {}", core::PathText(_runsDir),
                                          error ? error.message() : std::string("it is not a folder")));
    }
}

RunRegistry::~RunRegistry()
{
    // Every worker stops at once; then _runs' destruction joins them one by one.
    StopAll();
}

void RunRegistry::CheckRunId(std::string_view runId)
{
    const bool allowed = std::all_of(runId.begin(), runId.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
    });
    if (runId.empty() || runId.size() > MAX_RUN_ID_LENGTH || !allowed) {
        throw core::DataError(std::format("run_id: '{}' is not 1 to {} characters of A-Z, a-z, 0-9, '_' and '-'", runId,
                                          MAX_RUN_ID_LENGTH));
    }
}

std::filesystem::path RunRegistry::RunFile(std::string_view runId) const
{
    CheckRunId(runId);
    return _runsDir / (std::string(runId) + std::string(RUN_FILE_EXTENSION));
}

bool RunRegistry::Open(const std::string& runId, turn::HostData data, std::uint64_t seed)
{
    const std::filesystem::path file = RunFile(runId);
    std::scoped_lock opening(_openMutex);

    // A run opened in this process reopens on its own worker, after its queued jobs.
    if (RunWorker* pWorker = Find(runId); pWorker != nullptr) {
        pWorker->Reopen(std::move(data), seed);
        return false;
    }

    // Otherwise it gets a worker, which opens the file or creates it. Nothing is registered when that throws.
    std::error_code error;
    const bool existed = std::filesystem::exists(file, error);
    if (error) {
        throw store::StoreError(std::format("cannot check '{}': {}", core::PathText(file), error.message()));
    }
    auto pWorker = std::make_unique<RunWorker>(runId, file, std::move(data), _settings, seed, _makeBackend(), _log);
    std::scoped_lock lock(_mutex);
    _runs.emplace(runId, std::move(pWorker));
    return !existed;
}

RunWorker* RunRegistry::Find(std::string_view runId) const
{
    std::scoped_lock lock(_mutex);
    const auto it = _runs.find(runId);
    return it == _runs.end() ? nullptr : it->second.get();
}

void RunRegistry::StopAll()
{
    std::scoped_lock lock(_mutex);
    for (auto& [runId, pWorker] : _runs) {
        pWorker->RequestStop();
    }
}

} // namespace morpheus::server
```

Add `RunRegistry.cpp` to `src/server/CMakeLists.txt`.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[registry]"`
Expected: `All tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/server/RunRegistry.h src/server/RunRegistry.cpp src/server/CMakeLists.txt tests/server/RunRegistryTests.cpp
git commit -m "RunRegistry: run ids, run files, and one worker per opened run"
```

---

### Task 8: `TurnStream` and the `Api`: every route but the turn

**Files:**
- Create: `src/server/TurnStream.h`, `src/server/TurnStream.cpp`, `src/server/Api.h`, `src/server/Api.cpp`
- Modify: `src/server/CMakeLists.txt` (add `Api.cpp`, `TurnStream.cpp`)
- Modify: `tests/server/ServerTestSupport.h` (add `ApiHarness`, `ReadAll`)
- Test: `tests/server/ApiTests.cpp`, `tests/server/TurnStreamTests.cpp`

**Interfaces:**
- Consumes: `server::RunRegistry`, `server::RunWorker` (Tasks 6, 7), `server::SlotPool`, `server::Log` (Task 4), `turn::HostDataFromJson`, `turn::HostedSession` (`StartConversation`, `RecordEvents`, `SetParticipants`, `EndConversation`, `Conversation`, `Turns`, `Events`), `turn::TurnError` (`src/turn/MemoryUpkeep.h`).
- Produces:
  - `struct server::StreamEvent { std::string name; nlohmann::json data; }` and `class server::TurnStream` with `void Push(StreamEvent event)`, `void End()`, `std::optional<StreamEvent> Next(std::chrono::milliseconds wait)`, `bool Ended() const`, `bool WaitStarted(std::chrono::milliseconds wait)`, `std::optional<StreamEvent> Front() const`, `void Cancel()`, `std::stop_token StopToken() const`.
  - `inline constexpr std::string_view server::API_VERSION = "1.0.0";`, `struct server::HealthInfo { std::string chatModel; std::string embeddingModel; int slots; }`, `struct server::ApiResponse { int status; nlohmann::json body; std::shared_ptr<TurnStream> stream; }`, `class server::Api` with `Api(RunRegistry& runs, SlotPool& slots, inference::LlmBackend& utility, HealthInfo health, Log log)` and `ApiResponse Handle(std::string_view method, std::string_view path, std::string_view body)`.
  - In `Api.cpp`'s anonymous namespace, for Task 9: `class NotFound`, `class NotOpened`, `int StatusOf(const std::exception&)`, `void RequireConversation(turn::HostedSession&, std::string_view)`, `nlohmann::json ActionJson(const core::Adjudication&)`, `std::string SlotKey(std::string_view runId, std::string_view conversationId)`, `std::string RequiredString(const nlohmann::json&, std::string_view)`, `std::vector<std::string> StringList(const nlohmann::json&, std::string_view)`, `std::vector<nlohmann::json> ObjectList(const nlohmann::json&, std::string_view)`, `const nlohmann::json& RequiredObject(const nlohmann::json&, std::string_view)`.
  - Test helpers `test::ApiHarness` and `test::ReadAll`.

- [ ] **Step 1: Write the test helpers**

Add to `tests/server/ServerTestSupport.h` (new includes: `<atomic>`, `<mutex>`, `<optional>`, `<string_view>`, `<vector>`, `<nlohmann/json.hpp>`, `"TestPaths.h"`, `"inference/Llm.h"`, `"server/Api.h"`, `"server/RunRegistry.h"`, `"server/SlotPool.h"`, `"server/TurnStream.h"`):

```cpp
// Every event of `stream` until it ends, waiting at most 10 s in all.
inline std::vector<server::StreamEvent> ReadAll(server::TurnStream& stream)
{
    std::vector<server::StreamEvent> events;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!stream.Ended() && std::chrono::steady_clock::now() < deadline) {
        if (std::optional<server::StreamEvent> event = stream.Next(std::chrono::milliseconds(50))) {
            events.push_back(std::move(*event));
        }
    }
    return events;
}

// An Api over runs in a fresh folder for test `name`: every run's backend is a scripted one answering with
// `responder` (pieceDelay before each streamed piece; backendDown makes new run backends fail to start), the utility
// backend counts and embeds as ScriptedBackend does, and the pool has 3 slots. Log lines are kept.
struct ApiHarness {
    std::filesystem::path runsDir = {};
    ScriptedBackend::Responder responder = {};
    std::chrono::milliseconds pieceDelay = std::chrono::milliseconds(0);
    std::atomic<bool> backendDown = false;
    std::mutex mutex = {};                     // Guards logLines and backends.
    std::vector<std::string> logLines = {};
    std::vector<ScriptedBackend*> backends = {}; // Each run's backend, in the order the runs opened.
    ScriptedBackend utility{16384};
    server::SlotPool slots{3};
    server::RunRegistry runs;
    server::Api api;

    ApiHarness(std::string_view name, ScriptedBackend::Responder responderIn)
        : runsDir(FreshTempDir(name) / "runs")
        , responder(std::move(responderIn))
        , runs(runsDir, HostedSettings(), [this] { return MakeBackend(); }, [this](const std::string& line) { Log(line); })
        , api(runs, slots, utility, {"chat-model", "embed-model", 3}, [this](const std::string& line) { Log(line); })
    {
    }

    std::unique_ptr<inference::LlmBackend> MakeBackend()
    {
        if (backendDown) {
            throw inference::InferenceError("cannot reach http://127.0.0.1:1/props");
        }
        auto pBackend = std::make_unique<ScriptedBackend>(16384, responder);
        pBackend->SetPieceDelay(pieceDelay);
        std::scoped_lock lock(mutex);
        backends.push_back(pBackend.get());
        return pBackend;
    }

    void Log(const std::string& line)
    {
        std::scoped_lock lock(mutex);
        logLines.push_back(line);
    }

    server::ApiResponse Post(const std::string& path, const nlohmann::json& body)
    {
        return api.Handle("POST", path, body.dump());
    }

    server::ApiResponse Get(const std::string& path)
    {
        return api.Handle("GET", path, "");
    }
};
```

- [ ] **Step 2: Write the failing tests**

Create `tests/server/TurnStreamTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <optional>
#include <thread>

#include "core/Errors.h"
#include "server/TurnStream.h"

using namespace morpheus;
using namespace std::chrono_literals;

TEST_CASE("TurnStream hands events over in order, then says it ended", "[hosted-server][stream]")
{
    server::TurnStream stream;
    CHECK_FALSE(stream.WaitStarted(10ms));
    CHECK_FALSE(stream.Front().has_value());
    std::jthread worker([&stream] {
        stream.Push({"action", {{"speaker", "a"}}});
        stream.Push({"token", {{"text", "Hi"}}});
        stream.End();
    });
    CHECK(stream.WaitStarted(5000ms));
    REQUIRE(stream.Front().has_value());
    CHECK(stream.Front()->name == "action"); // Front does not take it.
    std::optional<server::StreamEvent> first = stream.Next(5000ms);
    REQUIRE(first);
    CHECK(first->name == "action");
    worker.join();
    std::optional<server::StreamEvent> second = stream.Next(5000ms);
    REQUIRE(second);
    CHECK(second->data.at("text") == "Hi");
    CHECK(stream.Ended());
    CHECK_FALSE(stream.Next(10ms).has_value());
    CHECK_THROWS_AS(stream.Push({"token", {}}), core::ContractError);
}

TEST_CASE("TurnStream::Cancel fires its stop token", "[hosted-server][stream]")
{
    server::TurnStream stream;
    const std::stop_token token = stream.StopToken();
    CHECK_FALSE(token.stop_requested());
    stream.Cancel();
    CHECK(token.stop_requested());
}
```

Create `tests/server/ApiTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "hosted/HostedTestSupport.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// The body of a request starting a conversation of Hulda and Aela in observation.json.
nlohmann::json HuldaAndAela()
{
    return {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}};
}

// Checks that `response` is the error `status` with a message containing `part`, and the API version.
void CheckError(const server::ApiResponse& response, int status, const std::string& part)
{
    INFO(response.body.dump());
    CHECK(response.stream == nullptr);
    CHECK(response.status == status);
    CHECK(response.body.at("api_version") == "1.0.0");
    CHECK(response.body.at("error").at("status") == status);
    CHECK_THAT(response.body.at("error").at("message").get<std::string>(), ContainsSubstring(part));
}

} // namespace

TEST_CASE("/health reports the models, the slots and the API version", "[hosted-server][api]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_health", script.Responder());
    const server::ApiResponse health = harness.Get("/health");
    CHECK(health.status == 200);
    CHECK(health.body == nlohmann::json{{"ready", true},
                                        {"chat_model", "chat-model"},
                                        {"embedding_model", "embed-model"},
                                        {"slots", 3},
                                        {"api_version", "1.0.0"}});
}

TEST_CASE("Opening a run creates its file once; opening it again is idempotent; a seed goes to a new run",
          "[hosted-server][api]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_open", script.Responder());
    const nlohmann::json open = test::ReadFixture("open.json");

    const server::ApiResponse first = harness.Post("/v1/runs/save_1/open", open);
    CHECK(first.status == 200);
    CHECK(first.body == nlohmann::json{{"run_id", "save_1"}, {"created", true}, {"api_version", "1.0.0"}});
    CHECK(std::filesystem::exists(harness.runsDir / "save_1.morpheus"));
    const server::ApiResponse second = harness.Post("/v1/runs/save_1/open", open);
    CHECK(second.status == 200);
    CHECK(second.body.at("created") == false);

    nlohmann::json seeded = open;
    seeded["seed"] = 7;
    REQUIRE(harness.Post("/v1/runs/save_2/open", seeded).status == 200);
    std::uint64_t seed = 0;
    harness.runs.Find("save_2")->Call([&seed](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
        seed = session.Meta().rngSeed;
    });
    CHECK(seed == 7);
    nlohmann::json negative = open;
    negative["seed"] = -1;
    CheckError(harness.Post("/v1/runs/save_3/open", negative), 400, "seed");
}

TEST_CASE("A conversation starts, records events, changes participants, ends, and reads back", "[hosted-server][api]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_conversation", script.Responder());
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);

    const server::ApiResponse started = harness.Post("/v1/runs/save_1/conversations", HuldaAndAela());
    REQUIRE(started.status == 200);
    CHECK(started.body == nlohmann::json{{"conversation_id", "c1"}, {"api_version", "1.0.0"}});
    const std::string base = "/v1/runs/save_1/conversations/c1";

    const server::ApiResponse recorded =
        harness.Post(base + "/events", {{"events", {{{"type", "ambient"}, {"text", "A door opens."}}}}});
    CHECK(recorded.status == 200);
    CHECK(recorded.body == nlohmann::json{{"api_version", "1.0.0"}});

    const server::ApiResponse joined =
        harness.Post(base + "/participants", {{"joined", {test::SAADIA}}, {"observation", test::FixtureObservation()}});
    CHECK(joined.status == 200);
    CHECK(joined.body.at("participants") == nlohmann::json{test::HULDA, test::AELA, test::SAADIA});
    const server::ApiResponse left =
        harness.Post(base + "/participants", {{"left", {test::AELA}}, {"observation", test::FixtureObservation()}});
    CHECK(left.body.at("participants") == nlohmann::json{test::HULDA, test::SAADIA});

    const server::ApiResponse read = harness.Get(base);
    REQUIRE(read.status == 200);
    const nlohmann::json& conversation = read.body.at("conversation");
    CHECK(conversation.at("id") == "c1");
    CHECK(conversation.at("ended") == false);
    CHECK(conversation.at("folded_through") == 0);
    REQUIRE(conversation.at("participants").size() == 2);
    CHECK(conversation.at("participants")[0].at("id") == test::HULDA);
    CHECK(conversation.at("participants")[0].at("name") == "Hulda");
    CHECK(read.body.at("turns") == nlohmann::json::array());
    REQUIRE(read.body.at("events").size() == 1);
    CHECK(read.body.at("events")[0].at("after_turn") == 0);
    CHECK(read.body.at("events")[0].at("event").at("text") == "A door opens.");

    CHECK(harness.Post(base + "/end", nlohmann::json::object()).status == 200);
    CHECK(harness.Get(base).body.at("conversation").at("ended") == true);
    CheckError(harness.Post(base + "/end", nlohmann::json::object()), 400, "has ended");
}

TEST_CASE("Errors carry their status, their message naming the fault, and the API version", "[hosted-server][api]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_errors", script.Responder());
    const nlohmann::json open = test::ReadFixture("open.json");
    REQUIRE(harness.Post("/v1/runs/save_1/open", open).status == 200);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations", HuldaAndAela()).status == 200);
    const std::string c1 = "/v1/runs/save_1/conversations/c1";

    CheckError(harness.Get("/nope"), 404, "/nope");
    CheckError(harness.Post("/health", nlohmann::json::object()), 404, "/health");
    CheckError(harness.api.Handle("POST", "/v1/runs/save_1/open", "{not json"), 400, "body");
    CheckError(harness.api.Handle("POST", "/v1/runs/save_1/open", "[1, 2]"), 400, "body");
    CheckError(harness.Post("/v1/runs/a.b/open", open), 400, "run_id");
    CheckError(harness.Post("/v1/runs/save_9/conversations", HuldaAndAela()), 409, "save_9");
    CheckError(harness.Get("/v1/runs/save_9/conversations/c1"), 409, "save_9");
    CheckError(harness.Post("/v1/runs/save_2/open", {{"observation_schema", open.at("observation_schema")}}), 400,
               "host data at /catalog");
    CheckError(harness.Post("/v1/runs/save_1/conversations",
                            {{"participants", {"0000BEEF"}}, {"observation", test::FixtureObservation()}}),
               400, "participants/0");
    CheckError(harness.Post("/v1/runs/save_1/conversations", {{"participants", test::HULDA}}), 400, "participants");
    CheckError(harness.Post("/v1/runs/save_1/conversations", {{"participants", {test::HULDA}}}), 400, "observation");
    CheckError(harness.Get("/v1/runs/save_1/conversations/c9"), 404, "conversation_id");
    CheckError(harness.Post("/v1/runs/save_1/conversations/c9/events", {{"events", nlohmann::json::array()}}), 404,
               "c9");
    CheckError(harness.Post(c1 + "/events", {{"events", {{{"type", "ambient"}}}}}), 400, "events/0/text");
    CheckError(harness.Post(c1 + "/events", {{"events", {"not an object"}}}), 400, "events/0");
    CheckError(harness.Post(c1 + "/nope", nlohmann::json::object()), 404, "/nope");
    CheckError(harness.Post("/v1/tokenize", nlohmann::json::object()), 400, "content");
    CheckError(harness.Post("/v1/embeddings", {{"input", {"a"}}, {"purpose", "other"}}), 400, "purpose");

    // A backend that cannot start: 503.
    harness.backendDown = true;
    CheckError(harness.Post("/v1/runs/save_3/open", open), 503, "cannot reach");
}

TEST_CASE("/v1/tokenize and /v1/embeddings answer with the utility backend", "[hosted-server][api]")
{
    test::HostedScript script;
    test::ApiHarness harness("api_utility", script.Responder());
    const server::ApiResponse tokens = harness.Post("/v1/tokenize", {{"content", "abcdefgh"}});
    CHECK(tokens.status == 200);
    CHECK(tokens.body.at("tokens") == harness.utility.CountTokens("abcdefgh"));

    const server::ApiResponse vectors =
        harness.Post("/v1/embeddings", {{"input", {"a lighthouse", "a keeper"}}, {"purpose", "query"}});
    CHECK(vectors.status == 200);
    CHECK(vectors.body.at("model") == harness.utility.EmbeddingModelId());
    REQUIRE(vectors.body.at("embeddings").size() == 2);
    CHECK(vectors.body.at("embeddings")[0].get<std::vector<float>>() ==
          harness.utility.Embed({"a lighthouse"}, inference::EmbedPurpose::Query)[0]);
}
```

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/Api.h'`.

- [ ] **Step 4: Write `TurnStream`**

Create `src/server/TurnStream.h`:

```cpp
#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>

#include <nlohmann/json.hpp>

namespace morpheus::server {

// One event of a turn's stream (morpheus_server spec, section 1, "The turn stream"): action, token, sentence, done or
// error, and its JSON.
struct StreamEvent {
    std::string name = {};
    nlohmann::json data = nlohmann::json::object();
};

// A turn's events on their way from the run's worker (Push, End) to the HTTP handler that writes them (Next, Cancel).
// The handler's side also tells the turn to stop: Cancel fires StopToken, which the turn runs under. Thread-safe.
class TurnStream {
private:
    mutable std::mutex _mutex = {};
    std::condition_variable _changed = {};
    std::deque<StreamEvent> _events = {};
    bool _ended = false;
    std::stop_source _stop = {};

public:
    // Queues an event. Throws ContractError after End.
    void Push(StreamEvent event);
    // No event follows.
    void End();
    // Takes the next event, waiting up to `wait` for one; nullopt when none came (Ended() tells whether one ever will).
    std::optional<StreamEvent> Next(std::chrono::milliseconds wait);
    // Whether the stream ended and every event was taken.
    bool Ended() const;
    // Waits up to `wait` for the stream to start: true once an event is queued or the stream ended.
    bool WaitStarted(std::chrono::milliseconds wait);
    // The next event without taking it; nullopt when none is queued.
    std::optional<StreamEvent> Front() const;
    // The client left, or the server stops: the turn's stop token fires.
    void Cancel();
    // What the turn runs under.
    std::stop_token StopToken() const;
};

} // namespace morpheus::server
```

Create `src/server/TurnStream.cpp`:

```cpp
#include "server/TurnStream.h"

#include <utility>

#include "core/Errors.h"

namespace morpheus::server {

void TurnStream::Push(StreamEvent event)
{
    {
        std::scoped_lock lock(_mutex);
        MORPHEUS_REQUIRE(!_ended, "TurnStream::Push: the stream has ended");
        _events.push_back(std::move(event));
    }
    _changed.notify_all();
}

void TurnStream::End()
{
    {
        std::scoped_lock lock(_mutex);
        _ended = true;
    }
    _changed.notify_all();
}

std::optional<StreamEvent> TurnStream::Next(std::chrono::milliseconds wait)
{
    std::unique_lock lock(_mutex);
    _changed.wait_for(lock, wait, [this] { return !_events.empty() || _ended; });
    if (_events.empty()) {
        return std::nullopt;
    }
    StreamEvent event = std::move(_events.front());
    _events.pop_front();
    return event;
}

bool TurnStream::Ended() const
{
    std::scoped_lock lock(_mutex);
    return _ended && _events.empty();
}

bool TurnStream::WaitStarted(std::chrono::milliseconds wait)
{
    std::unique_lock lock(_mutex);
    return _changed.wait_for(lock, wait, [this] { return !_events.empty() || _ended; });
}

std::optional<StreamEvent> TurnStream::Front() const
{
    std::scoped_lock lock(_mutex);
    if (_events.empty()) {
        return std::nullopt;
    }
    return _events.front();
}

void TurnStream::Cancel()
{
    _stop.request_stop();
}

std::stop_token TurnStream::StopToken() const
{
    return _stop.get_token();
}

} // namespace morpheus::server
```

- [ ] **Step 5: Write the `Api` without the turn route**

Create `src/server/Api.h`:

```cpp
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "inference/Llm.h"
#include "server/RunRegistry.h"
#include "server/ServerLog.h"
#include "server/SlotPool.h"
#include "server/TurnStream.h"

namespace morpheus::server {

// morpheus_server's API version, on every response (spec, section 1). A breaking change bumps the major version and
// gets a CHANGELOG.md entry.
inline constexpr std::string_view API_VERSION = "1.0.0";

// What /health reports about the models.
struct HealthInfo {
    std::string chatModel = {};
    std::string embeddingModel = {};
    int slots = 0;
};

// One answer: a status and a JSON body, or, for a turn that started, the stream of its events.
struct ApiResponse {
    int status = 200;
    nlohmann::json body = nlohmann::json::object(); // Carries api_version. Unused when `stream` is set.
    std::shared_ptr<TurnStream> stream = nullptr;   // A started turn: its events, the first (action) already queued.
};

// morpheus_server's routes without sockets (spec, sections 1 and 4): a method, a path and a body in; a status and JSON,
// or a turn's stream, out. Every route, check and status lives here; HttpServer only carries bytes. A run's requests
// run on its worker (RunWorker), so requests of different runs proceed at once; /v1/tokenize and /v1/embeddings take
// turns on the utility backend. Thread-safe.
class Api {
private:
    RunRegistry* _pRuns = nullptr;              // Not owned; outlives this.
    SlotPool* _pSlots = nullptr;                // Not owned; outlives this.
    inference::LlmBackend* _pUtility = nullptr; // Not owned; outlives this. Used under _utilityMutex.
    std::mutex _utilityMutex = {};
    HealthInfo _health = {};
    Log _log = {};

public:
    // Throws ContractError for an empty log.
    Api(RunRegistry& runs, SlotPool& slots, inference::LlmBackend& utility, HealthInfo health, Log log);

    // Handles one request: `method` ("GET", "POST", ...), `path` ("/v1/runs/save_1/open"), `body` (JSON text, or empty).
    // Never throws for the request's or the run's fault: every failure is its status and
    // {"api_version", "error": {"status", "message"}}; a 500 is logged too.
    ApiResponse Handle(std::string_view method, std::string_view path, std::string_view body);
};

} // namespace morpheus::server
```

Create `src/server/Api.cpp`:

```cpp
#include "server/Api.h"

#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "inference/Llm.h"
#include "server/RunWorker.h"
#include "server/SlotBackend.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"
#include "turn/MemoryUpkeep.h"

namespace morpheus::server {

namespace {

// A request for something that does not exist: an unknown route, or an unknown conversation of an opened run (404).
class NotFound : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// A run used before its open in this process: its host data comes with the open (409).
class NotOpened : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

// The status of a request that failed with `error` (spec, section 1, "Errors").
int StatusOf(const std::exception& error)
{
    if (dynamic_cast<const NotFound*>(&error) != nullptr) {
        return 404;
    }
    if (dynamic_cast<const NotOpened*>(&error) != nullptr) {
        return 409;
    }
    if (dynamic_cast<const core::DataError*>(&error) != nullptr) {
        return 400;
    }
    if (dynamic_cast<const turn::TurnError*>(&error) != nullptr) {
        return 422;
    }
    if (dynamic_cast<const inference::InferenceError*>(&error) != nullptr) {
        return 503;
    }
    return 500;
}

// Conversation `id`, which must exist in the run (NotFound otherwise); an ended one is HostedSession's DataError.
void RequireConversation(turn::HostedSession& session, std::string_view id)
{
    if (!session.Conversation(id)) {
        throw NotFound(std::format("conversation_id: there is no conversation '{}'", id));
    }
}

// An adjudication's action as the host reads it: {"id", "params"}, or null for a refused one.
nlohmann::json ActionJson(const core::Adjudication& adjudication)
{
    if (!adjudication.requested) {
        return nullptr;
    }
    return {{"id", adjudication.actionId}, {"params", adjudication.params}};
}

// A conversation's key in the slot pool: its run's id and its own.
std::string SlotKey(std::string_view runId, std::string_view conversationId)
{
    return std::format("{}/{}", runId, conversationId);
}

// The body's string `key`. DataError "<key>: ..." when it is missing or not a string.
std::string RequiredString(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_string()) {
        throw core::DataError(std::format("{}: expected a string", key));
    }
    return it->get<std::string>();
}

// The body's array of strings `key`. DataError naming `key` or `key/<i>` when it is missing or malformed.
std::vector<std::string> StringList(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_array()) {
        throw core::DataError(std::format("{}: expected an array of strings", key));
    }
    std::vector<std::string> list;
    for (std::size_t i = 0; i < it->size(); ++i) {
        if (!(*it)[i].is_string()) {
            throw core::DataError(std::format("{}/{}: expected a string", key, i));
        }
        list.push_back((*it)[i].get<std::string>());
    }
    return list;
}

// The body's array of objects `key` (events). DataError naming `key` or `key/<i>` when it is missing or malformed.
std::vector<nlohmann::json> ObjectList(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_array()) {
        throw core::DataError(std::format("{}: expected an array of objects", key));
    }
    std::vector<nlohmann::json> list;
    for (std::size_t i = 0; i < it->size(); ++i) {
        if (!(*it)[i].is_object()) {
            throw core::DataError(std::format("{}/{}: expected an object", key, i));
        }
        list.push_back((*it)[i]);
    }
    return list;
}

// The body's object `key` (an observation). DataError "<key>: ..." when it is missing or not an object.
const nlohmann::json& RequiredObject(const nlohmann::json& body, std::string_view key)
{
    const auto it = body.find(key);
    if (it == body.end() || !it->is_object()) {
        throw core::DataError(std::format("{}: expected an object", key));
    }
    return *it;
}

} // namespace

Api::Api(RunRegistry& runs, SlotPool& slots, inference::LlmBackend& utility, HealthInfo health, Log log)
    : _pRuns(&runs)
    , _pSlots(&slots)
    , _pUtility(&utility)
    , _health(std::move(health))
    , _log(std::move(log))
{
    MORPHEUS_REQUIRE(static_cast<bool>(_log), "Api: a log is needed");
}

ApiResponse Api::Handle(std::string_view method, std::string_view path, std::string_view body)
{
    ApiResponse response;
    try {
        // The body: a JSON object, or nothing (an empty object).
        nlohmann::json json = nlohmann::json::object();
        if (!body.empty()) {
            json = nlohmann::json::parse(body, nullptr, false);
            if (json.is_discarded() || !json.is_object()) {
                throw core::DataError("body: expected a JSON object");
            }
        }

        // The path's segments: "/v1/runs/save_1/open" is {"v1", "runs", "save_1", "open"}.
        std::vector<std::string_view> parts;
        for (std::size_t start = 0; start < path.size();) {
            const std::size_t end = std::min(path.find('/', start), path.size());
            if (end > start) {
                parts.push_back(path.substr(start, end - start));
            }
            start = end + 1;
        }
        const bool get = method == "GET";
        const bool post = method == "POST";
        auto route = [&parts](std::initializer_list<std::string_view> shape) {
            return std::equal(parts.begin(), parts.end(), shape.begin(), shape.end());
        };
        auto unknown = [&] { return NotFound(std::format("there is no route {} {}", method, path)); };

        if (get && route({"health"})) {
            response.body = {{"ready", true},
                             {"chat_model", _health.chatModel},
                             {"embedding_model", _health.embeddingModel},
                             {"slots", _health.slots}};
        } else if (post && route({"v1", "tokenize"})) {
            // Exact counts from the chat model, for the host's own budgeting.
            const std::string content = RequiredString(json, "content");
            std::scoped_lock lock(_utilityMutex);
            response.body = {{"tokens", _pUtility->CountTokens(content)}};
        } else if (post && route({"v1", "embeddings"})) {
            const std::vector<std::string> input = StringList(json, "input");
            const std::string purpose = RequiredString(json, "purpose");
            if (purpose != "query" && purpose != "document") {
                throw core::DataError("purpose: expected \"query\" or \"document\"");
            }
            std::scoped_lock lock(_utilityMutex);
            response.body = {{"embeddings", _pUtility->Embed(input, purpose == "query" ? inference::EmbedPurpose::Query
                                                                                       : inference::EmbedPurpose::Document)},
                             {"model", _pUtility->EmbeddingModelId()}};
        } else if (parts.size() >= 4 && parts[0] == "v1" && parts[1] == "runs") {
            const std::string runId(parts[2]);
            RunRegistry::CheckRunId(runId);
            // The run's worker; a run not opened in this process has none (its host data comes with the open).
            auto worker = [this, &runId]() -> RunWorker& {
                RunWorker* pWorker = _pRuns->Find(runId);
                if (pWorker == nullptr) {
                    throw NotOpened(std::format("run '{}' is not open: open it first (POST /v1/runs/{}/open)", runId, runId));
                }
                return *pWorker;
            };

            if (post && parts.size() == 4 && parts[3] == "open") {
                // The host data, and an optional seed for a new run's sampling.
                std::uint64_t seed = 0;
                if (json.contains("seed")) {
                    if (!json.at("seed").is_number_unsigned()) {
                        throw core::DataError("seed: expected an integer of at least 0");
                    }
                    seed = json.at("seed").get<std::uint64_t>();
                    json.erase("seed");
                }
                const bool created = _pRuns->Open(runId, turn::HostDataFromJson(json), seed);
                response.body = {{"run_id", runId}, {"created", created}};
            } else if (post && parts.size() == 4 && parts[3] == "conversations") {
                const std::vector<std::string> participants = StringList(json, "participants");
                const nlohmann::json& observation = RequiredObject(json, "observation");
                std::string id;
                worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                    id = session.StartConversation(participants, observation);
                });
                response.body = {{"conversation_id", id}};
            } else if (parts.size() >= 5 && parts[3] == "conversations") {
                const std::string conversationId(parts[4]);
                if (get && parts.size() == 5) {
                    // The stored records, for debugging and the host's own tests.
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        const core::Conversation conversation = *session.Conversation(conversationId);
                        nlohmann::json participants = nlohmann::json::array();
                        for (const core::Participant& participant : conversation.participants) {
                            participants.push_back(
                                {{"id", participant.id}, {"name", participant.name}, {"bio", participant.bio}});
                        }
                        nlohmann::json turns = nlohmann::json::array();
                        for (const core::HostedTurn& turn : session.Turns(conversationId)) {
                            nlohmann::json entry = {
                                {"number", turn.number},
                                {"speaker", turn.speakerId},
                                {"listeners", turn.listenerIds},
                                {"player_text", turn.playerText ? nlohmann::json(*turn.playerText) : nlohmann::json(nullptr)},
                                {"events", turn.events},
                                {"observation", turn.observation},
                                {"resolve_output", turn.resolveOutput},
                                {"action", ActionJson(turn.adjudication)},
                                {"line", turn.line},
                                {"events_omitted", turn.eventsOmitted},
                                {"timings", turn.timings}};
                            if (!turn.adjudication.requested) {
                                entry["refused"] = turn.adjudication.reasons;
                            }
                            turns.push_back(std::move(entry));
                        }
                        nlohmann::json events = nlohmann::json::array();
                        for (const core::ConversationEvent& event : session.Events(conversationId)) {
                            events.push_back({{"after_turn", event.afterTurn}, {"event", event.event}});
                        }
                        response.body = {{"conversation",
                                          {{"id", conversation.id},
                                           {"participants", participants},
                                           {"ended", conversation.ended},
                                           {"folded_through", conversation.foldedThrough}}},
                                         {"turns", turns},
                                         {"events", events}};
                    });
                } else if (post && parts.size() == 6 && parts[5] == "events") {
                    const std::vector<nlohmann::json> events = ObjectList(json, "events");
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        session.RecordEvents(conversationId, events);
                    });
                } else if (post && parts.size() == 6 && parts[5] == "participants") {
                    const std::vector<std::string> joined =
                        json.contains("joined") ? StringList(json, "joined") : std::vector<std::string>();
                    const std::vector<std::string> left =
                        json.contains("left") ? StringList(json, "left") : std::vector<std::string>();
                    const nlohmann::json& observation = RequiredObject(json, "observation");
                    nlohmann::json ids = nlohmann::json::array();
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        session.SetParticipants(conversationId, joined, left, observation);
                        for (const core::Participant& participant : session.Conversation(conversationId)->participants) {
                            ids.push_back(participant.id);
                        }
                    });
                    response.body = {{"participants", ids}};
                } else if (post && parts.size() == 6 && parts[5] == "end") {
                    // The conversation ends; its slot is free for the next one, and folding follows in the background.
                    worker().Call([&](turn::HostedSession& session, SlotBackend&, std::stop_token) {
                        RequireConversation(session, conversationId);
                        session.EndConversation(conversationId);
                    });
                    _pSlots->Release(SlotKey(runId, conversationId));
                } else {
                    throw unknown();
                }
            } else {
                throw unknown();
            }
        } else {
            throw unknown();
        }
    } catch (const std::exception& error) {
        // Every failure is its status and message; a 500 is a bug or damaged data, so the operator sees it too.
        const int status = StatusOf(error);
        if (status == 500) {
            _log(std::format("{} {} failed: {}", method, path, error.what()));
        }
        response = ApiResponse{status, {{"error", {{"status", status}, {"message", error.what()}}}}, nullptr};
    }
    if (!response.stream) {
        response.body["api_version"] = std::string(API_VERSION);
    }
    return response;
}

} // namespace morpheus::server
```

Notes for the implementer:
- `ActionJson` and `SlotKey` each have a second caller in Task 9; until then MSVC may warn that `ActionJson` is used once (it is not: the GET route uses it), and `SlotKey` is used by the end route. `StatusOf` and `RequireConversation` are used here and in Task 9.
- `std::equal` and `std::min` need `<algorithm>`; `std::initializer_list` needs `<initializer_list>`.
- `nlohmann::json::find(std::string_view)` works with nlohmann 3.12; if it does not compile, use `body.find(std::string(key))`.

Add `Api.cpp` and `TurnStream.cpp` to `src/server/CMakeLists.txt`.

- [ ] **Step 6: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[api],[stream]"`
Expected: `All tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/server/ tests/server/
git commit -m "Api and TurnStream: health, open, conversations, events, participants, end, tokenize and embeddings, with every error's status"
```

---

### Task 9: The turn route: an action, tokens and sentences, then done

**Files:**
- Modify: `src/server/Api.cpp` (the turn route in `Handle`)
- Test: `tests/server/ApiTurnTests.cpp`

**Interfaces:**
- Consumes: everything Task 8 put in `Api.cpp`'s anonymous namespace, `server::SentenceSplitter` (Task 5), `server::TurnStream` (Task 8), `turn::HostedSession::RunTurn`, `turn::HostedTurnRequest`, `turn::HostedCallbacks`.
- Produces: `POST /v1/runs/{run_id}/conversations/{cid}/turn` with body `{speaker, player_text?, events?, observation, allowed_actions?}`: an error before the action is a JSON answer with its status; otherwise `ApiResponse::stream` carries `action` (with `api_version`), `token`s, `sentence`s, then `done` or `error`.

- [ ] **Step 1: Write the failing tests**

Create `tests/server/ApiTurnTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <chrono>
#include <cstddef>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "server/Api.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string FOLLOW = R"({"action":"follow","params":{}})";
const std::string NONE = R"({"action":"none","params":{}})";

// Opens run save_1 and starts conversation c1 of Hulda and Aela in `harness`.
void OpenWithConversation(test::ApiHarness& harness)
{
    REQUIRE(harness.Post("/v1/runs/save_1/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(harness
                .Post("/v1/runs/save_1/conversations",
                      {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
}

// A turn request body: `speaker` speaks after `playerText` in observation.json.
nlohmann::json TurnBody(const std::string& speaker, const std::string& playerText)
{
    return {{"speaker", speaker}, {"player_text", playerText}, {"observation", test::FixtureObservation()}};
}

const std::string TURN = "/v1/runs/save_1/conversations/c1/turn";

} // namespace

TEST_CASE("A turn streams its action, its tokens and its sentences, then done, and commits", "[hosted-server][api]")
{
    test::HostedScript script;
    script.actions = {FOLLOW};
    script.lines = {"Lead the way, then. I will follow you."};
    test::ApiHarness harness("api_turn", script.Responder());
    OpenWithConversation(harness);

    const server::ApiResponse response = harness.Post(TURN, TurnBody(test::HULDA, "Come with me."));
    REQUIRE(response.status == 200);
    REQUIRE(response.stream != nullptr);
    const std::vector<server::StreamEvent> events = test::ReadAll(*response.stream);
    REQUIRE(events.size() >= 4);

    // The action first, with the API version; done last.
    CHECK(events.front().name == "action");
    CHECK(events.front().data == nlohmann::json{{"api_version", "1.0.0"},
                                                {"speaker", test::HULDA},
                                                {"action", {{"id", "follow"}, {"params", nlohmann::json::object()}}}});
    CHECK(events.back().name == "done");

    // Tokens make the line; each sentence comes as soon as it ends, before the next sentence's tokens.
    std::string tokens;
    std::string tokensBeforeFirstSentence;
    std::vector<std::string> sentences;
    for (std::size_t i = 1; i + 1 < events.size(); ++i) {
        if (events[i].name == "token") {
            tokens += events[i].data.at("text").get<std::string>();
        } else {
            REQUIRE(events[i].name == "sentence");
            if (sentences.empty()) {
                tokensBeforeFirstSentence = tokens;
            }
            sentences.push_back(events[i].data.at("text").get<std::string>());
        }
    }
    CHECK(tokens == "Lead the way, then. I will follow you.");
    CHECK(sentences == std::vector<std::string>{"Lead the way, then.", "I will follow you."});
    CHECK_THAT(tokensBeforeFirstSentence, ContainsSubstring("then."));
    CHECK_THAT(tokensBeforeFirstSentence, !ContainsSubstring("follow"));

    const nlohmann::json& done = events.back().data;
    CHECK(done.at("speaker") == test::HULDA);
    CHECK(done.at("action") == nlohmann::json{{"id", "follow"}, {"params", nlohmann::json::object()}});
    CHECK(done.at("line") == "Lead the way, then. I will follow you.");
    CHECK(done.at("turn") == 1);
    CHECK(done.at("events_omitted") == 0);
    CHECK(done.at("timings").contains("resolve"));
    CHECK(done.at("timings").contains("speak"));

    // Committed: the stored turn reads back.
    const server::ApiResponse read = harness.Get("/v1/runs/save_1/conversations/c1");
    REQUIRE(read.body.at("turns").size() == 1);
    CHECK(read.body.at("turns")[0].at("line") == "Lead the way, then. I will follow you.");
}

TEST_CASE("A refused action streams null with its reasons, and the turn goes on", "[hosted-server][api]")
{
    test::HostedScript script;
    script.actions = {R"({"action":"fly","params":{}})"};
    script.lines = {"No."};
    test::ApiHarness harness("api_turn_refused", script.Responder());
    OpenWithConversation(harness);

    const server::ApiResponse response = harness.Post(TURN, TurnBody(test::HULDA, "Fly away."));
    REQUIRE(response.stream != nullptr);
    const std::vector<server::StreamEvent> events = test::ReadAll(*response.stream);
    REQUIRE(events.size() >= 2);
    CHECK(events.front().data.at("action").is_null());
    REQUIRE(events.front().data.at("refused").is_array());
    CHECK_FALSE(events.front().data.at("refused").empty());
    CHECK(events.back().name == "done");
    CHECK(events.back().data.at("action").is_null());
    CHECK(events.back().data.at("line") == "No.");
}

TEST_CASE("A turn's errors before its action are JSON with their status, and nothing is committed",
          "[hosted-server][api]")
{
    test::HostedScript script; // No reply: only the backend-failure case below reaches the model, and it throws.
    test::ApiHarness harness("api_turn_errors", [&script](const inference::Prompt& prompt,
                                                          const inference::GenerationSpec& spec) -> std::string {
        if (prompt.messages.back().content.find("Fail now.") != std::string::npos) {
            throw inference::InferenceError("cannot reach http://127.0.0.1:1/completion");
        }
        return script.Responder()(prompt, spec);
    });
    OpenWithConversation(harness);
    // Checks that `response` is the error `status` naming `part`.
    auto check = [](const server::ApiResponse& response, int status, const std::string& part) {
        INFO(response.body.dump());
        CHECK(response.stream == nullptr);
        CHECK(response.status == status);
        CHECK(response.body.at("api_version") == "1.0.0");
        CHECK_THAT(response.body.at("error").at("message").get<std::string>(), ContainsSubstring(part));
    };

    check(harness.Post(TURN, TurnBody("decide", "Hello.")), 400, "decide");
    check(harness.Post(TURN, {{"speaker", test::HULDA}}), 400, "observation");
    check(harness.Post(TURN, {{"speaker", 7}, {"observation", test::FixtureObservation()}}), 400, "speaker");
    check(harness.Post(TURN, {{"speaker", test::HULDA}, {"player_text", 7}, {"observation", test::FixtureObservation()}}),
          400, "player_text");
    check(harness.Post(TURN, {{"speaker", test::HULDA}, {"allowed_actions", {7}}, {"observation", test::FixtureObservation()}}),
          400, "allowed_actions/0");
    check(harness.Post("/v1/runs/save_1/conversations/c9/turn", TurnBody(test::HULDA, "Hello.")), 404, "c9");
    check(harness.Post("/v1/runs/save_9/conversations/c1/turn", TurnBody(test::HULDA, "Hello.")), 409, "save_9");
    check(harness.Post(TURN, TurnBody(test::AELA, "Hello.")), 400, "is the player");
    check(harness.Post(TURN, TurnBody(test::HULDA, std::string(5000, 'x'))), 422, "tail_allowance");
    check(harness.Post(TURN, TurnBody(test::HULDA, "Fail now.")), 503, "cannot reach");

    CHECK(harness.Get("/v1/runs/save_1/conversations/c1").body.at("turns").empty());
}

TEST_CASE("A turn's calls use its conversation's slot, and ending the conversation frees it", "[hosted-server][api]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"Welcome.", "Hello."};
    test::ApiHarness harness("api_turn_slots", script.Responder());
    OpenWithConversation(harness);
    REQUIRE(harness.Post("/v1/runs/save_1/conversations",
                         {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .body.at("conversation_id") == "c2");

    test::ReadAll(*harness.Post(TURN, TurnBody(test::HULDA, "Hi.")).stream);
    test::ReadAll(*harness.Post("/v1/runs/save_1/conversations/c2/turn", TurnBody(test::HULDA, "Hi.")).stream);

    // The backend is the run's: read its calls on the run's worker.
    std::vector<int> slots;
    harness.runs.Find("save_1")->Call([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
        for (const test::ScriptedBackend::Call& call : harness.backends.at(0)->Calls()) {
            slots.push_back(call.spec.slot);
        }
    });
    CHECK(slots == std::vector<int>{1, 1, 2, 2});
    CHECK(harness.slots.HolderOf(1) == "save_1/c1");

    REQUIRE(harness.Post("/v1/runs/save_1/conversations/c1/end", nlohmann::json::object()).status == 200);
    CHECK(harness.slots.HolderOf(1) == "");
    CHECK(harness.slots.HolderOf(2) == "save_1/c2");
}

TEST_CASE("A turn whose stream is cancelled stops, commits nothing, and the next turn works", "[hosted-server][api]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"A long answer that streams slowly, piece by piece, for a while.", "Fine."};
    test::ApiHarness harness("api_turn_cancel", script.Responder());
    harness.pieceDelay = std::chrono::milliseconds(100);
    OpenWithConversation(harness);

    const server::ApiResponse response = harness.Post(TURN, TurnBody(test::HULDA, "Hello."));
    REQUIRE(response.stream != nullptr);
    // Read up to the first token, then leave.
    bool sawToken = false;
    for (int i = 0; i < 100 && !sawToken; ++i) {
        if (std::optional<server::StreamEvent> event = response.stream->Next(std::chrono::milliseconds(100))) {
            sawToken = event->name == "token";
        }
    }
    REQUIRE(sawToken);
    response.stream->Cancel();
    const std::vector<server::StreamEvent> rest = test::ReadAll(*response.stream);
    for (const server::StreamEvent& event : rest) {
        CHECK(event.name != "done");
    }

    // Nothing was committed: the next turn is turn 1.
    const server::ApiResponse next = harness.Post(TURN, TurnBody(test::HULDA, "Still there?"));
    REQUIRE(next.stream != nullptr);
    const std::vector<server::StreamEvent> events = test::ReadAll(*next.stream);
    REQUIRE_FALSE(events.empty());
    CHECK(events.back().name == "done");
    CHECK(events.back().data.at("turn") == 1);
}
```

- [ ] **Step 2: Run the tests to see them fail**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[api]"`
Expected: the five new test cases FAIL (each turn request answers 404 "there is no route POST ..."); the Task 8 tests pass.

- [ ] **Step 3: Add the turn route**

In `src/server/Api.cpp`, add `#include <chrono>`, `#include <future>`, `#include <memory>`, `#include "server/SentenceSplitter.h"`, and to the anonymous namespace's constants (put a `// ---- Constants` group first in the namespace):

```cpp
// How long the handler waits for a turn's first event before it checks that the worker took the turn at all.
constexpr std::chrono::milliseconds START_POLL = std::chrono::milliseconds(100);
```

In `Api::Handle`, insert before the `} else if (post && parts.size() == 6 && parts[5] == "events") {` branch:

```cpp
                } else if (post && parts.size() == 6 && parts[5] == "turn") {
                    // The request, checked for its shape here and for its content by HostedSession on the run's worker.
                    turn::HostedTurnRequest request;
                    request.conversationId = conversationId;
                    request.speakerId = RequiredString(json, "speaker");
                    if (request.speakerId == "decide") {
                        throw core::DataError("speaker: \"decide\" (the engine choosing the speaker) is not supported yet");
                    }
                    if (json.contains("player_text")) {
                        if (!json.at("player_text").is_string()) {
                            throw core::DataError("player_text: expected a string");
                        }
                        request.playerText = json.at("player_text").get<std::string>();
                    }
                    if (json.contains("events")) {
                        request.events = ObjectList(json, "events");
                    }
                    request.observation = RequiredObject(json, "observation");
                    if (json.contains("allowed_actions")) {
                        request.allowedActions = StringList(json, "allowed_actions");
                    }

                    // The turn runs on the run's worker, on its conversation's slot; its events come back through the
                    // stream. The stream's stop token stops it when the client leaves (TurnStream::Cancel) or the worker
                    // stops.
                    auto stream = std::make_shared<TurnStream>();
                    std::future<void> taken = worker().Post(
                        [stream, request = std::move(request), slotKey = SlotKey(runId, conversationId),
                         pSlots = _pSlots](turn::HostedSession& session, SlotBackend& backend, std::stop_token workerStop) {
                            std::stop_callback stopWithWorker(workerStop, [&stream] { stream->Cancel(); });
                            try {
                                RequireConversation(session, request.conversationId);
                                backend.SetSlot(pSlots->Acquire(slotKey));
                                SentenceSplitter sentences;
                                turn::HostedCallbacks callbacks;
                                callbacks.onAction = [&](const core::Adjudication& adjudication) {
                                    nlohmann::json action = {{"api_version", std::string(API_VERSION)},
                                                             {"speaker", request.speakerId},
                                                             {"action", ActionJson(adjudication)}};
                                    if (!adjudication.requested) {
                                        action["refused"] = adjudication.reasons;
                                    }
                                    stream->Push({"action", std::move(action)});
                                };
                                callbacks.onToken = [&](std::string_view piece) {
                                    stream->Push({"token", {{"text", std::string(piece)}}});
                                    for (std::string& sentence : sentences.Add(piece)) {
                                        stream->Push({"sentence", {{"text", std::move(sentence)}}});
                                    }
                                };
                                const std::optional<core::HostedTurn> record =
                                    session.RunTurn(request, callbacks, stream->StopToken());
                                // A cancelled turn committed nothing and ends with no done.
                                if (record) {
                                    const std::string rest = sentences.Finish();
                                    if (!rest.empty()) {
                                        stream->Push({"sentence", {{"text", rest}}});
                                    }
                                    stream->Push({"done",
                                                  {{"speaker", record->speakerId},
                                                   {"action", ActionJson(record->adjudication)},
                                                   {"line", record->line},
                                                   {"turn", record->number},
                                                   {"events_omitted", record->eventsOmitted},
                                                   {"timings",
                                                    {{"resolve", record->timings.at("resolve")},
                                                     {"speak", record->timings.at("speak")}}}}});
                                }
                            } catch (const std::exception& error) {
                                stream->Push({"error", {{"status", StatusOf(error)}, {"message", error.what()}}});
                            }
                            stream->End();
                        });

                    // The status is known with the first event: an error before the action is that error's JSON
                    // answer, as for any other request; otherwise the stream starts with the action. A turn the
                    // worker refused (it is stopping) never runs: its future says so.
                    while (!stream->WaitStarted(START_POLL)) {
                        if (taken.wait_for(std::chrono::seconds(0)) == std::future_status::ready) {
                            taken.get(); // Rethrows the refusal: InferenceError, the server is stopping.
                        }
                    }
                    const std::optional<StreamEvent> first = stream->Front();
                    if (!first) {
                        throw inference::InferenceError("the turn was stopped before it began: the server is stopping");
                    }
                    if (first->name == "error") {
                        const int status = first->data.at("status").get<int>();
                        if (status == 500) {
                            _log(std::format("{} {} failed: {}", method, path, first->data.at("message").get<std::string>()));
                        }
                        response = ApiResponse{status, {{"error", first->data}}, nullptr};
                    } else {
                        response.stream = stream;
                    }
```

`stream->Push` after the client cancelled still queues (the stream has not ended); nothing reads it, and the shared stream is freed when the job ends.

- [ ] **Step 4: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"`
Expected: `All tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/server/Api.cpp tests/server/ApiTurnTests.cpp
git commit -m "Api: the turn route streams its action, tokens, sentences and done, on its conversation's slot"
```

---

### Task 10: `HttpServer`, and the fixtures over real HTTP

**Files:**
- Create: `src/server/HttpServer.h`, `src/server/HttpServer.cpp`
- Modify: `src/server/CMakeLists.txt` (add `HttpServer.cpp`)
- Modify: `tests/server/ServerTestSupport.h` (add `ServerConfigFor`, `HttpHarness`, `HttpTurn`, `PostTurn`, `JsonPost`, `JsonGet`, `Pieces`)
- Test: `tests/server/HttpServerTests.cpp`

**Interfaces:**
- Consumes: `server::Api`, `server::ApiResponse`, `server::TurnStream`, `server::StreamEvent` (Task 8), `llamaserver::LlamaServerBackend` with `SlotCount()` and `ChatModelId()` (Task 3), `test::FakeLlamaServer` (`tests/fakes/FakeLlamaServer.h`), `test::ConfigFor` (`tests/llamaserver/ServerTestSupport.h`), the hosted fixtures (`tests/hosted/fixtures/`).
- Produces: `class server::HttpServer` with `explicit HttpServer(Api& api)`, `~HttpServer()`, `void Start(int port)` (0 picks a free port; `core::DataError` naming the port when it cannot be bound), `int Port() const`, `void Stop()`. Test helpers `test::HttpHarness`, `test::HttpTurn`, `test::PostTurn`, `test::JsonPost`, `test::JsonGet`, `test::Pieces`, `test::ServerConfigFor`.

- [ ] **Step 1: Write the test helpers**

Add to `tests/server/ServerTestSupport.h` (new includes: `<sstream>`, `"fakes/FakeLlamaServer.h"`, `"llamaserver/LlamaServerBackend.h"`, `"llamaserver/ServerTestSupport.h"`, `"server/HttpServer.h"`; `<httplib.h>` comes through `fakes/FakeLlamaServer.h`):

```cpp
// A backend config on fake servers `chat` and `embed` for a whole server: calls left at -1 go to the upkeep slot, and
// the context is HostedSettings' 16384 (the fakes hold 16384 per slot).
inline llamaserver::LlamaServerConfig ServerConfigFor(const FakeLlamaServer& chat, const FakeLlamaServer& embed)
{
    llamaserver::LlamaServerConfig config = ConfigFor(chat, embed);
    config.slot = server::SlotPool::UPKEEP_SLOT;
    config.contextSize = 16384;
    return config;
}

// `text` in pieces of 4 bytes, as a fake server streams a completion. The fixtures' texts are ASCII.
inline std::vector<std::string> Pieces(const std::string& text)
{
    std::vector<std::string> pieces;
    for (std::size_t at = 0; at < text.size(); at += 4) {
        pieces.push_back(text.substr(at, 4));
    }
    return pieces;
}

// The whole server over real sockets, with fake llama-servers behind it, for test `name`: each run's backend and the
// utility backend on the fakes, HostedSettings, and the server on a free port of 127.0.0.1. Log lines are dropped.
struct HttpHarness {
    FakeLlamaServer chat;
    FakeLlamaServer embed;
    std::filesystem::path runsDir = {};
    llamaserver::LlamaServerBackend utility;
    server::SlotPool slots;
    server::RunRegistry runs;
    server::Api api;
    server::HttpServer http;

    explicit HttpHarness(std::string_view name)
        : runsDir(FreshTempDir(name) / "runs")
        , utility(ServerConfigFor(chat, embed))
        , slots(utility.SlotCount())
        , runs(runsDir, HostedSettings(),
               [this] { return std::make_unique<llamaserver::LlamaServerBackend>(ServerConfigFor(chat, embed)); },
               [](const std::string&) {})
        , api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
              [](const std::string&) {})
        , http(api)
    {
        http.Start(0);
    }

    // Running turns stop first, so the handlers waiting on them return; then the server stops.
    ~HttpHarness()
    {
        runs.StopAll();
        http.Stop();
    }

    httplib::Client Client() const
    {
        return httplib::Client("127.0.0.1", http.Port());
    }
};

// A JSON answer over HTTP: its status, its Content-Type and its body.
struct HttpJson {
    int status = 0;
    std::string contentType = {};
    nlohmann::json body = nullptr;
};

inline HttpJson JsonPost(httplib::Client& client, const std::string& path, const nlohmann::json& body)
{
    const httplib::Result result = client.Post(path, body.dump(), "application/json");
    if (!result) {
        return {};
    }
    return {result->status, result->get_header_value("Content-Type"), nlohmann::json::parse(result->body, nullptr, false)};
}

inline HttpJson JsonGet(httplib::Client& client, const std::string& path)
{
    const httplib::Result result = client.Get(path);
    if (!result) {
        return {};
    }
    return {result->status, result->get_header_value("Content-Type"), nlohmann::json::parse(result->body, nullptr, false)};
}

// A turn over HTTP: the status and Content-Type, the stream's events parsed back ("event:" and "data:" lines), or
// the JSON body of a refused turn.
struct HttpTurn {
    int status = 0;
    std::string contentType = {};
    std::vector<server::StreamEvent> events = {};
    nlohmann::json body = nullptr;
};

// Posts a turn and reads its answer. `keepReading`, when given, sees each event as it arrives; returning false
// closes the connection there (a client that leaves mid-stream; the status is then 0).
inline HttpTurn PostTurn(httplib::Client& client, const std::string& path, const nlohmann::json& body,
                         const std::function<bool(const server::StreamEvent&)>& keepReading = {})
{
    HttpTurn turn;
    std::string raw;     // Every byte received.
    std::string pending; // Bytes not yet parsed into events.
    httplib::Request request;
    request.method = "POST";
    request.path = path;
    request.body = body.dump();
    request.set_header("Content-Type", "application/json");
    request.content_receiver = [&](const char* pData, std::size_t length, std::size_t, std::size_t) {
        raw.append(pData, length);
        pending.append(pData, length);
        // Each event ends with a blank line.
        for (std::size_t end = pending.find("\n\n"); end != std::string::npos; end = pending.find("\n\n")) {
            std::istringstream lines(pending.substr(0, end));
            pending.erase(0, end + 2);
            server::StreamEvent event;
            for (std::string line; std::getline(lines, line);) {
                if (line.starts_with("event: ")) {
                    event.name = line.substr(7);
                } else if (line.starts_with("data: ")) {
                    event.data = nlohmann::json::parse(line.substr(6));
                }
            }
            turn.events.push_back(event);
            if (keepReading && !keepReading(event)) {
                return false;
            }
        }
        return true;
    };
    const httplib::Result result = client.send(request);
    if (result) {
        turn.status = result->status;
        turn.contentType = result->get_header_value("Content-Type");
        // cpp-httplib may hand an error answer's body to the receiver or keep it in the response.
        const std::string& text = raw.empty() ? result->body : raw;
        if (turn.events.empty() && !text.empty()) {
            turn.body = nlohmann::json::parse(text, nullptr, false);
        }
    }
    return turn;
}
```

- [ ] **Step 2: Write the failing tests**

Create `tests/server/HttpServerTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "fakes/FakeLlamaServer.h"
#include "hosted/HostedTestSupport.h"
#include "server/HttpServer.h"
#include "server/ServerTestSupport.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;
using Catch::Matchers::StartsWith;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// The fixture's turn object as a request body: with observation.json when it has none.
nlohmann::json TurnBody(const nlohmann::json& turnJson)
{
    nlohmann::json body = turnJson;
    if (!body.contains("observation")) {
        body["observation"] = test::FixtureObservation();
    }
    return body;
}

// Replays conversation fixture `name` over HTTP: the fake chat server answers each turn's resolve and speak calls as
// the fixture's model says, each step goes as its request, and each turn's stream and the stored turns are checked
// against its "expect".
void ReplayOverHttp(const std::string& name)
{
    test::HttpHarness harness("http_fixture_" + name);
    httplib::Client client = harness.Client();
    REQUIRE(test::JsonPost(client, "/v1/runs/fixture/open", test::ReadFixture("open.json")).status == 200);
    const nlohmann::json fixture = test::ReadFixture(name);
    const test::HttpJson started = test::JsonPost(
        client, "/v1/runs/fixture/conversations",
        {{"participants", fixture.at("participants")}, {"observation", test::FixtureObservation()}});
    REQUIRE(started.status == 200);
    const std::string base = "/v1/runs/fixture/conversations/" + started.body.at("conversation_id").get<std::string>();

    std::vector<nlohmann::json> expected; // Each turn step's "expect", in order.
    for (std::size_t i = 0; i < fixture.at("steps").size(); ++i) {
        const nlohmann::json& step = fixture.at("steps")[i];
        INFO(name << " step " << i);
        if (step.contains("events")) {
            CHECK(test::JsonPost(client, base + "/events", {{"events", step.at("events")}}).status == 200);
            continue;
        }
        harness.chat.QueueCompletion({test::Pieces(step.at("model").at("resolve").get<std::string>())});
        harness.chat.QueueCompletion({test::Pieces(step.at("model").at("line").get<std::string>())});
        const test::HttpTurn turn = test::PostTurn(client, base + "/turn", TurnBody(step.at("turn")));
        REQUIRE(turn.status == 200);
        CHECK_THAT(turn.contentType, StartsWith("text/event-stream"));
        REQUIRE(turn.events.size() >= 2);
        expected.push_back(step.at("expect"));

        // The stream: the action first with the API version, then tokens and sentences, done last.
        const nlohmann::json& expect = step.at("expect");
        const nlohmann::json& action = turn.events.front().data;
        CHECK(turn.events.front().name == "action");
        CHECK(action.at("api_version") == "1.0.0");
        CHECK(action.at("action").is_null() == !expect.at("requested").get<bool>());
        if (expect.contains("action")) {
            CHECK(action.at("action") == expect.at("action"));
        }
        if (expect.contains("reasons")) {
            CHECK(action.at("refused") == expect.at("reasons"));
        }
        std::string tokens;
        for (const server::StreamEvent& event : turn.events) {
            if (event.name == "token") {
                tokens += event.data.at("text").get<std::string>();
            }
        }
        CHECK(tokens == step.at("model").at("line").get<std::string>());
        CHECK(turn.events.back().name == "done");
        CHECK(turn.events.back().data.at("line") == expect.at("line"));
        CHECK(turn.events.back().data.at("turn") == static_cast<int>(expected.size()));
    }

    // The stored turns, through GET.
    const test::HttpJson stored = test::JsonGet(client, base);
    REQUIRE(stored.status == 200);
    const nlohmann::json& turns = stored.body.at("turns");
    REQUIRE(turns.size() == expected.size());
    for (std::size_t i = 0; i < turns.size(); ++i) {
        CHECK(turns[i].at("line") == expected[i].at("line"));
        CHECK(turns[i].at("listeners") == expected[i].at("listeners"));
    }
}

} // namespace

TEST_CASE("Over HTTP every answer is JSON with the API version, and an unknown route is a 404", "[hosted-server][http]")
{
    test::HttpHarness harness("http_health");
    httplib::Client client = harness.Client();
    const test::HttpJson health = test::JsonGet(client, "/health");
    CHECK(health.status == 200);
    CHECK_THAT(health.contentType, StartsWith("application/json"));
    CHECK(health.body.at("api_version") == "1.0.0");
    CHECK(health.body.at("chat_model") == "nomic-embed-text-v1.5.Q8_0"); // The fake names one model file for both.
    CHECK(health.body.at("slots") == 3);
    const test::HttpJson unknown = test::JsonGet(client, "/nope");
    CHECK(unknown.status == 404);
    CHECK(unknown.body.at("api_version") == "1.0.0");
    CHECK(unknown.body.at("error").at("status") == 404);
}

TEST_CASE("The conversation fixture plays over HTTP", "[hosted-server][http][fixtures]")
{
    ReplayOverHttp("conversation.json");
}

TEST_CASE("The radiant fixture plays over HTTP", "[hosted-server][http][fixtures]")
{
    ReplayOverHttp("radiant.json");
}

TEST_CASE("Each invalid request fixture is refused over HTTP with its status and field, with no model call",
          "[hosted-server][http][fixtures]")
{
    test::HttpHarness harness("http_fixture_invalid");
    httplib::Client client = harness.Client();
    REQUIRE(test::JsonPost(client, "/v1/runs/fixture/open", test::ReadFixture("open.json")).status == 200);
    const nlohmann::json fixture = test::ReadFixture("invalid_requests.json");
    const nlohmann::json start = {{"participants", fixture.at("participants")}, {"observation", test::FixtureObservation()}};
    const std::string open = test::JsonPost(client, "/v1/runs/fixture/conversations", start).body.at("conversation_id").get<std::string>();
    const std::string ended = test::JsonPost(client, "/v1/runs/fixture/conversations", start).body.at("conversation_id").get<std::string>();
    REQUIRE(test::JsonPost(client, "/v1/runs/fixture/conversations/" + ended + "/end", nlohmann::json::object()).status ==
            200);

    for (const nlohmann::json& entry : fixture.at("requests")) {
        const std::string name = entry.at("name").get<std::string>();
        INFO(name);
        const std::string named = entry.at("conversation").get<std::string>();
        const std::string id = named == "open" ? open : named == "ended" ? ended : named;
        const test::HttpTurn turn =
            test::PostTurn(client, "/v1/runs/fixture/conversations/" + id + "/turn", TurnBody(entry.at("turn")));
        // An unknown conversation is a 404 over HTTP; every other fault is the request's (400).
        CHECK(turn.status == (name == "unknown conversation" ? 404 : 400));
        CHECK(turn.events.empty());
        CHECK(turn.body.at("api_version") == "1.0.0");
        CHECK_THAT(turn.body.at("error").at("message").get<std::string>(),
                   ContainsSubstring(entry.at("error").get<std::string>()));
    }
    CHECK(harness.chat.RequestsTo("/completion").empty());
}

TEST_CASE("A client that leaves mid-line stops the turn: nothing is committed, and the next turn works",
          "[hosted-server][http]")
{
    test::HttpHarness harness("http_disconnect");
    httplib::Client client = harness.Client();
    REQUIRE(test::JsonPost(client, "/v1/runs/r/open", test::ReadFixture("open.json")).status == 200);
    REQUIRE(test::JsonPost(client, "/v1/runs/r/conversations",
                           {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                .status == 200);
    const nlohmann::json body = {{"speaker", test::HULDA}, {"player_text", "Hello."}, {"observation", test::FixtureObservation()}};

    // The line streams two pieces, then the fake holds the stream open until its client closes it.
    harness.chat.QueueCompletion({test::Pieces(NONE)});
    test::FakeLlamaServer::CompletionScript held;
    held.pieces = {"Well, ", "let me "};
    held.holdOpen = true;
    harness.chat.QueueCompletion(held);
    const test::HttpTurn left = test::PostTurn(client, "/v1/runs/r/conversations/c1/turn", body,
                                               [](const server::StreamEvent& event) { return event.name != "token"; });
    REQUIRE_FALSE(left.events.empty());
    CHECK(left.events.back().name == "token");

    // The server sees the client gone and stops the turn, which closes the held completion stream.
    CHECK(test::WaitUntil([&] { return harness.chat.ClientClosedStream(); }));

    // Nothing was committed: the next turn is turn 1.
    harness.chat.QueueCompletion({test::Pieces(NONE)});
    harness.chat.QueueCompletion({test::Pieces("Fine.")});
    httplib::Client again = harness.Client();
    const test::HttpTurn next = test::PostTurn(again, "/v1/runs/r/conversations/c1/turn", body);
    REQUIRE(next.status == 200);
    REQUIRE_FALSE(next.events.empty());
    CHECK(next.events.back().name == "done");
    CHECK(next.events.back().data.at("turn") == 1);
}

TEST_CASE("Two runs play at once, each conversation on its own slot, and folding uses the upkeep slot",
          "[hosted-server][http]")
{
    test::HttpHarness harness("http_two_runs");
    for (const char* runId : {"a", "b"}) {
        httplib::Client client = harness.Client();
        REQUIRE(test::JsonPost(client, std::string("/v1/runs/") + runId + "/open", test::ReadFixture("open.json")).status ==
                200);
        REQUIRE(test::JsonPost(client, std::string("/v1/runs/") + runId + "/conversations",
                               {{"participants", {test::HULDA, test::AELA}}, {"observation", test::FixtureObservation()}})
                    .status == 200);
    }

    // Every completion gets the same reply, so it does not matter which run the fake answers first.
    for (int i = 0; i < 4; ++i) {
        harness.chat.QueueCompletion({test::Pieces(NONE)});
    }
    std::vector<test::HttpTurn> turns(2);
    {
        std::vector<std::jthread> players;
        for (std::size_t i = 0; i < 2; ++i) {
            players.emplace_back([&harness, &turns, i] {
                httplib::Client client = harness.Client();
                turns[i] = test::PostTurn(client, std::string("/v1/runs/") + (i == 0 ? "a" : "b") + "/conversations/c1/turn",
                                          {{"speaker", test::HULDA}, {"player_text", "Hi."}, {"observation", test::FixtureObservation()}});
            });
        }
    }
    for (const test::HttpTurn& turn : turns) {
        REQUIRE(turn.status == 200);
        REQUIRE_FALSE(turn.events.empty());
        CHECK(turn.events.back().name == "done");
    }

    // Each conversation's two calls carried one slot, and the two conversations had different ones.
    std::vector<int> slots;
    for (const test::FakeLlamaServer::Request& request : harness.chat.RequestsTo("/completion")) {
        slots.push_back(request.body.at("id_slot").get<int>());
    }
    std::sort(slots.begin(), slots.end());
    CHECK(slots == std::vector<int>{1, 1, 2, 2});

    // Run a's conversation ends: its turn is folded in the background, on the upkeep slot.
    harness.chat.QueueCompletion({test::Pieces("They met at the inn.")});
    httplib::Client client = harness.Client();
    REQUIRE(test::JsonPost(client, "/v1/runs/a/conversations/c1/end", nlohmann::json::object()).status == 200);
    CHECK(test::WaitUntil([&] {
        return test::JsonGet(client, "/v1/runs/a/conversations/c1").body.at("conversation").at("folded_through") == 1;
    }));
    const std::vector<test::FakeLlamaServer::Request> completions = harness.chat.RequestsTo("/completion");
    REQUIRE(completions.size() == 5);
    CHECK(completions.back().body.at("id_slot") == server::SlotPool::UPKEEP_SLOT);
}

TEST_CASE("HttpServer refuses a port another server listens on", "[hosted-server][http]")
{
    test::HttpHarness harness("http_port");
    server::HttpServer second(harness.api);
    CHECK_THROWS_AS(second.Start(harness.http.Port()), core::DataError);
    CHECK_THROWS_WITH(second.Start(harness.http.Port()), ContainsSubstring(std::to_string(harness.http.Port())));
    // The first still answers.
    httplib::Client client = harness.Client();
    CHECK(test::JsonGet(client, "/health").status == 200);
}
```

Notes for the implementer:
- `QueueCompletion({test::Pieces(...)})` builds a `CompletionScript` whose first member, `pieces`, is the list.
- Watch for nlohmann's initializer-list ambiguity: `{test::HULDA, test::AELA}` is an array of two strings here because it sits in a `{"participants", ...}` pair; if a body comes out as an object where an array was meant, write `nlohmann::json::array({...})`.
- If `CHECK(... .at("folded_through") == 1)` inside `WaitUntil` fails because the GET's body is null (a failed request), let the lambda return false in that case.

- [ ] **Step 3: Run the tests to see them fail**

Run: `cmake --build --preset tests`
Expected: a compile error, `cannot open include file: 'server/HttpServer.h'`.

- [ ] **Step 4: Write `HttpServer`**

Create `src/server/HttpServer.h`:

```cpp
#pragma once

#include <memory>
#include <thread>

#include "server/Api.h"

namespace httplib {
class Server; // cpp-httplib, included only by HttpServer.cpp.
}

namespace morpheus::server {

// morpheus_server's HTTP side (spec, sections 1 and 4) on cpp-httplib: every request goes to Api::Handle, whatever
// its method and path. A JSON answer is written as it is; a turn's stream as Server-Sent Events, one "event: <name>"
// and one "data: <JSON>" line each, written as they come. A client that disconnects during a stream (or the server
// stopping under it) cancels its turn. Listens on 127.0.0.1 only, on a port it owns alone.
class HttpServer {
private:
    Api* _pApi = nullptr;                                 // Not owned; outlives this.
    std::unique_ptr<httplib::Server> _pServer = nullptr; // Set by the constructor.
    int _port = 0;                                        // 0 until Start succeeds.
    std::jthread _listener = {};                          // Last: joined first on destruction.

public:
    explicit HttpServer(Api& api);
    // Stop.
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds port `port` of 127.0.0.1 (0 picks a free port), alone, and serves on a thread of its own. Throws
    // DataError naming the port when it cannot be bound (another server listens on it, ours or not), and
    // ContractError when already started or for a port out of range.
    void Start(int port);

    // The port it listens on; 0 before Start.
    int Port() const;

    // Stops accepting and ends open streams (their turns are cancelled), then waits for the handlers. Callers stop the
    // runs first (RunRegistry::StopAll), so no handler waits on a turn. Safe to call twice.
    void Stop();
};

} // namespace morpheus::server
```

Create `src/server/HttpServer.cpp`:

```cpp
#include "server/HttpServer.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // cpp-httplib includes windows.h: leave out what Morpheus never uses.
#endif
#endif
#include <httplib.h>

#include "core/Errors.h"

namespace morpheus::server {

namespace {

constexpr const char* HOST = "127.0.0.1";
constexpr int MAX_PORT = 65535;
// How long a stream waits for its turn's next event before it lets cpp-httplib check the client again.
constexpr std::chrono::milliseconds STREAM_POLL = std::chrono::milliseconds(50);

} // namespace

HttpServer::HttpServer(Api& api)
    : _pApi(&api)
    , _pServer(std::make_unique<httplib::Server>())
{
}

HttpServer::~HttpServer()
{
    Stop();
}

void HttpServer::Start(int port)
{
    MORPHEUS_REQUIRE(_port == 0, "HttpServer::Start: the server is already started");
    MORPHEUS_REQUIRE(port >= 0 && port <= MAX_PORT, std::format("HttpServer::Start: port {} is out of range", port));

    // Every request goes to the Api, whatever its method: it answers 404 for what it does not know.
    auto handle = [this](const httplib::Request& request, httplib::Response& response) {
        ApiResponse answer = _pApi->Handle(request.method, request.path, request.body);
        response.status = answer.status;
        if (!answer.stream) {
            response.set_content(answer.body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                                 "application/json");
            return;
        }
        // A turn: its events as Server-Sent Events, as they come. Between events the provider returns without
        // writing, so cpp-httplib checks the client is still there before calling it again. When the client is gone
        // or the server stops, the stream ends unsuccessfully and the releaser cancels the turn.
        std::shared_ptr<TurnStream> stream = std::move(answer.stream);
        response.set_header("Cache-Control", "no-cache");
        response.set_chunked_content_provider(
            "text/event-stream",
            [stream](std::size_t, httplib::DataSink& sink) {
                if (std::optional<StreamEvent> event = stream->Next(STREAM_POLL)) {
                    const std::string text =
                        "event: " + event->name + "\ndata: " +
                        event->data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n\n";
                    return sink.write(text.data(), text.size());
                }
                if (stream->Ended()) {
                    sink.done();
                }
                return true;
            },
            [stream](bool success) {
                if (!success) {
                    stream->Cancel();
                }
            });
    };
    _pServer->Get(".*", handle);
    _pServer->Post(".*", handle);
    _pServer->Put(".*", handle);
    _pServer->Patch(".*", handle);
    _pServer->Delete(".*", handle);

    // The listening socket owns its port alone. cpp-httplib's default (SO_REUSEADDR on Windows, SO_REUSEPORT
    // elsewhere) lets a second server bind a port another one still listens on, and connections then split between
    // them; that must fail instead.
    _pServer->set_socket_options([](socket_t sock) {
#ifdef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });
    int bound = -1;
    if (port == 0) {
        bound = _pServer->bind_to_any_port(HOST);
    } else if (_pServer->bind_to_port(HOST, port)) {
        bound = port;
    }
    if (bound <= 0) {
        throw core::DataError(
            std::format("port {} of {} cannot be bound: another server may be listening on it", port, HOST));
    }
    _port = bound;
    _listener = std::jthread([this] { _pServer->listen_after_bind(); });
    _pServer->wait_until_ready();
}

int HttpServer::Port() const
{
    return _port;
}

void HttpServer::Stop()
{
    _pServer->stop();
    if (_listener.joinable()) {
        _listener.join();
    }
}

} // namespace morpheus::server
```

If a failed `bind_to_port` leaves the `httplib::Server` unable to bind again, `HttpServer::Start` may be called only once per object; the port test calls it twice on `second`, so in that case make `Start` replace `_pServer` with a fresh `httplib::Server` before binding.

Add `HttpServer.cpp` to `src/server/CMakeLists.txt`.

- [ ] **Step 5: Run the tests to see them pass**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[http]"`
Expected: `All tests passed`. Then the whole server: `./build/tests/bin/Debug/morpheus_tests.exe "[hosted-server]"` passes, three runs in a row.

- [ ] **Step 6: Commit**

```bash
git add src/server/HttpServer.h src/server/HttpServer.cpp src/server/CMakeLists.txt tests/server/ServerTestSupport.h tests/server/HttpServerTests.cpp
git commit -m "HttpServer: the Api over HTTP with Server-Sent Events, stop on disconnect, an exclusive port; the fixtures replayed over HTTP"
```

---

### Task 11: The `morpheus_server` executable, and the live test

**Files:**
- Create: `tools/server/CMakeLists.txt`, `tools/server/main.cpp`
- Modify: `CMakeLists.txt` (add `tools/server` beside `src/server`)
- Test: `tests/server/ServerLiveTests.cpp`

**Interfaces:**
- Consumes: everything in `src/server` (Tasks 4-10), `core::LoadSettings`, `core::BackendKindName` (`src/core/Settings.h`), `llamaserver::LlamaServerBackend`, `test::LiveServerConfig` (`tests/llamaserver/LiveServer.h`), the Task 10 helpers.
- Produces: the executable `morpheus_server [--port N] [--runs DIR] [--settings FILE]`; exit code 0 after a signal, 1 for a startup failure, 2 for bad flags. The `[server]` test `"morpheus_server against real llama-servers: ..."`.

- [ ] **Step 1: Write the live test**

Create `tests/server/ServerLiveTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <cstddef>
#include <format>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "hosted/HostedTestSupport.h"
#include "llamaserver/LiveServer.h"
#include "llamaserver/LlamaServerBackend.h"
#include "server/Api.h"
#include "server/HttpServer.h"
#include "server/RunRegistry.h"
#include "server/ServerTestSupport.h"
#include "server/SlotPool.h"

using namespace morpheus;

TEST_CASE("morpheus_server against real llama-servers: the conversation fixture over HTTP, and the time to each "
          "turn's first sentence",
          "[server]")
{
    // The owner's servers (README, "The [server] tests"); skipped when the variables are unset.
    const llamaserver::LlamaServerConfig config = test::LiveServerConfig(server::SlotPool::UPKEEP_SLOT);
    llamaserver::LlamaServerBackend utility(config);
    server::SlotPool slots(utility.SlotCount());
    core::Settings settings = test::HostedSettings();
    settings.contextSize = test::LIVE_CONTEXT_SIZE;
    server::RunRegistry runs(test::FreshTempDir("server_live") / "runs", settings,
                             [config] { return std::make_unique<llamaserver::LlamaServerBackend>(config); },
                             [](const std::string&) {});
    server::Api api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
                    [](const std::string&) {});
    server::HttpServer http(api);
    http.Start(0);
    httplib::Client client("127.0.0.1", http.Port());
    client.set_read_timeout(std::chrono::seconds(600));

    REQUIRE(test::JsonPost(client, "/v1/runs/live/open", test::ReadFixture("open.json")).status == 200);
    const nlohmann::json fixture = test::ReadFixture("conversation.json");
    const test::HttpJson started =
        test::JsonPost(client, "/v1/runs/live/conversations",
                       {{"participants", fixture.at("participants")}, {"observation", test::FixtureObservation()}});
    REQUIRE(started.status == 200);
    const std::string base = "/v1/runs/live/conversations/" + started.body.at("conversation_id").get<std::string>();

    // The fixture's requests, answered by the real model. The first turn fills the slot's cache; later turns show the
    // warm figure the host's 1.5 s target is about (measured here, not asserted: the target is for its RTX 3060 Ti).
    int turnNumber = 0;
    for (const nlohmann::json& step : fixture.at("steps")) {
        if (step.contains("events")) {
            REQUIRE(test::JsonPost(client, base + "/events", {{"events", step.at("events")}}).status == 200);
            continue;
        }
        nlohmann::json body = step.at("turn");
        if (!body.contains("observation")) {
            body["observation"] = test::FixtureObservation();
        }
        const auto sent = std::chrono::steady_clock::now();
        std::optional<std::chrono::steady_clock::duration> firstSentence;
        const test::HttpTurn turn = test::PostTurn(client, base + "/turn", body, [&](const server::StreamEvent& event) {
            if (event.name == "sentence" && !firstSentence) {
                firstSentence = std::chrono::steady_clock::now() - sent;
            }
            return true;
        });
        ++turnNumber;
        INFO("turn " << turnNumber);
        REQUIRE(turn.status == 200);
        REQUIRE_FALSE(turn.events.empty());
        CHECK(turn.events.front().name == "action");
        CHECK(turn.events.back().name == "done");
        const nlohmann::json& done = turn.events.back().data;
        WARN(std::format("turn {}: first sentence after {} ms; action {}; line: {}", turnNumber,
                         firstSentence ? std::chrono::duration_cast<std::chrono::milliseconds>(*firstSentence).count() : -1,
                         done.at("action").dump(), done.at("line").get<std::string>()));
    }
    runs.StopAll();
    http.Stop();
}
```

- [ ] **Step 2: Run it to see it compile and skip**

Run: `cmake --build --preset tests` then `./build/tests/bin/Debug/morpheus_tests.exe "[server]"`
Expected (without the `MORPHEUS_TEST_*_URL` variables): every `[server]` test is skipped, this one included.

- [ ] **Step 3: Write the executable**

Create `tools/server/CMakeLists.txt`:

```cmake
add_executable(morpheus_server main.cpp)
target_link_libraries(morpheus_server PRIVATE morpheus_server_lib)
morpheus_target_defaults(morpheus_server)
```

In `CMakeLists.txt`, change the server block to:

```cmake
if(MORPHEUS_BUILD_SERVER)
    add_subdirectory(src/server)
    add_subdirectory(tools/server)
endif()
```

Create `tools/server/main.cpp`:

```cpp
// morpheus_server: the HTTP server for hosted runs (docs/superpowers/specs/2026-11-16-morpheus-server-design.md).
//   morpheus_server [--port N] [--runs DIR] [--settings FILE]
// It reads settings.json (which must name the llama-server backend), checks both llama-servers, and serves
// http://127.0.0.1:N until Ctrl+C or SIGTERM.

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <format>
#include <iostream>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

#include "core/Settings.h"
#include "core/Text.h"
#include "llamaserver/LlamaServerBackend.h"
#include "server/Api.h"
#include "server/HttpServer.h"
#include "server/RunRegistry.h"
#include "server/ServerLog.h"
#include "server/SlotPool.h"

using namespace morpheus;

namespace {

constexpr int DEFAULT_PORT = 4998;
constexpr const char* DEFAULT_RUNS = "runs";
constexpr const char* DEFAULT_SETTINGS = "settings.json";
constexpr const char* USAGE = "usage: morpheus_server [--port N] [--runs DIR] [--settings FILE]\n";
constexpr int MAX_PORT = 65535;
constexpr std::chrono::milliseconds SIGNAL_POLL = std::chrono::milliseconds(100);

// Set by the signal handler, polled by main. A lock-free atomic may be set from a handler.
std::atomic<bool> stopRequested = false;

extern "C" void OnSignal(int)
{
    stopRequested = true;
}

} // namespace

int main(int argc, char** argv)
{
    // The flags, each with a value.
    int port = DEFAULT_PORT;
    std::filesystem::path runsDir = DEFAULT_RUNS;
    std::filesystem::path settingsFile = DEFAULT_SETTINGS;
    for (int i = 1; i < argc; ++i) {
        const std::string flag = argv[i];
        if (i + 1 >= argc || (flag != "--port" && flag != "--runs" && flag != "--settings")) {
            std::cerr << USAGE;
            return 2;
        }
        const std::string value = argv[++i];
        if (flag == "--port") {
            // Digits only, from 1 to 65535.
            std::size_t used = 0;
            try {
                port = std::stoi(value, &used);
            } catch (const std::exception&) {
                used = 0;
            }
            if (used != value.size() || port < 1 || port > MAX_PORT) {
                std::cerr << "morpheus_server: --port needs a number from 1 to 65535\n" << USAGE;
                return 2;
            }
        } else if (flag == "--runs") {
            runsDir = value;
        } else {
            settingsFile = value;
        }
    }

    // One logger for every thread: a line at a time on stderr.
    std::mutex logMutex;
    const server::Log log = [&logMutex](const std::string& line) {
        std::scoped_lock lock(logMutex);
        std::cerr << "morpheus_server: " << line << '\n';
    };

    try {
        const core::Settings settings = core::LoadSettings(settingsFile);
        if (settings.backend != core::BackendKind::LlamaServer) {
            log(std::format("{}: the backend is \"{}\"; morpheus_server needs \"backend\": \"{}\"",
                            core::PathText(settingsFile), core::BackendKindName(settings.backend),
                            core::BackendKindName(core::BackendKind::LlamaServer)));
            return 1;
        }

        // Every backend talks to the same servers; a call left on the default slot goes to the upkeep slot.
        // settings.json's server slot is for Morpheus's own game: the server hands out slots itself.
        llamaserver::LlamaServerConfig config;
        config.chatUrl = settings.chatUrl;
        config.embeddingUrl = settings.embeddingUrl;
        config.slot = server::SlotPool::UPKEEP_SLOT;
        config.contextSize = settings.contextSize;
        config.embeddingQueryPrefix = settings.embeddingQueryPrefix;
        config.embeddingDocumentPrefix = settings.embeddingDocumentPrefix;

        // The utility backend checks both servers now, serves /v1/tokenize and /v1/embeddings, and tells the slot
        // count and the model ids.
        llamaserver::LlamaServerBackend utility(config);
        server::SlotPool slots(utility.SlotCount());
        if (utility.SlotCount() == 1) {
            log("the chat server has one slot: folding and turns share it, so their cached prompts evict each other "
                "(start llama-server with --parallel 3 or more)");
        }
        server::RunRegistry runs(runsDir, settings,
                                 [config] { return std::make_unique<llamaserver::LlamaServerBackend>(config); }, log);
        server::Api api(runs, slots, utility, {utility.ChatModelId(), utility.EmbeddingModelId(), utility.SlotCount()},
                        log);
        server::HttpServer http(api);
        http.Start(port);
        log(std::format("listening on http://127.0.0.1:{}; runs in '{}'; chat model {} ({} slots), embedding model {}",
                        http.Port(), core::PathText(std::filesystem::absolute(runsDir)), utility.ChatModelId(),
                        utility.SlotCount(), utility.EmbeddingModelId()));

        std::signal(SIGINT, OnSignal);
        std::signal(SIGTERM, OnSignal);
        while (!stopRequested) {
            std::this_thread::sleep_for(SIGNAL_POLL);
        }

        // Running turns and folds stop first, so the HTTP handlers waiting on them return; then the server stops,
        // and the workers join as `runs` goes.
        log("stopping");
        runs.StopAll();
        http.Stop();
        return 0;
    } catch (const std::exception& error) {
        log(error.what());
        return 1;
    }
}
```

- [ ] **Step 4: Build it in each preset and run it**

Run: `cmake --build --preset tests`, then `cmake --build --preset dev --target morpheus_server` (configure `dev` first if needed: `cmake --preset dev`).
Expected: both build with no warning.

Then, with no settings file: `./build/tests/bin/Debug/morpheus_server.exe --settings nowhere.json`
Expected: exit code 1 and a message naming `nowhere.json`. Bad flags: `./build/tests/bin/Debug/morpheus_server.exe --port 0` gives exit code 2 and the usage line.

If llama-servers are running on this machine (README, "Models through llama-server") and `settings.json` names the llama-server backend: start `./build/tests/bin/Debug/morpheus_server.exe` in the background (Bash `run_in_background`), then `curl -s http://127.0.0.1:4998/health`. Expected: JSON with `"ready":true` and `"api_version":"1.0.0"`. Then stop it (TaskStop) and check `tasklist | grep -i morpheus` shows nothing. If no llama-server is running, skip this check and say so in the report.

- [ ] **Step 5: Commit**

```bash
git add CMakeLists.txt tools/server/ tests/server/ServerLiveTests.cpp
git commit -m "morpheus_server: the executable (flags, settings, both servers checked, signals) and its live test"
```

---

### Task 12: Documentation

**Files:**
- Modify: `README.md`, `CLAUDE.md`
- Create: `CHANGELOG.md`
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, `docs/superpowers/specs/2026-11-16-hosted-world-design.md`

**Interfaces:**
- Consumes: the finished server (Tasks 1-11).
- Produces: documentation only.

- [ ] **Step 1: README: a "morpheus_server" section**

Add a section `## morpheus_server (hosted runs over HTTP)` after `## Play in the terminal`, with:

- What it is, in two sentences: a localhost HTTP server through which a host (the Skyrim/Mantella fork) plays hosted runs; design in `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, hosted runs in `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, the host data and request format in `tests/hosted/fixtures/README.md`.
- Build: it builds in every top-level preset (`MORPHEUS_BUILD_SERVER`, default on when top level), with no llama.cpp; the binary is `build/<preset>/bin/<config>/morpheus_server(.exe)`.
- Run: start the two llama-servers as in "Models through llama-server" (with `--parallel 3` or more: slot 0 is for folding, the others for conversations), set `"backend": "llama_server"` in settings.json (its `server.slot` is not used by the server, which hands out slots itself), then:

````markdown
```
./build/dev/bin/Release/morpheus_server --port 4998 --runs runs --settings settings.json
```
````

- The flags and defaults (`--port 4998`, `--runs ./runs`, `--settings ./settings.json`), that it listens on 127.0.0.1 only, that a run's file is `<runs>/<run_id>.morpheus`, and that Ctrl+C stops it (running turns commit nothing).
- The endpoint table from the spec's section 1 and the turn stream's events, in short.
- An example session with curl (open with `tests/hosted/fixtures/open.json`, start a conversation, record an event, play a turn with `curl -N`, end the conversation), with the stream's shape shown as text:

````markdown
```
curl -s -X POST http://127.0.0.1:4998/v1/runs/save_1/open -H "Content-Type: application/json" --data @tests/hosted/fixtures/open.json
curl -s -X POST http://127.0.0.1:4998/v1/runs/save_1/conversations -H "Content-Type: application/json" \
  --data '{"participants": ["0001A6A4", "00000007"], "observation": '"$(cat tests/hosted/fixtures/observation.json)"'}'
curl -N -X POST http://127.0.0.1:4998/v1/runs/save_1/conversations/c1/turn -H "Content-Type: application/json" \
  --data '{"speaker": "0001A6A4", "player_text": "Can you follow me?", "observation": '"$(cat tests/hosted/fixtures/observation.json)"'}'
```

```
event: action
data: {"action":{"id":"follow","params":{}},"api_version":"1.0.0","speaker":"0001A6A4"}

event: token
data: {"text":"Lead"}
...
event: sentence
data: {"text":"Lead the way, then."}

event: done
data: {"action":{"id":"follow","params":{}},"events_omitted":0,"line":"Lead the way, then.","speaker":"0001A6A4","timings":{...},"turn":1}
```
````

- The errors table (400, 404, 409, 422, 503, 500) in one short list.
- The live test: `"[server]"` now also plays the conversation fixture over HTTP and prints each turn's time to its first sentence. Write no figure: the controller adds the measured one after running the test against the owner's servers.

In `## Layout`, add:

```markdown
- `src/server`: `morpheus_server`'s library: run workers, slots, the transport-free API and the HTTP adapter (cpp-httplib; every top-level preset, `MORPHEUS_BUILD_SERVER`)
- `tools/server`: `morpheus_server`, the HTTP server for hosted runs (every top-level preset)
```

and in `### The [server] tests`, add one sentence: the server's own `[server]` test plays the conversation fixture over HTTP against your servers and prints each turn's time to its first sentence.

- [ ] **Step 2: CHANGELOG.md**

Create `CHANGELOG.md` at the repository root:

```markdown
# Changelog

The morpheus_server API's versions (`api_version` on every response). A breaking change bumps the major version and
gets an entry here with a migration note.

## API 1.0.0

The first version (design: `docs/superpowers/specs/2026-11-16-morpheus-server-design.md`).

- `GET /health`; `POST /v1/runs/{run_id}/open`; `POST /v1/runs/{run_id}/conversations`;
  `POST /v1/runs/{run_id}/conversations/{cid}/turn` (Server-Sent Events: `action`, `token`, `sentence`, then `done` or
  `error`), `/events`, `/participants`, `/end`; `GET /v1/runs/{run_id}/conversations/{cid}`; `POST /v1/tokenize`;
  `POST /v1/embeddings`.
- Errors: 400 (a malformed request, naming the field), 404, 409 (a run not opened in this process), 422 (the
  observation and player text do not fit the tail), 503 (a llama-server unreachable), 500.
- `speaker: "decide"` is refused with 400 until a later version.
```

- [ ] **Step 3: CLAUDE.md**

- In "Build and test", after the `MORPHEUS_GPU_BACKEND` line, add: `` - `MORPHEUS_BUILD_SERVER` (default on when top level) builds `morpheus_server` and its library `morpheus_server_lib` in every preset, without llama.cpp; it needs `MORPHEUS_BUILD_LLAMA_SERVER`. ``
- Change the "Real-server tests" line's end to mention that `[server]` also covers `morpheus_server` over HTTP, and add: `` - The server's own tests that need no real server are tagged `[hosted-server]`. ``
- In "Layout", change the `src/llamaserver` line's "Only it and the tests include <httplib.h>" to "Only it, src/server/HttpServer.cpp and the tests include <httplib.h>", and add after it:

```markdown
- src/server: `morpheus_server_lib`, the server for hosted runs: `SlotPool` and `SlotBackend` (a llama-server slot per conversation, slot 0 for folding), `RunWorker` (one thread per run owning its `HostedSession` and backend, folding when idle), `RunRegistry` (run ids and files), `Api` (every route and status, without sockets), `TurnStream`, `SentenceSplitter`, `HttpServer` (cpp-httplib, Server-Sent Events). Never links llama.cpp.
- tools/server: `morpheus_server` (`--port`, `--runs`, `--settings`), every top-level preset.
```

- [ ] **Step 4: The architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`'s module table, add a row after `turn`:

```markdown
| `server` | `morpheus_server` for hosted runs: a worker thread per run owning its `HostedSession` and backend, a llama-server slot per conversation (slot 0 for folding), a transport-free `Api` and an `HttpServer` on cpp-httplib streaming each turn as Server-Sent Events; the `morpheus_server` executable in `tools/server`. Behind `MORPHEUS_BUILD_SERVER` (default on when top level); never links llama.cpp | turn, llamaserver |
```

In its `hosted_turns` table row, add `, events omitted` after `timings`. In "## Hosted runs", replace the sentence "Every event recorded since a conversation's last turn joins its next turn's tail; when they alone are over `tail_allowance`, every later turn of that conversation is a `TurnError` until it ends." with "Every event recorded since a conversation's last turn joins its next turn's tail; when the tail is over `tail_allowance`, the oldest events leave the prompt (and that turn's L3 entry) until it fits, the turn records how many (`eventsOmitted`, store version 4), and the store keeps them all." and replace "A slot per conversation and any concurrency across conversations are left to D2 (`morpheus_server`), which can add them without changing this API." with "`morpheus_server` (module `server`) keeps that contract: one worker thread per run, a slot per conversation."

- [ ] **Step 5: The hosted-world spec's section 6**

In `docs/superpowers/specs/2026-11-16-hosted-world-design.md`, replace the bullet that starts "**Events recorded between turns that do not fit are a `turn::TurnError` too.**" with:

```markdown
- **Events recorded between turns that do not fit leave the prompt, oldest first.** This replaced D1's `TurnError` in sub-project D2 (`docs/superpowers/specs/2026-11-16-morpheus-server-design.md`, section 3): the newest events that fit stay in the tail, the turn records how many it left out (`eventsOmitted`), its L3 entry leaves out the same ones, and the store keeps them all. The observation, the player text and the instruction must still fit on their own, or the turn is a `TurnError`.
```

and in its section 1, change "(section 6 for the overflow)" to "(section 6 for the overflow, as D2 changed it)".

- [ ] **Step 6: Check the docs**

Run: `LC_ALL=C grep -c $'[\x80-\xFF]' README.md CLAUDE.md CHANGELOG.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md docs/superpowers/specs/2026-11-16-hosted-world-design.md`
Expected: 0 for each.

- [ ] **Step 7: Commit**

```bash
git add README.md CLAUDE.md CHANGELOG.md docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md docs/superpowers/specs/2026-11-16-hosted-world-design.md
git commit -m "Document morpheus_server: README section, CHANGELOG for API 1.0.0, CLAUDE.md layout, the architecture and hosted-world specs"
```

---

## Final verification (controller)

- `ctest --preset tests` and `ctest --preset asan`: all pass; the `[server]` tests skip.
- `cmake --build --preset dev`: clean, including `morpheus_server`.
- The consumer check (README, "Using Morpheus from another project"): ok.
- With the owner's llama-servers running: `./build/tests/bin/Debug/morpheus_tests.exe "[server]"`, and the measured first-sentence figures go into the README.
- No `morpheus_tests`, `morpheus_server` or `ctest` process left running.
