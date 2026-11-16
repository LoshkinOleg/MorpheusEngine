### Task 4: E2's follow-ups: the clock after a rewind, no resync for a rewind that changes nothing

**Files:**
- Modify: `src/store/RunStore.h`, `src/store/RunStore.cpp` (`LatestClock`), `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp` (`Rewind`), `docs/superpowers/specs/2026-11-16-rollback-design.md`
- Test: `tests/store/RollbackStoreTests.cpp`, `tests/hosted/HostedRewindTests.cpp`

- [ ] **Step 1: Write the failing tests**

`tests/store/RollbackStoreTests.cpp` (use the file's existing helpers for a run with clocked records): a run whose latest record is clocked 4.0; `ApplyRollback(2.5)` makes `LatestClock() == 2.5` (it was 2.0, the highest visible record, before the fix); `UndoRollback()` makes it 4.0 again; `ApplyRollback(2.5)`, then an abandon (as the file's abandon tests do), then `ApplyRollback(2.4)` makes it 2.4 (the most recent rollback, not the abandoned 2.5).

`tests/hosted/HostedRewindTests.cpp`: after a rewind to 2.5 of a run whose records reach 4.0 (with a clocked turn at 2.0), `RecordEvents` with one event: the stored event's clock is 2.5 (read it through the store's events as the file's other tests read clocks). And: a rewind that changes nothing (`gameTime` at or after the latest clock with no open rollback) returns `rewound == false` and makes no backend call (`backend.Calls()` and `backend.EmbeddedTexts()` unchanged).

- [ ] **Step 2: Run them to see them fail**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[rollback],[rewind]"`
Expected: LatestClock is 2.0 after the rollback; the event is dated 2.0.

- [ ] **Step 3: Implement**

`src/store/RunStore.cpp`, `LatestClock`'s query gains, in the union: `SELECT game_time FROM rollbacks WHERE id = (SELECT MAX(id) FROM rollbacks WHERE status <> 'undone')`, and the comment says why: after a rewind to T, the run is at T, so a record with no observation of its own is dated T, not with the last record before T (an undone rollback is gone; an abandoned one still marks the time play went on from). `RunStore.h`'s `LatestClock` comment says the same.

`src/turn/HostedSession.cpp`, `Rewind`: after `result.rewound` is computed, `if (!result.rewound) { return result; }` with the comment "Nothing changed: the index is as it was." before `SyncHistory()`. `HostedSession.h`'s `Rewind` comment notes it.

Rollback spec, section 1's last bullet ("The run's latest seen clock ..."): add "raised to the game time of the most recent rollback that is not undone, when that is higher" and the reason; section 3: a rewind that changes nothing does not resync.

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[rollback],[rewind],[hosted]"`, then `ctest --preset tests`
Expected: all pass (the reload fixture included).

- [ ] **Step 5: Commit**

```bash
git add src/store/RunStore.h src/store/RunStore.cpp src/turn/HostedSession.h src/turn/HostedSession.cpp docs/superpowers/specs/2026-11-16-rollback-design.md tests/store/RollbackStoreTests.cpp tests/hosted/HostedRewindTests.cpp
git commit -m "Rewind: records after a rewind are dated at its time; a rewind that changes nothing does not resync"
```

---

