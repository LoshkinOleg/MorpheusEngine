# E2 final fix round: requirements

The final whole-branch review of sub-project E2 (rolling a hosted run back) is `.superpowers/sdd/e2-final-review.md`;
read it for the file:line detail and the scenarios. The controller decided the items below (2026-11-15). Make each
numbered item its own commit, tests first, in this order. HEAD at the start: 9292990.

## 1. A rollback hides every condense that holds what it hid (I1)

Today a condense is hidden only when its `to_turn` is past the run's last visible turn. A condense can hold a shared
copy made after the rollback time, or a fold whose range reaches past the condense's own `to_turn` (interleaved
conversations), and then it stays visible with what the rollback hid.

- In `ApplyRollback`, after the existing hides, repeat until nothing changes: hide (with this rollback's id) every
  visible summary that a link in `summary_supersessions` names as the condense of a summary this rollback hid.
- Store tests in `tests/store/RollbackStoreTests.cpp`, one per scenario in the review: a condense of a shared copy
  clocked after T is hidden with the copy; a condense whose range ends before a hidden fold's end is hidden with that
  fold. In each, check what is shown after the apply and that an undo restores the condense.
- Update the spec's rule (section 3, the hidden-summary bullet) to say so. Edit only that bullet.

## 2. Abandon only when something will be recorded (I2)

The spec says the first new record abandons the rollback ("a turn committed, events recorded, ..."). Today `RunTurn`
abandons before its catch-up, so a cancelled turn or one that fails (InferenceError, TurnError) drops the hidden future
having recorded nothing; `ShareMemories` abandons when the share is already pending, and `RecordEvents` abandons for an
empty `events` array.

- `RunTurn`: abandon immediately before `CommitHostedTurn`. Everything before it (catch-up folds) is allowed while a
  rollback is open and carries `made_in`, so an undo deletes it and an abandon keeps it.
- `ShareMemories` and `RecordEvents`: abandon only on the path that writes (a new pending share or copies; a non-empty
  events array). If `RecordEvents` with no events writes nothing today, it must not abandon.
- Tests in `tests/hosted/HostedRewindTests.cpp`:
  - each of `StartConversation`, `SetParticipants`, `EndConversation`, `ShareMemories` (a share that writes) and
    `RunTurn` abandons (one SECTION each; check `Rewind` past the old latest time restores nothing afterwards, or read
    the store's open rollback through a test-visible path the session already has);
  - a turn cancelled through its stop token, and a turn whose model call fails, leave the rollback open (a later rewind
    to the old latest time restores the hidden turns);
  - a refused request (an event without `text`; ending an ended conversation) leaves the rollback open;
  - a share that is already pending leaves the rollback open.
- Update the `Rewind` doc and the comments that describe where each writer abandons.

## 3. An undo clears its saved marks (M2)

`UndoRollback` sets `saved_marks = '{}'` with the status `undone`, as `AbandonRollback` does, so the file does not grow
on every reload. Test it.

## 4. A clock that is an empty string is refused (ledger, Task 1)

`"clock": ""` in the host data is a `DataError` at `/clock` (it would otherwise silently mean no clock; a run without a
clock omits the key). Test it.

## 5. Tests and docs the review lists as before-merge

- `HostedSummaries()` gets its own hidden-row test (it feeds the retrieval index).
- `RunStore.h`: the class comment's table list names `summary_supersessions`.
- The `Rewind` doc names the InferenceError that `SyncHistory` can throw (embedding), and says the store has already
  rewound when it does (the next sync repairs the index).
- The architecture spec: the wording fixes the review lists (M5).

## Finally

- `ctest --preset tests`, then `cmake --build --preset asan` and `ctest --preset asan`: both pass.
- `"[turn]"`, `"[replay]"`, `"[context]"`: unchanged (Morpheus's own game).
- `"[rewind]"`, `"[rollback]"`, `"[hosted]"`, `"[hosted-server]"` pass; the reload fixture still passes in-process and
  over HTTP.
- Byte checks on every changed file.
- Report which review minors you left, in the report's last section.
