### Task 3: The review grammar, its parser, its prompt and the engine text

The review call is standalone. Its system message is `REVIEW_RULES`; its user message holds the player's input, the outcome rendered exactly as the narrate message renders it, the narration, then `REVIEW_INSTRUCTION`. A grammar fixes the output's shape for each combination of the narration check and the digest. This task also extracts the outcome rendering from `RenderNarrateMessage` as `RenderOutcome`, and gives `RenderNarrateMessage` the renarrate's correction paragraph. Nothing calls the review yet.

**Files:**
- Create: `src/turn/Review.h`, `src/turn/Review.cpp`, `tests/turn/ReviewTests.cpp`
- Modify: `src/turn/ResolveGrammar.h`, `src/turn/ResolveGrammar.cpp`, `src/turn/EngineText.h`, `src/turn/Resolve.h`, `src/turn/Resolve.cpp`, `src/turn/Session.cpp` (the one `RenderNarrateMessage` call), `src/turn/CMakeLists.txt`
- Test: `tests/turn/ResolveGrammarTests.cpp`, `tests/turn/ResolveTests.cpp`, `tests/turn/ReviewTests.cpp`

**Interfaces:**
- Consumes: `turn::ResolveProposal`, `turn::Resolution`, `context::RenderChangePart`, `core::Trim`.
- Produces:
  - `constexpr int turn::REVIEW_MAX_PROBLEMS = 3`, `REVIEW_PROBLEM_MAX_CHARS = 200`, `DIGEST_MAX_CHARS = 300` (`turn/ResolveGrammar.h`).
  - `std::string turn::BuildReviewGrammar(bool check, bool digest)`; its first line is always `root ::= review`.
  - `turn::REVIEW_RULES`, `turn::REVIEW_INSTRUCTION`, `turn::RENARRATE_CORRECTION` (`std::string_view`, `turn/EngineText.h`).
  - `struct turn::OutcomeDetail { bool rejected = true; std::size_t appliedShown = SIZE_MAX; bool stakes = true; bool cutIntent = false; }` and `std::string turn::RenderOutcome(const ResolveProposal&, const Resolution&, const OutcomeDetail&)` (`turn/Resolve.h`).
  - `std::string turn::RenderNarrateMessage(const ResolveProposal&, const Resolution&, const std::vector<std::string>& problems, int allowance, inference::LlmBackend&)` (the problems are new, third).
  - `struct turn::Review { std::optional<bool> consistent; std::vector<std::string> problems; std::string digest; std::string error; }`, `turn::Review turn::ParseReviewOutput(std::string_view text, bool check, bool digest)`, `inference::Prompt turn::ReviewPrompt(const std::string& input, const ResolveProposal&, const Resolution&, const std::string& narration)` (`turn/Review.h`).

- [ ] **Step 1: Write the failing grammar test**

Append to `tests/turn/ResolveGrammarTests.cpp`:

```cpp
TEST_CASE("The review grammar asks for the verdict, the digest or both", "[grammar]")
{
    // Both: consistent with no problems, or inconsistent with one to three of them; the digest follows the verdict.
    const std::string both = turn::BuildReviewGrammar(true, true);
    CHECK(both.starts_with("root ::= review\n"));
    CHECK_THAT(both, ContainsSubstring(R"(review ::= "{\"consistent\":true,\"problems\":[]" ",\"digest\":" digest-text "}" | "{\"consistent\":false,\"problems\":[" problem-text ("," problem-text){0,2} "]" ",\"digest\":" digest-text "}")"));
    CHECK_THAT(both, ContainsSubstring(R"(problem-text ::= "\"" char{0,200} "\"")"));
    CHECK_THAT(both, ContainsSubstring(R"(digest-text ::= "\"" char{0,300} "\"")"));
    CHECK(GrammarProblem(both).empty());

    // The check alone: no digest.
    const std::string check = turn::BuildReviewGrammar(true, false);
    CHECK(check.starts_with("root ::= review\n"));
    CHECK_THAT(check, ContainsSubstring(R"(review ::= "{\"consistent\":true,\"problems\":[]" "}" | "{\"consistent\":false,\"problems\":[" problem-text ("," problem-text){0,2} "]" "}")"));
    CHECK_THAT(check, !ContainsSubstring("digest"));
    CHECK(GrammarProblem(check).empty());

    // The digest alone: no verdict.
    const std::string digest = turn::BuildReviewGrammar(false, true);
    CHECK(digest.starts_with("root ::= review\n"));
    CHECK_THAT(digest, ContainsSubstring(R"(review ::= "{\"digest\":" digest-text "}")" "\n"));
    CHECK_THAT(digest, !ContainsSubstring("consistent"));
    CHECK(GrammarProblem(digest).empty());

    // A review that asks for nothing is an engine bug.
    CHECK_THROWS_AS(turn::BuildReviewGrammar(false, false), core::ContractError);
}
```

- [ ] **Step 2: Write the failing narrate message tests**

`RenderNarrateMessage` takes the problems as its third argument; update every existing call in `tests/turn/ResolveTests.cpp` (two of them span two lines):

```bash
sed -i -E 's/RenderNarrateMessage\((proposal|unreadable), (resolution|nothing), /RenderNarrateMessage(\1, \2, {}, /' tests/turn/ResolveTests.cpp
sed -i 's/, 42, 1), 4096, backend));/, 42, 1), {}, 4096, backend));/' tests/turn/ResolveTests.cpp
grep -n -A1 "RenderNarrateMessage(" tests/turn/ResolveTests.cpp
```

The grep must show five arguments, the third `{}`, in every call (14 calls before the new tests).

Append to `tests/turn/ResolveTests.cpp`:

```cpp
TEST_CASE("RenderOutcome is the record the narrate message starts with", "[resolve]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(16384);
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(CLIMB);
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);
    const std::string outcome = turn::RenderOutcome(proposal, resolution, {});
    CHECK(outcome.starts_with("# Resolution\nIntent: climb\nCheck: agility vs hard (16): rolled "));
    CHECK_THAT(outcome, ContainsSubstring(". Stakes: a fall\n"));
    CHECK(outcome.ends_with("\n"));
    CHECK(turn::RenderNarrateMessage(proposal, resolution, {}, 512, backend) ==
          outcome + "\n# Task\n" + std::string(turn::NARRATE_INSTRUCTION));

    // Less detail: no stakes, no applied entries (two were applied), the intent cut.
    const turn::OutcomeDetail least = {.rejected = false, .appliedShown = 0, .stakes = false, .cutIntent = true};
    const std::string shed = turn::RenderOutcome(proposal, resolution, least);
    CHECK_THAT(shed, !ContainsSubstring("Stakes:"));
    CHECK_THAT(shed, ContainsSubstring("Applied: and 2 more\n"));
}

TEST_CASE("A renarrate's narrate message carries the review's problems, shed last", "[resolve]")
{
    const core::World world = MiniWorld();
    test::FakeBackend backend(16384);
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(CLIMB);
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);
    const std::vector<std::string> problems = {"The narration has the player fly.", std::string(200, 'p')};

    // The correction and the problems sit between the record and the instruction.
    const std::string message = turn::RenderNarrateMessage(proposal, resolution, problems, 4096, backend);
    CHECK(message == turn::RenderOutcome(proposal, resolution, {}) + "\n# Task\n" +
                         std::string(turn::RENARRATE_CORRECTION) + "\n- The narration has the player fly.\n- " +
                         std::string(200, 'p') + "\n\n" + std::string(turn::NARRATE_INSTRUCTION));

    // Over the allowance, the record's detail goes first and the problem lines last; the correction stays.
    const turn::OutcomeDetail least = {.rejected = false, .appliedShown = 0, .stakes = false, .cutIntent = true};
    const std::string minimal = turn::RenderOutcome(proposal, resolution, least) + "\n# Task\n" +
                                std::string(turn::RENARRATE_CORRECTION) + "\n\n" +
                                std::string(turn::NARRATE_INSTRUCTION);
    const int allowance = backend.CountTokens(minimal);
    CHECK(turn::RenderNarrateMessage(proposal, resolution, problems, allowance, backend) == minimal);
    CHECK_THROWS_AS(turn::RenderNarrateMessage(proposal, resolution, problems, allowance - 1, backend),
                    core::DataError);
}
```

- [ ] **Step 3: Write the failing review tests**

Create `tests/turn/ReviewTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <string>
#include <vector>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/World.h"
#include "turn/EngineText.h"
#include "turn/Resolve.h"
#include "turn/Review.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

TEST_CASE("ParseReviewOutput reads a verdict, its problems and a digest", "[review]")
{
    const turn::Review consistent =
        turn::ParseReviewOutput(R"({"consistent":true,"problems":[],"digest":"  The player waited. "})", true, true);
    CHECK(consistent.error.empty());
    CHECK(consistent.consistent == true);
    CHECK(consistent.problems.empty());
    CHECK(consistent.digest == "The player waited."); // Trimmed.

    const turn::Review contradicts =
        turn::ParseReviewOutput(R"({"consistent":false,"problems":["The roll failed."]})", true, false);
    CHECK(contradicts.error.empty());
    CHECK(contradicts.consistent == false);
    CHECK(contradicts.problems == std::vector<std::string>{"The roll failed."});
    CHECK(contradicts.digest.empty());

    // With the check off only the digest is read: a verdict in the output anyway is ignored.
    const turn::Review digestOnly =
        turn::ParseReviewOutput(R"({"consistent":false,"problems":["x"],"digest":"The player waited."})", false, true);
    CHECK(digestOnly.error.empty());
    CHECK_FALSE(digestOnly.consistent);
    CHECK(digestOnly.problems.empty());
    CHECK(digestOnly.digest == "The player waited.");
}

TEST_CASE("ParseReviewOutput reports output it cannot read and keeps nothing of it", "[review]")
{
    // Each case: the output, the parts asked for, and a piece of the reason.
    struct Case {
        const char* text = "";
        bool check = false;
        bool digest = false;
        const char* reason = "";
    };
    const std::vector<Case> cases = {
        {R"({"consistent":false,"probl)", true, true, "not complete JSON"}, // Cut off by review_max_tokens.
        {R"(["consistent"])", true, true, "not a JSON object"},
        {R"({"problems":[],"digest":"x"})", true, true, "'consistent'"},
        {R"({"consistent":true,"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":true,"problems":[1],"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":false,"problems":[],"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":true,"problems":["odd"],"digest":"x"})", true, true, "'problems'"},
        {R"({"consistent":true,"problems":[]})", true, true, "'digest'"},
        {R"({"digest":7})", false, true, "'digest'"},
    };
    for (const Case& entry : cases) {
        INFO(entry.text);
        const turn::Review review = turn::ParseReviewOutput(entry.text, entry.check, entry.digest);
        CHECK_THAT(review.error, ContainsSubstring(entry.reason));
        CHECK_FALSE(review.consistent);
        CHECK(review.problems.empty());
        CHECK(review.digest.empty());
    }
    CHECK_THROWS_AS(turn::ParseReviewOutput("{}", false, false), core::ContractError);
}

TEST_CASE("ReviewPrompt shows the input, the whole outcome and the narration under the review rules", "[review]")
{
    const core::World world = core::LoadWorld(test::TestDataDir() / "worlds" / "mini");
    const turn::ResolveProposal proposal = turn::ParseResolveOutput(
        R"({"intent":"walk","checks":[],"changes":[{"op":"set","path":"player.location","value":"lighthouse","reason":"walks"},)"
        R"({"op":"add","path":"player.inventory.oil","value":-1,"reason":"spills"}]})");
    const turn::Resolution resolution = turn::ApplyProposal(world, world.initialState, proposal, 42, 1);

    const inference::Prompt prompt =
        turn::ReviewPrompt("I walk to the lighthouse.", proposal, resolution, "You walk to the lighthouse.");
    REQUIRE(prompt.messages.size() == 2);
    CHECK(prompt.stableCount == 1);
    CHECK(prompt.messages[0].role == inference::Role::System);
    CHECK(prompt.messages[0].content == turn::REVIEW_RULES);
    CHECK(prompt.messages[1].role == inference::Role::User);
    CHECK(prompt.messages[1].content == "# Player\nI walk to the lighthouse.\n\n" +
                                            turn::RenderOutcome(proposal, resolution, {}) +
                                            "\n# Narration\nYou walk to the lighthouse.\n\n# Task\n" +
                                            std::string(turn::REVIEW_INSTRUCTION));
    // The outcome is shown in full, rejected changes and their reasons included.
    CHECK_THAT(prompt.messages[1].content,
               ContainsSubstring("Rejected: add player.inventory.oil (no item 'oil' to decrease)"));
}
```

- [ ] **Step 4: Run the tests to verify they fail**

Run: `cmake --build --preset tests`
Expected: compile errors: `BuildReviewGrammar`, `RenderOutcome`, `OutcomeDetail`, `RENARRATE_CORRECTION`, `ParseReviewOutput`, `ReviewPrompt`, `REVIEW_RULES` and `REVIEW_INSTRUCTION` are not members of `morpheus::turn`, `turn/Review.h` does not exist, and `RenderNarrateMessage` does not take five arguments.

- [ ] **Step 5: Add the engine text**

In `src/turn/EngineText.h`, after `NARRATE_INSTRUCTION`, add:

```cpp
// Opens a renarrate's correction, which the narrate message gains before NARRATE_INSTRUCTION; the review's problems
// follow it, one "- " line each (RenderNarrateMessage).
inline constexpr std::string_view RENARRATE_CORRECTION =
    "Your previous narration of this outcome contradicted the resolution above. Narrate the outcome exactly as the "
    "resolution states it.";

// The review call's system message: the reviewer's job, what counts as a contradiction, and what a digest is. Which
// fields the output holds is fixed by the call's grammar (BuildReviewGrammar).
inline constexpr std::string_view REVIEW_RULES = R"(# Review
You review one turn of a solo text role-playing game. A game engine decided the turn's outcome: it rolled the checks, and it applied or rejected the changes the player's action proposed. A narrator then told the player what happened. Reply with one JSON object and nothing else. It holds some of these fields:
- "consistent": false when the narration contradicts the outcome, true otherwise. A contradiction is a failed check narrated as a success or a successful one narrated as a failure, an applied change left out or reversed, or a rejected change narrated as if it happened. Tone, style and details the outcome does not decide are not contradictions.
- "problems": each contradiction in one short sentence, at most three. Empty when the narration is consistent.
- "digest": the turn in one or two sentences, past tense, third person: who did what and what came of it. No dice, roll numbers or engine terms.)";

// Ends the review call's user message, after the player's input, the outcome and the narration.
inline constexpr std::string_view REVIEW_INSTRUCTION =
    "Review the narration above against the resolution. Reply with the JSON object only.";
```

- [ ] **Step 6: Add the review grammar**

In `src/turn/ResolveGrammar.h`, after `constexpr int MAX_ITEM_CHARS     = 32;  // New counter items; held items are matched by name.`, add:

```cpp
constexpr int REVIEW_MAX_PROBLEMS      = 3;   // Contradictions one review may list.
constexpr int REVIEW_PROBLEM_MAX_CHARS = 200; // Each of them.
constexpr int DIGEST_MAX_CHARS         = 300; // A turn's digest.
```

Replace the `static_assert` with:

```cpp
// Every list bound is written as "first element, then up to bound - 1 more", so each needs at least 2.
static_assert(MAX_CHECKS >= 2 && MAX_CHANGES >= 2 && MAX_BRANCH_CHANGES >= 2 && MAX_NOTES >= 2 && MAX_LOOKUPS >= 2 &&
                  MAX_LIST_ITEMS >= 2 && REVIEW_MAX_PROBLEMS >= 2,
              "ResolveGrammar: every list bound must be at least 2");
```

After the `BuildLookupGrammar` declaration, add:

```cpp
// GBNF for the review call (architecture spec, "Turn flow"). Its first line is always "root ::= review". The output
// is one compact JSON object:
//   check:  {"consistent":true,"problems":[]} or {"consistent":false,"problems":[1 to REVIEW_MAX_PROBLEMS strings of
//           at most REVIEW_PROBLEM_MAX_CHARS]}
//   digest: "digest": a string of at most DIGEST_MAX_CHARS, after the verdict, or alone: {"digest": str}
// Throws ContractError unless `check` or `digest` is set.
std::string BuildReviewGrammar(bool check, bool digest);
```

In `src/turn/ResolveGrammar.cpp`, after `BuildLookupGrammar`, add:

```cpp
std::string BuildReviewGrammar(bool check, bool digest)
{
    MORPHEUS_REQUIRE(check || digest, "BuildReviewGrammar: needs the check, the digest or both");
    // The digest closes the object when it is asked for: after the verdict, or alone.
    const std::string digestPart = digest ? Literal(check ? ",\"digest\":" : "{\"digest\":") + " digest-text " : "";
    std::string review;
    if (check) {
        // Consistent with no problems, or inconsistent with 1 to REVIEW_MAX_PROBLEMS of them.
        review = Literal("{\"consistent\":true,\"problems\":[]") + " " + digestPart + Literal("}") + " | " +
                 Literal("{\"consistent\":false,\"problems\":[") +
                 std::format(" problem-text ({} problem-text){{0,{}}} ", Literal(","), REVIEW_MAX_PROBLEMS - 1) +
                 Literal("]") + " " + digestPart + Literal("}");
    } else {
        review = digestPart + Literal("}");
    }

    // A fixed root line, whatever is asked for, so the call is recognised by it (CallKindOf).
    std::string grammar = "root ::= review\nreview ::= " + review + "\n";
    if (check) {
        grammar += std::format(R"(problem-text ::= "\"" char{{0,{}}} "\"")", REVIEW_PROBLEM_MAX_CHARS) + "\n";
    }
    if (digest) {
        grammar += std::format(R"(digest-text ::= "\"" char{{0,{}}} "\"")", DIGEST_MAX_CHARS) + "\n";
    }
    return grammar + Terminals();
}
```

- [ ] **Step 7: Extract RenderOutcome and add the correction**

In `src/turn/Resolve.h`, add `#include <limits>` to the includes, and replace the `RenderNarrateMessage` comment and declaration with:

```cpp
// How much of the outcome RenderOutcome shows. The defaults show everything; RenderNarrateMessage sheds detail
// from them to fit the narrate allowance.
struct OutcomeDetail {
    bool rejected = true;                                               // The Rejected line.
    std::size_t appliedShown = std::numeric_limits<std::size_t>::max(); // Applied entries shown; the rest are counted.
    bool stakes = true;                                                 // Each check line's ". Stakes: ..." part.
    bool cutIntent = false; // The intent cut to INTENT_SHORT_CHARS bytes and ended with "...".
};

// The resolution record, as the narrate message and the review prompt show it:
//   # Resolution
//   Intent: climb
//   Check: agility vs hard (16): rolled 11 +3 = 14, failure. Stakes: a fall
//   Applied: player.health -2
//   Rejected: add player.inventory.oil (no item 'oil' to decrease)
// "Applied: none" when nothing applied, "Applied: a; b; and 2 more" when entries are left out, and no Rejected line
// when nothing was rejected. An unreadable proposal gets "The action could not be resolved (<error>). Nothing
// changed." instead. Ends with a newline.
std::string RenderOutcome(const ResolveProposal& proposal, const Resolution& resolution, const OutcomeDetail& detail);

// The user message the narrate call appends: RenderOutcome, then "\n# Task\n", then for a renarrate (non-empty
// `problems`, the review's) RENARRATE_CORRECTION and one "- <problem>" line per problem and a blank line, then
// NARRATE_INSTRUCTION. When the text exceeds `allowance` tokens (budget.narrate_allowance), detail is shed one step
// at a time, only while still over:
//   1. the Rejected line;
//   2. Applied entries from the end ("and N more");
//   3. every check line's ". Stakes: ..." part;
//   4. the intent, cut to INTENT_SHORT_CHARS bytes and ended with "...";
//   5. a renarrate's problem lines (the correction sentence stays).
// Throws DataError if it still does not fit (narrate_allowance is too small). An unreadable proposal's message has
// nothing to shed and is returned as it is.
std::string RenderNarrateMessage(const ResolveProposal& proposal, const Resolution& resolution,
                                 const std::vector<std::string>& problems, int allowance,
                                 inference::LlmBackend& backend);
```

In `src/turn/Resolve.cpp`, replace the whole `RenderNarrateMessage` function with:

```cpp
std::string RenderOutcome(const ResolveProposal& proposal, const Resolution& resolution, const OutcomeDetail& detail)
{
    if (!proposal.error.empty()) {
        return std::format("# Resolution\nThe action could not be resolved ({}). Nothing changed.\n", proposal.error);
    }
    // Model-written parts in a "; "-separated list: the first `count` of them.
    auto join = [](const std::vector<std::string>& parts, std::size_t count) {
        std::string text;
        for (std::size_t i = 0; i < count; ++i) {
            text += (i == 0 ? "" : "; ") + parts[i];
        }
        return text;
    };

    // The intent, cut short when asked.
    std::string intent = proposal.intent;
    if (detail.cutIntent) {
        intent = core::CutUtf8(intent, INTENT_SHORT_CHARS) + (intent.size() > INTENT_SHORT_CHARS ? "..." : "");
    }
    std::string text = "# Resolution\nIntent: " + intent + "\n";

    // One line per rolled check; the stakes are model-written text and can be left out.
    for (const core::CheckOutcome& check : resolution.checks) {
        std::string rolls;
        for (const int roll : check.result.rolls) {
            rolls += (rolls.empty() ? "" : " + ") + std::to_string(roll);
        }
        // A stat of 0 adds nothing, so its "+N" is left out.
        const std::string stat = check.result.statValue != 0 ? std::format(" {:+}", check.result.statValue) : "";
        text += std::format("Check: {} vs {} ({}): rolled {}{} = {}, {}", check.stat, check.difficulty,
                            check.result.target, rolls, stat, check.result.total,
                            check.result.success ? "success" : "failure");
        text += detail.stakes ? std::format(". Stakes: {}\n", check.stakes) : "\n";
    }

    // The applied changes: the first appliedShown of them, and how many more there were.
    std::vector<std::string> applied;
    for (const core::AppliedChange& change : resolution.applied) {
        applied.push_back(context::RenderChangePart(change));
    }
    if (applied.empty()) {
        text += "Applied: none\n";
    } else {
        const std::size_t shown = std::min(detail.appliedShown, applied.size());
        text += "Applied: " + join(applied, shown);
        if (shown < applied.size()) {
            text += std::format("{}and {} more", shown == 0 ? "" : "; ", applied.size() - shown);
        }
        text += "\n";
    }

    // The rejected changes with their reasons, named by op and path when the raw change has them.
    if (detail.rejected && !resolution.rejected.empty()) {
        std::vector<std::string> rejected;
        for (const core::RejectedChange& change : resolution.rejected) {
            const nlohmann::json& raw = change.raw;
            const bool named = raw.is_object() && raw.contains("op") && raw.at("op").is_string() &&
                               raw.contains("path") && raw.at("path").is_string();
            rejected.push_back(named ? std::format("{} {} ({})", raw.at("op").get<std::string>(),
                                                   raw.at("path").get<std::string>(), change.reason)
                                     : change.reason);
        }
        text += "Rejected: " + join(rejected, rejected.size()) + "\n";
    }
    return text;
}

std::string RenderNarrateMessage(const ResolveProposal& proposal, const Resolution& resolution,
                                 const std::vector<std::string>& problems, int allowance,
                                 inference::LlmBackend& backend)
{
    // The task: for a renarrate the correction and, unless shed, the review's problems; then the instruction.
    auto task = [&problems](bool showProblems) {
        std::string text = "\n# Task\n";
        if (!problems.empty()) {
            text += std::string(RENARRATE_CORRECTION) + "\n";
            if (showProblems) {
                for (const std::string& problem : problems) {
                    text += "- " + problem + "\n";
                }
            }
            text += "\n";
        }
        return text + std::string(NARRATE_INSTRUCTION);
    };
    if (!proposal.error.empty()) {
        // Nothing was resolved, so the record holds nothing to shed.
        return RenderOutcome(proposal, resolution, {}) + task(true);
    }

    // Render, then shed detail one step at a time until the message fits the narrate allowance: the rejected
    // changes, applied changes from the end, the stakes, the intent's tail, then a renarrate's problems. Each step
    // runs only while over.
    OutcomeDetail detail;
    detail.rejected = !resolution.rejected.empty();
    detail.appliedShown = resolution.applied.size();
    bool showProblems = !problems.empty();
    for (;;) {
        const std::string text = RenderOutcome(proposal, resolution, detail) + task(showProblems);
        if (backend.CountTokens(text) <= allowance) {
            return text;
        }
        if (detail.rejected) {
            detail.rejected = false;
        } else if (detail.appliedShown > 0) {
            --detail.appliedShown;
        } else if (detail.stakes) {
            detail.stakes = false;
        } else if (!detail.cutIntent) {
            detail.cutIntent = true;
        } else if (showProblems) {
            showProblems = false;
        } else {
            // Everything model-written is shed: the allowance itself is too small.
            throw core::DataError(std::format("narrate_allowance {} is too small for the resolution record ({} tokens)",
                                              allowance, backend.CountTokens(text)));
        }
    }
}
```

In `src/turn/Session.cpp`, the narrate step's call becomes:

```cpp
    context::ExtendForNarrate(narratePrompt, resolved.text,
                              RenderNarrateMessage(proposal, resolution, {}, _settings.budget.narrateAllowance,
                                                   *_pBackend));
```

- [ ] **Step 8: Add the review parser and prompt**

Create `src/turn/Review.h`:

```cpp
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "inference/Llm.h"
#include "turn/Resolve.h"

namespace morpheus::turn {

// The review call's output, read (architecture spec, "Turn flow"). `consistent` and `problems` come from the
// narration check, `digest` from the compact transcript. Output that cannot be read leaves everything empty except
// `error`, which says why: a model mistake, so the turn goes on as if the review had found nothing.
struct Review {
    std::optional<bool> consistent = {};    // Empty when the check was off or the output could not be read.
    std::vector<std::string> problems = {}; // The contradictions; empty when consistent.
    std::string digest = {};                // Trimmed. Empty when the compact transcript was off.
    std::string error = {};
};

// Reads the output of a review call made with BuildReviewGrammar(check, digest): with `check`, a boolean
// "consistent" and an array of string "problems" that is empty exactly when "consistent" is true; with `digest`, a
// string "digest". Fields that were not asked for are ignored. Throws ContractError unless `check` or `digest` is set.
Review ParseReviewOutput(std::string_view text, bool check, bool digest);

// The review call's prompt, standalone: REVIEW_RULES as the system message, then one user message:
//   # Player
//   <input>
//
//   <RenderOutcome(proposal, resolution, {})>
//   # Narration
//   <narration>
//
//   # Task
//   <REVIEW_INSTRUCTION>
// stableCount is 1 (the rules).
inference::Prompt ReviewPrompt(const std::string& input, const ResolveProposal& proposal,
                               const Resolution& resolution, const std::string& narration);

} // namespace morpheus::turn
```

Create `src/turn/Review.cpp`:

```cpp
#include "turn/Review.h"

#include <algorithm>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

Review ParseReviewOutput(std::string_view text, bool check, bool digest)
{
    MORPHEUS_REQUIRE(check || digest, "ParseReviewOutput: needs the check, the digest or both");
    // Every unreadable output ends here: nothing is kept, only the reason.
    auto failed = [](std::string why) {
        Review review;
        review.error = std::move(why);
        return review;
    };
    // Parse without exceptions: a cut-off or malformed output is a model mistake, not an engine bug.
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) {
        return failed("the review output is not complete JSON; it may have hit review_max_tokens");
    }
    if (!json.is_object()) {
        return failed("the review output is not a JSON object");
    }

    Review review;
    if (check) {
        // The verdict, and its problems: listed exactly when the narration contradicts the outcome.
        if (!json.contains("consistent") || !json.at("consistent").is_boolean()) {
            return failed("'consistent' must be true or false");
        }
        const bool problemsReadable =
            json.contains("problems") && json.at("problems").is_array() &&
            std::all_of(json.at("problems").begin(), json.at("problems").end(),
                        [](const nlohmann::json& problem) { return problem.is_string(); });
        if (!problemsReadable) {
            return failed("'problems' must be an array of strings");
        }
        review.consistent = json.at("consistent").get<bool>();
        for (const nlohmann::json& problem : json.at("problems")) {
            review.problems.push_back(problem.get<std::string>());
        }
        if (*review.consistent != review.problems.empty()) {
            return failed("'problems' must list the contradictions exactly when 'consistent' is false");
        }
    }
    if (digest) {
        if (!json.contains("digest") || !json.at("digest").is_string()) {
            return failed("'digest' must be a string");
        }
        review.digest = core::Trim(json.at("digest").get<std::string>());
    }
    return review;
}

inference::Prompt ReviewPrompt(const std::string& input, const ResolveProposal& proposal,
                               const Resolution& resolution, const std::string& narration)
{
    // The rules, then what the reviewer judges: the input, the whole outcome (nothing shed: the review has no
    // allowance to fit) and the narration.
    inference::Prompt prompt;
    prompt.messages.push_back({inference::Role::System, std::string(REVIEW_RULES)});
    prompt.messages.push_back({inference::Role::User, "# Player\n" + input + "\n\n" +
                                                          RenderOutcome(proposal, resolution, {}) + "\n# Narration\n" +
                                                          narration + "\n\n# Task\n" +
                                                          std::string(REVIEW_INSTRUCTION)});
    prompt.stableCount = 1;
    return prompt;
}

} // namespace morpheus::turn
```

In `src/turn/CMakeLists.txt`, add `Review.cpp` after `Retrieval.cpp`:

```cmake
add_library(morpheus_turn STATIC
    Folding.cpp
    Replay.cpp
    Resolve.cpp
    ResolveGrammar.cpp
    Retrieval.cpp
    Review.cpp
    Session.cpp
    Worker.cpp
)
```

- [ ] **Step 9: Run the tests to verify they pass**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[grammar],[resolve],[review]"`
Expected: all pass.

Then: `ctest --preset tests`
Expected: all pass. Turns still play as before: nothing calls the review yet, and the first narration's message is unchanged.

- [ ] **Step 10: Commit**

```bash
git add src/turn/ResolveGrammar.h src/turn/ResolveGrammar.cpp src/turn/EngineText.h src/turn/Resolve.h src/turn/Resolve.cpp src/turn/Review.h src/turn/Review.cpp src/turn/CMakeLists.txt src/turn/Session.cpp tests/turn/ResolveGrammarTests.cpp tests/turn/ResolveTests.cpp tests/turn/ReviewTests.cpp
git commit -m "Add the review grammar, parser and prompt, and the renarrate correction" -m "Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>"
```

---
