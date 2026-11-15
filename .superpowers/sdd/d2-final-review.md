# D2 (morpheus_server) final whole-branch review

Range: ef96ac8..57e045c (18 commits). Spec: docs/superpowers/specs/2026-11-16-morpheus-server-design.md. Plan:
docs/superpowers/plans/2026-11-16-morpheus-server.md. Read: the whole src/server module, tools/server/main.cpp,
tests/server/*, the HostedSession, RunStore and LlamaServerBackend changes, the doc diffs, the D2 ledger, and the
parts of cpp-httplib 0.59.0 (write_content_chunked, the releaser, the thread pool) that the stream depends on. No
tests were run: no concrete doubt needed one, and the controller's live evidence covers the real servers.

## Strengths

- The layering holds. RunWorker owns the thread, the session and the backend. RunRegistry owns the workers. Api
  holds every route and status. HttpServer only carries bytes. Dependencies flow downward (server -> turn,
  llamaserver), and only HttpServer.cpp includes <httplib.h>.
- RunWorker's stop logic is correct and carefully argued. `_stopping` is checked as well as the thread's stop token,
  queued entries are failed under the same lock that Enqueue checks, every popped entry's promise is always set, and
  `_thread` is the last member, so it joins before the session and the backend go (RunWorker.h:58). On a failed open,
  the constructor's member unwinding joins the thread.
- The turn path's lifetimes are right. The job copies the log, the route name and the SlotPool pointer instead of
  touching Api. TurnStream is shared between the job and the provider. A stop_callback ties the worker's stop to the
  stream's stop source. cpp-httplib calls the releaser with false both on a client disconnect and on server shutdown
  (write_content_chunked returns false when is_shutting_down), so each case cancels the turn.
- Shutdown order is consistent in main.cpp, HttpHarness and the live test: StopAll, then Stop, then the workers join
  as the registry goes. The SlotPool, the fakes and the log mutex are declared before the registry, so they outlive
  the workers.
- No deadlock is reachable. Lock order is registry _openMutex -> registry _mutex -> worker _mutex, and no worker job
  takes a registry lock. Reopen-under-_openMutex waits only on that run's own queue. StopAll fails a queued Reopen.
- The event overflow follows the spec exactly. Hits are shed first, then the oldest events. The record counts them,
  and the L3 entry drops the same events. A stored count larger than the events is a StoreError. The v3->v4 and
  v2->v4 upgrades run in one transaction, with rollback tests.
- The tests are strong at the Api level. Every status is covered with its field. Event order is checked, sentences
  are checked against token arrival, done's fields are checked, cancellation commits nothing, and slots are checked
  per conversation. The HTTP tests use real sockets against FakeLlamaServer, including a mid-line disconnect, two runs
  at once with id_slot checked, and an exclusive port.

## Spec promises checked against code

| Promise | Status |
|---|---|
| api_version on every response | Holds for every Api answer (Api.cpp:432) and the first stream event. It fails for answers cpp-httplib makes itself (I3). |
| Stream events in order: action, then token/sentence, then done or error | Holds, except that a turn stopped by shutdown ends with neither done nor error (I2). |
| Sentences | Holds (SentenceSplitter, with the remainder flushed before done). |
| Slots: upkeep slot 0, LRU, freed on end | Holds (SlotPool, RunWorker.cpp:175, Api.cpp:318/414). See M1 for in-use eviction across runs. |
| Event overflow cure | Holds (HostedSession.cpp RunTurn/Entries). See M2 for cost. |
| Store version 4 | Holds (RunStore.cpp, with tests for 3->4, 2->4 and failed upgrades). |
| Error statuses | Each exception maps as the spec says, except that lore which cannot be embedded is always 400, even when the cause is an unreachable server (I4). |

### Exceptions that can reach Api::Handle and the turn job

| Exception | Status |
|---|---|
| DataError (from HostDataFromJson, HostedWorld, HostedSession checks, CheckRunId, the case clash, body parsing) | 400 |
| NotFound | 404 |
| NotOpened | 409 |
| TurnError (from Fit, when the tail is over tail_allowance without hits or events) | 422 |
| InferenceError: backend failures, "the server is stopping" from Post, Open and Reopen, and a run closed after a failed fallback | 503 |
| StoreError, ContractError and nlohmann exceptions | 500, logged once (the job logs its own) |

Lore embedding wraps InferenceError into DataError (Retrieval.cpp:309), so it reports 400 for both a too-long entry
and a dead or busy embedding server (I4). Non-std exceptions are not caught by the job, so stream->End() would be
skipped (see I2's fix).

## Issues

### Critical

None introduced by this branch. I1 is pre-existing, but it blocks the host's own model and should be treated as a
must-fix before the host integrates.

### Important

**I1. Every prompt through llama-server with Qwen3.5, the host's named model, fails (pre-existing since C, found in
D2's live run).**
- Location: src/context/PromptBuilder.cpp:67 (`systemCost` renders a system-only prompt) and :112
  (`resolveOutputOverhead` renders an assistant-only prompt).
- Problem: Qwen3.5's template raises "No user query found in messages" for any message list without a user message,
  so both renders fail. The live run tripped only on :67 because it comes first. After :67 is fixed, :112 fails the
  same way. Every turn and every background fold goes through LayOut (Maintain -> Pass), so both fail: a 503 for turns
  and a log line per job for folds.
- Impact: The requirements brief names Qwen3.5 9B as the host's model, so morpheus_server cannot serve its customer
  as is.
- Fix: see design question 2.

**I2. A turn stopped by shutdown mid-line ends its stream with neither done nor error.**
- Location: Api.cpp:337-366.
- Problem: When the worker stops, the stop_callback cancels the stream. RunTurn returns nullopt and the job only
  calls End(). The README and the spec say a stream ends with done or error, and API 1.0.0 is the contract the host
  builds against. A non-std exception escaping the job would also skip End(), which leaves a stream that never ends
  (the provider polls until the client leaves).
- Fix (cheap):
  - When `!record && workerStop.stop_requested()`, push `error {503, "the server is stopping: the turn was not
    committed"}`.
  - When the client cancelled, nobody reads the stream, so nothing needs to be pushed.
  - Make End() a scope guard at the top of the job.
  - Add one test: StopAll during a slow line yields an error 503 event and no done.

**I3. Answers that cpp-httplib produces itself carry no JSON body and no api_version.**
- Location: HttpServer.cpp:129-133.
- Problem: Only GET, POST, PUT, PATCH and DELETE are routed. OPTIONS gets cpp-httplib's own 404 and an unknown
  method its own 400. So do its own 400/413/414 for malformed requests, and its 500 for anything that escapes a handler. All of these
  have an empty body, which breaks the spec's "every response, error or not, carries api_version".
- Fix (cheap): add a `set_error_handler` that writes `{api_version, error: {status, message}}` when the body is
  empty, and a `set_exception_handler` that writes a 500 the same way. Add one test (OPTIONS /health).

**I4. Lore that cannot be embedded is always a 400, including when the embedding server is unreachable or answers
5xx.**
- Location: src/turn/Retrieval.cpp:305-311.
- Problem: The spec maps an unreachable llama-server to 503. In practice, a fresh open checks /props first (the
  backend factory), so the exposed window is mostly the Reopen path (changed lore, run already open) and transient
  server errors. Those are exactly the cases where a host should retry. A 400 tells the host its data is wrong, so it
  will not retry.
- Fix options:
  1. Give InferenceError a reason: the server refused this input (it answered 4xx), or it was unreachable or failed
     (no response, or 5xx). Vectors then wraps only the refused case into DataError.
  2. If that is too much for this branch, document the deviation in the spec's error table and the README, and file
     it.

  Option 1 touches inference, llamaserver, llama (which can call everything "refused") and one catch in Retrieval.

### Minor (new in this review)

- **M1. LRU eviction can take a slot that a turn of another run is using right now** (SlotPool.cpp:95-102). Lru picks
  the oldest `lastUse`, which is the Acquire time, not "idle since". So a long-running turn's slot can be taken while a
  more recently used slot sits idle. The new turn then queues inside llama-server behind the running one. This only
  happens with concurrent runs and more conversations than slots. Fix: mark a seat busy between Acquire and the end
  of its job, and evict only idle seats while one exists.
- **M2. The overflow cure runs one full Pass per dropped event** (HostedSession.cpp:281-284). Through llama-server, a
  Pass is one /apply-template plus a /tokenize per message piece and per transcript message, which is dozens of HTTP
  calls. A burst of 50 events can add seconds to the turn, against R4's 1.5 s target. Fix: count each event once
  (CountTokens), drop as many of the oldest as the overshoot needs, then run one confirming Pass (and loop only if it
  is still over).
- **M3. `done.timings` forwards CallTelemetry whole** (Api.cpp:352-354). With a thinking model (Qwen3.5 is one), this
  includes `thinking.text`, the model's private reasoning, which can be thousands of tokens, plus the refusal reasons
  a second time. The spec promises only times and reused tokens. Fix: send a trimmed object (prompt_ms, generate_ms,
  reused, evaluated, generated) or drop `thinking.text`.
- **M4. The fixtures over HTTP check the transport well but the stored state thinly** (HttpServerTests.cpp:91-99).
  GET is compared only on `line` and `listeners`. Fix: also compare the stored `action`, `refused` against
  `expect.reasons`, and GET's `events` against `new_events`. Also check sentence events concatenate to the line, done's
  `speaker`, `action` and `events_omitted`, and open's `created` (true, then false on a second open). The Api-level
  tests cover these fields, so the gap is only that the transport is not proven to carry them.
- **M5. Turn jobs hold a raw SlotPool pointer beyond Api's lifetime** (Api.cpp:313). main.cpp and the harnesses
  declare the pool before the registry, so the pool outlives the workers, but nothing states this requirement. Fix:
  say in Api.h (or RunRegistry.h) that the SlotPool must outlive the RunRegistry.

## Triage of the ledger's Minor findings

### Fix before merge

- **Task 9: a shutdown-stopped turn ends with neither done nor error; End() is not guarded.** This is I2. It breaks
  the stream contract of API 1.0.0, and the fix is a few lines.
- **Task 10: OPTIONS, cpp-httplib-rejected requests and escaped exceptions answer without JSON or api_version.** This
  is I3. It breaks the spec's "every response" rule, and the fix is two handlers.
- **Task 6 (Api consequence): unembeddable lore at open/reopen is a 400, not 503.** This is I4. Fix it, or at least
  document it in the spec and README before the host relies on the status table.
- **FOUND (Qwen3.5 /apply-template).** This is I1. It blocks the host's model. Land it on this branch if the owner
  agrees (design question 2), or as the first commit after the merge, before the host integrates.

### Can wait (worth a follow-up list)

- **Task 2: one full Pass per dropped event.** This is M2. It is a performance cost only in the overflow case.
- **Task 2: the StoreError for events_omitted names the turn and column but not the file.** CLAUDE.md asks that a
  saved-data error name the file. HostedSession knows its path from Open, so this is a cheap fix to include next time
  the file is touched.
- **Task 6: Call/Reopen from inside a job deadlocks.** No caller does this today. A
  `MORPHEUS_REQUIRE(std::this_thread::get_id() != _thread.get_id())` in Enqueue would make it fail fast, as CLAUDE.md
  wants. Cheap.
- **Task 6: the background-fold catch swallows ContractError.** This is plan-mandated, and a server should not die
  over one fold. But a ContractError is a bug: log it distinctly (for example with "BUG:") rather than as an ordinary
  failure.
- **Task 6: a down model logs one failure per job.** Log spam only. With I1 unfixed on Qwen3.5, every job will log.
- **Task 6: Reopen ignores its stop token.** It is bounded by the embed time and delays shutdown only.
- **Task 6: untested paths** (queued jobs failed by RequestStop, a stop during a fold, a job on a closed run).
- **Task 6: the 600 ms timing test.** It is a flake risk under ASan. Watch it.
- **Task 7: Windows reserved names (CON, NUL, AUX, PRN, COM1-9, LPT1-9) pass CheckRunId.** On Windows 10,
  `runs/CON.morpheus` names the console device, so the result is a confusing 500. Host ids may come from save names.
  Reject them (case-insensitive) in CheckRunId. Cheap; do it soon.
- **Task 7: a refused brand-new open leaves its file; no test of StopAll during a build; no concurrent-Open test.**
  The file is harmless (the next start reports created=false).
- **Task 8: the body is parsed before routing; empty path segments are dropped; untested items.** Cosmetic.
- **Task 9: the slot is acquired before RunTurn's checks** (a rejected request or an ended conversation holds a
  slot). Evicted by LRU anyway. Cheap fix: skip Acquire when `Conversation(id)->ended`.
- **Task 9: the handler cannot notice a disconnect before the first event.** By design, and bounded by the resolve
  call. Document it.
- **Task 9: the dereference at ApiTurnTests:362.** Test-only.
- **Task 10: untested Start-after-failed-bind, Stop alone ending a stream, the Cache-Control header.** Fine to defer.
- **Task 11: `--port` accepts "+5" and " 5".** Cosmetic. **Unused includes; the live test prints -1 ms when no
  sentence came.** Cosmetic.
- **Tasks 1, 3, 4, 5 (style and coverage):**
  - Comment wraps over 120 columns: RunStore.h, HostedSession.h:81, LlamaServerBackend.h and CLOSERS.
  - The missing CHECK_THROWS_AS and `<cstddef>`.
  - The dead `!sentence.empty()` check.
  - The cryptic "Hello.5" comment.
  - The untested SlotBackend SaveState/RestoreState forwarding.
  - The threaded SlotPool test checking no holder invariant.

  Batch these into one tidy-up commit.
- **Task 5: only ASCII whitespace ends a sentence.** Document it next to the abbreviation note. A model emitting
  U+00A0 or U+3000 after a period would delay that sentence to the next ASCII space.
- **Task 4: the README's consumer-check Windows command fails against an existing cache.** This predates D2; it is
  docs-only.

### Wrong or harmless as stated

- **Task 6: "slot inherited from the fold by a job that sets none".** Harmless. Only the turn job calls Complete, and
  it always sets its slot. StartConversation, RecordEvents, SetParticipants, EndConversation and GET make no
  completion calls.
- **Task 6: "_seed not updated on Reopen".** Harmless. The seed matters only when the file is missing, and the
  fallback reopen is meant to restore the run exactly as it was opened.
- **Task 4: "Release("") accepted".** Harmless. It only clears seats that are already free, and slot 0 is never held
  by a key.
- **Task 8: "error messages echo raw path bytes".** Not a defect. The JSON is dumped with `error_handler_t::replace`,
  so invalid UTF-8 cannot break the body.
- **Task 1 and Task 3: "RED run skipped".** Process notes, not code findings.

## Design question 1: any job cancels a running background fold

### What happens now

Every Enqueue requests `_maintenanceStop` (RunWorker.cpp:286). Folds land per conversation (each summary is stored as
it lands), so a preemption wastes only the call in flight. But a fold call on the host's hardware is several seconds
(prefill of the conversation plus about 200 generated tokens). A host that posts `/events` or `/participants` more
often than that, or polls GET, restarts the fold forever. Ended conversations then never reach summaries and the
retrieval index, so cross-conversation memory never forms.

### Recommendation

Only a turn job preempts a fold. Other jobs queue behind the fold's current call.

FIFO order makes this cheap in latency. A turn arriving behind a waiting `/events` job still stops the fold at once,
so turns never wait for folding. Only a non-turn request with no turn behind it waits.

### Cost

- **Code:**
  - Post/Enqueue gain a flag (for example `Post(job, Preempts::Fold)`), and Api passes it only for `/turn`.
  - One RunWorker test: a non-turn job during a fold waits and the fold completes. A turn queued behind it still
    stops the fold.
  - About 40 lines.
- **Behaviour:**
  - `/events`, `/participants`, `/conversations`, `/end` and GET may wait for the rest of Maintain while it runs: one
    fold call per ended conversation, condensing included. This can be tens of seconds after a long session with
    several ended conversations.
  - To bound that, Maintain could run one conversation's FoldAll per worker iteration (the loop already re-enters
    while `_maintenanceDue`). Then a non-turn job waits for at most one conversation's fold. That is a small change
    inside RunWorker, or a `MaintainOne` on HostedSession.
- **Docs:** one spec and README line saying that non-turn routes may wait up to one fold call during background
  upkeep.

### Alternatives

- **Keep the current behaviour and document it.** No code, but folding can starve on an event-heavy host.
- **A starvation guard** (after N preemptions, the next non-turn job waits for the fold). It is more state for the
  same effect.

## Design question 2: counting the system message without a system-only prompt

### Facts

Both backends tokenize the template pieces and the contents separately. TokenizePrompt (LlamaServerBackend.cpp:651-662)
and the in-process backend (LlamaCppBackend.cpp:667, same TemplatePieces scheme) both do this. A message's content
therefore contributes exactly `CountTokens(content)` tokens to `CountPromptTokens`, wherever it sits.

### Recommendation

1. **systemCost.** In LayOut, replace the system-only render with
   `systemCost = fixedTotal - tokens.tail;`

   This is exact: the templated prompt (system and tail) minus the tail's text tokens. It costs no extra render, and
   in fact saves one /apply-template call and its /tokenize calls per LayOut.

   Its meaning changes slightly. It now also includes the tail message's frame (`<|im_start|>user\n ... <|im_end|>\n`,
   about 5 tokens). That is more honest: tail_allowance budgets raw tail text, so the frame was previously
   unbudgeted. Only the final contextOverflow check caught it.
2. **resolveOutputOverhead.** Replace the assistant-only render (:110-112) with
   `CountPromptTokens({user "", assistant ""}) - CountPromptTokens({user ""})`.

   The second term is already computed as narrateMessageOverhead. This stays an approximation, as the comment already
   says, but every render now has a user message. It costs one extra render per LayOut. Both overheads are constant
   per backend, so they could also be computed once and cached by the caller.
3. **Tests:**
   - Give FakeLlamaServer's /apply-template an option that refuses a message list without a user message (Qwen3.5's
     rule).
   - Run the hosted turn tests and the HTTP fixture tests through it.
   - Add a LayOut unit test pinning `systemCost == fixedTotal - tail` with ScriptedBackend.

### Cost

- transcriptBudget shrinks by the user frame (a few tokens) for both backends and both kinds of run.
- Tests that pin exact budgets change, in the context tests with ScriptedBackend's per-message overhead.
- A recorded replay whose fold boundary sits within those few tokens could fold one turn earlier and need
  re-recording. Check `[replay]` after the change.
- Owned saves are unaffected: budgets are computed, not stored.
- The in-process path changes the same way, so the two backends stay in step.

### Alternative

Keep the old meaning (the system message alone, plus the generation prompt). That needs the user frame's cost, which
cannot be measured without a reference render that itself has no user message. The few-token difference is not worth
a backend API addition (for example `TemplateOverhead(Role)`).

## Assessment

**Ready to merge: with fixes.** The concurrency design is sound: no deadlock, no leaked thread, no dangling capture,
and shutdown ends every stream. The store, overflow and slot promises hold. Before merge, fix the stream end on
shutdown (I2) and the JSON/api_version gap for cpp-httplib's own answers (I3). Either fix I4 or record it as a
documented deviation. Land the Qwen3.5 counting fix (I1, design question 2) before the host integrates, ideally on
this branch.

## Re-review (fix round 912dc0b..4937a21, six commits on 57e045c)

Read: d2-final-fix-brief.md, d2-final-fix-report.md, the whole 57e045c..4937a21 diff, and the current RunWorker.cpp/.h,
Api.cpp (turn job), HttpServer.cpp and PromptBuilder.cpp. No tests were run; the report gives 571/571 in tests and in
asan.

### Each finding

| Finding | Status |
|---|---|
| I1 (no prompt without a user message) | Fixed in code |
| Design question 1 (only turns stop a fold) | Fixed, with one bound not done |
| I2 (stream end on shutdown) | Fixed |
| I3 (JSON for cpp-httplib's own answers) | Fixed |
| I4 (unreachable embedding server is 503) | Fixed |
| Guards (device names; Call/Reopen on the worker's thread) | Fixed |

**I1: fixed in code.**
- What changed in PromptBuilder.cpp:
  - `systemCost = fixedTotal - tokens.tail`, with the exactness argument in a comment.
  - The resolve overhead is `CountPromptTokens({user, assistant}) - CountPromptTokens({user})`.
  - No render without a user message is left. The fix's grep of src/context and src/turn matches mine: SummarizePrompt,
    CondensePrompt, Review and the narrate extension all end with a user message.
- Tests:
  - FakeLlamaServer::RefuseWithoutUserMessage mirrors llama-server's 400.
  - LlamaServerTurnTests drives an owned turn and a hosted turn through it, with a RED run shown.
  - HttpHarness turns the refusal on for every HTTP test, folds included.
- Morpheus's own game: transcriptBudget shrinks by the tail message's frame (a few tokens), for both backends. That is
  the intended, more conservative accounting. `[replay]` replays unchanged. In-process, the hosted [llama] test passed
  on Qwen3-4B. The code is the same TemplatePieces scheme, so the subtraction is exact there too.
- Not verified: no run against the real Qwen3.5 template. The fake models only the "needs a user message" rule. The
  new `{user "", assistant ""}` render with add_generation_prompt has not been seen by Qwen3.5's real Jinja template
  (its handling of assistant messages after the last query, its think-block insertion, and any other raise).
  models/ holds Qwen3.5-9B-Q4_K_M.gguf, so this check is cheap (see R1).

**Design question 1: fixed; one part not done.**
- What changed:
  - `Post(job, Preempt)` exists, and Enqueue requests the stop at queue time only for `Preempt::Fold`.
  - The worker loop is unchanged and still correct. A preempted pass returns false and the queue is served FIFO. A
    pass that ran to the end clears `_maintenanceDue`.
  - A fresh stop source per pass means a turn queued at any point while a pass runs stops it. A turn queued before
    the pass starts is popped first.
  - Api uses Fold only on the turn route. Reopen and Call never preempt.
- Tests: the new worker and Api tests cover a turn behind a waiting job, a poller letting the fold land, and a turn
  stopping a slow fold.
- Not done: the bound of one ended conversation per pass needs an engine API (a per-conversation Maintain). The spec
  and README now say non-turn requests may wait for a whole pass. Accepted, but see M6.

**I2: fixed.**
- The job pushes `error 503 "the server is stopping"` when RunTurn returns nullopt under the worker's stop.
- A local RAII guard calls End() on every exit, and the old trailing End() is gone. The guard is declared first, so it
  runs after the catch block's Push.
- A stop during the resolve call makes that error the first event, so the handler answers a JSON 503. That is right.
- A client cancel stays silent. Test added (StopAll mid-line).

**I3: fixed.**
- `ErrorBody` in Api is the single source of the error body's shape.
- `set_error_handler` fills only answers with an empty body, so the Api's own bodies and the 200 stream are untouched.
- `set_exception_handler` answers 500 through `Api::EscapedError`, which logs.
- Test over real sockets: OPTIONS gets 404 and BREW gets 400, both as JSON with api_version. The exception handler has
  no test (no route can throw); acceptable, as the code is a few lines.

**I4: fixed.**
- `inference::InputRefused : InferenceError` is thrown:
  - by LlamaServerBackend::Ask for any 4xx;
  - by LlamaCppBackend::Embed for a text over the embedding context;
  - by ScriptedBackend's embed limit.
- SyncLore wraps only InputRefused into DataError.
- Every other catch of InferenceError still sees it, so no other path's status changes.
- Tests: 500 and a dropped connection give 503; 400 gives 400, naming the entry.

**Guards: fixed.**
- CheckRunId refuses the 22 device names in any case. "With an extension" is already impossible, since '.' is not an
  allowed character.
- Call and Reopen MORPHEUS_REQUIRE not to run on the worker's thread. `_thread` is set in the constructor before any
  other thread can reach the worker, so reading its id is race-free.

### Concurrency re-check

- No new lock and no new wait cycle.
- One new wait chain exists: RunRegistry::Open holds `_openMutex` while Reopen now waits for a whole Maintain pass
  (Reopen does not preempt). Opens of other runs wait behind that. This is bounded, but see M6.
- Shutdown still ends every stream:
  - RequestStop stops the running pass and fails queued work.
  - The turn job's guard ends its stream.
  - The error handler runs only for non-stream answers.

### Remaining

- **R1 (Important, verification only, not a code defect):** I1's fix has not been seen against the real Qwen3.5
  template on llama-server. Before the host integrates, run the [server] tests (and ideally the README curl session,
  which includes a background fold) with models/Qwen3.5-9B-Q4_K_M.gguf on llama-server.
- **M6:** without the one-conversation bound, a non-turn request can wait for every fold call of a pass. So can a
  reopen, which holds the registry's `_openMutex`, so opens of other runs wait too. Follow-up: a `MaintainOne` on
  HostedSession, or let Reopen preempt (it is rare and already slow).
- **M7:** Ask maps every 4xx to InputRefused. An embedding URL that points at a wrong path or service (404) then makes
  lore at open a 400 naming the entry, not a configuration 503. This is rare, and the message names the URL and the
  status, so it can wait.
- **M8:** the fix round left comments wrapped past 120 columns:
  - RunWorker.h:28 (202) and :88
  - RunWorker.cpp:73 (182)
  - Api.cpp:313 (173)
  - PromptBuilder.cpp:91
  - PromptBuilder.h:58-62
  - Session.h:80
  - FakeLlamaServer.h:42

  Re-wrap them in the tidy-up commit.
- **M9:** the exception handler is untested (noted above).

### Verdict

**Ready to merge.** No Critical findings and no Important code findings remain. R1 is a live check of I1 against the
host's real model, to run before the host integrates, not a merge blocker.
