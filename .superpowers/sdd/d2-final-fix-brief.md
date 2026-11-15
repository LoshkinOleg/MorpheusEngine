# D2 final fix round: requirements

The final whole-branch review of sub-project D2 (morpheus_server) is `.superpowers/sdd/d2-final-review.md`; read it for the
file:line detail of each finding. The owner decided the open questions (2026-11-15). Make each item below as its own
commit, tests first, in this order. HEAD at the start: 57e045c.

## 1. Prompts with no user message (I1; owner: fix now)

llama-server with Qwen3.5's chat template refuses /apply-template for a message list without a user message ("No user
query found in messages"). `context::LayOut` (src/context/PromptBuilder.cpp) renders two such lists: the system message
alone (`systemCost`) and an assistant-only list (`emptyAssistant`, the resolve output's template overhead). Every turn and
fold through llama-server with Qwen3.5 fails, Morpheus's own game included.

- Count the system message's templated cost as `fixedTotal - tokens.tail` (both backends tokenize contents separately, so
  the tail adds exactly CountTokens(tail); say so in a comment), and the assistant message's overhead as
  `CountPromptTokens({user "", assistant ""}) - CountPromptTokens({user ""})`. No list LayOut renders may lack a user
  message. Check every other CountPromptTokens / prompt render in src/context and src/turn for the same pattern (grep),
  and fix any you find the same way.
- Tests first: give `test::FakeLlamaServer` an opt-in mode (for example `RefuseWithoutUserMessage(bool)`) in which
  /apply-template answers HTTP 400 with llama-server's message for a list with no user message. A test that drives an
  owned turn and a hosted turn through LlamaServerBackend on that fake must fail before the fix and pass after.
- Update the tests that pin exact budgets or counts. If a replay fixture (tests/data/replays) no longer replays because a
  fold point moved, re-record it only if the README says how without llama.cpp; otherwise report which fixture and why,
  and stop (NEEDS_CONTEXT).
- In-process (`LlamaCppBackend`) behaviour must stay correct: say in the report how you checked (it is built only in the
  dev preset; build `morpheus_tests` there with `cmake --build build/dev --config RelWithDebInfo --target morpheus_tests`
  if needed, but the [llama] tests need models the env vars name, which are set only if the owner set them; do not
  assume).

## 2. Only turns stop a background fold (design question 1; owner: only turns)

Today `RunWorker::Post` stops a running Maintain for any job, so a host polling a run can keep folding from finishing.

- `RunWorker::Post` gains a way to say whether the job preempts folding (for example `Post(Job job, Preempt preempt)` with
  an enum, or a bool parameter named at the call site); only preempting work stops a running Maintain. Other jobs wait in
  the queue behind the fold's current pass. Reopen does not preempt.
- A turn queued behind a non-preempting job must still stop the fold at once: preemption is decided when work is queued,
  so the queue must not make a turn wait for a fold. Cover that in a test.
- Api uses the preempting form only for the turn route.
- Bound the wait: make one Maintain pass fold at most one ended conversation (if HostedSession::Maintain cannot do that
  without an engine API change, report it and keep Maintain as is).
- Tests: a GET during a slow fold waits and the fold completes (folded_through reaches 1 while a client keeps polling);
  a turn during a slow fold stops it and runs promptly.
- Update the spec (docs/superpowers/specs/2026-11-16-morpheus-server-design.md, section 2, "One worker per run") and the
  README if it describes this.

## 3. A turn stopped by shutdown ends with an error (I2)

- In Api's turn job, when RunTurn returns nullopt and the worker's stop was requested, push
  `{"error", {"status": 503, "message": "the server is stopping"}}` before ending. A client cancel stays silent.
- Make `stream->End()` run on every exit of the job (a small RAII guard), so a stream handed to the writer always ends.
- Test: a turn whose worker stops mid-line ends its stream with an `error` 503 event.

## 4. Every HTTP answer carries api_version (I3)

- In HttpServer::Start, `set_error_handler` writes the JSON error body `{"api_version", "error": {"status", "message"}}`
  for answers cpp-httplib makes itself when the body is empty (OPTIONS, unknown methods, 400/413/414, its 404), and
  `set_exception_handler` answers 500 with the same body and logs through the Api's log. Keep Api the single source of
  the body's shape (a small shared function, used by both, is fine).
- Test over real sockets: an OPTIONS request and a request line cpp-httplib rejects both get JSON with api_version.

## 5. An unreachable embedding server is 503, not 400 (I4; owner: fix)

Lore that cannot be embedded is always a DataError today (src/turn/Retrieval.cpp, SyncLore wraps any InferenceError).

- Distinguish "the server refused this text" (an HTTP 4xx answer for that input, or a text too long for the embedding
  context) from "the server could not be reached or failed" (connection errors, 5xx, timeouts). Only the first stays a
  DataError naming the lore entry; the second propagates as InferenceError (503). Choose the smallest design: for
  example an `InferenceError` field or subclass saying the input was refused, set by LlamaServerBackend (and by
  LlamaCppBackend where it already knows), and read by SyncLore. Explain the choice in the report.
- Tests: with the fake embedding server answering 500 or being down, opening a run with new lore is InferenceError (Api:
  503); answering 400 for that text stays DataError (Api: 400). Update the RunWorker Reopen test that expects DataError for
  ScriptedBackend's embed limit, if its meaning changes.
- Update HostedSession's Open comment and the spec's errors table if they say otherwise.

## 6. Two cheap guards

- `RunRegistry::CheckRunId` refuses Windows reserved device names as run ids, case-insensitively, with or without an
  extension: CON, PRN, AUX, NUL, COM1-COM9, LPT1-LPT9 (DataError "run_id: ..."). Test.
- `RunWorker::Call` and `RunWorker::Reopen` called on the worker's own thread would wait forever: make them fail fast
  with MORPHEUS_REQUIRE (ContractError). Test.

## Finally

- Run `ctest --preset tests` and, after `cmake --build --preset asan`, `ctest --preset asan`; both must pass.
- Update CHANGELOG.md (still API 1.0.0, unreleased: note the 503 for unreachable embedding, the error event at shutdown,
  JSON for every answer) and the README where these behaviours are described.
- Byte checks on every changed file (0 and 0).
