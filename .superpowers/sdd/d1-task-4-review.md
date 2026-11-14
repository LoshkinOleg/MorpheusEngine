# Task 4 review: turn::HostedWorld (5d161f9..ead0f4d)

### Spec Compliance
- Spec compliant. The seven files are byte-identical to the brief's blocks. Check run: each code block was pulled out of d1-task-4-brief.md and compared with `diff -q` against src/turn/HostedWorld.{h,cpp}, tests/hosted/HostedTestSupport.h, tests/hosted/HostedWorldTests.cpp and tests/hosted/fixtures/{open,observation}.json. All six match, and the CMake hunk is the brief's one line (src/turn/CMakeLists.txt:3).
- Spec section 1 (HostedWorld):
  - The constructor checks the catalog, the observation schema and the lore, and throws a DataError naming the pointer (HostedWorld.cpp:215-416).
  - Observe checks the core, then grammar::Validate, collects every fault and joins them with "; " (HostedWorld.cpp:432-444, ReadActors 46-113).
  - ProposalGrammar is FromSchema of ProposalSchema (HostedWorld.cpp:540-544).
  - Adjudicate parses, validates and returns requested or refused with reasons (HostedWorld.cpp:546-568).
- Spec section 3 (the catalog):
  - Ids are non-empty and unique; params may be absent; there are four parameter types, each with its own allowed keys (HostedWorld.cpp:236-391).
  - The proposal is a oneOf in catalog order, discriminated by "action" (the first key of `required`, as FromSchema requires: Schema.cpp:171-178).
  - Every parameter is required, and there is no additionalProperties, so extra keys are closed.
  - An actor enum skips the speaker (HostedWorld.cpp:489) and is deduplicated (HostedWorld.cpp:496).
  - An action with an empty actor enum is dropped before FromSchema sees it (HostedWorld.cpp:516-527).
  - No allowed action left is a DataError, as spec section 2 step 0 asks (HostedWorld.cpp:533-536).
- FromSchema subset and limits:
  - Every branch shape the generator emits stays in FromSchema's grammar subset (Schema.cpp:155-388):
    - an object with `properties` and `required` and no extra keys; an empty params object becomes the literal `{}` (Schema.cpp:402-404);
    - a string `const` discriminator;
    - a string enum that is non-empty and free of duplicates, because empties are dropped and the `taken` set deduplicates;
    - a string with `maxLength` from 0 to GBNF_MAX_STRING_CHARS, checked at open (HostedWorld.cpp:322-343);
    - an integer with both bounds, the same full long long range as Schema.cpp:243-249;
    - a catalog enum checked non-empty, all strings, no duplicates (HostedWorld.cpp:366-385).
  - So a catalog that passes open always yields a schema FromSchema accepts.
- Spec section 4 / R5 section 5:
  - The observation schema goes through grammar::CheckValidationSchema at open, and its DataError is re-prefixed "observation_schema: " (HostedWorld.cpp:393-398).
  - CheckEvents requires an object with a string `text` (HostedWorld.cpp:456-466).
  - The fixtures follow sections 5.1 and 5.2 (Saadia moved from `nearby` into `actors`, as the brief says).
- No Skyrim knowledge in engine code. Check run: read every line of HostedWorld.h and HostedWorld.cpp in the diff; the only host-flavoured words are the generic "mod update" (HostedWorld.h, HostDataHash comment). Host names appear only in tests/ and the fixtures.
- Spec "Tests" list for HostedWorld: every item is covered:
  - section 3 schema plus grammar accept and refuse: HostedWorldTests.cpp, test 2;
  - each scope: test 3;
  - left out, and allowedActions: test 4;
  - each core fault with its pointer: test 9;
  - schema at open with an unsupported keyword: test 6;
  - Adjudicate's accept, intent, unknown action, absent target and unparseable cases: test 10.
- Cannot verify from diff:
  - The reported totals (464 = 453 + 11; the brief expected 457 = 446 + 11). The difference comes from earlier tasks' counts.
  - The RED/GREEN runs. Neither was re-run.

### Strengths
- One parser reads the core for both Observe and ActorsOf (ReadActors, two callers, HostedWorld.cpp:46). A stored observation therefore reads with exactly the rules a live one passed.
- Observe collects every fault of both the core and the host schema rather than stopping at the first. The test at HostedWorldTests.cpp (test 9, "Every fault is reported, joined in order") pins the ordering.
- Catalog checking happens at open and is strictly narrower than FromSchema's grammar subset. A host mistake therefore surfaces at open with a catalog pointer, never as a FromSchema "schema at /oneOf/3/..." error mid-turn.
- The speaker exclusion and dedup sit in one lambda (HostedWorld.cpp:485-501), and the tests exercise the case where the speaker would otherwise be in every scope (Saadia speaking, test 3).
- Test 2 checks the grammar against hand-written expectations: the compact form only, the speaker as target refused, an extra `intent` key refused, a 49-char item refused. It goes beyond equality with FromSchema.

### Issues

#### Critical (Must Fix)
None.

#### Important (Should Fix)
1. **Adjudicate can throw on model text (plan-mandated).** HostedWorld.cpp:553 catches only `nlohmann::json::parse_error`.
   - nlohmann reports a float overflow such as `{"action":"wait","params":{},"x":1e999}` as `out_of_range` 406, which it rethrows unchanged. Check run: build/asan/_deps/nlohmann-src/include/nlohmann/detail/input/parser.hpp:268-272 passes `out_of_range::create(406, ...)` to `sax->parse_error`, and json_sax.hpp:325-333 does `JSON_THROW(ex)` with that type.
   - Such text escapes Adjudicate as an exception. This breaks the header's "Never throws for the model's text" (HostedWorld.h, Adjudicate comment) and the spec's "Model mistakes are not exceptions".
   - The proposal grammar admits no exponent, so a backend that applies the grammar cannot produce it. But Adjudicate is a public operation over arbitrary text, and its contract is unconditional.
   - Fix: catch `nlohmann::json::exception` (as LlamaServerBackend.cpp:110 and Inspect.cpp do), and add a test case with an overflowing number.

#### Minor (Nice to Have)
1. **PointerToken is duplicated (plan-mandated).** HostedWorld.cpp:22-36 is a byte-identical copy, comment included, of grammar/Schema.cpp:63-77. It is small and pure, so not a merge blocker, but it is a candidate to hoist into a shared header (for example core) during the whole-branch pass.
2. **The "no allowed action" message is wrong for an empty list (plan-mandated).** HostedWorld.cpp:533-535 says "each needs an actor its scope does not offer". An explicitly empty `allowed` list (a request whose `allowedActions` is present but `[]`) gets the same message, which names the wrong cause.
3. **The commit trailer does not match the global constraint.** Commit ead0f4d ends with `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`; d1-global-constraints.md ("Commits") requires `Claude Opus 5.5`. The report flags this, yet also says "Deviations: none".
4. **The reported build output has warnings.** The asan run shows LNK4300 ("the usual"). It predates this task and is not caused by this change, but it is noise in the reported output.
5. **One coverage gap.** A valid `minLength` reaching the proposal schema (`param.schema["minLength"]`, HostedWorld.cpp:340) is never asserted; only its refusal is (HostedWorldTests.cpp, test 5).

### Assessment
**Task quality:** Needs fixes
**Reasoning:** The implementation matches the brief byte for byte and meets spec sections 1, 3 and 4. The generated schema provably stays inside FromSchema's subset. However, Adjudicate's catch misses nlohmann's `out_of_range` on numeric overflow, so a model mistake can surface as an exception, against the documented contract. The fix is one line plus one test.

## Re-review 1 (ead0f4d..2e27b2d)

### Spec Compliance
- Spec compliant.
- The Important is resolved. `Adjudicate` now catches `nlohmann::json::exception` (src/turn/HostedWorld.cpp:540), which includes `out_of_range` 406. New test at tests/hosted/HostedWorldTests.cpp:424: `CHECK_NOTHROW` on `1e999`, then refused with one "the output is not valid JSON: " reason. The report's RED run (1e999 threw) shows that the test drives the fix.
- Minor 1 is resolved: `PointerToken` now has one definition.
  - It is declared in src/grammar/Schema.h:69 and defined at src/grammar/Schema.cpp:636, outside the anonymous namespace.
  - Its body is byte-identical to the removed copies (compared in the diff).
  - Grammar's own call sites (Schema.cpp Compile and CheckValue, all inside the anonymous namespace before line 636) still resolve to it. Schema.cpp:1 includes grammar/Schema.h, so the enclosing namespace's declaration is visible there.
  - The behaviour is therefore unchanged. The report says `[grammar]` passes.
  - HostedWorld.cpp's copy is removed, and all six call sites use `grammar::PointerToken`. turn already depends on grammar, so no new dependency appears.
- Minor 2 is resolved: an empty `allowed` list throws `DataError "allowed_actions: the list names no action"` before the per-id checks (src/turn/HostedWorld.cpp:455-457), tested at HostedWorldTests.cpp:436-440. The scope-emptied case keeps its original message and test.
- Minor 5 is resolved: a valid `minLength` is asserted to reach the proposal schema (HostedWorldTests.cpp:442-447).
- Minors 3 and 4 were closed by controller decision (the owner's trailer rule; pre-existing LNK4300). I make no finding on them.
- Cannot verify from diff: the reported totals (466 = 464 + 2) and the GREEN runs. Neither was re-run.

### Issues
#### Critical (Must Fix)
None.
#### Important (Should Fix)
None.
#### Minor (Nice to Have)
1. **A comment is now incomplete.** The inline comment at src/turn/HostedWorld.cpp:536 still gives lone surrogate escapes as the only reason a parse can fail. The catch now also covers numbers the parser cannot hold, so the comment should mention that case.

### Assessment
**Task quality:** Approved
**Reasoning:** Each fix is minimal and correct and has a test. The PointerToken move keeps grammar's behaviour byte for byte, and the fix introduces nothing new beyond one slightly stale comment.
