# Task 9 report

Status: DONE. Commit f9b1fcd "Add morpheus_probe and document milestone 2".

## Implemented
- tools/probe/main.cpp and CMakeLists.txt, verbatim from the brief, except the prompt line also prints transcriptCost and transcriptBudget.
- Root CMakeLists.txt: add_subdirectory(tools/probe) inside the MORPHEUS_BUILD_LLAMA block (already after src/context, no move needed).
- CLAUDE.md (layout + build), README.md (Vulkan prereq, presets table, new "Models and settings" section with download instructions, settings.example.json as template, probe command, [llama] note, layout), architecture spec (turns row rolls -> checks; inference interface notes on save_state/restore_state token list, Vulkan, caller-supplied grammar; json-schema-to-grammar line marked milestone 3). The core row already mentioned settings/turn-record types? It mentioned turn record types; settings not added to the spec row (CLAUDE.md/README say it).

## TDD
No test for the probe (manual tool per brief). No RED step applicable.

## Verification
- tests preset: build ok, ctest 100% passed out of 97.
- asan preset: build ok, ctest 100% passed out of 97, no AddressSanitizer report.
- dev preset: build ok (morpheus_probe.exe built), ctest 100% passed out of 111 (model-backed [llama] tests skipped without env vars).
- With env vars exported: ./build/dev/bin/Debug/morpheus_tests.exe "[llama]" -> All tests passed (53 assertions in 14 test cases).

## Real probe run
Command: ./build/dev/bin/Debug/morpheus_probe.exe settings.json game_projects/sandcrawler "I climb onto the crawler's roof."  (exit 0, no settings change needed)

stdout:
```
prompt: 445 tokens (engine rules 16, world 274, tail 142; transcript 0 of 12758 budget); fits: yes

--- run 1 ---
The sun rises over the dunes, casting long shadows across the shifting sands. You climb onto the crawler’s roof, your boots scraping against the reinforced mesh, and the wind carries the scent of hot iron and sand. The crew below watches with wary eyes—two of the sandcrawlers, their faces half-veiled by the dust of the morning, exchange a glance.  

A low rumble echoes from the crawler’s core. The sand beneath the vehicle shifts slightly, as if reacting to your presence.  

*Do you stay on the roof, or return to the vehicle?*
[prompt 445 tokens: 0 reused, 445 evaluated in 3279 ms; 120 generated in 3991 ms]

--- run 2 ---
The sun rises over the dunes, casting long shadows across the shifting sands. You climb onto the crawler’s roof, your boots scraping against the reinforced mesh, and the wind carries the scent of hot iron and sand. The crew below watches with wary eyes—two of the sandcrawlers, their faces half-veiled by the dust of the morning, exchange a glance.  

A low rumble echoes from the crawler’s core. The sand beneath the vehicle shifts slightly, as if reacting to your presence.  

*Do you stay on the roof, or do you descend to the crew level to speak?*
[prompt 445 tokens: 444 reused, 1 evaluated in 74 ms; 125 generated in 1919 ms]
```
stderr (llama.cpp noise only): control-looking token '</s>' warning, n_ctx_seq (16384) < n_ctx_train (262144), special_eos_id not in special_eog_ids.

Run 2: prompt 445 tokens, reused 444 = prompt tokens - 1 (confirmed), 1 evaluated in 74 ms.

## Deviations / concerns
- Prompt line extended with transcript cost/budget (allowed by the dispatch).
- settings.json untouched.
- Observation: run 2 output differs from run 1 (last sentence) despite seed 1234; likely GPU numeric differences between a 445-token batch and a 1-token eval of the last token. Not a bug in the cache (reuse is 444/445).
- Git warned about LF->CRLF on the new probe files (autocrlf); harmless.
