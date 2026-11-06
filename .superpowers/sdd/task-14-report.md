# Task 14 report: morpheus_play, recorded sandcrawler playtest, M3 docs

Status: DONE_WITH_CONCERNS (concerns are model quality and retrieval tuning, not engine failures)

Commit: cbb921d Add morpheus_play, a recorded sandcrawler playtest, and M3 docs

## What was implemented

- `tools/play/CMakeLists.txt` and `tools/play/main.cpp`: verbatim from the brief, plus `#include <stdexcept>` and
  `<utility>` as the brief asks. `PrintUntilClosing` handles all eight `turn::Event` variants of the current
  `Worker.h` (RunOpened, TokenStreamed, TurnResolved, TurnCompleted, TurnCancelled, StateChanged, MaintenanceFailed
  (non-closing, loop continues), Error), so no change to the brief's code was needed.
- Root `CMakeLists.txt`: `add_subdirectory(tools/play)` after `tools/probe` inside `if(MORPHEUS_BUILD_LLAMA)`.
- Built `morpheus_play` in Debug and RelWithDebInfo (`cmake --build --preset dev --config RelWithDebInfo`), both
  clean under /W4 /WX.
- Recorded playtest with the RelWithDebInfo exe, `--seed 1234 --record tests/data/replays/sandcrawler-playtest.json`,
  exactly the brief's input script. Fixture committed unedited.
- Short Debug session (2 inputs, no recording, separate DB `saves/playtest/m3-debug.db`).
- Docs: README (presets table row, settings bullets for `retrieval_cap` and `memory_mode`, new "Play in the
  terminal" section, layout lines for `src/turn` and `tools/play`), CLAUDE.md (layout lines, build line, the
  `<llama.h>` include rule now names tools/play), architecture spec (all brief items plus the coordinator's list).

## Verification (this task has no new unit tests; the TDD evidence is the replay test against the recording)

- `./build/tests/bin/Debug/morpheus_tests.exe "[replay]"` -> `All tests passed (58 assertions in 4 test cases)`.
  With `-s`, both sections ran (`mini-undo-regenerate.json`, `sandcrawler-playtest.json`), no "summary fallback"
  WARN, every non-summarize reply consumed (strict CHECK passed), turns and final state equal.
- `ctest --preset tests` -> `100% tests passed out of 188`.
- `ctest --preset dev` with both model variables exported -> `100% tests passed out of 208` (189 s, `[llama]` ran).

## Timing lines

RelWithDebInfo (recorded run):
```
[resolve 48 tok in 829 ms (57.9 tok/s), narrate 159 tok in 3963 ms; prompt 1493 tok, 0 reused]
[resolve 73 tok in 3064 ms (23.8 tok/s), narrate 185 tok in 2176 ms; prompt 1689 tok, 914 reused]
[resolve 105 tok in 3491 ms (30.1 tok/s), narrate 169 tok in 2027 ms; prompt 1875 tok, 1096 reused]
[resolve 130 tok in 1920 ms (67.7 tok/s), narrate 190 tok in 2243 ms; prompt 1887 tok, 1696 reused]
[resolve 103 tok in 3159 ms (32.6 tok/s), narrate 146 tok in 1748 ms; prompt 1887 tok, 1886 reused]
```
Debug (same seed, same first two inputs; identical text and changes to the RelWithDebInfo run):
```
[resolve 48 tok in 3269 ms (14.7 tok/s), narrate 159 tok in 7373 ms; prompt 1493 tok, 0 reused]
[resolve 73 tok in 4452 ms (16.4 tok/s), narrate 185 tok in 5914 ms; prompt 1689 tok, 914 reused]
```

## Quality observations

- **Resolve tok/s (grammar decode):** RelWithDebInfo 23.8 to 67.7 tok/s (mean about 42); Debug 14.7 and 16.4.
  The spread is wide for similar outputs; most likely runs of grammar rejections that force the full-vocabulary
  mask and resample. Narrate (no grammar) in RelWithDebInfo runs about 83-87 tok/s after the first turn (40 tok/s
  on turn 1), Debug 22-31 tok/s. Cold prompt eval of turn 1: 1493 tokens in 5364 ms.
- **Prompt reuse:** turn 2 reused 914/1689 and turn 3 1096/1875. That is the system message (L0-L2) plus the
  transcript up to the newest entry: the new transcript entry goes where the cached tail was, so the tail and the
  newest entry (about 775 tokens) are re-evaluated each turn, as designed. After /undo the new turn 3 reused
  1696/1887; the regenerate reused 1886/1887 (1 token evaluated). Each narrate call reused the whole resolve prompt
  (1495/1614, 1691/1840, 1889/2085).
- **Cut-off resolves:** none. `stopped_at_limit` is false on every call; no "could not be resolved" record.
- **Checks:** none in any of the 5 resolves. The inputs were low-stakes, so this is defensible, but climbing onto
  the roof could have earned an agility check. The real-model dice path was not exercised in this session.
- **Changes:**
  - Turns 1 and 2: a no-op `set player.location = "open desert"` whose reason says the location did not change.
  - Turn 2: `add player.inventory.compass +1` "player uses the compass" - a wrong change the engine cannot catch;
    the player now holds 2 compasses.
  - Water rationing (3 attempts): every attempt added a negative `player.inventory.water` (the player holds no
    water item), correctly rejected with "no item 'water' to decrease". The first (undone) attempt also proposed
    `crawler.water -5`, which is the sensible change and was applied. The regenerated attempts did not touch
    `crawler.water`.
  - `player.wits -1` for "mental effort" (turn 3 before regen) - questionable stat drain.
  - Conditions `water_rationing` / `water_rationing_initiated` are reasonable.
  - Several reasons are cut at SHORT_TEXT_CHARS (120) mid-word ("...from the ral", "...from the shared"): the model
    writes long reasons and the grammar bound binds.
  - All paths were valid schema paths (the grammar guarantees it).
- **Rejections:** 3 in total, one per rationing attempt, all the same "no item 'water' to decrease" model mistake;
  the narration then still described rationing without inventing the rejected change.
- **Narration:** coherent, consistent with state (fuel 60 -> "60% full"), follows the applied changes; ends every
  turn with "What do you do next?". Undo/regen produced distinct text with the same mechanics framing.
- **Retrieval scores (from `saves/playtest/m3.db` turn telemetry, throwaway Python query, not committed):** every
  turn returned the maximum 6 hits, all lore (no folds, so no history; engine mode, no notes).
  - Turn 1 "I check the fuel gauge.": petrol 0.655, water 0.628, navigation 0.618, sandcrawler 0.616, coal 0.614,
    natural gas 0.605.
  - Turn 2 "...scan the horizon with the compass.": navigation 0.675, radio 0.631, weather 0.630, water 0.613,
    sandcrawler captain 0.610, vegetation 0.602.
  - Turn 3 "...ration water, half portions for everyone.": water 0.799 (boosted), crew 0.760 (boosted),
    sandcrawler captain 0.597, player starting conditions 0.595, faction player crew 0.587, oasis 0.586.
  - **RETRIEVAL_MIN_SCORE = 0.25 looks wrong:** it never binds. nomic-embed cosines for this lore sit at about
    0.58-0.68 even for loosely related entries, so every turn fills all 6 slots. The best hit is the right one
    each time (petrol, navigation, water/crew), and loosely related entries trail by only 0.02-0.07. A floor near
    0.62-0.65 would drop the tail. Not tuned, per the brief.

## Files changed

- Created: `tools/play/CMakeLists.txt`, `tools/play/main.cpp`, `tests/data/replays/sandcrawler-playtest.json`
- Modified: `CMakeLists.txt`, `README.md`, `CLAUDE.md`,
  `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`

## Deviations from the brief

- The brief runs the playtest with the Debug exe; per the dispatch, the recorded run used
  `build/dev/bin/RelWithDebInfo/morpheus_play.exe`, and a 2-input Debug run was added.
- Docs additions beyond the brief's list, per the dispatch: undo/regenerate resync the memory index; regenerate
  restores the previous turn on cancel or failure; too-small `summary_cap` is a DataError; narrate shedding order;
  engine fallback summary; held counter items matched by name; grammar sampling checks the sampled token first.
  Also: the worker's maintenance scheduling (runs before the next retrieving command), unreadable resolve output
  as a model mistake, and assisted mode's lookup count (3, from MAX_LOOKUPS) and sources (lore, history, notes).
  The over-removal rule went into "World data and persistence" as a short "Changes are validated" paragraph,
  since that section had no change-rule list to extend.
- README presets table and CLAUDE.md build line now name `morpheus_play`; the CLAUDE.md `<llama.h>` rule names
  tools/play, matching global-constraints.md.

## Concerns

1. Model quality: the compass +1, no-op location sets, and inventory-vs-field confusion for water are accepted or
   rejected correctly by the engine but suggest prompt work (L0 rules or world rules: "only change what the action
   changes"; crawler supplies are fields, not items).
2. RETRIEVAL_MIN_SCORE never binds (see scores above).
3. Recording edge cases in `morpheus_play` (not hit here): a command that closes with TurnCancelled is still
   appended to `steps` (the tool never cancels today, so unreachable); and a SubmitInput that fails with an Error
   after a backend call completed would leave a recorded reply the replay never consumes, which the strict replay
   test would flag. Both are fine for this session; worth handling if the tool gains Ctrl+C or is used to record
   failing turns.
4. Resolve decode speed varies 3x between turns of similar length (grammar resample cost).
5. The fixture's narration is UTF-8 (em dashes, curly quotes) as the model wrote it. It is test data, not a source
   comment or doc.

## Full console output, recorded run (RelWithDebInfo)

Command (from repo root):
```
mkdir -p saves/playtest && rm -f saves/playtest/m3.db
printf 'I check the fuel gauge.\nI climb onto the roof and scan the horizon with the compass.\nI order the crew to ration water.\n/undo\nI order the crew to ration water, half portions for everyone.\n/regen\n/state\n/quit\n' \
  | ./build/dev/bin/RelWithDebInfo/morpheus_play.exe settings.json game_projects/sandcrawler saves/playtest/m3.db --seed 1234 --record tests/data/replays/sandcrawler-playtest.json
```
Exit code 0. Output (stdout and stderr, unfiltered):
```
load: control-looking token: 128247 '</s>' was not control-type; this is probably a bug in the model. its type will be overridden
llama_context: n_ctx_seq (16384) < n_ctx_train (262144) -- the full capacity of the model will not be utilized
load: special_eos_id is not in special_eog_ids - the tokenizer config may be incorrect
0 turn(s) played in world 'sandcrawler'.

> 
[player.location = "open desert"]

The fuel gauge reads steady—60% full. The numbers are clear, the needle not flickering. You know the crawler’s range is still good, and with the desert stretching out in every direction, there’s no immediate need to worry. But the wind carries a low hum, like distant engines waking up. The sun climbs higher, and the sand shifts beneath your feet, whispering of routes long forgotten. Ahead, the dunes rise like silent sentinels. You’ve heard stories of sandstorms that follow the wind patterns—some say they only come when a crawler passes through a certain point. You don’t know if it’s true. But you also know that the open desert doesn’t care about your fuel levels. It only watches.  

What do you do next?
[resolve 48 tok in 829 ms (57.9 tok/s), narrate 159 tok in 3963 ms; prompt 1493 tok, 0 reused]

> 
[player.location = "open desert"; player.inventory.compass +1]

The compass spins gently in your hands, its needle trembling slightly with the wind. You lift it to your eye and point it toward the horizon. The desert stretches endlessly—one side a flat expanse of dunes, the other a jagged silhouette of a rocky ridge you’ve never seen before. The needle doesn’t settle, but it doesn’t spin wildly either. Something is off. The alignment doesn’t match any known route. You remember the elders saying that certain dunes shift with the wind, and that some patterns repeat only after storms. The sky above is clear, but the wind carries the scent of dry stone and distant dust.  

The ridge ahead seems wrong—too sharp, too isolated. It doesn’t appear on any map. And yet, you feel it in your bones. You don’t know why, but you *know* it’s not supposed to be there.  

What do you do next?
[resolve 73 tok in 3064 ms (23.8 tok/s), narrate 185 tok in 2176 ms; prompt 1689 tok, 914 reused]

> 
[player.conditions +water_rationing_initiated; crawler.water -5]
  (rejected: no item 'water' to decrease)

The order spreads through the crew like a ripple through sand. They nod, eyes sharp with discipline, and begin adjusting their daily use of water—less for washing, less for cooking, less for drinking. You feel the weight of it in the air, a quiet tension that settles into the bones of the crawler. The water level in the tanks drops by five units, a small mark, but it’s real. The crew knows the risk: if an oasis dries up, they’ll be stranded. You’ve seen what happens when the water runs out. The desert doesn’t forgive.  

But the horizon still holds secrets. That jagged ridge—still unmapped—now pulses with a faint, unnatural stillness. The wind seems to pause when it passes near it. You can’t ignore it.  

What do you do next?
[resolve 105 tok in 3491 ms (30.1 tok/s), narrate 169 tok in 2027 ms; prompt 1875 tok, 1096 reused]

> 
Back to turn 2.

> 
[player.conditions +water_rationing; player.wits -1]
  (rejected: no item 'water' to decrease)

The order is passed through the crew, and for a moment, silence falls over the crawler. Then, the voices begin—low, measured, like a rhythm beneath the wind. Half the water is now shared, rationed carefully, each person given only what they need to survive. The air tastes drier, the sand sharper. You feel it in your skin, the weight of the decision. Your wits flicker, drained by the effort of making it official.  

But the crew doesn’t react with anger. They don’t complain. They simply begin to follow the new rules, their eyes scanning the horizon with more purpose now. Still, one of the younger crew members, who always talks too much, mutters something about the desert not being fair. You don’t answer. You know what’s coming.  

Ahead, the ridge shifts in the wind. A new shadow forms in the sand.  

What do you do next?
[resolve 130 tok in 1920 ms (67.7 tok/s), narrate 190 tok in 2243 ms; prompt 1887 tok, 1696 reused]

> 
[player.conditions +water_rationing]
  (rejected: no item 'water' to decrease)

The order is given, quiet and firm. The crew exchanges a glance—some nod, others look uneasy—but no one argues. Water is rationed: half a cup per person, measured with a chipped ceramic spoon. You know the crew will survive, but the silence in the air feels heavier now. The desert holds its breath.  

A distant wind carries a whisper, not of sand, but of dry roots—like something deep beneath the ground is stirring. You don’t know where it comes from.  

The compass still spins, its needle pointing not at the ridge, but at a pattern you didn’t see before—a faint line across the dunes, like a buried path.  

What do you do next?
[resolve 103 tok in 3159 ms (32.6 tok/s), narrate 146 tok in 1748 ms; prompt 1887 tok, 1886 reused]

> crawler (crawler): ammunition 40, cargo {}, crew 20, food 50, fuel 60, hull 100, location "open desert", morale "steady", vehicles {sand_bike 2}, water 50
player (captain): agility 1, conditions [water_rationing], grit 2, health 10, inventory {compass 2, pistol 1}, location "open desert", presence 2, wits 3
world (world): day 1, time_of_day "dawn"

> Wrote tests/data/replays/sandcrawler-playtest.json
```

## Full console output, Debug run (2 inputs, no recording)

Command: same input script cut to the first two inputs and /quit, `./build/dev/bin/Debug/morpheus_play.exe settings.json game_projects/sandcrawler saves/playtest/m3-debug.db --seed 1234`. Exit code 0.
```
load: control-looking token: 128247 '</s>' was not control-type; this is probably a bug in the model. its type will be overridden
llama_context: n_ctx_seq (16384) < n_ctx_train (262144) -- the full capacity of the model will not be utilized
load: special_eos_id is not in special_eog_ids - the tokenizer config may be incorrect
0 turn(s) played in world 'sandcrawler'.

> 
[player.location = "open desert"]

The fuel gauge reads steady—60% full. The numbers are clear, the needle not flickering. You know the crawler’s range is still good, and with the desert stretching out in every direction, there’s no immediate need to worry. But the wind carries a low hum, like distant engines waking up. The sun climbs higher, and the sand shifts beneath your feet, whispering of routes long forgotten. Ahead, the dunes rise like silent sentinels. You’ve heard stories of sandstorms that follow the wind patterns—some say they only come when a crawler passes through a certain point. You don’t know if it’s true. But you also know that the open desert doesn’t care about your fuel levels. It only watches.  

What do you do next?
[resolve 48 tok in 3269 ms (14.7 tok/s), narrate 159 tok in 7373 ms; prompt 1493 tok, 0 reused]

> 
[player.location = "open desert"; player.inventory.compass +1]

The compass spins gently in your hands, its needle trembling slightly with the wind. You lift it to your eye and point it toward the horizon. The desert stretches endlessly—one side a flat expanse of dunes, the other a jagged silhouette of a rocky ridge you’ve never seen before. The needle doesn’t settle, but it doesn’t spin wildly either. Something is off. The alignment doesn’t match any known route. You remember the elders saying that certain dunes shift with the wind, and that some patterns repeat only after storms. The sky above is clear, but the wind carries the scent of dry stone and distant dust.  

The ridge ahead seems wrong—too sharp, too isolated. It doesn’t appear on any map. And yet, you feel it in your bones. You don’t know why, but you *know* it’s not supposed to be there.  

What do you do next?
[resolve 73 tok in 4452 ms (16.4 tok/s), narrate 185 tok in 5914 ms; prompt 1689 tok, 914 reused]

> 
```

## Fix round 1

Commit 0f290b9 "Roll back failed steps in morpheus_play recordings and harden its arguments".

### Changes
- `turn::RecordingBackend::Mark()` (reply count per kind; kinds with none absent) and `RollBack(mark)` (truncates each kind to its marked count, erases a kind the mark lacks, throws ContractError if a kind has fewer replies than marked). Documented in `src/turn/Replay.h`.
- `tools/play/main.cpp`:
  - `PrintUntilClosing` returns `StepOutcome { Completed, Cancelled, Failed }`; RunOpened and StateChanged count as Completed.
  - When recording, `Mark()` is taken before each command; Completed appends the step, Cancelled and Failed call `RollBack(mark)` and append nothing.
  - `RunView::maintenanceFailed` is set on any `MaintenanceFailed`; at quit with `--record` the fixture is not written, a message goes to stderr, exit code 1.
  - `--seed` parsed with `std::from_chars` (whole string must parse); bad, negative or overflowing value prints usage, returns 2.
  - Final `else` of `PrintUntilClosing`: explicit `std::get_if<turn::Error>`, anything else throws ContractError via MORPHEUS_REQUIRE naming the unhandled event's variant index.
  - `--record` with an absolute world path is refused (exit 2) with a message.
- Spec Testing line: grammar generation and tests now said to live in `turn` (`ResolveGrammarTests`).
- README "Play in the terminal": failed steps omitted, no fixture on maintenance failure, `--record` must run from the repo root with a repo-relative world path.

### TDD
- New test "RecordingBackend Mark and RollBack restore the recorded reply counts" `[replay]` (ScriptedBackend wrapped in a RecordingBackend, `Complete` called directly; checks Mark, rollback of a marked kind and an unmarked kind, and both ContractError cases).
- RED: `cmake --build --preset tests` -> `error C2039: 'Mark': is not a member of 'morpheus::turn::RecordingBackend'` (and the same for `RollBack`).
- GREEN: `./build/tests/bin/Debug/morpheus_tests.exe "[replay]"` -> `All tests passed (66 assertions in 5 test cases)`.
- `ctest --preset tests` -> `100% tests passed out of 189`.
- Builds: `cmake --build --preset dev` (Debug) and `--config RelWithDebInfo` both succeed, morpheus_play links; only warnings are pre-existing ones in src/main.cpp.

### Smoke test (RelWithDebInfo, models)
`printf 'I check the fuel gauge.\n/undo\n/quit\n' | morpheus_play.exe settings.json game_projects/sandcrawler saves/playtest/fix14.db --seed 5 --record saves/playtest/fix14.json`:
```
0 turn(s) played in world 'sandcrawler'.
> [player.location = "open desert"; player.health = 10]
The fuel gauge reads steady--60 percent full. ... What do you do now?
[resolve 77 tok in 3255 ms (23.7 tok/s), narrate 139 tok in 1636 ms; prompt 1493 tok, 0 reused]
> Back to turn 0.
> Wrote saves/playtest/fix14.json
```
- Copied the fixture to `tests/data/replays/fix14-tmp.json`: `[replay]` -> `All tests passed (74 assertions in 5 test cases)` (8 more assertions than without it, so the fixture ran), then deleted the copy and `saves/playtest/fix14.*`.
- `--seed abc` and `--seed -1`: print the usage line, exit 2. `--record` with an absolute world path: refuses, exit 2.

### Notes
- Not exercised against real models: the failure paths (Error after resolve, MaintenanceFailed) are covered only by the unit test of Mark/RollBack plus code reading; they are not reproducible on demand.
- Kinds rolled back to zero replies are erased, so a fixture never contains empty reply arrays.

## Fix round 2

### Changes
- `RecordingBackend` (src/turn/Replay.h/.cpp): new `mutable std::mutex _mutex = {}` guarding `_replies`. `Complete` locks only around the push (kind is computed before the lock, the inner call is unlocked); `Mark`, `RollBack` and `Replies` lock. `Replies()` now returns the map by value (a copy taken under the lock); `MakeReplayFixture` takes one such copy and loops over it. Replay.h documents that Mark, RollBack and Replies are safe while another thread is in Complete.
- tools/play/main.cpp: `Mark()` is taken BEFORE `worker.Post(...)`. After the input loop, still inside the worker's scope, `worker.PollEvents()` is drained and any `MaintenanceFailed` is printed and sets the flag. Comments (file header, drain, record check) and README say the guard covers failures reported before quitting; maintenance running at quit is cancelled by shutdown and cancelled summary calls are not recorded. The "would fail at the same place" comment now says the replay uses scripted embeddings and the recording is refused because its maintenance cannot be trusted (stderr message reworded the same way).
- `--record` world path: rejected when `has_root_name()` or `has_root_directory()`; also rejected (exit 2) unless the world folder exists relative to the cwd, with a "run morpheus_play from the repo root" message. README updated.

### Tests
- New `[replay]` test "RecordingBackend is safe to mark, roll back and read while another thread completes calls", two sections (200 `Complete` calls on a RecordingBackend over a ScriptedBackend):
  1. a reader thread loops `Mark()` + `Replies()` copy; at the end the Narrate count is exactly 200 and Mark equals it.
  2. a second thread loops `RollBack(Mark())`; survivors must be whole replies in strictly increasing call order, and after the writer stops `RollBack(Mark())` changes nothing.
- Deviation: `RollBack(Mark())` is a no-op only if nothing is recorded between the two calls, which cannot be guaranteed while another thread completes calls (replies recorded in the gap are legitimately dropped). So the "count equals completed calls" check is done in the read-only section, and the rollback section checks integrity/order instead (its assertion count varies by run, which is expected). The test would not catch a race without TSan, but exercises the locking paths and passes under asan.
- Results: `[replay]` -> All tests passed (6 test cases); `ctest --preset tests` -> 100% passed out of 190; `ctest --preset asan` -> 100% passed out of 190.

### Builds and smoke tests
- `cmake --build --preset dev --config RelWithDebInfo` succeeded (morpheus_play, morpheus_probe, morpheus_tests link).
- Smoke: `printf 'I check the fuel gauge.\n/quit\n' | morpheus_play.exe settings.json game_projects/sandcrawler saves/playtest/fix14b.db --seed 5 --record saves/playtest/fix14b.json` -> one turn played, "Wrote saves/playtest/fix14b.json", exit 0, fixture contains the expected state. Files removed afterwards.
- `--record` with `\game_projects\sandcrawler` -> refused, exit 2, no fixture. `--record` with `game_projects/nonexistent` -> refused, exit 2.

### Concerns
- The race test cannot detect a data race without TSan; it relies on the lock being correct by construction.
- Not exercised with real models: a trailing MaintenanceFailed (not reproducible on demand).
- SHA: 254e0e9

## Fix round 3

Problem: `morpheus_play --record` marks before posting a command; the previous turn's maintenance can still record Summarize replies after the mark, and a failed step's `RollBack` dropped them.

Fix: `RecordingBackend::RollBack` now ignores Summarize entirely (no truncation, and the mark's summarize count is not validated). Lookup, Resolve and Narrate are truncated as before and still throw ContractError if a mark exceeds the recorded count. Reason documented in the `RollBack` comment in `src/turn/Replay.h`; the mark-site comment in `tools/play/main.cpp` and the README `--record` note are updated.

Files: src/turn/Replay.h, src/turn/Replay.cpp, tests/turn/ReplayTests.cpp, tools/play/main.cpp, README.md.

Tests:
- Extended "RecordingBackend Mark and RollBack restore the recorded reply counts" with a Lookup-mark ContractError check (alongside Narrate and Resolve).
- New test case "RecordingBackend RollBack keeps summarize replies" `[replay]`: summary before the mark, then resolve, summary and narrate after it; after RollBack both summaries survive and Resolve and Narrate are gone; a mark with Summarize 5 does not throw and changes nothing.
- RED (`./build/tests/bin/Debug/morpheus_tests.exe "[replay]"`): 3 failed assertions (`{ "prose" } == { "prose", "prose" }`; CHECK_NOTHROW threw "RollBack: fewer 'summarize' replies recorded than the mark holds"; size 1 == 2).
- GREEN: `[replay]` "All tests passed (460 assertions in 7 test cases)".
- `ctest --preset tests`: 100% passed out of 191. `ctest --preset asan`: 100% passed out of 191.
- `cmake --build --preset dev --config RelWithDebInfo`: succeeded (morpheus_play.exe built, no errors or warnings).

SHA: 9871bf3
