# Task 8 report: SQLite wrapper and run database creation

Status: DONE

## Implemented
- SQLite 3.50.4 amalgamation via FetchContent (URL + SHA256 verified by CMake), `sqlite3` static target with SYSTEM include, `SQLITE_OMIT_LOAD_EXTENSION`, `SQLITE_DQS=0`.
- `morpheus_store` static library: `Database`/`Statement` RAII wrappers (`Sqlite.h/.cpp`), `RunStore`/`RunMeta` (`RunStore.h/.cpp`) creating the six-table schema, meta rows and turn-0 snapshot in one transaction.
- Tests linked against `morpheus_store`; 3 new `[store]` test cases.
- All code transcribed verbatim from the brief; no changes were needed.

## TDD evidence
RED: `cmake --preset tests`
```
-- Configuring done (6.3s)
CMake Error at src/store/CMakeLists.txt:1 (add_library):
  Cannot find source file:
    Sqlite.cpp
CMake Generate step failed.
```
(SQLite archive downloaded and passed the SHA256 check beforehand.)

GREEN: `cmake --preset tests && cmake --build --preset tests` built with no warnings under /W4 /WX; then
`./build/tests/bin/Debug/morpheus_tests.exe "[store]"`:
```
All tests passed (16 assertions in 3 test cases)
```
`ctest --preset tests`: `100% tests passed out of 44`.

## Files changed
- Modified: `CMakeLists.txt`, `tests/CMakeLists.txt`
- Created: `src/store/CMakeLists.txt`, `src/store/Sqlite.h`, `src/store/Sqlite.cpp`, `src/store/RunStore.h`, `src/store/RunStore.cpp`, `tests/store/RunStoreTests.cpp`

## Deviations from the brief
- Commit trailer is `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` (per the attribution instruction in this session) instead of the brief's `Claude Opus 5.5`.

## Self-review
- Matches the interface list in the brief; table names exactly as in global constraints.
- `RunStore::StateAt` binds an `int` to `Bind(int, std::int64_t)`: implicit widening, no warning, but note `Bind(1, 0)` style calls with literals could be ambiguous between int64 and string_view overloads only for `0`/null pointer; not used here.

## Concerns
- None blocking. Git warns about LF->CRLF conversion on the new files (autocrlf behavior, same as existing files).
- Commit: 5d84de4 "Add SQLite wrapper and run database creation".

## Fix round 1

### What changed
- `Database::Transaction` (Sqlite.h): `Exec("COMMIT")` moved inside the try block, so a failed COMMIT (for example SQLITE_BUSY) runs `RollbackNoThrow()` and rethrows. Doc comment updated.
- `Statement::ColumnFloats`: ragged blob size now throws `StoreError` (stored data), not `ContractError`.
- `RunStore::Open`: `rng_seed` parsed with `std::from_chars` into `uint64_t`, whole string must be consumed; otherwise `StoreError("'<file>': run_meta 'rng_seed' is not a valid seed: '<value>'")`. `settings` parse errors become `StoreError` naming file and `settings`.
- `RunStore::StateAt`: unparseable snapshot throws `StoreError` naming file and turn. Added private `_file` member (constructor now takes the path; Create and Open pass it). Header comments updated.
- `RunStore::Create`: the whole create path is in a try block; on any exception the Database (local to the try) is already closed by unwinding, then `std::filesystem::remove(file, ec)` (non-throwing) runs and the original exception is rethrown.
- Public signatures and the six table names unchanged.

### Tests added (tests/store/RunStoreTests.cpp, tag [store])
a. failed COMMIT (two connections; B holds SHARED via `BEGIN; SELECT COUNT(*)`; A's COMMIT hits SQLITE_BUSY as expected; afterwards A inserts 2 and only that row exists), b. corrupt rng_seed ("12abc", "-1", "", overflow "99999999999999999999"), c. unparseable settings, d. unparseable snapshot, plus a ColumnFloats ragged-blob test, e. Create cleanup (injection: a directory named `run.db-journal` next to the target; SQLite cannot open it as the journal, so the schema transaction fails after the file exists; no production hook needed; the test also retries Create successfully after removing the obstacle).
SQLite locking behaved as expected for (a); no deviation.

### Failing output before the fix (`./build/tests/bin/Debug/morpheus_tests.exe "[store]"`, excerpts)
```
A failed COMMIT ... : due to unexpected exception: SQL failed: cannot start a transaction within a transaction | BEGIN IMMEDIATE
rng_seed '12abc' / '-1': CHECK_THROWS_AS(RunStore::Open(file), StoreError) no exception was thrown
rng_seed '' : "invalid stoull argument" ; overflow: "stoull argument out of range" (not StoreError, no file/field)
settings: [json.exception.parse_error.101] ... (not StoreError)
snapshot: [json.exception.parse_error.101] ... (not StoreError)
ColumnFloats: ContractError "blob is not a whole number of floats"
Create cleanup: CHECK_FALSE(std::filesystem::exists(file)) FAILED: !true ; retry: "already exists"
```

### Passing output after the fix
```
All tests passed (43 assertions in 9 test cases)       # [store]
100% tests passed out of 50                            # ctest --preset tests
```

### Commands run
`cmake --build --preset tests`; `./build/tests/bin/Debug/morpheus_tests.exe "[store]"` (before and after); `ctest --preset tests`; `git add src/store tests/store`; `git commit`.

### Tests not written
None skipped.
