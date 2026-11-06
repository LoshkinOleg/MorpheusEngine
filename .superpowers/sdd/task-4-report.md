# Task 4 report: backend hardening

Commit: e5384f1 "Tokenize message contents as plain text, route llama logs, add SamplingFrom"

## Implemented
- inference::SamplingFrom (Llm.h/.cpp; Llm.h now includes core/Settings.h).
- llama::LogLevel, LogSink, SetLogSink; LogFilter replaced by routed LogCallback (LogRoute with mutex, sink, lastLevel); default (no sink) still writes warn/error to stderr.
- TokenizePrompt split tokenization: template rendered with U+001E markers, template text parsed with special tokens, contents as plain text; U+001E in content throws InferenceError. Header comment updated.
- tools/probe/main.cpp uses SamplingFrom(settings.narrateSampling, 1234).
- Tests: tests/inference/SamplingTests.cpp, tests/llama/LlamaHardeningTests.cpp (verbatim from brief).

## TDD evidence
RED: `cmake --build --preset tests` -> SamplingTests.cpp(11,53): error C2039: 'SamplingFrom': is not a member of 'morpheus::inference'; C3861 identifier not found.
GREEN: `cmake --build --preset tests && morpheus_tests.exe "[inference]"` -> All tests passed (13 assertions in 3 test cases).
Dev: `cmake --build --preset dev` clean (/W4 /WX). With model env exported: `morpheus_tests.exe "[llama]"` -> All tests passed (64 assertions in 18 test cases), including the 3 new cases (9 assertions) and the existing prefix-reuse, grammar, save/restore cases.
ctest --preset tests: 100% passed out of 115. ctest --preset dev (with model env): 100% passed out of 133.

## Probe
prompt: 445 tokens (engine rules 16, world 274, tail 142; transcript 0 of 12246 budget); fits: yes
[prompt 445 tokens: 0 reused, 445 evaluated in 136 ms; 120 generated in 5423 ms]
[prompt 445 tokens: 444 reused, 1 evaluated in 82 ms; 125 generated in 2589 ms]

## Files changed
src/inference/Llm.h, Llm.cpp; src/llama/LlamaCppBackend.h, LlamaCppBackend.cpp; tools/probe/main.cpp; tests/inference/SamplingTests.cpp; tests/llama/LlamaHardeningTests.cpp.

## Deviations
None. CONTENT_MARKER was placed together with the log types at the top of the anonymous namespace (constants first).

## Concerns
None. Git prints LF->CRLF warnings on the touched files (repo autocrlf), harmless.

## Fix round 1

Changes:
- src/llama/LlamaCppBackend.cpp: `CONTENT_MARKER` is spelled `'\x1E'` (it held a raw 0x1E byte). No other raw control bytes were found in the three changed files.
- src/llama/LlamaCppBackend.cpp: the `TokenizePrompt` comment gained the sentence on chat templates that copy each content verbatim.
- src/llama/LlamaCppBackend.h: the `LogSink` comment gained "The sink must not throw: it is called from llama.cpp's C code."
- tests/llama/LlamaHardeningTests.cpp: the log sink test declares `lines` first, then a block that installs the sink and constructs a local `SinkGuard` right after it. The guard's destructor calls `llama::SetLogSink({})`. The manual reset was removed; the guard ends before the final CHECK.

Verification:

    $ grep -n "CONTENT_MARKER = " src/llama/LlamaCppBackend.cpp | od -c
    0000000   3   0   :   c   o   n   s   t   e   x   p   r       c   h   a
    0000020   r       C   O   N   T   E   N   T   _   M   A   R   K   E   R
    0000040       =       '   \   x   1   E   '   ;       /   /       A   S
    0000060   C   I   I       r   e   c   o   r   d       s   e   p   a   r
    0000100   a   t   o   r   .  \n
    0000106

    $ LC_ALL=C grep -n $'[\001-\010\013\014\016-\037]' src/llama/LlamaCppBackend.cpp src/llama/LlamaCppBackend.h tests/llama/LlamaHardeningTests.cpp
    (no matches; before the fix the only match was line 30 of LlamaCppBackend.cpp)

`[llama]` result (model variables exported in the same command): All tests passed (64 assertions in 18 test cases).

Full suites: `ctest --preset dev` 133/133 passed; `ctest --preset tests` 115/115 passed.

Commit: c36b92c "Spell the content marker as an escape and harden the log sink test".

Concerns: none. Git prints an LF-to-CRLF warning because core.autocrlf is true. The index and working copy are both LF, so the commit keeps LF.
