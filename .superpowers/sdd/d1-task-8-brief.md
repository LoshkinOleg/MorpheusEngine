### Task 8: Folding in hosted runs: over budget, and `EndConversation` plus `Maintain`

When a conversation's L3 is over its budget, Task 7's fit loop already folds its oldest turns through the shared upkeep; this task pins that with a test and adds `Maintain`, which folds every remaining turn of each ended conversation (in one call when it fits), condensing on the way, so a later conversation sees the summary in L2 and retrieves the folded turns.

**Files:**
- Modify: `src/turn/HostedSession.h`, `src/turn/HostedSession.cpp`
- Create: `tests/hosted/HostedFoldingTests.cpp`

**Interfaces:**
- Consumes: `MemoryUpkeep::FoldAll` (Task 3); `HostedSession::Pass`, `Upkeep` (Task 7); `HostedTestSupport.h` (Tasks 4, 6, 7).
- Produces: `bool HostedSession::Maintain(std::stop_token stop)`.

- [ ] **Step 1: Write the failing tests**

Create `tests/hosted/HostedFoldingTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "turn/HostedSession.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// For a context of 2600 with lines of 2400 bytes. FakeBackend counts 1 token per 4 bytes: the hosted system message
// is about 350 tokens, so L3 gets about 1400 while each turn adds about 615. Two turns fit the conversation, three do
// not. The tail (the observation is about 190 tokens) stays well within its 600.
core::Settings FoldingSettings()
{
    core::Settings settings = test::HostedSettings();
    settings.budget = {64, 128, 64, 600, 1024, 1024, 512, 100};
    return settings;
}

} // namespace

TEST_CASE("A conversation over its transcript budget folds its oldest turns during a turn", "[hosted][folding]")
{
    test::HostedScript script;
    for (int i = 0; i < 6; ++i) {
        script.actions.push_back(NONE);
        script.lines.push_back(std::string(2400, 'w'));
    }
    test::ScriptedBackend backend(2600, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fold_budget"),
                                                            test::FixtureHostData(), FoldingSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    for (int i = 0; i < 6; ++i) {
        REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Go on."), {}, {}));
        CHECK(session.LastAssembly().Fits());
    }

    // The oldest turns went into run-wide summaries, and the conversation's folded mark moved with them.
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE_FALSE(summaries.empty());
    CHECK(summaries.front().fromTurn == 1);
    CHECK(session.Conversation(id)->foldedThrough >= 1);
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    CHECK(prompt.stableCount < 1 + 2 * 5);
    CHECK_THAT(prompt.messages.front().content, ContainsSubstring("# Story so far\nTurns 1-"));
}

TEST_CASE("EndConversation and Maintain fold the rest, and a later conversation retrieves it", "[hosted][folding]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"The silver key is under the bar.", "Nobody else knows about the silver key.", "Under the bar."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fold_end"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string first = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(first, test::HULDA, "Where did you hide the silver key?"), {}, {}));
    REQUIRE(session.RunTurn(test::Request(first, test::HULDA, "Does anyone else know?"), {}, {}));

    // While the conversation is open, Maintain leaves it alone.
    CHECK(session.Maintain({}));
    CHECK(session.Summaries().empty());

    // Ended, its remaining turns fold in one call.
    session.EndConversation(first);
    CHECK(session.Maintain({}));
    const std::vector<core::Summary> summaries = session.Summaries();
    REQUIRE(summaries.size() == 1);
    CHECK(summaries[0].fromTurn == 1);
    CHECK(summaries[0].toTurn == 2);
    CHECK(summaries[0].text == "Summary.");
    CHECK(session.Conversation(first)->foldedThrough == 2);
    const std::size_t calls = backend.Calls().size();
    CHECK(session.Maintain({})); // Nothing is left to fold.
    CHECK(backend.Calls().size() == calls);

    // A later conversation sees the summary in L2, starts with an empty L3, and retrieves the folded turns.
    const std::string second = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(second, test::HULDA, "Remind me where the silver key is."), {}, {}));
    const inference::Prompt& prompt = session.LastAssembly().prompt;
    CHECK_THAT(prompt.messages.front().content, ContainsSubstring("# Story so far\nTurns 1-2: Summary.\n"));
    CHECK(prompt.stableCount == 1);
    CHECK_THAT(prompt.messages.back().content,
               ContainsSubstring("\n- Turn 1: Aela: Where did you hide the silver key? Hulda [none]: The silver key "
                                 "is under the bar.\n"));
}

TEST_CASE("A cancelled Maintain folds nothing and can simply run again", "[hosted][folding]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE};
    script.lines = {"One.", "Two."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = turn::HostedSession::Open(test::HostedRunFile("hosted_fold_cancel"),
                                                            test::FixtureHostData(), test::HostedSettings(), 42, backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "First."), {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Second."), {}, {}));
    session.EndConversation(id);

    std::stop_source stop;
    stop.request_stop();
    CHECK_FALSE(session.Maintain(stop.get_token()));
    CHECK(session.Summaries().empty());
    CHECK(session.Conversation(id)->foldedThrough == 0);

    CHECK(session.Maintain({}));
    REQUIRE(session.Summaries().size() == 1);
    CHECK(session.Conversation(id)->foldedThrough == 2);
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build --preset tests`
Expected: compile error `'Maintain': is not a member of 'morpheus::turn::HostedSession'` in `tests/hosted/HostedFoldingTests.cpp`. (The first test needs no new code: Task 7's fit loop folds; it pins that behaviour.)

- [ ] **Step 3: Write Maintain**

In `src/turn/HostedSession.h`, after the `EndConversation` declaration (its line `    void EndConversation(std::string_view conversationId);`), insert:

```cpp

    // Between-turn upkeep, through the shared memory upkeep: folds every remaining turn of each ended conversation into
    // summaries (in one call when it fits), condensing summaries over their cap on the way, and indexes the folded
    // turns for retrieval. Returns false when `stop` cancelled it; it can simply run again. Throws DataError when
    // summary_cap is too small for even a condensed summary. Open conversations are left to their turns' fit loop.
    bool Maintain(std::stop_token stop);
```

In `src/turn/HostedSession.cpp`, replace

```cpp
// A hosted turn retrieves from the host's lore and the run's folded turns; a hosted run has no notes.
```

with

```cpp
// The task line of the placeholder tail Maintain measures an ended conversation's prompt with.
constexpr std::string_view MAINTENANCE_INSTRUCTION = "(the conversation's next turn)";
// A hosted turn retrieves from the host's lore and the run's folded turns; a hosted run has no notes.
```

and after the definition of `HostedSession::EndConversation`, insert:

```cpp

bool HostedSession::Maintain(std::stop_token stop)
{
    // Every ended conversation with turns still in L3 is folded whole. Its seeds name the run's latest turn.
    const int latest = _store.LatestHostedTurn();
    for (const core::Conversation& conversation : _store.Conversations()) {
        if (!conversation.ended) {
            continue;
        }
        const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
        if (played.empty() || played.back().number <= conversation.foldedThrough) {
            continue;
        }
        // The prompt the conversation's next turn would start with: its last observation and a placeholder task stand
        // in for the tail, which no fold reads.
        context::HostedInputs tail;
        tail.observation = played.back().observation;
        tail.instruction = std::string(MAINTENANCE_INSTRUCTION);
        auto assemble = [&](const std::vector<RetrievalHit>&) { return Pass(conversation.id, {}, tail); };
        // Each fold's summary is stored and its turns indexed as it lands (Upkeep's StoreFold).
        if (!Upkeep(conversation.id, latest).FoldAll(assemble, stop)) {
            return false;
        }
    }
    return true;
}
```

- [ ] **Step 4: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`
Expected: builds with no warnings; `All tests passed`.

Run: `ctest --preset tests`
Expected: `100% tests passed, 0 tests failed out of 485` (482 + 3), the three `[server]` tests "Not Run (Skipped)".

- [ ] **Step 5: Byte check and commit**

```bash
FILES="src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/HostedFoldingTests.cpp"
LC_ALL=C grep -c $'[\x80-\xFF]' $FILES
LC_ALL=C grep -c $'[\x01-\x08\x0B\x0C\x0E-\x1F\x7F]' $FILES
git add $FILES
git commit -m "HostedSession::Maintain folds ended conversations through the shared upkeep; folding tests for hosted runs" -m "Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>"
```

Expected: every count printed is `0`.

---

