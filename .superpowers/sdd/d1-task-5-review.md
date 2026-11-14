# Task 5 review: hosted prompt layers and default hosted texts (2e27b2d..6edc53d)

### Spec Compliance
- Spec compliant. The diff matches the brief's code verbatim (PromptBuilder.h, PromptBuilder.cpp, EngineText.h, HostedPromptTests.cpp), apart from the one reported test deviation (tests/context/HostedPromptTests.cpp:150, `CHECK((... == ...))`).
- Owned builder output is byte-identical after the `LayOut` extraction. I compared the removed and added lines path by path:
  - System text: still `engineRules + "\n\n" + worldText + summaryText` (src/context/PromptBuilder.cpp:145).
  - Counts: tokens.engineRules, world and summary are counted by the caller before LayOut (PromptBuilder.cpp:134-136). tail, fixedTotal, systemCost, transcript and total are counted in LayOut with the same calls on the same strings (PromptBuilder.cpp:31-49).
  - Budgets: summaryOverBudget, tailOverBudget, transcriptBudget (the same expression, only re-wrapped), the "no room" DataError text, transcriptOverBudget and contextOverflow are unchanged.
  - Errors: the engine_rules_cap ContractError and world_cap DataError messages are unchanged (PromptBuilder.cpp:138-143).
  - Ordering: the cap checks now run before the tail and template counting rather than after it. The exceptions thrown and their precedence (rules, then world, then no room) are the same; only some backend calls before a throw are skipped. No observable change.
- Hosted layer order is L0, L1 lore, participants, L2, L3, then the tail (PromptBuilder.cpp:219 for the system message, LayOut's loop for L3, PromptBuilder.cpp:184-202 for the tail). This matches the spec's table, section 2 "The prompt". The tail order (retrieved, observation, events, player, instruction) also matches.
- Prefix stability: the prefix through L3 depends only on systemText, lore, participants, summaries and transcript. The tail's inputs never reach messages [0, stableCount). Tests pin this: "keeps its stable prefix" (HostedPromptTests.cpp:184-198) and "A join changes ... from the participants layer on" (HostedPromptTests.cpp:156-182).
- Budget checks follow the global constraints:
  - world_cap covers the lore and the participants together and throws a DataError naming world_cap (PromptBuilder.cpp:209, 214-217).
  - engine_rules_cap covers the system text and throws a ContractError (PromptBuilder.cpp:211-213).
  - The tail allowance and every flag come from the shared LayOut.
- Default texts are generic. HOSTED_SYSTEM_TEXT (src/turn/EngineText.h:76-85) names no game, place or person and contains no `{`, which the test checks at HostedPromptTests.cpp:204. The instructions use only `{speaker}` and `{player}` (EngineText.h:89-97). HOSTED_TURN_CUE is exactly `({speaker} speaks.)`.
- Cannot verify from the diff:
  - The report's totals (472 = 466 + 6; the brief expected 463 = 457 + 6). The difference comes from test counts in earlier tasks, which this diff does not show.
  - The no-warning build and the asan run.
- Not a finding for this task, recorded for the whole-branch review: the prompt never lists the allowed action ids. The model learns them only through the grammar. The spec's prompt table also does not list them, so this is consistent with the spec, but Task 6 should confirm that the resolve instruction or the tail is enough for the model to choose well.

### Strengths
- The shared `LayOut` helper (PromptBuilder.cpp:14-87) has exactly two callers, keeps all template-aware accounting in one place, and documents its contract with the caller (tokens.summary set before the call). The shared upkeep therefore sees the same flags for both kinds of prompt.
- Hosted error placement is right: the caps are checked by each caller with its own message and error class, and only the context-size check lives in the shared helper.
- The tests check exact bytes for the full layout and for the layout with every optional layer left out. They recompute systemCost, transcriptBudget and transcriptCost from first principles, set every flag the upkeep acts on, cover all three throws, and check both cache properties (a join, and a turn appended).
- The text test checks every brace in the instructions, not only that the two variables are present.

### Issues

#### Critical (Must Fix)
None.

#### Important (Should Fix)
1. Two logic blocks are copied verbatim between the owned and hosted builders (plan-mandated: the brief's Step 4 prescribes both copies).
   - Where: the always-on lore filter-and-format loop (src/context/PromptBuilder.cpp:102-107 and 158-163) and the L2 "# Story so far" renderer (PromptBuilder.cpp:113-119 and 173-179).
   - Why it matters: the L2 format is a contract shared with the upkeep's condense call, which relies on the system message's story-so-far section (EngineText.h CONDENSE_INSTRUCTION). If one copy is edited, the two kinds of run drift apart silently, and no test compares them.
   - Fix: add two small helpers in the anonymous namespace next to `LayOut` (for example `RenderAlwaysOnLore(const std::vector<core::LoreEntry>&)` and `RenderSummaries(const std::vector<core::Summary>&)`). Each would have two callers, so the "helper only with more than one caller" rule allows them. Owned output stays byte-identical.

#### Minor (Nice to Have)
1. HOSTED_SYSTEM_TEXT (src/turn/EngineText.h:85) says the conversation shows "the action the speaker tried in brackets". A refused action is also tried, but it shows no bracket (global constraints, "L3 is two messages per turn"). "Requested" or "the action the host was asked for" would describe what L3 actually shows.
2. `LayOut` (PromptBuilder.cpp:21) assumes a freshly constructed `Assembly`: it appends to prompt.messages and adds to tokens.transcript. Nothing enforces this. Both callers comply today; a `MORPHEUS_REQUIRE(assembly.prompt.messages.empty() && assembly.tokens.transcript == 0, ...)` would make the contract fail loudly.
3. The comments on `core::BudgetSettings` were not updated for hosted use: `worldCap` reads "L1: world rules + always-on lore" and `tailAllowance` "State view + ..." (src/core/Settings.h:38, 40). For a hosted run, world_cap now also covers the participants and the tail holds the observation. LayerTokens and Assembly were updated (PromptBuilder.h:49, 62), but Settings.h was not.
4. The report's workaround `CHECK((turn::HOSTED_TURN_CUE == "({speaker} speaks.)"))` (HostedPromptTests.cpp:150) drops Catch's value printing on failure, so a failure would not show the actual cue. `CHECK(std::string(turn::HOSTED_TURN_CUE) == "({speaker} speaks.)")` keeps the decomposition and avoids the string_view StringMaker link error.

### Assessment
**Task quality:** Needs fixes
**Reasoning:** The behavior is correct and well tested, and the owned prompt is provably unchanged. The one Important finding is a plan-mandated verbatim duplication of the lore and L2 summary renderers, which is cheap to fold into two two-caller helpers next to `LayOut`.

## Re-review 1 (6edc53d..5c4716c)

### Spec Compliance
- Spec compliant. All five items are resolved.
- The owned prompt is still byte-identical (src/context/PromptBuilder.cpp):
  - `RenderAlwaysOnLore` (lines 17-26) is the removed loop with the accumulator renamed. AssembleResolvePrompt still adds `"# Lore\n" + lore` only when the result is non-empty.
  - `RenderSummaries` (lines 30-40) returns an empty string for no summaries and otherwise the same header and lines as before.
  - Neither string changes, and neither do the counts taken from them, so every budget and flag is unchanged.
- The hosted prompt is unchanged apart from the wording: it calls the same two helpers (the hosted lore and summary lines in AssembleHostedPrompt), and the only text change is "tried" to "requested" in HOSTED_SYSTEM_TEXT (src/turn/EngineText.h:85). No test pinned the old wording, and the test that the system text has no brace still holds.

### Issues resolved
- Important 1: the lore and summary loops are now the two helpers, each called by both builders, so the "helper with more than one caller" rule is met. Each helper's comment says why both kinds of run render the same way (for the summaries, because the condense call reads that section).
- Minor 1: the system text now says "requested" (EngineText.h:85).
- Minor 2: LayOut now starts with `MORPHEUS_REQUIRE(assembly.prompt.messages.empty() && assembly.tokens.transcript == 0, ...)`. Both callers pass a default-constructed Assembly that only has engineRules, world and summary set, so the check is satisfied.
- Minor 3: the `tailAllowance` and `worldCap` comments in src/core/Settings.h (lines 38 and 40) now cover the hosted prompt.
- Minor 4: the test now reads `CHECK(std::string(turn::HOSTED_TURN_CUE) == "({speaker} speaks.)")` (tests/context/HostedPromptTests.cpp), so a failure prints the actual cue. `<string>` was already included.

### New issues
None. The fix adds no behavior, no new error path, and no test that could pass while checking nothing.

### Assessment
**Task quality:** Approved
**Reasoning:** Every finding is fixed with minimal edits. The owned prompt is provably byte-identical, and the hosted prompt differs only by the requested wording.
