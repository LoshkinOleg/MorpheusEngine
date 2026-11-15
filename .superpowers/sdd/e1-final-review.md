# E1 (per-speaker memory): final whole-branch review

Range: 25e2a5b..40ca5e5 (11 commits, 50 files, +2864/-369). Reviewer: Opus, read-only.
Method: the requesting-code-review template. Read the spec, the global constraints, the progress ledger from
"# Sub-project E1", the full source diff (store, upkeep, folding, retrieval, prompt builder, HostedSession, server) and
the changed tests; re-ran `[memory],[hosted],[replay],[turn],[context]` on the existing tests build (129 cases: 128
passed, 1 skipped, 1821 assertions). Live evidence ([llama][memory] on Qwen3-4B; five [server] tests on Qwen3.5-9B,
including the three-actor fixture over HTTP) is the controller's and was not re-run.

## Strengths

- The SummaryScope seam is clean and small: MemoryUpkeep reaches summaries only through `shown`, `storeFold`,
  `storeCondensed`, `foldInstruction` and `condenseMessage` (MemoryUpkeep.h:39-50). Session passes the run-wide scope
  with the exact old texts (Session.cpp:536-551), so the owned fold and condense prompts and seeds are byte-identical.
  The owned prompt's LayOut arithmetic is unchanged for `SummaryPlace::System` (PromptBuilder.cpp:88-104). No
  Session test or replay fixture was edited (ReplayTests.cpp only had two comments reworded), and [turn], [replay]
  and [context] pass.
- Store transactions are where the invariants need them: FoldConversation adds the summary, moves folded_through and
  the speaker's mark in one transaction (RunStore.cpp:858-875); CondenseActorSummaries supersedes and adds together;
  RememberFold moves only the actor's mark. The v2/v3/v4 upgrade is one transaction and rolls back on a damaged turn or
  a summary over no turn, with tests for both.
- Owned-only methods now refuse a hosted run (`RequireOwned` on AddSummary, ReplaceSummaries, Summaries) and
  `ReadSummaryRow` refuses a hosted row without its actor as a StoreError naming the file and column.
- The witness model is applied consistently in the three places a speaker can learn something: L3
  (`Entries(..., witnessId)`), its memory (catch-up folds over witnessed entries only), and retrieval (turn entries
  carry speaker+listeners, summary entries their owner; `Retrieve(..., witness)`).
- Catch-up before speaking is real and tested ("An NPC behind ... is caught up before it speaks" checks the resolve
  tail holds the new memory). Shares are idempotent (keyed by sourceId, including superseded copies), snapshots, and
  a failed ApplyShare re-runs safely.
- The three-actor fixture encodes R6's acceptance directly and runs in-process, over HTTP and live.
- Embeddings are never recomputed needlessly: Vectors reuses a stored vector by refId and content hash.

## Invariant walk (the focus areas)

Checked path by path; "OK" means no way found to make an NPC remember or retrieve an unwitnessed turn, nor to leave a
witnessed turn in neither its L3 nor its memory (outside the documented transient "behind" window that catch-up
closes before the NPC speaks).

- In-turn fold (fit loop): the speaker's L3 is its witnessed turns after folded_through; the fold summarizes the
  oldest of those, moves folded_through and the speaker's mark to toTurn together. Other witnesses become behind. OK.
- Catch-up before speaking: `CatchUp(conversation, speaker, conversation.foldedThrough)` folds witnessed turns in
  (mark, foldedThrough], then sets the mark. A mark is never above folded_through (catch-up targets are <= it; the v4
  upgrade sets marks equal to it), so FoldConversation's "mark only moves forward" precondition always holds. OK.
- Maintain, open conversations: every NPC witness is caught up to folded_through. OK.
- Maintain, ended conversations: MarkFolded to the last turn, SyncHistory, then catch-ups. A stop between them leaves
  folded_through ahead of some marks; the next pass (through = folded_through) finishes. The conversation is ended, so
  no L3 depends on it. OK (the Task 5 deviation; folded_through no longer means "memories written").
- Cancelled turn or Maintain: every store write is per-fold and transactional; partial progress self-heals. OK.
- Condense: per NPC, over its shown memories only (own and told), condensed has no conversation, originals kept and
  still found by shares. OK.
- Share: copies the teller's own (toldBy empty) summaries of that conversation; the teller's summaries cover only its
  witnessed turns. Pending until ended and the teller's mark reaches the last turn. OK.
- v4 upgrade: NOT OK for interleaved conversations, see I1.
- Retrieval: turn hits filtered by speaker/listeners, summary hits by owner, lore unfiltered. OK.

## Issues

### Critical (must fix)

None.

### Important (should fix)

**I1. The version 4 upgrade gives one conversation's fold to the NPCs of another conversation whose turns fall inside
its range.** RunStore.cpp:356-380 (`moveSummaries`).

- What: D1 folded a conversation's oldest unfolded turns, and `SummarizeTurns` gives the summary the range
  `entries.front().turn .. entries.back().turn` in run-wide numbers. With two open conversations interleaving (c1 turns
  1 and 3, c2 turn 2, a case D1 supports and HostedFoldingTests exercises), c1's fold of turns 1 and 3 is stored as
  1..3. The upgrade takes every turn numbered 1..3, so it copies c1's fold to c2's NPC witnesses (Saadia, who never
  heard it) and, seeing two conversations, labels it with no conversation (so the c1 witnesses can no longer share it).
- Why it matters: it is the one path found where an NPC ends up remembering something it did not witness, which is
  E1's core invariant. The test "A version 4 summary over turns of two conversations ..." (HostedStoreTests.cpp:834)
  asserts exactly this outcome, calling the 1..3 row "a D1 condense", but a plain D1 fold of c1's turns 1 and 3 is the
  same row. The Task 1 review accepted the empty label as "acceptable, safer"; it overlooked that the NPC set is also
  widened.
- Fix: decide a summary's conversation from its endpoints. A D1 fold starts and ends on turns of the conversation it
  folded, so when the turns numbered fromTurn and toTurn belong to the same conversation C, treat it as C's fold: copy
  it, labelled C, to the NPC witnesses of C's turns in the range only. Otherwise it is a condense: no conversation, the
  witnesses of every turn in range (today's behaviour, which the spec chose). The only misclassification left is a D1
  condense whose two ends happen to fall in one conversation: it then goes to fewer NPCs (memory lost, never leaked),
  the safe direction. Add the interleaved-fold case to the upgrade tests.
- Owner alternative: D1/D2 are unreleased (CHANGELOG "API 1.0.0 (unreleased)"), so v4 hosted runs exist only on dev
  machines. If the owner confirms none matter, this can be waived; then the test comment should say the row may also be
  an interleaved fold and that the upgrade over-shares it.

**I2. Cost: every memory write re-reads and re-renders the whole run; Maintain reads every turn of every conversation
on every pass.** HostedSession.cpp:632-651 (SyncHistory), 711-730 and 808-810 (its callers), 411-435 (Maintain).

- What: `SyncHistory` reads every conversation's turns twice (once for the witness map, once inside `Entries`), renders
  every folded turn (`Entries` scans all of a conversation's events for each turn: turns x events), then
  `MemoryIndex::Vectors` loads every stored turn embedding and every summary embedding of the run from SQLite to
  compare hashes. It runs after every fold, every catch-up fold, every condense, every applied share and every
  ended-conversation mark. A catch-up fold, a condense and a share change only summaries, yet redo the whole turn
  index. In D1 this ran once per fold; with E1 it runs once per fold per NPC witness (the speaker's fold is followed by
  one catch-up per other witness in Maintain), and on the turn's latency path when the speaker is behind (catch-up runs
  before the resolve call). Separately, Maintain now calls `HostedTurns` (full JSON parse of every turn) and
  `NpcWitnesses` for every conversation, open or ended, each pass, and RunWorker schedules a Maintain pass after every
  job, GETs included.
- Why it matters: each of these is O(run) per event, so O(run^2) over a playthrough, with a real constant (thousands
  of turns x KB-sized observations x a vector per turn). No needless re-embedding happens (content-hash cache), so this
  is latency and CPU, not model cost. A long Skyrim run will feel it first on the first sentence of a turn whose
  speaker is behind.
- Fix (small, can land before merge): split SyncHistory: catch-up folds, condenses and shares call only
  `_index.SyncSummaries`; the turn part runs only when folded_through moves (in-turn fold, MarkFolded, Open). Read
  `HostedTurns` once per conversation in SyncHistory. Larger follow-up (can wait): make Maintain skip a conversation
  whose NPC marks all reach its last turn with an SQL check that does not parse turns, and make Vectors/SyncSummaries
  incremental instead of reloading every vector.

### Minor (nice to have)

1. Spec deviation: "a mark of an unknown conversation" should be a StoreError (spec, Errors). Nothing reads marks
   except by (conversation, actor) key, so an orphan row is inert; either check it on open or drop it from the spec.
2. The upgrade silently skips a non-string listener id (RunStore.cpp:330) where every normal read
   (RunStore.cpp:1143-1147) throws a StoreError: fail fast here too.
3. The pending-share StoreError (HostedSession.cpp:448) names the share row, not the file (CLAUDE.md asks for the file).
   Needs a RunStore path accessor; logged in Task 6.
4. A turn's `timings.retrieval[].source` labels summary hits "history" (HostedSession.cpp:395). Add "memory".
5. Retrieval searches the speaker's shown summaries too (as the spec says), so `# Relevant` can repeat a memory the
   tail already shows, spending retrieval_cap. Consider excluding shown ones (spec change, owner's call).
6. Shared copies and v4-upgraded copies of the same text are embedded once per copy (refId is the row id). A vector
   could be reused by content hash; matters only at upgrade time and for large shares.
7. Events recorded while an NPC was away join the next turn's user message, so an NPC who rejoins and speaks sees
   events from its absence (the spec defines witnesses per turn only). Spec-level; note for F/G.
8. An NPC who left a still-open conversation cannot recall its unfolded turns from another conversation until it is
   folded or ended (inherited from D1: other conversations see only folded turns). Spec-level; the host should end
   conversations promptly.
9. Maintain with a stop already requested still does MarkFolded and SyncHistory for one ended conversation (Task 5).

## Triage of the minors the task reviews logged

Must fix before merge:
- Task 1 "v4 move ... an interleaved plain D1 fold also gets an empty conversation (acceptable, safer)": wrong; it is
  I1 (the NPC set is widened too).

Should fix with I2 (cheap, same code):
- Task 3 "SyncHistory reads HostedTurns twice per conversation"; Task 5 "Maintain reads every turn of every conversation
  each pass (O(run))".

Already fixed on the branch (verified in the diff):
- Task 4 "ReplayTests.cpp:38 comment stale" (now "the budget the replays were recorded with").
- Task 5 "ApiTurnTests FoldedThrough==1 weak" (now waits on Hulda's memory through the memories route).
- Task 5 "catch-up test doesn't assert before it speaks" (the test now checks the resolve tail holds the memory).
- Task 7 "CHANGELOG says only additions" (it now states the behaviour change).
- Task 1 "untested several conversations" (tested, but see I1 for what the test pins down).

Can wait (follow-up list):
- Task 1: untested guards (MarkFolded bounds, RememberFold empty conversation, CondenseActorSummaries mismatches,
  SetRememberedThrough unknown conversation, AddPendingShare empty fields); SetRememberedThrough not bounded by
  LatestHostedTurn (marks are always <= folded_through by construction).
- Task 2: ModelSeatTests "cannot be built" comment; MemoryUpkeep class comment reflow; no test of the scope constructor
  contract.
- Task 3: no positive in-session test that a speaker retrieves its own superseded summary after a condense (the unit
  test RetrievalTests:340 covers the filter); `<map>` include order; repeated vector-move loop.
- Task 4: redundant loop in the witness test.
- Task 5: Maintain with stop requested still marks once (Minor 9); NpcWitnesses could be static.
- Task 6: player check only at share time and only over this conversation's turns; StoreError names the row (Minor 3).
- Task 7: HTTP fixture's absent/count checks do not wait; unused `<vector>` in HttpMemoryTests.
- Task 8: arch spec "any other version is refused" should list 2, 3, 4 as upgradable; the non-echoed HTTP wait waits
  for one memory; ReplayTests comment about 140 columns; duplicated LlamaHostedTests overrides.

Nothing on the logged list is wrong beyond the Task 1 "safer" judgement above.

## Morpheus's own game

No change on the owned path found: SummarizePrompt/CondensePrompt receive the same texts; seeds unchanged; Session's
retrieval sources (ALL_SOURCES) do not include Summary; the owned store rows keep actor '' and `Summaries()` filters
on it with the same order; LayOut's System branch computes the same flags and L3 budget; ModelSeat and Replay still
recognise the owned instructions. The only owned-visible change is settings.example.json (summary_cap 2048 -> 1024,
tail_allowance 2048 -> 3072), the owner's decision: a fresh owned settings.json now condenses at half the cap and gives
L3 1024 fewer tokens. Existing settings.json files are untouched, and no owned validation was added.

## Errors and statuses

- Hosted Open refuses summary_cap >= tail_allowance as a DataError naming both keys, before touching the file.
- Damaged hosted summary rows, damaged turns read by the upgrade, a summary over no turn, and a pending share of a
  missing conversation are StoreErrors.
- Server: share answers 404 for an unknown conversation (RequireConversation first), 400 for the session's DataError
  ("from:", "to:"); GET memories answers an empty list for an unknown actor. A share whose copies cannot be embedded
  is an InferenceError (503) after the copies were stored; resharing copies nothing new, so a retry is safe.

## Recommendations

- Land I2's split of SyncHistory with this branch; it is a few lines and removes the per-NPC multiplier.
- Keep a follow-up for an incremental index (append new turn/summary entries instead of reloading every vector) before
  long-run Skyrim testing.

## Owner decisions

1. I1: fix the v4 upgrade by endpoints (recommended), or waive it because v4 hosted runs are unreleased dev data.
2. Maintain is now much longer (a catch-up call per behind NPC per conversation), and D2's decision "only turns stop a
   fold" means an event POST, a share, a participants change or a GET memories waits behind a whole pass (RunWorker.cpp
   comment at 197-198). Revisit: let any job stop Maintain (it is resumable and keeps finished folds), or have Maintain
   stop between NPC catch-ups when a job is queued.
3. Reminder from Task 4: the owner's local settings.json still has summary_cap 2048 / tail_allowance 2048, which every
   hosted Open now refuses (morpheus_server answers 400 on open).

## Assessment

**Ready to merge? With fixes.**

**Reasoning:** The design is implemented faithfully and the owned game is untouched; the invariants hold on every
live path. One upgrade path (interleaved D1 folds) breaks the core "never remember what you did not witness" rule
unless the owner waives v4 hosted data, and the per-NPC SyncHistory multiplier should be removed before long runs.

## Re-review (fix round 40ca5e5..82431b4)

Commits ebe1d50 (I1), 0201a76 (I2), ce43a0b (yield, owner decision), 82431b4 (minors 1, 2, 4). Read against
`.superpowers/sdd/e1-final-fix-brief.md` and the report. Read the full source diff and the new or changed tests. Re-ran
`[memory],[hosted],[replay],[turn],[context],[store],[worker]` on the current tests build: 181 cases, 180 passed and
1 skipped, 2211 assertions. The build includes the fix round's new tests. The report's ctest and asan 611/611 were
not re-run.

### I1: resolved

`moveSummaries` (RunStore.cpp:363-385) now classifies each summary by its end turns. If both ends are in one
conversation C, it is C's fold: it goes only to the NPC witnesses of C's turns in the range, labelled C. If the ends
are in different conversations, it is a condense: it goes to the witnesses of every turn in range, with no label. An
end turn that is not a hosted turn, or a reversed range, is a StoreError and the upgrade rolls back. Every D1 fold and
condense ends on real turns, because both came from entries and summaries, so the new StoreError cannot reject a
genuine v4 file. The interleaved test now pins down the right outcome: Saadia gets only the condense. The "safer"
comments are gone.

Residual (Minor, accepted by the rule's design): a D1 condense whose two ends fall in one conversation C is taken for
C's fold. It goes to fewer NPCs, which is correct. But it carries C's label, so a later share of C from one of those
NPCs passes on the condense's content from other conversations. That content is still inside the teller's own memory,
which is the spec's condense rule, so nobody gets more than the teller holds. Only matters for unreleased v4 dev data.

### I2: resolved (the agreed part)

The split is right:
- A catch-up fold, a condense and an applied share now call only `SyncMemories`.
- `SyncHistory` runs only where folded_through moves: on Open, in a fold during a turn (`foldsConversation`), and on
  Maintain's MarkFolded.
- `Entries` takes the turns it was already given, so each conversation is read once per sync. RunTurn's `played`
  stays valid during the fit loop because no turn is committed there.
- No path moves folded_through without a SyncHistory after it.

The test removes the stored turn vectors before each memory-only write, then checks that none came back. That is a
sound way to observe a re-sync behind the content-hash cache.

Deferred as agreed: an incremental index, and a Maintain check that skips a conversation without parsing its turns.
The yield below makes the second one more pressing. Each pass now does at least one step, and every pass reads every
turn of every conversation first. So a host that polls often pays that O(run) scan once per step instead of once per
pass. Not a correctness issue. Keep it at the top of the follow-up list.

Minor (new, small): healing is narrower. Before, any later memory write re-ran the whole turn index. Now one of three
events must happen first: an in-turn fold, another MarkFolded, or a reopen. Case: Maintain's SyncHistory throws (the
embedder is down) after MarkFolded. The next pass does not mark that conversation again, so its turns stay out of
retrieval until one of those events. D1 had the same gap. A cheap fix: in Maintain, call SyncHistory when the index
holds fewer turn entries than the folded turns. Or leave it, since a reopen heals it.

### Yield (owner decision): correct

The worker side:
- `RunWorker` makes a fresh `_maintenanceYield` for each pass, under the lock, next to `_maintenanceStop`.
- `Enqueue` requests the yield for every job, under the same lock. A request that targets a finished pass's source is
  harmless.
- A preempting job also requests the stop, as before.
- After a yield, Maintain returns false, `_maintenanceDue` stays set, and the job runs. Maintain then starts a fresh
  pass, which resumes from the marks.
- I found no race: the source is swapped and requested only under `_mutex`, and the token is read before Maintain
  starts.

Maintain checks `yielding()` only before a step that has work: before an ended conversation's mark, before each NPC
that is behind, and before each complete pending share. A step is one NPC's whole catch-up, which can be several fold
calls and a condense. That matches the brief ("between NPC catch-ups").

The deviation, "yield only after a step of its own", is the right call. Without it, a polling host could keep every
pass from doing anything. Keep it.

The memory invariants are unaffected by yields:
- Every step's writes are transactional.
- A yield after MarkFolded and before the catch-ups leaves folded_through ahead of the marks. That is the known
  self-healing state, and RunTurn's catch-up of the speaker still closes the gap before the speaker talks.
- Turns still stop a running call (both sources are requested).

The new worker test waits for the first fold call before it queues a job, so the step it observes is the catch-up.
The report says it passed 10 times in a row and under asan.

### Minors 1, 2, 4: resolved

- An orphan memory mark is now a StoreError on a hosted Open. It names the file, the column and the id, and costs one
  small query per open. Note for E2: a rollback that drops conversations must drop their marks in the same
  transaction, or the next open refuses the run.
- The upgrade now refuses a non-string listener id.
- A retrieved memory's timings source is now "memory".

The skipped minors (3, 5 to 9 and the "can wait" list) were skipped correctly.

### Owned game

Unchanged. The fix round touches only hosted paths in RunStore (the upgrade's hosted branch and the hosted-only mark
check), HostedSession and RunWorker. `[turn]`, `[replay]` and `[context]` pass. No Session test, replay or owned prompt
was edited.

### Verdict

**Ready to merge.** There are no Critical or Important findings left. The follow-ups are the incremental index and the
cheap Maintain skip (more pressing now that passes are split into steps), the narrower healing after a failed
MarkFolded sync, and E2 keeping the memory marks consistent when it drops conversations.
