# The model seat - Design

Date: 2026-11-13. Source: the brief `.superpowers/briefs/2026-11-06-human-model-seat.md` (MorpheusAmaAgent), with the owner's decisions from the brainstorming session that followed.

## Goal

A debug feature of the normal app in which the owner answers the engine's model calls by hand. The owner sees exactly what the model would see for each call: the prompt, the token limits and the grammar. The engine treats the reply as the model's. The point is to understand, call by call, what the model is given and how the engine reacts to what comes back, mistakes included.

## Owner decisions

- It is part of the normal app, not a separate build or target.
- It is switched on and off in its window, at any time, with nothing stored in `settings.json`.
- Every call label starts in Human mode (the owner writes the reply from scratch). Each label's mode can be changed between calls.
- The real chat model stays loaded: there is no tokenizer-only mode.
- The first version has a grammar check.
- `morpheus_play` is out of scope.
- Recording the owner's replies as replay fixtures is out of scope (the app cannot record runs today).

## Call labels and modes

The seat labels each `Complete` call with one of seven labels:

| Label | How it is recognised |
|---|---|
| `resolve` | `turn::CallKindOf` returns `Resolve` |
| `narrate` | `CallKindOf` returns `Narrate` and the last message does not contain `turn::RENARRATE_CORRECTION` |
| `rewrite` | `CallKindOf` returns `Narrate` and the last message contains `RENARRATE_CORRECTION` (the renarrate) |
| `lookup` | `CallKindOf` returns `Lookup` |
| `review` | `CallKindOf` returns `Review` |
| `summarize` | `CallKindOf` returns `Summarize` and the last message ends with `turn::SUMMARIZE_INSTRUCTION` |
| `condense` | `CallKindOf` returns `Summarize` and the last message ends with `turn::CONDENSE_INSTRUCTION` |

`app::SeatLabel` is an enum with these seven values and `SeatLabelOf(prompt, spec)` computes it. A `Summarize` call whose last message ends with neither instruction is a `ContractError`: the engine makes no such call.

Each label has a mode, `app::SeatMode`:

- **Human:** the real model is not asked. The request waits for the owner's reply. This is every label's mode when the app starts.
- **Draft:** the real model answers first. The request then waits with the model's answer as a draft the owner can submit unchanged or edit.
- **Model:** the call goes to the real model and does not wait, as when the seat is off.

## Architecture

### The backend interface gains a grammar check

`inference::LlmBackend` gains one method:

```cpp
// Whether `text`, tokenized as an answer, is accepted by `grammar` (GBNF, root rule "root") from start to end.
// Returns "" when it is; otherwise a short reason naming where it stops fitting (the first rejected token and the
// text before it, or "the text ends before the grammar does"). Throws ContractError for a grammar that does not
// parse.
virtual std::string CheckGrammar(const std::string& grammar, std::string_view text) = 0;
```

- `llama::LlamaCppBackend` implements it with llama.cpp's grammar sampler and no generation:
  - it tokenizes `text` as an answer;
  - for each token it applies the sampler to a candidate array holding only that token, and a rejected token is the mismatch;
  - it accepts each token in turn;
  - at the end it checks that the end-of-generation token is allowed.
- Every other implementation implements it too:
  - `turn::RecordingBackend` and the new seat backend forward it;
  - the test fake `ScriptedBackend` returns a configurable answer, "" by default.
- Only `src/llama` uses llama.cpp, so this method is how the app reaches the grammar matcher without breaking module boundaries.

### ModelSeat: the shared state

`app::ModelSeat` (new, `src/app/model/ModelSeat.h/.cpp`) is shared between the engine worker thread and the UI thread. A mutex guards every member.

- **`enabled`:** the window's "Answer as the model" toggle. False at start.
- **`modes`:** one `SeatMode` per `SeatLabel`, all Human at start.
- **The pending request,** at most one, as a `SeatRequest`:
  - `label`;
  - `prompt` (`inference::Prompt`);
  - `spec` (`inference::GenerationSpec`: `maxTokens`, `thinkingMaxTokens`, `grammar`);
  - `draft` (`std::optional<inference::Completion>`, the model's answer in Draft mode).
- **The reply:** text and thinking, set by Submit.

The worker runs one job at a time, so at most one request is ever pending.

The worker waits on a `std::condition_variable_any` with the call's `std::stop_token`, so a Cancel, or a new command preempting maintenance, wakes it. A cancelled wait clears the pending request and returns a completion with `cancelled` set, exactly as a cancelled model call does.

Methods for the UI thread:

- `Enabled()`, `SetEnabled(bool)`, `ModeOf(SeatLabel)`, `SetMode(SeatLabel, SeatMode)`;
- `Pending()`: a copy of the pending request, or nullopt;
- `Submit(text, thinking)`: requires a pending request (ContractError otherwise);
- `Check(text)`: requires a pending request with a grammar. It returns the backend's `CheckGrammar` result and the reply's token count. The engine thread is blocked in the wait whenever a request is pending, so calling the backend from the UI thread at that moment does not race with the worker. That is why Check is allowed only then.

Turning the seat off while a request is pending does not abandon it: the request stays until it is submitted or the command is cancelled. Turning it off only affects later calls.

### ModelSeatBackend: the wrapper

`app::ModelSeatBackend` (new, `src/app/model/ModelSeatBackend.h/.cpp`) implements `LlmBackend` around the real backend and a `std::shared_ptr<ModelSeat>`.

- `CountTokens`, `CountPromptTokens`, `ContextSize`, `Embed`, `EmbeddingModelId`, `SaveState`, `RestoreState` and `CheckGrammar` forward to the real backend unchanged.
- `Complete`:
  1. When the seat is off, or the call's label is in Model mode, it forwards to the real backend.
  2. In Draft mode, it first runs the real `Complete` with no token callback, so nothing streams to the Play window yet. It returns early if that call is cancelled. The result becomes the request's draft.
  3. It publishes the request and waits for Submit or the stop token.
  4. It builds the completion from the reply:
     - **Draft submitted unchanged:** the text and thinking equal the draft's. The completion is the model's own completion, with its token counts and timings.
     - **Otherwise, the reply:**
       - `text` is cut at `spec.maxTokens` tokens, by the real tokenizer, with `stoppedAtLimit` set when cut. The cut is at a whole token's text, so the result stays valid UTF-8.
       - `thinking` is cut at `spec.thinkingMaxTokens` tokens, with `thinkingStoppedAtLimit` set when cut. It is empty and ignored when the budget is 0.
       - `tokensGenerated` and `thinkingTokens` are the real token counts of what is kept.
       - `promptTokens` and `tokensEvaluated` are `CountPromptTokens(prompt)`, and `tokensReused` is 0.
       - `generateMs` is the time from publishing the request to Submit; `promptMs` and `thinkingMs` are 0.
  5. It calls `onToken` once with the whole answer, so the Play window shows it as if it had streamed.
- A human reply is not constrained by the grammar. The engine's handling of model mistakes then shows: an unreadable resolve proposes nothing, an invalid change is rejected with its reason, an unreadable review is recorded as an error.

### Wiring into the app

- `AppController` owns the `std::shared_ptr<ModelSeat>` and exposes it (`ModelSeat& Seat()`).
- `AppController`'s constructor wraps the factory it is given, so every backend the engine host builds is a `ModelSeatBackend` around the factory's backend. This holds in tests too: with the seat off, the wrapper is transparent.
- The seat outlives every backend that refers to it.
- **Restarts:** when settings are applied, the engine host keeps or rebuilds the backend as today. A rebuilt backend is wrapped again around the same seat, so the toggle and the modes survive. A restart while a request is pending cancels the running command first (existing behaviour), which wakes the wait.
- **Status bar:** while a request is pending, it says the engine is waiting for the model seat.

No change to `core`, `context` or `turn` behaviour: the engine does not know who answers.

## The window: "Model seat"

A dockable window in `src/app/ui` (`ModelSeatWindow.cpp`), declared in `Windows.h` and drawn by `Ui` like the others. It must draw headless in tests.

- **Always shown:**
  - an "Answer as the model" checkbox;
  - a table of the seven labels, each with a Human / Draft / Model choice.
- **When a request is pending:**
  - the label;
  - the limits: maximum answer tokens, and the thinking budget when it is above 0;
  - the prompt, message by message and split by layer, drawn as the Context window draws the last assembly. The drawing code is shared with the Context window, not copied.
  - the grammar text, in a collapsed section, when the call has one;
  - in Draft mode, the model's thinking, read-only, when it has some;
  - the reply box (multi-line). It is pre-filled with the draft's text in Draft mode, and empty in Human mode.
  - a thinking box (multi-line) when `thinkingMaxTokens > 0`, pre-filled with the draft's thinking in Draft mode;
  - **Check:** enabled only when the call has a grammar. It shows the reply's token count against `maxTokens`, then "Fits the grammar" or the reason it does not.
  - **Submit:** sends the reply whether or not it fits.
  - **Cancel turn:** cancels the running command through the controller, as the Play window's Cancel does.
- **Edits survive frames:** the reply and thinking texts stay in the window's own state until Submit, and are reset when a new request arrives.

## Error handling

- **Engine contract breaks** throw `ContractError`:
  - `Submit` or `Check` with no pending request;
  - `Check` on a call without a grammar;
  - a second request published while one is pending;
  - an unrecognised summary call.
- **Your reply is never an exception:** it goes to the engine like a model's, and the engine's existing rejection paths apply.
- **Real backend errors propagate as today:** an `InferenceError` from the real model in Draft or Model mode, or from the tokenizer.

## Tests (written first, Catch2)

- `SeatLabelOf`: each of the seven labels from a prompt and spec built the way the session and folding build them; a summary call with neither instruction is a ContractError.
- `ModelSeatBackend` over `ScriptedBackend`:
  - forwards `CountTokens`, `CountPromptTokens`, `ContextSize`, `Embed`, `EmbeddingModelId`, `SaveState`, `RestoreState` and `CheckGrammar` unchanged;
  - with the seat off, or in Model mode, `Complete` forwards and publishes nothing;
  - Human mode: `Complete` on a worker thread publishes a request with the right label, prompt and spec, waits, and returns the submitted text with real token counts, `tokensReused` 0 and `onToken` called once with the whole text;
  - Draft mode: the inner reply is the draft, nothing streams before Submit, and an unchanged submit returns the inner completion itself; an edited one returns the edit;
  - a reply over `maxTokens` is cut with `stoppedAtLimit` set; thinking over `thinkingMaxTokens` is cut with `thinkingStoppedAtLimit` set;
  - a pending request wakes on the stop token, returns `cancelled` and clears the request;
  - turning the seat off while a request is pending leaves the request pending.
- `ModelSeat`: Submit and Check without a request are ContractErrors; Check returns the backend's grammar verdict and the token count.
- `LlamaCppBackend::CheckGrammar` (`[llama]`, model-backed): text matching a small grammar returns ""; text that breaks it or ends early returns a reason.
- Engine reaction, through `AppController` with a scripted factory: a resolve reply typed in Human mode that is not JSON gives a turn with nothing proposed, as for a model mistake; a cancel while the seat waits closes the command with `TurnCancelled`.
- Restart: applying settings that rebuild the backend keeps the toggle and the modes.
- UI, headless: the window draws with no request, and with a pending request in Human and Draft modes; Submit through the window answers the request.

## Documentation

- Architecture spec: the UI section gains the Model seat window; the inference section gains `CheckGrammar`.
- README: a short "Model seat" paragraph in "Play in the app".
