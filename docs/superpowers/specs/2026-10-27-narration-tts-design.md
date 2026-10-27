# MorpheusEngine (C++) - Narration Text-to-Speech

Date: 2026-10-27. Status: approved design, pre-implementation. Builds on `2026-10-27-morpheus-cpp-architecture-design.md`.

## Goal

Narration can optionally be read aloud by one narrator voice per world. Speech starts while the narration is still streaming.

## Constraints

- **CPU only.** The GPU belongs to the LLM, or to the image model during a swap. TTS never takes part in GPU residency.
- **Off by default.** Enabling TTS changes nothing else in the engine.

## Backend

```
TtsBackend
  synthesize(text, voice, speed) -> pcm (float samples + sample rate)
```

- **`SherpaOnnxBackend`.** Uses sherpa-onnx through its C/C++ API (prebuilt Windows libraries), pinned to a fixed version. It runs Kokoro, Piper/VITS and other ONNX TTS models, so a voice is a model file plus a speaker id chosen in settings.
- **CPU threads.** Synthesis is limited to 2 CPU threads (configurable), so it does not starve LLM layers offloaded to the CPU.

**Experiment (alongside visualization phase 0).** Compare a few Kokoro and Piper voices on sandcrawler narration. Measure the real-time factor on the Ryzen 7 5800H while the LLM generates with partial CPU offload. Pick the default voice from the results.

## Pipeline

The new module `speech` runs on its own thread, and `app` wires it to the engine's narration events.

1. **Chunk.** `TokenStreamed` text builds up in a sentence chunker. Each completed sentence is queued for synthesis right away. On `TurnCompleted` the remainder is flushed. The chunker handles abbreviations, ellipses, quotes, dialogue punctuation and numbers.
2. **Clean.** Only narration prose is spoken. Mechanics lines, `[visualized: ...]` lines and markdown markup are stripped.
3. **Synthesize and play.** PCM chunks are pushed in order to an SDL3 audio stream. SDL3 is already a dependency.

**Cancellation.** A new turn, undo, regenerate or Stop clears both the synthesis and playback queues. Audio from an undone or regenerated turn never plays.

## Controls

- **Settings:**
  - `tts_enabled` (default false)
  - voice (model + speaker)
  - speed
  - volume
  - CPU threads
- **`world.json`:** an optional `narrator_voice` sets the default voice for that world.
- **Play window:**
  - a Stop button;
  - **Read aloud** in the context menu of selected text, which uses the same pipeline without streaming.

## Testing

- **Chunker unit tests:** abbreviations, ellipses, quoted dialogue, numbers, text arriving token by token.
- **Queue tests with a fake `TtsBackend`:** chunks play in order, a cancel flushes both queues, and nothing is synthesized while TTS is disabled.

## Not in scope

- Per-character voices (a later extension could store a `voice` on entities and have the narration tag who is speaking)
- Voice cloning
- Speech input
- Exporting audio files
