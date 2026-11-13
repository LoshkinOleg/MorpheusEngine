### Task 6: Documentation and the full check

The architecture spec and the README describe the seat; every suite runs.

**Files:**
- Modify: `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`, `README.md`

**Interfaces:**
- Consumes: everything above.
- Produces: documentation only.

- [ ] **Step 1: The architecture spec**

In `docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md`:

Replace

```
- **Engine worker thread:** the only owner of the llama.cpp context. Turns, summarization and embedding run here, one job at a time.
```

with

```
- **Engine worker thread:** the only owner of the llama.cpp context. Turns, summarization and embedding run here, one job at a time. The one exception is the model seat's Check (a debug feature of the app): it calls the backend's `CheckGrammar` from the UI thread, only while the worker is blocked waiting for the seat.
```

Replace

```
| `app` | SDL3 + Dear ImGui frontend: the Runs and Play windows, the inspectors, Settings | turn, llama (`main.cpp`'s backend factory) |
```

with

```
| `app` | SDL3 + Dear ImGui frontend: the Runs and Play windows, the inspectors, Settings, the model seat | turn, llama (`main.cpp`'s backend factory) |
```

Replace

```
  save_state() -> blob / restore_state(blob)   (KV sequence state in RAM, used when the GPU is handed to another model)
```

with

```
  save_state() -> blob / restore_state(blob)   (KV sequence state in RAM, used when the GPU is handed to another model)
  check_grammar(grammar, text) -> "" or a reason   (whether text, tokenized as an answer, fits the grammar from start to end)
```

After the bullet that starts `- Grammar sampling checks the sampled token first:`, add the bullet:

```
- `CheckGrammar` generates nothing: `LlamaCppBackend` tokenizes the text as an answer and runs llama.cpp's grammar sampler over it, checking each token alone and then accepting it, and at the end requires the end-of-generation token to be allowed. The reason names the first rejected token and the text before it, or says "the text ends before the grammar does". An empty or unparsable grammar is a `ContractError`. The app's model seat uses it to check a hand-written reply.
```

In the UI table, after the `| Settings | ... |` row, add:

```
| Model seat | Debug: answer the engine's model calls by hand. "Answer as the model" and a Human / Draft / Model mode per call label (resolve, narrate, rewrite, lookup, review, summarize, condense); the waiting call's label, limits, prompt by layer, grammar and the model's draft; reply and thinking boxes, Check (token count and grammar fit), Submit, and Withdraw (releases the call: cancels a running turn, or ends a between-turn fold or condense) |
```

After the paragraph that starts `The windows dock over one SDL3 window`, add:

```
The model seat (design: `docs/superpowers/specs/2026-11-13-model-seat-design.md`): `AppController` owns one `app::ModelSeat` and wraps every backend the engine host builds in an `app::ModelSeatBackend` around it, so the switch and the modes survive restarts; nothing is stored in `settings.json`. With the seat off, or a call's label in Model mode, the wrapper forwards. Otherwise the call waits on the worker thread for the owner's reply (in Draft mode after the real model's answer, offered as a draft); the reply is cut to the call's limits by the real tokenizer and returned as the model's completion, streamed once, whole. A cancel, or the window's Withdraw, wakes the wait and the call ends cancelled; the UI names the request it answers by its id, so a click that loses the race with a cancel is a no-op. The reply is not constrained by the grammar, so the engine's handling of model mistakes applies to it; `core`, `context` and `turn` do not know who answers.
```

- [ ] **Step 2: The README**

In `README.md`, in "Play in the app", after the bullet that starts `- **Settings:**` (before the `--frames N` line), add:

```
- **Model seat:** a debug window for answering the engine's model calls yourself. Tick "Answer as the model" and choose, per call (resolve, narrate, rewrite, lookup, review, summarize, condense), Human (you write the reply), Draft (the model answers first and you submit or edit its answer) or Model (as usual); every call starts in Human. A waiting call shows its token limits, its prompt by layer, its grammar and the model's draft. Check counts the reply's tokens and tests it against the grammar; Submit sends it whether or not it fits, and the engine treats it as the model's reply, mistakes included; Withdraw releases the call without a reply (during a turn it cancels the turn, as Cancel does; between turns it ends that fold or condense, and the next turn folds what it needs). Nothing is saved: the seat is off whenever the app starts.
```

- [ ] **Step 3: Run the tests preset**

Run: `cmake --build --preset tests && ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 311`.

- [ ] **Step 4: Run the AddressSanitizer preset**

Run: `cmake --preset asan && cmake --build --preset asan && ctest --preset asan`
Expected: `100% tests passed, 0 tests failed out of 311`, with no AddressSanitizer report.

- [ ] **Step 5: Build every dev target and start the app**

Run: `cmake --build build/dev --config RelWithDebInfo --target morpheus morpheus_tests morpheus_play morpheus_probe`
Expected: builds with no warnings.

Run: `./build/dev/bin/RelWithDebInfo/morpheus.exe --frames 120`
Expected: the window opens and the app exits with code 0 once the models have loaded (`echo $?` prints `0`).

- [ ] **Step 6: Commit**

```bash
git add docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md README.md
git commit -m "Document the model seat" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---

## Self-review: spec coverage

| Spec requirement | Task |
|---|---|
| Part of the normal app, no separate target; switched on in its window, nothing in `settings.json` | 4 (wrapped factory), 5 (window) |
| Every label starts in Human; modes change between calls | 2 (`_modes` all Human, `SetMode`), 5 (modes table) |
| The real chat model stays loaded | 4 (the wrapper owns the real backend) |
| A grammar check in the first version | 1 (`CheckGrammar`), 2 (`Check`), 5 (Check button) |
| `morpheus_play` and replay recording out of scope | not touched |
| Seven labels and how they are recognised; `SeatLabelOf`; unrecognised summary is a ContractError | 2 (the ContractError branch is kept but unreachable; see Choices) |
| Human / Draft / Model modes | 3 |
| `LlmBackend::CheckGrammar` with its comment | 1 |
| `LlamaCppBackend` implements it with the grammar sampler, single-token candidate arrays, accept, end-of-generation check | 1 |
| `RecordingBackend` forwards; seat backend forwards; `ScriptedBackend` configurable, "" by default | 1, 3 |
| `ModelSeat`: mutex, `enabled`, `modes`, pending `SeatRequest` (id, label, prompt, spec, draft), the reply | 2 |
| At most one pending request; a second is a ContractError | 2 |
| Wait on `std::condition_variable_any` with the stop token; a cancelled wait clears the request and returns `cancelled` | 2, 3 |
| UI methods `Enabled`, `SetEnabled`, `ModeOf`, `SetMode`, `Pending`, `Submit(id, ...)`, `Check(id, ...)` (backend verdict and token count, only with a grammar) | 2 |
| Request ids: a stale id makes `Submit`/`Withdraw` return false and `Check` nullopt without throwing; an id never issued is a ContractError; the window passes the id it shows | 2 (tests: stale Submit after a stop, unpublished id), 5 (window) |
| `ModelSeat::Withdraw(id)` wakes the waiter like the stop token; the call ends cancelled | 2, 3 |
| Withdraw on a maintenance request ends that maintenance cleanly and the next turn proceeds (and folds again) | 3 (worker-level test) |
| Withdraw on a turn call cancels the command, so the turn closes with TurnCancelled; one Withdraw button replaces Cancel turn, enabled whenever a request is pending | 5 (window, headless test clicking Withdraw) |
| Turning the seat off leaves a pending request | 2 (`SetEnabled`), 3 (test) |
| `ModelSeatBackend` forwards the eight other methods | 3 |
| `Complete`: off or Model forwards; Draft runs the real call silently, returns early if cancelled; publish and wait | 3 |
| Draft unchanged returns the model's completion; otherwise cut text and thinking, real counts, prompt counts, `tokensReused` 0, `generateMs` the wait, `promptMs` and `thinkingMs` 0 | 3 |
| `onToken` once with the whole answer | 3 |
| A human reply is not grammar-constrained; the engine's mistake handling shows | 4 (non-JSON resolve test) |
| `AppController` owns the seat, `ModelSeat& Seat()`, wraps the factory (tests too); the seat outlives the backends | 4 |
| Restarts keep the toggle and modes; a rebuilt backend is wrapped around the same seat | 4 |
| Status bar says the engine waits for the model seat | 5 |
| Window in `src/app/ui/ModelSeatWindow.cpp`, declared in `Windows.h`, drawn by `Ui`, drawable headless | 5 |
| Checkbox, modes table; label, limits, prompt by layer (shared drawing, not copied), grammar collapsed, draft thinking, reply box, thinking box, Check, Submit, Withdraw; edits survive frames and reset on a new request | 5 |
| Error handling: contract breaks, stale ids are no-ops, reply never an exception, backend errors propagate | 1, 2, 3 |
| Tests: `SeatLabelOf`; `ModelSeatBackend` over `ScriptedBackend` (all listed cases, Withdraw included); `ModelSeat` Submit/Check/Withdraw by id; `[llama]` CheckGrammar; engine reaction (non-JSON resolve, cancel); Withdraw on maintenance; restart; UI headless with no request, Human, Draft, Submit and Withdraw through the window | 1, 2, 3, 4, 5 |
| Documentation: architecture spec (UI section, inference section), README "Model seat" paragraph | 6 |
