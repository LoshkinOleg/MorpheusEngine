# Task 7 review: HostedSession::RunTurn (5bd4e63..97eae86)

### Spec Compliance
- Spec compliant. Section 2 steps 0-4, section 6 errors and R4's two-call shape are implemented as the brief gives them; the code matches the brief's Step 3 and Step 4 blocks verbatim.
  - Step 0 checks precede every write and every Complete call: conversation open (HostedSession.cpp:196, OpenConversation), Observe (:197), participant / present / not player (:198-211), allowed actions and actor scopes through ProposalSchema (:212-214), events (:215). The only backend call before the Fit is CountTokens for the speak instruction (:239) and the query embed in Retrieve (:269); neither writes.
  - Resolve: proposal grammar, resolve_max_tokens, resolve thinking, resolve sampling (:283-289); Adjudicate on the raw text (:294); a refused action goes on with no action and its reasons are kept in the record and in timings.resolve.refused (:331-333); onAction fires for requested and refused alike, before the speak call (:295-297).
  - Speak: `speakPrompt = assembly->prompt` then ExtendForNarrate (:301-302), which only appends Assistant(resolve text) and User(speak instruction) (checked src/context/PromptBuilder.cpp:233-240). So the speak prompt is exactly the resolve prompt + resolve output + speak instruction; speech is never in the JSON. No grammar, narrate_max_tokens, onToken streaming (:303-308). The test pins it message by message (HostedTurnTests.cpp:90-107).
  - Commit: number (run-wide, LatestHostedTurn + 1), conversation id, speaker, listeners (other participants present), cleaned player text, the request's events, observation, raw resolve output, adjudication, trimmed line, timings (resolve, speak, retrieval) (:314-343). Everything section 2 step 4 lists is there.
  - Cancel: the three cancel points (Fit, resolve, speak) each return nullopt before CommitHostedTurn (:279-281, :290-292, :309-311); the request's events are only stored inside the committed record, never in conversation_events, so a cancel keeps nothing (test :248-282). Folds the fit loop finished before a stop stay, as documented and as in Session.
  - Budget room for the speak call: the shared assembly reserves both generations, the narrate allowance and both message overheads (checked src/context/PromptBuilder.cpp:89-117), and RunTurn refuses a speak instruction over narrate_allowance as a DataError naming `speaker` (:239-243).
  - Seeds (named risk: collisions; checked src/core/Dice.h:45 and src/turn/MemoryUpkeep.cpp:152,204): resolve uses CallKind::Resolve, speak CallKind::Narrate, folds and condenses CallKind::Summarize, all on the new run-wide turn number with attempt 0. Three distinct kinds, so no collision within a turn; turn numbers are run-wide, so none across conversations.
  - Retrieval telemetry (named risk: logging the hits before the fit loop shed some): Fit takes `hits` by reference and pops from it (MemoryUpkeep.cpp:38,55,79), so timings.retrieval (:335-341) lists exactly the hits in the prompt.
  - TurnError ordering (named risk: a fold committed before the tail error): Fit tests tailOverBudget before any condense or fold (MemoryUpkeep.cpp:50-57), and the tail does not change between passes, so a too-long player text throws before anything is written.
  - Session telemetry unchanged: the CallTelemetry body is the old Telemetry body byte for byte (diff Session.cpp:426-439, context lines). All five call sites were renamed (:475, :498, :521, :544, :558) and nothing else in PlayTurn changed.
- Cannot verify from diff: that the store's CommitHostedTurn round-trips `adjudication.reasons` and `timings` (Task 2's code; the tests here read back only `line`, HostedTurnTests.cpp:63-65).

### Strengths
- The order of RunTurn reads as section 2 step by step, with step-numbered comments, and each fault class matches section 6: DataError for request faults, TurnError from the shared fit loop, model mistakes as refusals, nullopt on cancel.
- The two-call R4 shape is right and is tested strictly: same messages, the assistant role on the resolve output, the exact speak instruction, no grammar and the narrate cap on the second call (HostedTurnTests.cpp:86-108).
- The cache-stability tests are strong: stable prefixes compared message for message across three turns with recorded and request events (:168-212), and a join and a leave checked byte for byte against the participants layer (:214-246).
- The malformed-request test covers every request fault the spec names and asserts that no Complete call happened and no turn was stored (:284-318).
- HostedScript routes replies by call type (grammar or not, summarize/condense instruction), so the tests do not depend on the call count.
- Making CallTelemetry public adds no behaviour change to Session and gives both sessions one telemetry format.

### Issues

#### Critical (Must Fix)
None.

#### Important (Should Fix)
1. **Verbatim duplicated input-cleaning loop (plan-mandated).** src/turn/HostedSession.cpp:219-232 repeats src/turn/Session.cpp:120-128 line for line: the same control-character filter (`(byte >= 0x20 && byte != 0x7F) || c == '\n' || c == '\t'`) and the same Trim. The brief spells it out ("cleaned as Session cleans input") and copies the block. Two callers now exist, so CLAUDE.md allows a shared helper: one function (for example `std::string CleanInput(std::string_view)` beside `core::Trim` in core/Text.h, or in turn/Session.h next to SamplingFrom) that both sessions call. Then a later change to what counts as a control character cannot drift between the owned and the hosted input.

#### Minor (Nice to Have)
1. src/turn/HostedSession.cpp:262-269: retrieval runs before the fit loop. So a player text with no room in the tail is refused only after the query embed, a backend call. Retrieval also ignores `stop`, so a stop requested before the call still embeds. This is the same as Session (the spec says "as in Session"), and nothing is written. But the TurnError test's `CHECK(backend.Calls().empty())` (HostedTurnTests.cpp:330) does not cover it, because ScriptedBackend::Calls records only Complete (tests/fakes/ScriptedBackend.h:105). The assertion claims less than the test name suggests.
2. src/turn/HostedSession.cpp:284: the grammar is built after the Fit, which may already have folded, and it rebuilds the ProposalSchema that step 0 built at :213-214. Building the grammar in step 0, beside `proposalSchema`, would compute the schema once and put every request-derived failure before any write.
3. No test for the speak-instruction DataError (HostedSession.cpp:239-243, `speaker: ... over narrate_allowance`). No RunTurn test either for "no action left after the actor scopes" (spec section 2, step 0).
4. src/turn/HostedSession.h:82-90: the RunTurn comment does not say that onAction can fire for a turn that is later cancelled during the speak call: the host was told the action, but the turn is not committed (exercised at HostedTurnTests.cpp:263-273). The spec's order makes this unavoidable, but D2 needs that sentence to know a reported action can belong to an uncommitted turn.

### Assessment
**Task quality:** Needs fixes
**Reasoning:** The behaviour matches the spec and R4 in full, and the tests are thorough. The one blocking item is the plan-mandated verbatim copy of Session's input-cleaning block, which should become one shared helper now that it has two callers.

---

## Re-review 1 (97eae86..a035ab5)

### Spec Compliance
- Spec compliant. The fix keeps every section 2 / section 6 behaviour verified in the first review. The request checks now also cover the grammar build, so every request-derived failure comes before any write (HostedSession.cpp, diff :50-54).

### Resolution of earlier findings
- **Important 1 (duplicated input cleaning): resolved.**
  - `turn::CleanInput` (Session.h diff :226-229, Session.cpp diff :155-166) has the old loop byte for byte, the same condition `(byte >= 0x20 && byte != 0x7F) || c == '\n' || c == '\t'`, and returns `core::Trim(kept)`.
  - `Session::RunTurn` (diff :199-204) still throws `TurnError("the input is empty")` on an empty result and passes the same cleaned string to PlayTurn, so Session's behaviour is unchanged.
  - `HostedSession::RunTurn` (diff :68-71) keeps its "empty means no player text" rule.
  - The comment Session::RunTurn dropped ("hygiene only: the backend tokenizes any content as plain text") moved to the CleanInput declaration (Session.h diff :226-228), so nothing was lost.
  - Two callers now, as CLAUDE.md requires for a helper.
  - Direct test: HostedTurnTests.cpp diff :338-347 covers 0x01, 0x07, 0x1B and 0x7F removed, tab and newline kept, the UTF-8 bytes 0xC3 0xA9 kept, trimming, and an all-whitespace input.
  - Non-ASCII bytes are built with `static_cast<char>`, so there are no raw bytes in the source.
- **Minor 1 (retrieval before the fit loop): left by the controller's decision (matches Session).** Not re-raised.
- **Minor 2 (grammar built after the Fit, schema built twice): resolved.**
  - The grammar is `grammar::FromSchema(proposalSchema)`, built in step 0 (diff :52-53) and reused for resolveSpec (diff :91).
  - Named risk, divergence from the spec's `ProposalGrammar`: I checked src/turn/HostedWorld.cpp:527-531. ProposalGrammar is exactly `grammar::FromSchema(ProposalSchema(...))`, so the text is identical, and the first test still pins the resolve grammar against `world.ProposalGrammar(...)`, which would catch any divergence.
  - The `grammar/Schema.h` include respects the turn <- grammar direction, and the local is named `proposalGrammar`, not `grammar`.
- **Minor 3 (untested errors): resolved.**
  - New test (diff :321-336): narrate_allowance 4 gives a DataError naming the speaker and the allowance, with no Complete call and no turn stored.
  - The malformed-request test gains the scope case (diff :307-312): only `attack` allowed and no non-player target present gives "allowed_actions: no allowed action can be taken this turn". This rests on the fixture order Hulda, Saadia, Aela, which the earlier `absent` case already assumes, and the REQUIRE on index 0 guards it.
- **Minor 4 (onAction then cancel): resolved.** The RunTurn comment (HostedSession.h diff :122-129) now says that onAction fires before the line starts, and that a reported action is not a committed turn until RunTurn returns one.
- **Controller's added check (store round trip of a refusal): done.** The refused-action test reads the turn back through `Turns(id)` and compares `requested`, `reasons`, `resolveOutput` and the whole `timings` JSON, including `resolve.refused` (diff :278-284). This closes the earlier "cannot verify from diff" item.

### New issues
#### Critical (Must Fix)
None.
#### Important (Should Fix)
None.
#### Minor (Nice to Have)
1. tests/hosted/HostedTurnTests.cpp diff :338-347: the CleanInput test sits in the hosted file under `[hosted][turn]`. CleanInput is declared in turn/Session.h and Session uses it too, so `[session]` runs (the Session test tag) do not pick it up. A `[session]` tag, or a home under tests/turn, would fit better. Cosmetic.

### Assessment
**Task quality:** Approved
**Reasoning:** The duplicated block is now one shared helper, Session's behaviour is unchanged, and Minors 2-4 and the store round trip are fixed and tested. Nothing new of substance was introduced.
