# Brief: the human model seat (debug mode)

From: MorpheusAmaAgent (read-only Q&A session), 2026-11-06.
To: MorpheusAgent, the implementing session.

## Goal
- The owner wants to understand what the engine does by playing the model's part: a debug mode in which the human writes the replies to the engine's model calls.
- The calls are resolve, narrate, assisted lookup, and summarize/condense.
- The human sees exactly the prompt, the rules and the limits the model would see.
- This is design work: brainstorm it with the owner, then write a spec and a plan as usual. Below are the constraints from the code, a recommended shape and the open questions.

## Why it fits
- Every model call goes through `inference::LlmBackend::Complete` (`src/inference/Llm.h`), and the engine never knows what is behind the interface.
- `ScriptedBackend` (tests) and `turn::RecordingBackend` (`src/turn/Replay.h`) already stand in for, or wrap, the real model.
- The call sites are `Session.cpp:297` (lookup), `:349` (resolve), `:383` (narrate), `Folding.cpp:50` (summarize) and `:94` (condense).
- `turn::CallKindOf(prompt, spec)` already tells the calls apart (lookup, resolve, narrate, summarize), so no interface change is needed to label them. Summarize and condense share `CallKind::Summarize`; the prompt's last message tells them apart if the window wants to.

## Recommended shape
- **A wrapper, not a replacement.** A `HumanSeatBackend` wraps the real backend, built the same way `RecordingBackend` is. A human can only do `Complete`:
  - `CountTokens`, `CountPromptTokens` and `ContextSize` pass through. The budget needs the real tokenizer; the spec rules out estimates.
  - `Embed` and `EmbeddingModelId` pass through: retrieval needs real vectors.
  - `SaveState` and `RestoreState` pass through.
  - `Complete` asks the human, or, depending on a per-call-kind setting, forwards to the real model, or forwards and offers the model's reply as an editable draft.
- **Tokenizer-only chat model, optional.** `LlamaCppBackend` loads the chat model with `llama_model_default_params()` (`LlamaCppBackend.cpp:146`). A config flag setting `vocab_only` would load only the tokenizer, so a fully human session needs neither the chat weights nor a GPU.
  - In that mode, forwarding `Complete` to the real model must be impossible. Treat that as a contract failure, not a fallback.
- **Thread handoff.**
  - `Complete` runs on the worker thread; the window is on the UI thread.
  - The wrapper publishes one pending request (call kind, prompt, `GenerationSpec`, the optional model draft) under a mutex.
  - It waits on a `std::condition_variable_any` with the call's `stop_token`, so Cancel wakes it and returns a completion with `cancelled` set. A cancelled turn must not hang.
  - The UI polls the pending request once per frame, like the other model state, and submits the reply.
  - Only one request is ever pending, because the worker runs one job at a time.
- **The window ("Model seat").** It shows:
  - the call kind;
  - the prompt, split by layer as the Context window does (reuse its rendering);
  - `maxTokens` and `thinkingMaxTokens`;
  - the grammar text;
  - the model's draft, when there is one.
  The reply goes in a multi-line input; Submit and Cancel buttons; an optional thinking text field when `thinkingMaxTokens > 0`.
- **The completion.**
  - `text` is the reply.
  - `thinking` is the optional thinking text.
  - Token counts come from the real tokenizer (`promptTokens` from `CountPromptTokens`, `tokensGenerated` from `CountTokens`); `tokensReused` is 0.
  - `onToken` is called once with the whole reply, so the Play window shows it.
  - `stoppedAtLimit` is set and the text is cut at `maxTokens` tokens when the human writes too much, as a model would be cut off.
- **Grammar.** A human reply is not grammar-constrained. Let it through so the engine's handling of model mistakes is visible: unreadable resolve output means nothing proposed, and invalid changes are rejected with a reason. Optionally, a "Check against grammar" button can run llama.cpp's grammar matcher over the reply's tokens without sampling. That needs the vocab, which the wrapper has.
- **Turning it on.** A command-line flag, `morpheus --human-seat`, and/or a Settings switch. The per-call-kind choice (human, model, or model draft) lives in the window, so it can change between calls.
- **Recording.** With `RecordingBackend` around it, human replies become replay fixtures (`morpheus_play --record` already writes them). That is a way to hand-craft regression fixtures for specific model mistakes. Check that the app can record, or note it as a follow-up.

## Constraints
- **Module boundaries.** The wrapper uses only `inference` (and `turn` for `CallKindOf`), so it fits in `src/app/model` or `src/turn`. The window goes in `src/app/ui` and must draw headless in tests like the other windows.
- **One pending request, no queue.** Maintenance (summarize/condense) runs between turns and is preempted by new commands. A human-seat request for maintenance must still wake on cancel/preempt (the same `stop_token`).
- **No change to `core`, `context` or `turn` behavior.** The engine must not know a human is answering.

## Open questions for the owner
1. Should it be available in the normal app behind a flag, or in a separate debug build or target?
2. Should "model draft" be the default for each call kind, or a fully human reply?
3. Is the tokenizer-only (no GPU) mode wanted from the start, or later?
4. Should the grammar check button be in the first version?
5. Should the terminal player (`morpheus_play`) get the seat too? It reads stdin on the thread that waits for the turn, which makes this awkward, so the app is recommended first.

## Tests (write first)
- **Pass-through:** `HumanSeatBackend` over `ScriptedBackend` forwards CountTokens, Embed and the rest unchanged.
- **Human calls:** `Complete` for a human-assigned kind publishes the request with the right call kind, prompt and spec, and returns the submitted text with real token counts.
- **Model calls:** a model-assigned kind forwards to the inner backend with no request published. A draft kind publishes the inner reply as the draft.
- **Cancellation:** a pending request wakes on `stop` and returns `cancelled`. The worker closes the command with `TurnCancelled`.
- **Length limit:** a reply over `maxTokens` is cut with `stoppedAtLimit` set.
- **Engine reaction:** an invalid resolve reply typed by the human gives a turn with nothing proposed, as for a model mistake.
- **UI, headless:** the window draws a pending request and submits it.
