# D1 Task 2 review: RunStore schema version 3 (4ed97c7..4891ccc)

### Spec Compliance
- Spec compliant (section 5 and the brief's Interfaces). The code matches the brief verbatim except for the one header comment in Minor 1. Checked:
  - Meta `kind` is written by CreateWith (src/store/RunStore.cpp, metaRows in CreateWith) and read and validated in Open (src/store/RunStore.cpp:288-296 area, the `kindText` block). An unknown kind is a StoreError naming the file and `run_meta 'kind'`. A v3 file with no `kind` row is a StoreError from the existing `value()` lambda (src/store/RunStore.cpp:226-232), which names the file.
  - The tables `host_data`, `conversations`, `hosted_turns` and `conversation_events` are at src/store/RunStore.cpp:63-92. Every file gets every table (CreateWith at :177-180).
  - Upgrade (src/store/RunStore.cpp:236-246): it runs only for `"2"`. The DDL, the `kind` insert and the version update all run inside `Database::Transaction`, which is BEGIN IMMEDIATE / COMMIT with ROLLBACK on any throw. I checked that in src/store/Sqlite.h:75-89 (named risk: a half-upgraded file). SQLite DDL is transactional, so a failure leaves the file at version 2. The in-memory `values` change only after the commit. Existing tables and rows are not touched; only `run_meta` gets one new row and one updated value.
  - Other versions are still refused, through the unchanged check at src/store/RunStore.cpp:247-250 ("'<file>': schema version N is not the supported version 3"). That is pinned by tests/store/RunStoreTests.cpp:521-533.
  - Owned runs: Create keeps its old behavior, plus a `kind` row, the empty hosted tables and a ContractError for a hosted meta (src/store/RunStore.cpp, `RunStore::Create`). No owned method changed. `RequireHosted` guards every hosted method.
  - The ids `c1`, `c2`, ... come from COUNT+1 in creation order (src/store/RunStore.cpp:558-579), and `position` is UNIQUE.
  - core/HostedRecord.h matches the brief's types field for field, initializes every member at declaration and is ASCII. It depends only on std and nlohmann, so `core` gains no library dependency.
- Cannot verify from the diff alone:
  - Whether HostedSession (Tasks 5-8) re-validates stored events and observations before it renders them. The plan's own code at plan lines 6597, 6608 and 7413 reads them with `.at("text").get<std::string>()` and `ActorsOf(turn.observation)` (see Important 1).
  - The report's test counts (439, asan 439). I did not re-run them.

### Strengths
- The upgrade is minimal and atomic. It adds tables and one meta row inside one IMMEDIATE transaction and validates nothing before writing, so a v2 file's data really is untouched. The test builds a genuine v2 file from the old schema with turns, snapshots, summaries and notes, and checks:
  - the data reads back through the v3 API;
  - the raw `schema_version` and `kind` rows;
  - that reopening is idempotent;
  - that a bad `kind` is refused (tests/store/HostedStoreTests.cpp:231-295).
- `CreateWith` puts the shared create path in one place with two callers, as the helper rule requires. Removing a partial file on failure still covers both kinds, and the kind check runs before any file is touched; the test pins that (tests/store/HostedStoreTests.cpp:303-308).
- Damaged-data messages name the file, the record (conversation id or hosted turn number) and the column (src/store/RunStore.cpp, the `damaged` lambdas in Conversations and HostedTurns).
- `ReplaceHostData` and `FoldConversation` write their two rows in one transaction (src/store/RunStore.cpp:551, :658). The fold test checks that a bad range leaves the summaries unchanged (tests/store/HostedStoreTests.cpp:216-217).
- I checked the constraints that apply to every file:
  - Layout and naming in RunStore.h (src/store/RunStore.h) follow the class-layout rule: nested types, then the public constant, private data, public methods, private methods.
  - All five files are ASCII (`file` reports ASCII text).

### Issues
#### Critical (Must Fix)
- None.

#### Important (Should Fix)
1. **Stored events and observations are not checked for shape, so damaged saved data escapes as a raw nlohmann exception instead of a StoreError naming the file (plan-mandated: the brief's verbatim code omits the check).**
   - Where it is missing:
     - `HostedTurns` checks only that the `events` column is an array (src/store/RunStore.cpp:793-799).
     - `ConversationEvents` checks nothing beyond JSON syntax (src/store/RunStore.cpp:694).
     - `observation` is not checked to be an object (src/store/RunStore.cpp:800).
   - This contradicts the header's own contract at src/store/RunStore.h, HostedTurns: "Throws StoreError if a stored column is ... has the wrong shape". It also breaks the global error policy: "damaged saved data throws store::StoreError naming the file".
   - The plan's later readers dereference the stored events directly: `event.event.at("text").get<std::string>()` at plan lines 6608 and 7413, and `event.at("text")` at 6612. So a stored event that is not an object with a string `text` throws `nlohmann::json::out_of_range` / `type_error` in the middle of prompt assembly. That exception names neither the file nor the column.
   - The same file already validates participants, listener_ids, player_text and adjudication element by element, so this gap is an inconsistency, not a deliberate choice.
   - Fix: in both readers, require every event to be an object holding a string `text`, and throw `damaged("events", "holds an event without a string text")` or the conversation-event equivalent. In HostedTurns, require `observation` to be an object. Add one damaged-column test per reader (see Minor 4).

#### Minor (Nice to Have)
1. src/store/RunStore.h:25: `worldHash`'s comment still reads "World content hash at run creation." The brief's replacement header says "...; a hosted run's host data hash, kept current.", and `ReplaceHostData` now does change it. The report's "header text for existing parts kept as before" left the comment wrong for hosted runs.
2. src/store/RunStore.cpp:236-246: an upgrade that fails is a StoreError of the form "SQL failed: <sqlite message> | <whole HOSTED_SCHEMA_SQL>", from `Database::Exec` (src/store/Sqlite.cpp, Exec). Typical causes are a read-only or locked file, or a stray table. The message does not name the file, although every other Open error does. Wrapping the upgrade so the message names the file and "cannot upgrade from version 2" would match the rest of Open.
3. src/store/RunStore.cpp:739 and :741: `player_text`, `events`, `observation` and `listener_ids` are dumped without `error_handler_t::replace`, unlike `adjudication` and `timings`. A player text that is not valid UTF-8 and comes in through the C++ API would then throw `nlohmann::json::type_error` 316 at commit, after both model calls. Whether HostedSession rejects such text upstream cannot be verified from this diff.
4. tests/store/HostedStoreTests.cpp: none of the new StoreError branches is exercised. Untested are: host data that is not JSON or is missing, damaged participants, damaged listener_ids, player_text, events or adjudication, an event row that is not JSON, and a v3 file with no `kind` row. A failed upgrade that rolls back to version 2 is not tested either; a v2 file that already has a `conversations` table would do. Tests also do not cover `FoldConversation` on an unknown conversation or `ConversationEvents` / `HostedTurns` / `ReplaceHostData` on an owned run.
5. src/store/RunStore.cpp:581-590, used at :636, :647, :656, :671 and :719: every existence check calls `Conversation(id)`, which loads and parses every conversation's participants. That makes each check O(conversations), and a damaged participants column in one conversation makes writes to any other conversation throw. A `SELECT 1 FROM conversations WHERE id = ?1` would check existence without either effect.
6. src/store/RunStore.cpp:651-664: `FoldConversation` does not require `summary.toTurn` to be at least the conversation's current `folded_through` or at most `LatestHostedTurn()`, so a buggy caller could move the folded mark backwards or past the last turn without a ContractError.
7. src/store/RunStore.cpp:813: `adjudication.params` is not checked to be an object, while core::Adjudication documents it as one by its default. `timings` is not checked either (src/store/RunStore.cpp, HostedTurns), but that matches the owned path.
8. src/store/RunStore.cpp:74-92: `hosted_turns` and `conversation_events` have no index on `conversation_id`, so every `HostedTurns` / `ConversationEvents` call scans the whole run's table. Adding an index later means another schema bump, and this version is the cheap moment.
9. Owned-only methods do not refuse a hosted run: `CommitTurn`, `TruncateAfter`, `Turn` and `StateAt` (src/store/RunStore.h). The brief did not require this, but `TruncateAfter` on a hosted run would delete shared summaries and leave `hosted_turns` alone.
10. src/store/RunStore.cpp:724-727: the loop that copies `record.events` into a JSON array could be `const nlohmann::json events = record.events;`, as `listeners` is two lines above.
11. Commit 4891ccc ends with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`. The global constraints (d1-global-constraints.md:35) require `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. The report says this deviation was deliberate, following the implementer session's attribution. The owner decides which wins; it does not affect the code.

### Assessment
**Task quality:** Needs fixes
**Reasoning:** The schema, the atomic in-place upgrade, the refusal of other versions and the unchanged owned path are correct and well tested. However, stored events and observations are not shape-checked, so damaged hosted data reaches later tasks' `.at("text")` as a raw nlohmann exception rather than a StoreError naming the file and column.

## Re-review 1 (4891ccc..b4e639f)

### Spec Compliance
- Spec compliant. The fix commit stays inside src/store and tests/store; no interface named by the brief changed.
- Cannot verify from diff: the report's test counts (446 fast and asan); not re-run.

### Resolution of round-1 findings
- Important 1: resolved.
  - `EventIsWhole` (src/store/RunStore.cpp:110-113) is applied to every element of `hosted_turns.events` (src/store/RunStore.cpp:835) and to every `conversation_events` row (src/store/RunStore.cpp:732). Either one failing is a StoreError naming the file and the column or conversation.
  - `observation` must be an object (src/store/RunStore.cpp:841).
  - Tested in tests/store/HostedStoreTests.cpp, "Damaged saved hosted turns and events ...": raw-SQL damage covers events and observation (both the bad-JSON and wrong-shape cases), and five conversation-event cases each assert "run.db" plus the column or conversation.
  - Checked against the plan's later readers (`event.at("text").get<std::string>()` at plan lines 6608 and 7413): they can no longer see an event without a string text from a file.
- Minor 1 (worldHash comment): fixed (src/store/RunStore.h:25).
- Minor 2 (upgrade error names the file): fixed.
  - The `catch (const StoreError&)` around the upgrade's `Transaction` (src/store/RunStore.cpp:262) rethrows "'<file>': cannot upgrade from schema version 2 to 3: ...". It also covers a failed BEGIN IMMEDIATE.
  - "A failed upgrade names the file and leaves it a version 2 run" checks the message, that `schema_version` is still 2, that there is no `kind` row, and that `host_data` is gone: the rollback took back DDL that had already run. That is direct evidence the upgrade is atomic.
- Minor 3 (non-UTF-8 at commit): fixed for `CommitHostedTurn` (`DumpText` on all six JSON columns), and tested. Partly open; see new Minor 2.
- Minor 4 (StoreError branches untested): fixed. All of these have their own tests now:
  - host data that is not JSON, and host data that is missing;
  - four damaged-participants shapes;
  - a missing `kind`;
  - 15 turn-column cases and 5 event cases;
  - the failed upgrade;
  - the indexes, on a new file and on an upgraded one;
  - the fold bounds;
  - the kind guards in both directions.
- Minor 6 (fold bounds): fixed.
  - `FoldConversation` now requires `toTurn > foldedThrough` and `toTurn <= LatestHostedTurn()`; the header comment says so.
  - Checked against the plan's only later caller (HostedSession::Upkeep, plan line 7538): it folds a conversation's unfolded committed turns, so both bounds hold there.
  - `LatestHostedTurn()` in the message is evaluated only on failure (MORPHEUS_REQUIRE, src/core/Errors.h:23-29).
- Minor 7 (params shape): fixed (`adjudication.at("params").is_object()`), and tested.
- Minor 8 (indexes): fixed. Both indexes are in HOSTED_SCHEMA_SQL, so new files and upgraded files get them, and the test checks both.
- Minor 9 (owned writers on a hosted run): fixed. `RequireOwned` guards `CommitTurn` and `TruncateAfter`.
  - It is a private helper with two callers.
  - The header documents both.
  - No hosted caller of either exists in the plan; I grepped the plan for `TruncateAfter` and `CommitTurn(`.
- Minors 5 and 10: left for the ledger, as the controller decided. The commit trailer: accepted per the owner's rule.

### New issues
#### Critical (Must Fix)
- None.
#### Important (Should Fix)
- None.
#### Minor (Nice to Have)
1. src/store/RunStore.h:158: `ConversationEvents`'s comment still reads "Throws StoreError when one is not valid JSON". It now also throws when an event is not an object with a string `text`.
2. src/store/RunStore.cpp:710, :601, :664, :162, :577: the report says "all JSON columns go through DumpText", but only `CommitHostedTurn` uses it.
   - These hosted writers still call plain `dump()`: `AddConversationEvents` (the event), `CreateConversation` / `SetParticipants` (participants' names and bios), and `CreateHosted` / `ReplaceHostData` (the host data).
   - Text from the C++ API that is not UTF-8 in any of them still throws `nlohmann::json::type_error` 316.
   - Either route these through `DumpText` too, or decide that `DumpText` covers model output only and say so in its comment.
3. src/store/RunStore.cpp, CommitHostedTurn and AddConversationEvents: the new read-side shape checks have no write-side counterpart.
   - A caller that commits an event without a string `text`, a non-object observation or non-object params writes without complaint.
   - The next read then reports it as a damaged file (StoreError), so an engine bug is misreported as file damage.
   - A `MORPHEUS_REQUIRE` with `EventIsWhole` / `is_object()` on write would fail fast and point at the caller. `EventIsWhole` would then have the two callers it already justifies.
4. tests/store/HostedStoreTests.cpp, "A hosted turn whose text is not UTF-8 still commits": the test asserts only that the commit does not throw and that one turn reads back. It does not check what was stored. For example, it could check that `playerText` holds the replacement character, which would pin the lossy replacement as intended behavior.

### Assessment
**Task quality:** Approved
**Reasoning:** Important 1 is resolved and tested against the exact shapes the later tasks dereference, and every scheduled Minor is fixed with a test. The failed-upgrade test now proves the rollback leaves a version 2 file. What remains are two documentation and consistency gaps and a write-side check, all Minor.
