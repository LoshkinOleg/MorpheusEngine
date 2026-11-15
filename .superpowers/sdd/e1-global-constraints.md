# Global constraints (sub-project E1, per-speaker memory)

Verbatim from docs/superpowers/plans/2026-11-16-per-speaker-memory.md. Spec: docs/superpowers/specs/2026-11-16-per-speaker-memory-design.md.


- Morpheus's own game keeps its behaviour byte for byte: no `Session` test, replay fixture (`tests/data/replays`) or owned prompt changes. Run `"[turn]"`, `"[replay]"` and `"[context]"` after every task that touches `context` or `turn`.
- Only NPCs keep memories: an actor whose `is_player` is true in a turn's observation has none. A turn's witnesses are its speaker and its listeners.
- A summary belongs to one NPC (`actorId`) and one conversation (`conversationId`), except a condensed summary, which has no conversation and no teller (`toldBy`). A shared copy has `toldBy` set to the teller and `sourceId` set to the summary it copies.
- Condensing marks the NPC's shown summaries superseded and adds one condensed summary, in one transaction; superseded summaries stay in the store, are retrieved, and are found by shares.
- The hosted prompt, most stable first: L0 hosted system text; L1 lore and the participants; L3 the conversation's unfolded turns the speaker witnessed; the tail: `# What <name> remembers` with each shown summary oldest first (a shared one prefixed `(told by <teller's name>) `), then `# Relevant`, then the observation, the new events, the player's line and the instruction. No L2 in a hosted system message.
- `summary_cap` bounds the speaker's memories in the tail; a hosted run whose `summary_cap` is not below `tail_allowance` is a `core::DataError` from `HostedSession::Open` naming both keys. The owned game's settings are not checked for it.
- Retrieval for speaker X: the host's lore, the folded turns X witnessed, X's own summaries (shown or superseded, shared included). Nothing else.
- Store schema version 5. A version 4 hosted run's run-wide summaries are copied to every NPC who witnessed a turn in their range and removed; an owned run gains empty columns only.
- `api_version` becomes `"1.1.0"`. New routes: `POST /v1/runs/{run_id}/memories/share` (`{from, to, conversation_id}` -> `{shared, pending}`) and `GET /v1/runs/{run_id}/actors/{actor_id}/memories` (`{memories: [{conversation_id, from_turn, to_turn, text, told_by, superseded}]}`).
- CLAUDE.md coding rules apply to every line: fail fast (`MORPHEUS_REQUIRE` for engine bugs, `core::DataError` for bad data, `store::StoreError` for damaged saved data naming the file and column); method bodies commented enough to read top to bottom; a private helper only with more than one caller (local lambdas are fine); class layout order nested types, public data, private data, public methods, private methods; constants ALL_CAPS and grouped; types and methods PascalCase, locals camelCase, members `_camelCase`, raw pointers `pName` / `_pName`; members initialized at declaration; RAII; no legacy paths kept; ASCII only in source comments and docs.
- Warnings are errors: MSVC `/W4 /permissive-`, Linux `-Wall -Wextra -Wpedantic -Wconversion -Wshadow`. Cast between `std::size_t` / `std::int64_t` and `int` explicitly.
- Never range-for over a member of a temporary (`for (x : store.Conversation(id)->participants)` dangles in C++20): copy into a local first. Catch2 test names must not start with `/`. Catch2 macros only on the test thread.
- Tests are written first (Catch2). Fast loop: `cmake --build --preset tests`, then `./build/tests/bin/Debug/morpheus_tests.exe "<tag or name>"`; full: `ctest --preset tests`, and `ctest --preset asan` after `cmake --build --preset asan`.
- Never stage `settings.json`, `models/`, `imgui.ini` or `docs/references/`. Commit messages end with the implementing model's trailer (`Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>` or `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`).
- Before reporting, check every changed file with `LC_ALL=C grep -c $'[\x80-\xFF]' <file>` (must print 0) and leave no `morpheus_tests`, `morpheus_server` or `ctest` process running (`tasklist | grep -i -E "morpheus|ctest"`).

