# D2 final fix round: report

Start: 57e045c. Six commits, one per item, tests first. Commit trailer: Co-Authored-By: Claude Opus 5.5.

| Item | Commit |
|---|---|
| 1 | 912dc0b LayOut: count no prompt without a user message (Qwen3.5's template refuses one) |
| 2 | b07a79c RunWorker: only a turn stops a background fold; other jobs wait for its pass |
| 3 | 3cfd695 Api: a turn stopped by shutdown ends its stream with error 503; the stream always ends |
| 4 | 27080bc HttpServer: cpp-httplib's own answers carry the Api's JSON error body with api_version |
| 5 | 10d43d6 Lore the embedding server cannot embed: 503 when it failed, 400 only when it refused the text |
| 6 | 4937a21 Two guards: Windows device names are refused as run ids; Call and Reopen fail fast on the worker's thread |

## 1. Prompts with no user message (I1)

What changed
- `context::LayOut` (src/context/PromptBuilder.cpp): `systemCost = fixedTotal - tokens.tail` (comment: both backends
  tokenize the template pieces and each content apart, so the tail's content adds exactly CountTokens(tail); the tail's
  frame now belongs to the system cost, since tail_allowance budgets only its text). The resolve output's overhead is
  `CountPromptTokens({user "", assistant ""}) - CountPromptTokens({user ""})` (the second term is the existing
  narrateMessageOverhead). The system-only render is gone (one /apply-template fewer per LayOut), and one render was
  added; no list LayOut renders lacks a user message. PromptBuilder.h's doc says the same.
- Grep of every CountPromptTokens / prompt render in src/context and src/turn: the only user-less lists were the two in
  LayOut. SummarizePrompt, CondensePrompt (src/turn/Folding.cpp), ReviewPrompt (Review.cpp), ExtendForNarrate and the
  assembled resolve prompts all end with a user message; MemoryUpkeep counts SummarizePrompt/CondensePrompt.
- `test::FakeLlamaServer::RefuseWithoutUserMessage(bool)` (opt-in): /apply-template answers HTTP 400
  `{"error":{"code":400,"message":"No user query found in messages.","type":"invalid_request_error"}}` for a list
  without a user message.
- New tests/llamaserver/LlamaServerTurnTests.cpp: an owned turn (Session::Start on the mini world + RunTurn) and a
  hosted turn (HostedSession::Open + RunTurn) through LlamaServerBackend on the refusing fake; also checks no
  /apply-template request lacked a user message.
- Pinned budgets updated: PromptBuilderTests (Measure's systemCost is now content + 4 + 4 on FakeBackend; the
  narrate-allowance test's exact tail allowance is tail + 4 + 4, comments say why; "only the context overflows" comment)
  and HostedPromptTests (systemCost + 4 + 4).
- Beyond the brief (small, as the review recommended): `test::HttpHarness` turns the refusal on, so every HTTP test
  (both fixtures over HTTP, disconnect, two runs with a background fold) runs under Qwen3.5's rule. The architecture
  spec's budget line notes the tail frame.
- Replays: `[replay]` passes unchanged (7 test cases, 507 assertions); no fixture needed re-recording.

In-process check (LlamaCppBackend)
- Built `morpheus_tests` in build/dev (RelWithDebInfo, Vulkan): clean, no warnings.
- MORPHEUS_TEST_CHAT_MODEL / MORPHEUS_TEST_EMBED_MODEL were not set in this environment. I set them for one command
  only, to models/Qwen3-4B-Instruct-2507-Q4_K_M.gguf and models/nomic-embed-text-v1.5.Q8_0.gguf, and ran
  "A hosted conversation on the real model: every action validates and every line is spoken" (LayOut through the
  in-process backend, every turn): All tests passed (16 assertions in 1 test case). The rest of [llama] was not run.
- By code: LlamaCppBackend::TokenizePrompt uses the same TemplatePieces scheme (template pieces with parse_special,
  each content apart), so `fixedTotal - tail` is exact there too, and both backends move the same few tokens.

RED (`morpheus_tests.exe "Turns play through llama-server when its chat template refuses a prompt without a user message"`, before the LayOut change)
```
LlamaServerTurnTests.cpp(41): FAILED:  (an owned turn)
due to unexpected exception with message:
  http://127.0.0.1:59834/apply-template answered HTTP 400: {"error":{"code":
  400,"message":"No user query found in messages.","type": ...
LlamaServerTurnTests.cpp(65): FAILED:  (a hosted turn)
  http://127.0.0.1:59838/apply-template answered HTTP 400: ... "No user query found in messages." ...
test cases: 1 | 1 failed
```
GREEN
```
All tests passed (16 assertions in 1 test case)
[context]: All tests passed (224 assertions in 27 test cases)
[http]: All tests passed (211 assertions in 7 test cases)
[replay]: All tests passed (507 assertions in 7 test cases)
ctest --preset tests: 100% tests passed out of 562
```

## 2. Only turns stop a background fold (design question 1)

What changed
- `RunWorker::Preempt { None, Fold }`; `Post(Job, Preempt = Preempt::None)`. Enqueue takes the Preempt and requests
  `_maintenanceStop` only for `Preempt::Fold`, at queue time, so a turn queued behind a waiting job still stops the
  fold at once. Call and Reopen never preempt. Api passes `Preempt::Fold` only on the turn route.
- Comments in RunWorker.h/.cpp and Api.cpp; spec section 2 "One worker per run" and its test list; README (a paragraph
  after the stream description); the HttpServerTests comment that described the old behaviour.
- Bounding the wait (one ended conversation per Maintain pass): NOT done. `HostedSession::Maintain` folds every ended
  conversation in one call, and no public HostedSession method folds a single conversation; doing it needs an engine API
  change (a `MaintainOne`, or a changed meaning of Maintain's return value). Per the brief, Maintain is kept as is, and
  the spec says a non-turn request can wait several fold calls after a long session.
- Tests: RunWorkerTests "A job that does not preempt waits for a running fold, which lands first"; "A preempting job
  stops a running fold at once, also from behind a job that waits; the fold runs again" (replaces "A job stops a
  running fold"); ApiTurnTests "Requests other than turns wait for a background fold, which lands while a client
  keeps polling" and "A turn during a background fold stops it and runs promptly; the fold lands afterwards".

RED (Post given the parameter but still always preempting)
```
[worker]: RunWorkerTests.cpp(192): FAILED: CHECK( through == 1 ) with expansion: 0 == 1
  test cases: 19 | 18 passed | 1 failed
"A turn during a background fold...": ApiTurnTests.cpp(301): FAILED:
  CHECK( test::WaitUntil([&] { return FoldedThrough(harness) == 1; }) ) with expansion: false
"Requests other than turns wait...": ApiTurnTests.cpp(269): FAILED: CHECK( folded ) with expansion: false
  ApiTurnTests.cpp(270): FAILED: CHECK( foldCalls.load() == 1 ) with expansion: 318 (0x13e) == 1
```
(The preempting-worker test needs the new parameter, so before it existed it was a compile error.)

GREEN
```
[worker],[api] three runs in a row: All tests passed (454 / 455 / 454 assertions in 31 test cases)
ctest --preset tests: 100% tests passed out of 565
```

## 3. A turn stopped by shutdown ends with an error (I2)

What changed
- Api's turn job: when RunTurn returns nullopt and `workerStop.stop_requested()`, it pushes
  `{"error", {"status": 503, "message": "the server is stopping"}}`; a client cancel stays silent.
- A local RAII guard (`EndStream`, declared first in the job) calls `stream->End()` on every exit; the trailing
  `stream->End()` is gone.
- README (Ctrl+C sentence), spec (shutdown paragraph), CHANGELOG (API 1.0.0 marked unreleased; the error event).
- Test: ApiTurnTests "A turn stopped by shutdown mid-line ends its stream with a 503 error, and commits nothing"
  (reads to the first token, then `runs.StopAll()`).

RED
```
ApiTurnTests.cpp(330): FAILED: REQUIRE_FALSE( rest.empty() ) with expansion: !true
test cases: 1 | 1 failed
```
GREEN
```
All tests passed (10 assertions in 1 test case)
[api],[http] twice: All tests passed (472 assertions in 20 test cases)
ctest --preset tests: 100% tests passed out of 566
```

## 4. Every HTTP answer carries api_version (I3)

What changed
- Api.h: free function `server::ErrorBody(int status, std::string_view message)`, the one source of
  `{"api_version", "error": {"status", "message"}}`; Api::Handle uses it for every error (also the turn's error before
  its action). New public `Api::EscapedError(method, path, what)`: logs through the Api's log and returns the 500 body.
- HttpServer::Start: `set_error_handler` fills an answer with an empty body (status >= 400) with ErrorBody (message:
  cpp-httplib's status text plus "(METHOD path)" when a method was parsed), and leaves the Api's own bodies alone;
  `set_exception_handler` answers 500 with `EscapedError`.
- Spec section 1, README, CHANGELOG.
- Test (real sockets): HttpServerTests "Answers cpp-httplib makes itself are JSON with the API version too": OPTIONS
  /health (cpp-httplib's 404) and BREW /health (an unknown method, its 400): JSON content type, api_version 1.0.0,
  error.status equal to the HTTP status, a message. The exception handler is not covered by a test (no route can be
  made to throw without a test hook).

RED
```
HttpServerTests.cpp(133): FAILED: CHECK_THAT( result->get_header_value("Content-Type"), StartsWith("application/json") )
  with expansion: "" starts with: "application/json"   (OPTIONS)
HttpServerTests.cpp(135): FAILED: REQUIRE( body.is_object() ) with expansion: false
```
GREEN
```
All tests passed (14 assertions in 1 test case)   (OPTIONS: 404, BREW: 400)
ctest --preset tests: 100% tests passed out of 567
```

## 5. An unreachable embedding server is 503, not 400 (I4)

Design (the smallest): a subclass `inference::InputRefused : InferenceError` in src/inference/Llm.h, meaning "the
backend refused this input itself; sending it again cannot help". Every existing catch of InferenceError still catches
it (so every other path is unchanged, still 503), and only SyncLore tells it apart. A subclass needs no new field on
every throw site and no change to any interface signature.
- LlamaServerBackend's `Ask` (every non-streaming request: /tokenize, /apply-template, /embedding, /props) throws
  InputRefused for an HTTP 4xx answer; 5xx, unreachable, dropped connections and unreadable answers stay plain
  InferenceError. I checked llama.cpp's server in build/dev/_deps: a text over the context answers 400
  (exceed_context_size_error), so with the README's recommended `--batch-size`/`--ubatch-size` equal to `--ctx-size`
  a too-long lore entry is InputRefused. (A server whose ubatch is smaller than its context answers 500 "too large to
  process. increase the physical batch size" for a text between the two: that now becomes 503, which names the
  server's configuration.)
- LlamaCppBackend::Embed throws InputRefused for a text over the embedding context (it already knew).
- ScriptedBackend's embed limit throws InputRefused (it models a too-small embedding context), so the RunWorker
  Reopen test and the HostedSession/Retrieval tests keep their meaning (DataError naming the entry) unchanged.
- Retrieval.cpp SyncLore catches only InputRefused; comments in Retrieval.h, HostedSession.h (Open), Session.h
  (Start), LlamaServerBackend.h; spec errors table, hosted-world spec errors list, README statuses, CHANGELOG.
- Tests: LlamaServerBackendTests "LlamaServerBackend tells a text the server refused (HTTP 4xx) from a server that
  failed or cannot be reached" (400 and 413 refused; 500, 503 and a dropped connection not); HttpServerTests "Opening a
  run whose lore cannot be embedded: 503 when the embedding server fails, 400 when it refuses the text" (500 -> 503,
  dropped connection -> 503, 400 -> 400 naming "cannot be embedded").

RED (InputRefused declared, nothing throwing it yet)
```
LlamaServerBackendTests.cpp(324): FAILED: CHECK( refused() ) with expansion: false   (400)
LlamaServerBackendTests.cpp(326): FAILED: CHECK( refused() ) with expansion: false   (413)
HttpServerTests.cpp(302): FAILED: CHECK( opened.status == failure.expected ) with expansion: 400 (0x190) == 503  (answers 500)
HttpServerTests.cpp(302): FAILED: ... 400 (0x190) == 503  (drops the connection)
```
GREEN
```
All tests passed (5 assertions in 1 test case)
All tests passed (10 assertions in 1 test case)
ctest --preset tests: 100% tests passed out of 569
build/dev morpheus_tests (LlamaCppBackend.cpp changed): builds clean.
```

## 6. Two cheap guards

What changed
- `RunRegistry::CheckRunId` refuses CON, PRN, AUX, NUL, COM1-COM9, LPT1-LPT9 in any case (DataError "run_id: '<id>' is a
  Windows device name ..."). Ids cannot contain '.', so "with an extension" is already refused by the character rule;
  the run's file adds ".morpheus", which is why the bare names are refused. README, spec, CHANGELOG.
- `RunWorker::Call` and `RunWorker::Reopen` start with `MORPHEUS_REQUIRE(std::this_thread::get_id() !=
  _thread.get_id(), ...)` (ContractError). Doc comments in RunWorker.h.
- Tests: RunRegistryTests "RunRegistry refuses Windows device names as run ids, in any case" (also checks CON1,
  console, COM0, COM10, LPT, nul_save, aux-2 pass); RunWorkerTests "Call and Reopen from a job, on the worker's own
  thread, fail fast instead of waiting forever" (the nested results are kept in bools; Catch2 macros only on the test
  thread).

RED
```
"RunRegistry refuses Windows device names...": RunRegistryTests.cpp(49)/(50): FAILED: CHECK_THROWS_AS / CHECK_THROWS_WITH
  ... assertions: 25 | 7 passed | 18 failed
"Call and Reopen from a job...": deadlocked, killed by `timeout 15`: exit code 124
```
GREEN
```
All tests passed (25 assertions in 1 test case)
All tests passed (3 assertions in 1 test case)
ctest --preset tests: 100% tests passed out of 571
```

## Final runs

- `ctest --preset tests` (at HEAD 4937a21): 100% tests passed out of 571 (the 4 real-server/[llama] tests skip as
  before: their env vars are unset).
- `cmake --build --preset asan` clean, then `ctest --preset asan`: 100% tests passed out of 571 (275 s).
- The timing tests: `[worker],[api]` five times in a row under asan: all passed each time (468/469 assertions in 33
  test cases); three times in the tests preset: all passed.
- build/dev `morpheus_tests` (RelWithDebInfo) builds clean with items 1 and 5 (LlamaCppBackend.cpp).
- Nothing is left running (tasklist shows no morpheus or ctest process).

## Byte checks

Every changed file, after its edit: `LC_ALL=C grep -c $'[\x80-\xFF]'` 0 and `LC_ALL=C grep -c
$'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]'` 0 (all 0 0). Line endings stay LF as in the index. No escapes were written.

## Deviations and notes

- Item 2: the one-conversation bound is not done (needs an engine API change); reported above, Maintain unchanged.
- Item 1: HttpHarness refuses user-less prompts too (beyond the brief's one test; cheap and the review's suggestion).
- Item 4: no test of the exception handler (nothing can make a handler throw without a hook).
- Item 5: a 4xx on /tokenize or /apply-template is now InputRefused too; every caller but SyncLore still sees an
  InferenceError (503), so nothing else changes.
- CHANGELOG: the API 1.0.0 heading now says "(unreleased)"; entries for the shutdown error event, JSON for every
  answer, the 503 for an unreachable embedding server and the device-name run ids.

## Concerns

- Item 2's bound (one ended conversation per Maintain pass) needs an engine API change (a per-conversation Maintain);
  not done, as the brief allows. A non-turn request can wait for every remaining fold call of a pass.
- The timing tests added in item 2 (1000 ms and 1500 ms bounds against folds of about 2 s and 2.5 s) passed five runs
  in a row under asan; they keep a margin of about 2x.
- Only one [llama] test was run in-process (env vars set by me for that command only, models from models/).
