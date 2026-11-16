# G2 final review: generation controls (198171a..d2793b5)

Reviewer: read-only final review of the whole range against the spec
(`docs/superpowers/specs/2026-11-16-generation-controls-design.md`), the plan
(`docs/superpowers/plans/2026-11-16-generation-controls.md`) and CLAUDE.md. No builds or tests run (the controller
runs the live suites).

## Strengths

- Plan alignment: all three parts land as specified, with the exact error texts of the Global Constraints
  (`min_p`, `CallKindOf`, `SeatLabelOf`, every `thinking_format` fault, the in-process single-token `end` error).
  New struct fields go last (`SamplingSettings::minP`, `Sampling::minP`), so the existing positional initializers
  still compile.
- Settings compatibility: `min_p` and `thinking_format` are optional and default to today's behaviour (0, Auto), so
  existing settings.json files load unchanged. `checkKeys` still rejects typos, `SettingsToJson` writes both keys,
  and the settings editor patches the file's own JSON, so it keeps `thinking_format` and every other key it does not
  edit. `block.value("min_p", 0.0)` in `ReadSettingsEdit` matches the loader's optional-with-default rule.
- The call tag removes a fragile heuristic. The grammar first-line sniffing, the prompt-suffix checks in `CallKindOf`
  and the RENARRATE_CORRECTION / SUMMARIZE / CONDENSE instruction checks in `SeatLabelOf` are all gone. So are the
  matching "root line comes first because CallKindOf..." comments in Gbnf.h and ResolveGrammar.{h,cpp}, and
  ModelSeat.cpp no longer includes turn headers. An untagged or foreign tag now fails loudly (ContractError), and the
  replay fixtures stay valid because `CallKindName` and the reply grouping (renarrate into narrate, condense into
  summarize) are unchanged.
- Every turn-layer `Complete` call is tagged: grep confirms Session (lookup, resolve, narrate/renarrate, review),
  HostedSession (decide, resolve, speak) and MemoryUpkeep (summarize, condense), and Folding.cpp only forwards
  MemoryUpkeep's spec. `SlotBackend` copies the spec, so the tag survives the server's slot rewrite.
  CallTagTests.cpp checks the exact order of an owned turn's tags, including a renarrate, plus fold and condense.
  The hosted tests check decide, resolve, speak, summarize and condense.
- Layering is respected. `ThinkingFormat`, `ThinkingSetting`, `QWEN_THINKING` and `ResolveThinking` live in core
  with no new dependencies. inference loses its Qwen constants and gains only a plain `std::string callTag`. The
  backends resolve the format once at load.
- Backend parity for Auto and qwen is good: both tokenize the texts the same way (special tokens parsed, `afterEnd`
  plain), use `close` for the budget cut and use `end` as the stop. The fix round's `templateWritesOpen` keeps
  llama-server from cutting a custom `open` text that only happens to match the end of a plain template, and it has
  a test.
- EngineHost reloads the backend on a thinking change (`ThinkingSetting::operator==`), with a test for both reload
  and keep. All four executables (the app, server, probe and play) pass `settings.thinking` to both backend kinds.
- Docs: the README covers `min_p`, every form of `thinking_format`, the load checks, the in-process single-token
  rule and the hosted-dialogue budget recommendation. settings.example.json and the architecture spec's thinking
  bullet are updated. The owner's "none" decision is written the same way in the spec, Thinking.h and the README.

## Issues

### Critical (Must Fix)

None.

### Important (Should Fix)

1. **A custom `close` text can turn a data fault into a ContractError on every thinking call.**
   Code: src/llama/LlamaCppBackend.cpp:249, src/llamaserver/LlamaServerBackend.cpp:436, together with
   src/core/Settings.cpp (the thinking_format block) and `core::MIN_THINKING_TOKENS`.
   - What happens: both backends `MORPHEUS_REQUIRE(thinkingMaxTokens > close.size())`. That was a sound engine
     contract while `close` was a constant of about 20 tokens. `MIN_THINKING_TOKENS` (64) exists, by its own comment
     in Settings.h, so that "the backend closes a block that runs out with a sentence of its own, which must fit
     inside the budget." `close` is now settings data with no length limit. A custom `close` of 64 or more tokens
     passes the settings load and the backend load, then every call with a thinking budget throws ContractError,
     an "engine bug".
   - Why it matters: CLAUDE.md wants malformed data to fail at load with a DataError or InferenceError that names it.
     Here it fails late, with the wrong error class, and in morpheus_server it fails once per turn.
   - Fix: at load, in both backends, after tokenizing `close`, throw InferenceError when
     `close.size() >= core::MIN_THINKING_TOKENS`, for example "the thinking format's close text is N tokens of
     '<model>'; it must be fewer than MIN_THINKING_TOKENS (64), the smallest thinking budget". Every non-zero budget
     is at least 64, so this restores the invariant, and the REQUIRE becomes a true contract again.
   - Tests: a fake-server test of the llama-server refusal, and a `[llama]` one beside the single-token test.

### Minor (Nice to Have)

1. **Stale architecture spec.** docs/superpowers/specs/2026-10-27-morpheus-cpp-architecture-design.md:134, the
   LlamaServerBackend bullet, which this range edited. It still says "(`samplers` top_k, top_p, temperature; every
   other server sampler neutral)". It should read top_k, top_p, min_p, temperature, with min_p carrying the
   setting. The settings bullet below it also does not mention `min_p` or `thinking_format`. The historical
   llama-server design spec (2026-11-15, line 181) still lists `min_p: 0` as neutral; leave it or add a "superseded
   by G2" note.
2. **README budget bullet uses the old definition.** README.md:58 still defines a thinking model as "one whose chat
   template has `enable_thinking`". Since G2 that is only the Auto rule: point to `thinking_format`.
3. **"none" on a model whose template has the switch: the backends can diverge.** src/llamaserver/LlamaServerBackend.cpp:650-660.
   - In-process renders plain chatml, so the prompt has no block at all. Over llama-server no `enable_thinking`
     kwarg is sent and nothing is cut, so a template that opens the block itself when thinking is not switched off
     ends the prompt with its own `<think>\n`. The spoken text then begins inside a thinking block.
   - The owner decided this case is documented, not guarded, and the docs say so.
   - Recommendation (owner's call, a follow-up): under ThinkingMode::None, refuse a template with the switch at load
     in both backends, with an InferenceError: "the chat template has a thinking switch; use thinking_format auto with
     the thinking budgets at 0". No setup needs "none" on such a model, and fail-fast closes the trap.
4. **Custom format on a template with the switch (llama-server only).** The backend sends `enable_thinking: true`,
   and `templateWritesOpen` is true. The cut only removes the custom `openText`, so a Qwen-style template's own
   `<think>\n` stays, followed by the custom skip or open text. In-process does not write it. This is niche (a
   custom format on a Qwen template); a one-line README note or the guard from item 3 would cover it.
5. **The enable_thinking check is written twice.** src/core/Thinking.cpp:12 and
   src/llamaserver/LlamaServerBackend.cpp:372. The two must agree (Auto resolves Qwen exactly when the template
   writes the open text). With two callers, a `core::HasThinkingSwitch(std::string_view)` in Thinking.h is justified
   under the helper rule. This is also in progress.md's Task 4 minors.
6. **Backend link interface.** src/llama/CMakeLists.txt and src/llamaserver/CMakeLists.txt link `morpheus_core`
   PRIVATE, but their public headers now include `core/Thinking.h` and expose `core::ThinkingSetting` in the config.
   It works today only because every module shares the `src` include directory. Make `morpheus_core` PUBLIC for
   both libraries, so the usage requirement is declared. The consumer check is unaffected: it links Morpheus::turn.
7. **Call tags are scattered string literals.** Session.cpp, HostedSession.cpp, MemoryUpkeep.cpp, Replay.cpp,
   ModelSeat.cpp and the tests each spell them. A typo in a hosted tag (decide, speak) is caught only by a test,
   because nothing reads those tags. Optional: `constexpr std::string_view` tag constants in a small turn header,
   used by the producers and the two readers (app already depends on turn). The spec chose a string, so this is
   only DRY.
8. **Misleading error for a non-string, non-object `thinking_format`.** src/core/Settings.cpp:235-247. A number or
   `null` reaches `checkKeys` and is reported as "thinking_format must be an object", although a string preset is
   just as valid. Fold that case into the preset message.
9. **The live thinking test loads a second model copy.** tests/llama/LlamaThinkingTests.cpp:74. The Format backend
   is loaded while the static Auto backend stays alive, and the single-token test at :140 loads a third, which
   throws. The comment says this is cheap for the 0.8B model, but the README allows Qwen3.8-27B as
   MORPHEUS_TEST_THINKING_MODEL, and two copies of that, with `gpuLayers = -1`, will not fit on a 10 GB GPU. Either
   load both backends locally one after the other, or name the 0.8B model as required for this test in the README.
10. **No test that the settings editor keeps `thinking_format`.** It is kept by construction (the editor patches the
    JSON), but a two-line check in SettingsEditTests (write `"thinking_format": "qwen"`, save an edit, re-read)
    would pin it.
11. **The empty-text guard covers only `skip`.** LlamaServerBackend.cpp guards an empty `skip` before `/tokenize`,
    but not an empty `afterEnd`, which the settings also allow. It is harmless (llama-server answers [] for ""), but
    the asymmetry reads as if it mattered: guard both or neither.

## Minor triage (the per-task minors from progress.md)

| Minor | Verdict |
|---|---|
| T1: SettingsEdit `samplingJson` line about 130 columns; a test comment about 122 columns | Fix before merge in one formatting commit, with the T3 and T4 long lines (trivial). |
| T1: the `min_p` bad-value test matches "not 1" as a prefix of "not 1.0", and there is no non-number case | Fix before merge: pin the exact rendering (`minP.dump()`, so expect "not 1.0") and add a string value (the `!is_number()` branch is untested). |
| T1: the SamplingFrom test is tagged [sampling], not [turn] | Can wait (or retag in the same cleanup; the old case beside it is [turn]). |
| T2: no RED run | Can wait (process note; the tests do exercise the new paths). |
| T2: some RecordingBackend test responders still branch on grammar | Can wait: fakes choosing a reply, not production sniffing. |
| T2: CallTagTests uses std::size_t without <cstddef> | Fix before merge (one include, it relies on a transitive include). |
| T3: the "none" budget test covers only narrate | Can wait (the loop is generic); adding resolve and review to the test is cheap if touched. |
| T3: no test that a custom object equal to QWEN writes "qwen" | Can wait: loading "qwen" produces Format plus QWEN_THINKING, so the preset round-trip already exercises that branch. |
| T3: Settings.h comment over 120 columns | Fix in the formatting commit. |
| T4: LlamaServerBackend.h:279 about 170 columns and :249 over 120 | Fix in the formatting commit. |
| T4: the enable_thinking check duplicated in Thinking.cpp and LlamaServerBackend.cpp | Fix before merge (Minor 5: small, and the two must agree). |
| T4: no RED runs | Can wait. |
| T4: the [llama] single-token end test assumes "[/T]" is several tokens (not run) | Can wait: the controller's live run settles it. If "[/T]" turns out to be one token, use a longer marker such as "[/THOUGHT]". |

## Recommendations

1. Fix Important 1: refuse a close text of MIN_THINKING_TOKENS or more tokens at load in both backends, with tests.
2. In one cleanup commit: the architecture spec sampler list and settings bullet (Minor 1), the README budget bullet
   (Minor 2), `core::HasThinkingSwitch` (Minor 5), PUBLIC core linkage (Minor 6), the min_p test tightening and
   <cstddef>, and the long-line wraps.
3. Raise the "none" guard (Minor 3) with the owner as a follow-up; it would also cover Minor 4's worst case.
4. Live coverage the controller should confirm, since only live tests reach these paths:
   - the in-process `llama_sampler_init_min_p` with a non-zero value (only `[llama][hosted]` uses HostedSettings'
     0.05);
   - the in-process Format-qwen thinking run and the single-token refusal;
   - llama-server Format-qwen against a real Qwen3.5+ template;
   - the replay of every fixture (fast suite, already green at 705/705).

## Assessment

**Ready to merge?** With fixes

**Reasoning:** The range implements the spec faithfully, keeps settings files, the replay fixtures and the owned
game's behaviour unchanged, and replaces the grammar and prompt sniffing with an explicit, fail-loud tag. One real
gap remains before merge: a custom `close` text is unbounded settings data that still hits an engine-contract check
on every thinking call. The rest is docs and cleanup.
