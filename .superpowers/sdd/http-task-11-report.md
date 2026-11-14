# Task 11 report: documentation and full verification

Status: DONE. Commit ef8cece "Document model access over HTTP: the backend split, grammar::Check, LlamaServerBackend, the settings keys".

## What was done
- Architecture spec: all 17 replacements of the brief. The `LlamaServerBackend` bullet also describes the controller's later behaviour: URLs validated at construction (plain http://, IPv6 literal hosts refused), one kept-alive connection per server for non-streaming endpoints, a dedicated kept connection for /completion, one retry of a non-streaming request or an unanswered /completion over a stale socket (llama-server closes idle keep-alive connections after 5 s).
- README: the brief's items 1-4, 6, 7 (items 5 and the `src/llamaserver` layout line were already present from earlier tasks). Added the sentence on why `127.0.0.1` not `localhost` (IPv6 first on Windows, about 2 s per new connection), a Connections bullet (kept connections, retry), and "no IPv6 literal host; a bad URL fails at startup naming it" in the `backend` bullet. All URLs and command lines use 127.0.0.1; the only `localhost` is in that explanatory sentence.
- CLAUDE.md: items 2, 3, 4 (items 1 and 5 already present).
- consumer-check CMakeLists: already matched the brief (earlier task); no change.
- Stale spots: spec had `GbnfMatcher.h`, backend `CheckGrammar` in the model seat sentence and `check_grammar` in the interface (all fixed by the brief's items). Code: `src/app/model/ModelSeat.h` `_pBackend` comment now says "for Check's token count". `Check.cpp` and `LlamaCppBackend` comments about `CheckGrammar` are correct (it remains a plain method) and were left. Historic plans and other specs untouched.

## Verification
- Byte checks (`LC_ALL=C grep -n $'[\x80-\xFF]'` and the control-byte count) on the spec, README, CLAUDE.md, consumer-check CMakeLists and ModelSeat.h: nothing printed / 0. The docs contain no backslash-u or backslash-x escapes of mine.
- Grep for `GbnfMatcher|check_grammar|CheckGrammar` over spec, README, CLAUDE.md: only the intended `LlamaCppBackend::CheckGrammar` bullet in the spec.
- Consumer check: configures, builds, `consumer_check: ok (rolled 15, total 18)`.
- `ctest --preset tests`: 100% passed out of 426; the 3 `[server]` tests skipped. No build warnings.
- `ctest --preset asan`: 100% passed out of 426; same 3 skipped; no ASan report (only the usual LNK4300 /INCREMENTAL linker notice).
- `build/dev` RelWithDebInfo: morpheus, morpheus_tests, morpheus_play, morpheus_probe all build, no warnings.
- `morpheus_tests "[llama]"` with Qwen3-4B-Instruct-2507 and nomic embed: 27 passed, 6 skipped (thinking/server variables unset), 5000 assertions passed.
- `morpheus.exe --frames 3`: exit 0.

## Deviations / concerns
- The edit script initially duplicated two sentences in the spec (replacements whose old text was a prefix of the new); I removed the duplicates and restored a blank line before checking the diff; final diff reviewed.
- The README's "Prompt-cache reuse" section still says "Not measured yet" as the brief specifies (the owner records the figures).
- Left `.superpowers/sdd/progress.md` (modified before I started) and docs/references unstaged; settings.json, imgui.ini, models untouched. Nothing left running; build/llama-server not touched.
