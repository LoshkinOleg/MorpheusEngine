# MorpheusEngine (C++) - Narrative Visualization

Date: 2026-10-27. Status: approved design, pre-implementation. Builds on `2026-10-27-morpheus-cpp-architecture-design.md`.

## Goal

The player selects any text and the engine renders an image of what it describes, keeping the result visually consistent across images.

Details an accepted image adds flow back into world state. They then reach both the narration and future images. For example: the player character comes out blond, so later prose and later portraits keep the blond hair.

## Constraints

- **Hardware.** The target machine is an RTX 3070 Laptop GPU (8 GB VRAM) with 32 GB RAM. The chat LLM and the image model cannot both be on the GPU at once, so they are swapped. A larger GPU only removes the swap (`keep_both_resident`).
- **World state is canon.** Images follow state. An image changes state only when the player accepts it, and canon wins any conflict.
- **No third model on this machine.** The chat model is multimodal and also serves as the vision model.

## Phase 0: experiments (before engine code)

The experiments run in ComfyUI, and the results are recorded in `docs/experiments/`.

1. **Base model on 8 GB.** Candidates include SDXL and its distilled fast variants, and quantized Flux-class models. Measure quality, seconds per image and VRAM.
2. **Consistency technique.** Put one character in five scenes using:
   - text plus a fixed seed;
   - a reference adapter (IP-Adapter or similar);
   - a reference-based editing model (Kontext-style).
3. **Vision model.** Run 2-3 multimodal GGUF chat models through llama.cpp's multimodal CLI on the generated images. Check whether they reliably extract hair, eyes, clothing and marks.
4. **Swap cost on this laptop.** Measure the full cycle: save the LLM's KV cache, unload the LLM, load the image model, render, unload it, reload the LLM, restore the cache.

**Exit criteria:** a chosen image model, consistency technique and multimodal chat model, each with measured latency. These choices decide which image backend is built first.

## Image backend

```
ImageBackend
  render(ImageRequest{prompt, negative, width, height, seed, steps, references[]})
      -> ImageResult{png, seed, params, timings}
```

- **`SdCppBackend` (preferred).** stable-diffusion.cpp is built on ggml like llama.cpp. It runs in-process and is pulled in through FetchContent at a pinned commit.
- **`ComfyUiBackend` (fallback).** Used if the winning technique only exists in the Python ecosystem. It drives a local ComfyUI over its HTTP API, using a stored workflow file. This is the only external process in the system, it serves an optional feature, and it sits behind this one interface.

New module `visual` depends on `core`, `inference`, `store` and `context`. `turn` and `app` depend on it.

## GPU residency

A residency manager on the engine worker thread keeps only one heavy model on the GPU at a time. An image request runs:

1. `LlmBackend::save_state()` copies the KV cache into RAM.
2. Free the LLM.
3. Load the image model and render 2-4 drafts in one batch, spreading the swap cost across them.
4. Free the image model.
5. Reload the LLM and call `restore_state()`.

Commands that arrive during a swap are queued.

## Visualize flow

The flow is triggered from the Play window: select text, then **Visualize**. It runs as one job on the worker thread.

1. **Interpret (LLM, grammar).** Input: the selection, its surrounding turn, and the state view. Output: `{subjects[], setting, framing: portrait|scene|object, action, lighting}`. Subjects are existing entity ids or `new:<type>`. A `new:` subject becomes a draft `create` change, allowed only for creatable entity types.
2. **Gap-fill (LLM, grammar).** For each subject, any attribute in its type's `visual_attributes` that has no value yet becomes a draft `set` change on `<entity>.appearance.<attr>`.
3. **Build the prompt (deterministic, no model).** The prompt combines:
   - `art_style.prompt`
   - the framing
   - each subject's appearance (canon plus drafts)
   - the setting, action and lighting

   The negative prompt is `art_style.negative`. Each subject's accepted reference image, and the location's if it has one, goes into `references[]`.
4. **Render** the drafts, with the residency swap.
5. **Player decision.** Accept one, regenerate, or discard. Discarded drafts are deleted and leave no trace.
6. **Reconcile on accept (multimodal LLM, grammar).** Input: the accepted image and the spec it was built from. Output: `{new_details[{entity, attribute, value}], contradictions[{entity, attribute, canon, seen}]}`.
   - New details become draft `set` changes.
   - If there are contradictions, canon wins. The player chooses between:
     - **accept anyway:** the image is kept, but is not used as a reference, and no contradicting values are written;
     - **cancel.**
7. **Commit (one transaction).**
   - All drafts are validated against the schema, as turn changes are; rejected ones are logged.
   - Valid drafts are applied to state.
   - The image becomes the reference for its subjects.
   - A compact line is appended to the L3 transcript, for example `[visualized: player portrait; blond hair, scar over left eye]`.

   The narrator then sees the new appearance in the next turn's state view (T1) and in the transcript. The L3 append keeps the KV cache prefix intact.

**Undo.** Accepted visual changes are attached to the latest turn: its state snapshot is rewritten and the image row records the changes. Undoing that turn also reverts them and removes the reference links.

**Error policy.** It is the same as the turn flow. Model mistakes (an unknown attribute, an invalid change) are rejected and shown in the UI. Backend failures (out of memory, a missing model file) surface as errors, and the LLM is always reloaded and its cache restored.

## Data model

**State schema additions:**
- A new field type, `map<string,string>`.
- Entity types may declare `appearance: map<string,string>` and `visual_attributes: [hair, eyes, build, age, clothing, marks, ...]`.

**`world.json` additions:** `art_style {prompt, negative}`, `image_size`.

**New tables:**

| Table | Contents |
|---|---|
| `images` | id, turn, selected text, spec JSON, final prompt, negative, seed, backend / model / params, file path, status (draft / accepted / discarded), reconciliation JSON, applied changes |
| `references` | entity id, image id, since turn |

Image files are stored in `saves/<id>/<run>/images/<image id>.png`.

## UI

- **Play window:** a "Visualize" action in the context menu of any selected text.
- **Visual window (new):**
  - the grid of drafts, with accept / regenerate / discard;
  - the interpreted spec and the final prompt;
  - the reconciliation result: new details and contradictions;
  - timings, including the swap;
  - a gallery of accepted images.
- **State window:** a reference-image thumbnail next to each entity that has one.

## Testing

- **Prompt builder:** unit tests, since it is deterministic.
- **Interpret, gap-fill, reconcile and commit:** run through `ScriptedBackend` plus a fake `ImageBackend` that returns a fixed PNG. Tests assert on state, the `images` and `references` rows, the L3 line, and undo.
- **Residency manager:** tests with fake backends check the load/unload order, that the KV cache is saved and restored, and that the LLM is restored after a backend failure.

## Not in scope

- Per-character LoRA training
- Automatic visualization of every turn
- Manual image editing or inpainting
- Video
- Pushing images to a cloud API
