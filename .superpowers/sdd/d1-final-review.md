# D1 (hosted world) - final whole-branch review

Range: 759a892..283b108 (15 commits on `master`, not pushed). Reviewer: final whole-branch review, read-only.
Inputs read: the spec (`docs/superpowers/specs/2026-11-16-hosted-world-design.md`), the plan's Global Constraints and
Choices (`.superpowers/sdd/d1-global-constraints.md`), R2, R4, R5, R7, R8 and sections 5-6 of the integration brief,
the progress ledger's D1 section, and the code: `HostedRecord.h`, `RunStore.{h,cpp}` (hosted part and the upgrade),
`Schema.{h,cpp}`, `PromptBuilder.{h,cpp}`, `Transcript.{h,cpp}`, `Folding.{h,cpp}`, `Retrieval.{h,cpp}` (diff),
`MemoryUpkeep.{h,cpp}`, `HostedWorld.{h,cpp}`, `HostedSession.{h,cpp}`, `Session.{h,cpp}` (diff and `Load`),
`EngineText.h`, the hosted tests (folding, turn, session lists), the changed pre-existing tests, the fixtures README,
`CLAUDE.md` and the architecture spec diff. No build or test was run (evidence as reported: 499 tests in `tests` and
`asan`, dev targets, `[llama]` incl. the hosted conversation, the consumer check).

## Strengths

- **Module rule holds.** `core` includes only `core`; `store` only `core`; `grammar` only `core`; `context` only
  `core` and `inference`. The hosted record types sit in `core/HostedRecord.h` (header only, nlohmann only), so `store`
  stores them without reaching `turn`. No CMake dependency was added (`src/turn/CMakeLists.txt` only lists sources).
- **Morpheus's own game is unchanged.** `Session.cpp`'s diff is a mechanical move: `Pass` builds the same
  `ContextInputs`, `Upkeep` hands `MemoryUpkeep` the same seeds (turn, attempt), the same `AddSummary` +
  `SyncHistory(toTurn)` fold sink and the same tail text ("the input and the state"); `Settle` is the old `Maintain`
  loop line for line; `FoldOldest` with `wanted = half` is the old fold. `TranscriptEntries` reproduces
  `RenderTranscript`'s pairs (digests in L3, full narration for folds) and `SummarizeFallback`'s old
  "Turn N: input mechanics" text via `fact`. No `Session` test changed; the pre-existing tests that changed
  (`PromptBuilderTests`, `ModelSeatTests`, `RunStoreTests`, `FoldingTests`, `SchemaTests`) change inputs, not
  expected prompt text. The only owned-visible text change is the lore embed failure prefix (plan Choice).
- **Cache layout is right.** L0 has no variable; the participants follow L1 so a join/leave invalidates only from
  there; L2 is at the end of the system message; L3 is two messages per turn rendered only from the stored turn and
  its own observation (names never drift); new events render before the player line of the turn they preceded, so
  the next turn's prompt starts with this turn's stable part; the speak prompt is `ExtendForNarrate` of the resolve
  prompt; the fold prompt starts with the same system message and entries. The prefix and join tests pin this.
- **Error policy is mostly exact.** Host data faults are `DataError`s with pointers (`catalog at /actions/2/params/x`,
  `observation at /actors/1/id; ...`, `allowed_actions/0`, `events/1/text`, `speaker`, `conversation_id`,
  `participants/<i>`, `joined/<i>`, `left/<i>`), all raised before any model call; damaged hosted columns are
  `StoreError`s naming file, turn and column; `Adjudicate` never throws for model text (any `json::exception`);
  backend failures pass through as `InferenceError`; writers of the wrong run kind are `ContractError`s.
- **Writes are consistent under a stop.** Nothing is written before the resolve output; a fold lands atomically
  (`FoldConversation`: summary + folded mark in one transaction), its history sync is idempotent and redone on open;
  request events are kept only in the committed record, so a cancel leaves no orphan events; `Maintain` lands each
  fold as it completes and simply resumes. The run-wide turn number is taken from `LatestHostedTurn()` and checked
  again at commit.
- **The extended `Validate`** is a clean split of one compiler (`Use::Grammar` / `Use::Validation`) with
  `FromSchema` still refusing every extension, and `CheckValidationSchema` makes the host's schema fail at open.
- **Docs** (architecture spec section "Hosted runs", store table, CLAUDE.md layout, fixtures README) match the code,
  with one overstatement (Minor 5).

## Issues

### Critical (must fix)

None.

### Important (should fix)

1. **D2's concurrency model is undecided, and the D1 shape constrains it.**
   - Files: `src/turn/HostedSession.h:47-49` (one thread, not thread-safe), `src/turn/HostedSession.cpp:244` (the
     run-wide turn number is taken when the turn starts), `src/turn/HostedSession.cpp:281-306` and
     `src/turn/MemoryUpkeep.cpp:150-153,202-205` (no call sets `GenerationSpec::slot`).
   - What: R2 asks for 3 concurrent sessions with >80 % prompt reuse, and D2 is to give "one slot per conversation"
     and fold "asynchronously after end" (R4). As built, all conversations of a run share one `HostedSession`, one
     backend and its default slot; two `HostedSession`s on one file would race on `LatestHostedTurn()+1` (the
     second commit is a `ContractError`, an engine-bug type, for what would be a race) and on SQLite writes.
   - Why it matters: it decides whether D2 needs only additive changes or a rework of `RunTurn`. With turns of one
     run serialized (a per-run worker; `Maintain` run when idle and cancelled through its `stop` when a request
     arrives, which its "can simply run again" contract already supports), the public API holds and D2 needs only an
     additive way to route a conversation's calls (resolve, speak, its folds) to its slot, for example a slot on
     `HostedTurnRequest` or a conversation-to-slot map in the session. Parallel turns within one run would instead
     need the turn number assigned at commit (which changes the sampling seeds) and a store guarded across threads.
   - Fix before push: record the decision (serialize per run; slot routing is D2's additive change) in the spec's
     D2 bullet and in `HostedSession.h`'s class comment. No code change needed in D1.

2. **A conversation can be wedged by events recorded between turns.**
   - Files: `src/turn/HostedSession.cpp:249-258` (every event recorded since the conversation's last turn joins the
     tail), `src/turn/MemoryUpkeep.cpp:50-53` (tail over `tail_allowance` with no hit left is a `TurnError`).
   - What: `RecordEvents` persists events with `after_turn` = the conversation's last turn; they are all shown in the
     next turn's tail and nothing can remove them. If the host records more event text between two turns than
     `tail_allowance` leaves after the observation (a busy scene, R8's ambient lines), every later `RunTurn` of that
     conversation throws `TurnError` for good; only `EndConversation` gets out. The `TurnError` message says the tail
     is too long but the host cannot shrink the stored part.
   - Why it matters: under D2 this is a permanent 4xx for one conversation from ordinary host behaviour, and the spec
     only foresaw the player text as the tail's overflow.
   - Fix: at minimum, before push, document it in `RecordEvents`' and `RunTurn`'s header comments and the spec's
     error section (the host keeps event text short or ends the conversation), with a test that pins the
     `TurnError`. A real cure (show only the newest recorded events that fit, keeping all of them in L3 after the
     turn) is an owner decision for D2.

3. **No test interleaves two open conversations.**
   - Files: `tests/hosted/HostedFoldingTests.cpp`, `tests/hosted/HostedTurnTests.cpp` (every test plays one
     conversation at a time; the only two-conversation test ends the first before the second starts).
   - What: run-wide numbering shared with summaries, per-conversation folded marks, `after_turn` of events, L3
     membership and retrieval of another conversation's folded turns are the cross-task heart of D1, and they are
     only exercised sequentially. Reading the code, they are correct (`Entries` tracks the conversation's own
     previous turn; `FoldConversation` checks the mark against the conversation; `SyncEntries` keys by run-wide
     number), but nothing pins it, and the ledger's Task 8 notes the gap.
   - Fix before push: one fake-backend test that alternates turns of c1 and c2 (numbers 1..n interleaved), records
     events in each between turns, checks each L3 shows only its own turns with the right events, folds c1 through
     the fit loop or end + `Maintain` while c2 stays open (summary range = c1's numbers, c1's mark moves, c2's
     mark and L3 unchanged), and retrieves c1's folded turn in c2's tail. Add a stop that lands between the two
     ended conversations of one `Maintain` (the first's fold kept, the second's not).

### Minor

1. **Damaged saved turns surface as host or engine errors, not `StoreError`.** `src/turn/HostedSession.cpp:427-432`:
   `HostedWorld::ActorsOf(turn.observation)` throws a `DataError` "observation at ..." (D2 would answer 400 to a
   valid request), and a stored turn whose speaker or player is not in its observation hits `MORPHEUS_REQUIRE`
   (`ContractError`). `RunStore::HostedTurns` only checks the observation is an object. Fix before push: have
   `HostedTurns` check the actors core it needs (an `actors` array of objects with string `id`/`name`, boolean
   `is_player`, exactly one player, and the speaker among them) and throw `StoreError` naming file, turn and column;
   `Entries` can then keep its `MORPHEUS_REQUIRE` as a true invariant.
2. **`Session::Load` does not check the run kind.** `src/turn/Session.cpp:109-115`: a hosted file is refused only by
   the world-id comparison; for a world whose id is `hosted`, `StateAt(0)` throws `ContractError` (no snapshot).
   Fix before push: refuse `kind != Owned` with a `DataError` naming the file, mirroring
   `HostedSession.cpp:106-109`, with a test.
3. **Stale comment.** `src/store/RunStore.h:158`: `ConversationEvents` also throws `StoreError` for an event that is
   not an object with a string `text`. Fix before push (one line).
4. **`Adjudicate`'s parse-failure comment names only lone surrogates.** `src/turn/HostedWorld.cpp:536` and
   `src/turn/HostedWorld.h:121-122`: numbers the parser cannot hold (`1e999`) are refused the same way. Fix before
   push (comment only).
5. **Doc overstatement on the stable prefix.** `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md:179`
   ("The prompt of a conversation's next turn starts with this turn's stable part, byte for byte"): true only when no
   fold landed in between, in any conversation, because L2 is run-wide; a fold elsewhere (or `Maintain`) changes
   every open conversation's system message from L2 on (L0, L1 and the participants stay). Fix before push: add
   that qualifier; it is also what D2 should expect from per-conversation slots.
6. **Condensed range with overlapping summaries.** `src/turn/Folding.cpp:85,105`: the condensed summary ends at the
   last summary's `toTurn` by `from_turn`, so with interleaved conversations (A 1-9, then B 2-4) L2 reads
   "Turns 1-4" for text covering 1-9. Plan Choice ("as today"); nothing reads the range in hosted runs except the
   L2 label. Can stay; using the maximum `toTurn` would be byte-identical for `Session` (its summaries are disjoint
   and ordered) if the owner wants it.
7. **`world_cap` is measured only when a prompt is built.** `src/turn/HostedSession.cpp:176-190` (`SetParticipants`)
   accepts a join that puts lore + participants over `world_cap`; the next `RunTurn` throws the `DataError`, and if
   the conversation is then ended, every `Maintain` throws at it (`HostedSession.cpp:355-372`), so ended
   conversations created after it never fold. Can stay for D1 (edge case); D2 or a follow-up should measure at
   `StartConversation`/`SetParticipants` or make `Maintain` skip and report.
8. **O(run) work per fold and per lookup.** `src/turn/HostedSession.cpp:482-492,516-523`: every fold re-renders and
   re-hashes every folded turn of every conversation (`SyncHistory`), parsing each stored observation;
   `RunStore::Conversation` (`src/store/RunStore.cpp:606`) parses all conversations for each existence check, and
   one damaged conversation blocks writes to the others. Fine for D1's sizes; a save line played for months will
   feel it. Can stay; D2 should sync only the newly folded entries and look conversations up by `WHERE id = ?`.
9. **Conversation ids are per run.** `src/store/RunStore.cpp:583` makes `c1`, `c2`, ... per file, while R4's routes
   are `/v1/conversations/{id}/...` without a run id. Not a D1 defect; D2 must namespace them (for example
   `<run_id>/c1`) and recover the mapping after a restart. Can stay; note for D2.

### Ledger Minors, triaged

| Task | Minor | Verdict |
|---|---|---|
| 1 | numbers compared as doubles (precision above 2^53) | Stay; optional one-line note in `Schema.h` |
| 1 | no tests for an optional key in a oneOf branch, a nested optional pointer, a direct unbounded string | Stay |
| 1 | `Schema.h` comment table misaligned | Stay (fix if the file is touched) |
| 1 | asan stall on `LlamaServerBackend ... reconnects after a restart` | Stay for this push (sub-project C code); investigate before D2 |
| 2 | existence checks parse all conversations; one damaged conversation blocks others | Stay; D2 (Minor 8) |
| 2 | events copy loop | Stay |
| 2 | `RunStore.h:158` comment stale | **Fix before push** (Minor 3) |
| 2 | plain `dump()` for events, participants, host data | Stay: their strings come from parsed JSON (valid UTF-8); only a C++ caller building invalid UTF-8 reaches `type_error` 316 |
| 2 | no write-side shape `MORPHEUS_REQUIRE` | Stay |
| 2 | non-UTF-8 commit test does not pin the replacement | Stay |
| 3 | `MemoryUpkeep.cpp` includes `Session.h` for `SamplingFrom` | Stay |
| 3 | `Settle`/`FoldAll` near-duplicates; condense-or-throw block x3 | Stay |
| 3 | no test for `FoldAll` folding a prefix, the condense seed, a cancel during condense | Stay (Important 3 covers the cancel between conversations) |
| 4 | `Adjudicate` comment names only lone surrogates | **Fix before push** (Minor 4) |
| 4 | LNK4300 | Stay (pre-existing) |
| 6 | reopen writes host data before re-embedding | Stay - see verdict below |
| 6 | no test pins the world overload's embed-failure prefix | Stay (optional one-line check) |
| 6 | close-then-remove copied from `Session` | Stay |
| 6 | damaged stored turn hits `MORPHEUS_REQUIRE` | **Fix before push** (Minor 1) |
| 7 | retrieval runs before the tail `TurnError` and ignores `stop` | Stay (as `Session`) |
| 7 | `CleanInput` test tagged `[hosted][turn]` | Stay |
| 8 | cancel test stops before the call; no interleaved fold test | **Fix before push** (Important 3) |
| 8 | loose `stableCount` bound; shared fold seed across ended conversations | Stay |
| 9 | radiant checks only `requested`; `RequestFrom` parallels `test::Request`; c1/c2 hard-coded | Stay |
| 9 | model quality on Qwen3-4B (repeated `give_item`, addressing the player by name, drift) | Stay: prompt content for sub-project F and the owner. Two levers for F: the speak instruction's "{player} is the player's character" invites naming the player; the system text gives no guidance that most turns need no action (the engine cannot name the host's "none" id, so the pack must) |

**Verdict on Task 6's reopen order (`HostedSession.cpp:110-114`):** it can stay. The stored host data is
write-only in D1: nothing in `src/` reads `RunStore::HostData()`, the stored hash only decides whether the next open
replaces the copy, and `SyncLore` runs on every open whatever the hash says. After a failed re-embed the file holds
the new data and its hash; the next open with the same data skips the replace and re-embeds again (same outcome as
if it had not been saved), and an open with the old data replaces it back. Vectors stored before the failure are
keyed by content hash and harmless. So there is no observable inconsistency. Swapping the two steps (embed, then
`ReplaceHostData`) is risk-free and would make "a failed open changes nothing" literally true, as in `Session::Load`;
worth doing only if D2 starts reading the stored copy (for example to reopen without the host resending data).

## Recommendations

- For D2: decide the per-run serialization (Important 1) in its spec first; route each conversation's resolve,
  speak and fold calls to its slot; run `Maintain` in idle time with a stop source the next request triggers; map
  `DataError` -> 400, `TurnError` -> 4xx with the message, `InferenceError` -> 503, `StoreError` -> 500.
- Extend the `[llama]` hosted test (or the `[server]` suite) to assert the speak call's reuse share from
  `timings["speak"]`, the measurable form of "the speak call extends the resolve call" and R2's acceptance.
- Keep the hosted record's `timings["retrieval"]` format aligned with `Session`'s (it is), so one inspector reads both.

## Assessment

**Ready to push: with fixes.**

**Reasoning:** The engine seam is sound: dependencies flow down, `Session` is untouched in behaviour, the hosted
prompt is cache-friendly by construction, writes are consistent under any stop, and the error classes are right
except for damaged stored turns. Before push: record the D2 concurrency decision and the event-overflow behaviour
(documentation), add the interleaved-conversations test, and make the small error-policy and comment fixes listed.
