### Task 2: `HostedSession::RunTurn` chooses the speaker

**Files:**
- Modify: `src/turn/HostedSession.h` (`HostedCallbacks`, `RunTurn`'s comment), `src/turn/HostedSession.cpp` (`RunTurn`)
- Modify: `tests/hosted/HostedTestSupport.h` (`IsHostedDecide`, `HostedScript::decisions`, `ScriptOf`)
- Test: `tests/hosted/HostedDecideTests.cpp` (new)

**Interfaces:**
- Consumes: Task 1's `DECIDE_SPEAKER`, `PromptPack::decide`, `HostedInputs::candidates`.
- Produces: `HostedCallbacks::onSpeaker` (`std::function<void(const std::string&)>`); `test::IsHostedDecide(const inference::Prompt&)`; `test::HostedScript::decisions` (`std::vector<std::string>`) and `decided` (`std::size_t`); `ScriptOf` reads a step's `model.decide`.

- [ ] **Step 1: Test support**

`tests/hosted/HostedTestSupport.h`, after `IsHostedCondense`:

```cpp
// Whether `prompt` is a decide call: its tail lists who may speak (the engine's own heading, whatever the pack says).
inline bool IsHostedDecide(const inference::Prompt& prompt)
{
    return !prompt.messages.empty() && prompt.messages.back().content.find("\n# Who may speak\n") != std::string::npos;
}
```

`HostedScript` gains `std::vector<std::string> decisions = {};` after `lines` and `std::size_t decided = 0;` after `spoken`; its comment says decide calls get `decisions` in order. In `Responder()`, after the memory-call check and before `if (!spec.grammar.empty())`:

```cpp
            if (IsHostedDecide(prompt)) {
                MORPHEUS_REQUIRE(decided < decisions.size(), "HostedScript: no decision left");
                return decisions[decided++];
            }
```

`ScriptOf`: when a step's `model` has `"decide"`, push it to `script.decisions` (before the resolve and line pushes).

- [ ] **Step 2: Write the failing tests**

Create `tests/hosted/HostedDecideTests.cpp`:

```cpp
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstddef>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "grammar/Schema.h"
#include "hosted/HostedTestSupport.h"
#include "turn/EngineText.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// A decide answer naming `id`.
std::string Decision(const std::string& id)
{
    return R"({"speaker":")" + id + R"("})";
}

turn::HostedSession OpenRun(const std::string& name, test::ScriptedBackend& backend,
                            const turn::HostData& data = test::FixtureHostData())
{
    return turn::HostedSession::Open(test::HostedRunFile(name), data, test::HostedSettings(), 42, backend);
}

// observation.json without the actor `id`.
nlohmann::json Without(const std::string& id)
{
    nlohmann::json observation = test::FixtureObservation();
    nlohmann::json& actors = observation.at("actors");
    for (auto it = actors.begin(); it != actors.end(); ++it) {
        if (it->at("id") == id) {
            actors.erase(it);
            break;
        }
    }
    return observation;
}

// The decide calls the backend saw.
std::vector<test::ScriptedBackend::Call> DecideCalls(const test::ScriptedBackend& backend)
{
    std::vector<test::ScriptedBackend::Call> calls;
    for (const test::ScriptedBackend::Call& call : backend.Calls()) {
        if (test::IsHostedDecide(call.prompt)) {
            calls.push_back(call);
        }
    }
    return calls;
}

} // namespace

TEST_CASE("Decide chooses among the NPC participants with a model call when more than one may speak",
          "[hosted][decide]")
{
    test::HostedScript script;
    script.decisions = {Decision(test::SAADIA)};
    script.actions = {NONE};
    script.lines = {"Welcome."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_call", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());

    const std::optional<core::HostedTurn> record =
        session.RunTurn(test::Request(id, "decide", "Good evening, both of you."), {}, {});
    REQUIRE(record);
    CHECK(record->speakerId == test::SAADIA);
    CHECK(record->listenerIds == std::vector<std::string>{test::HULDA, test::AELA});
    REQUIRE(record->timings.contains("decide"));
    CHECK_FALSE(record->timings.at("decide").contains("refused"));

    // Three calls: decide, then Saadia's resolve and speak.
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 3);
    const test::ScriptedBackend::Call& decide = calls[0];
    REQUIRE(test::IsHostedDecide(decide.prompt));
    // The grammar offers the candidates' ids only; the answer fits maxTokens (6 tokens for {"speaker":"0001A6A4"} as the
    // fake counts them, plus 8).
    const nlohmann::json schema = {{"type", "object"},
                                   {"properties", {{"speaker", {{"enum", {test::HULDA, test::SAADIA}}}}}},
                                   {"required", nlohmann::json::array({"speaker"})},
                                   {"additionalProperties", false}};
    CHECK(decide.spec.grammar == grammar::FromSchema(schema));
    CHECK(decide.spec.maxTokens == 14);
    CHECK(decide.spec.thinkingMaxTokens == 0);
    // The speakers' system message; a tail with the player's line, the candidates and the decide text; no memories.
    CHECK(decide.prompt.messages[0].content == calls[1].prompt.messages[0].content);
    const std::string& tail = decide.prompt.messages.back().content;
    CHECK_THAT(tail, ContainsSubstring("# Player\nAela: Good evening, both of you.\n"));
    CHECK(tail.ends_with("# Who may speak\n- Hulda (0001A6A4)\n- Saadia (0001B07C)\n\n# Task\n" +
                         std::string(turn::HOSTED_DECIDE_INSTRUCTION)));
    CHECK_THAT(tail, !ContainsSubstring("remembers"));
}

TEST_CASE("With no input the last speaker is left out; with player text or events it may speak again",
          "[hosted][decide]")
{
    test::HostedScript script;
    script.decisions = {Decision(test::SAADIA), Decision(test::HULDA)};
    script.actions = {NONE, NONE, NONE, NONE};
    script.lines = {"One.", "Two.", "Three.", "Four."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_last", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());

    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "Hi."), {}, {}));
    // No input: Hulda spoke last, so Saadia is the one candidate: no decide call.
    const std::optional<core::HostedTurn> second = session.RunTurn(test::Request(id, "decide", ""), {}, {});
    REQUIRE(second);
    CHECK(second->speakerId == test::SAADIA);
    CHECK_FALSE(second->timings.contains("decide"));
    CHECK(DecideCalls(backend).empty());
    // Player text: both may answer, Saadia (the last speaker) included.
    const std::optional<core::HostedTurn> third = session.RunTurn(test::Request(id, "decide", "And you?"), {}, {});
    REQUIRE(third);
    CHECK(third->speakerId == test::SAADIA);
    CHECK(DecideCalls(backend).size() == 1);
    // Events and no player text: both may answer too.
    turn::HostedTurnRequest withEvents = test::Request(id, "decide", "");
    withEvents.events = {{{"text", "A door slams."}}};
    const std::optional<core::HostedTurn> fourth = session.RunTurn(withEvents, {}, {});
    REQUIRE(fourth);
    CHECK(fourth->speakerId == test::HULDA);
    CHECK(DecideCalls(backend).size() == 2);
    CHECK(script.decided == 2);
}

TEST_CASE("Decide keeps the last speaker when no one else may speak, and never offers the player or the absent",
          "[hosted][decide]")
{
    test::HostedScript script;
    script.actions = {NONE, NONE, NONE};
    script.lines = {"One.", "Two.", "Three."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_one", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());

    // Saadia is absent: Hulda is the one candidate, with player text and without.
    turn::HostedTurnRequest request = test::Request(id, "decide", "Hello.");
    request.observation = Without(test::SAADIA);
    const std::optional<core::HostedTurn> first = session.RunTurn(request, {}, {});
    REQUIRE(first);
    CHECK(first->speakerId == test::HULDA);
    request.playerText.reset();
    const std::optional<core::HostedTurn> second = session.RunTurn(request, {}, {});
    REQUIRE(second);
    CHECK(second->speakerId == test::HULDA); // The last speaker, as no one else may speak.
    CHECK(DecideCalls(backend).empty());
}

TEST_CASE("Decide with no NPC participant present is a DataError before any call or record", "[hosted][decide]")
{
    test::HostedScript script; // No reply: a model call would fail the test.
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_none", backend);
    const std::string id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    turn::HostedTurnRequest request = test::Request(id, "decide", "Hello?");
    request.observation = Without(test::HULDA);
    CHECK_THROWS_WITH(session.RunTurn(request, {}, {}), "speaker: \"decide\" needs an NPC participant in the observation");
    CHECK(backend.Calls().empty());
    CHECK(session.Turns(id).empty());
}

TEST_CASE("A refused decide answer goes to the candidate who spoke least recently and keeps the reasons",
          "[hosted][decide]")
{
    test::HostedScript script;
    // The fake ignores grammars: an answer that is not JSON, then one naming the player.
    script.decisions = {"Saadia, I think.", Decision(test::AELA)};
    script.actions = {NONE, NONE, NONE};
    script.lines = {"One.", "Two.", "Three."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_refused", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());

    // Neither spoke yet: the tie goes to the first in join order, Hulda.
    const std::optional<core::HostedTurn> first = session.RunTurn(test::Request(id, "decide", "Hello."), {}, {});
    REQUIRE(first);
    CHECK(first->speakerId == test::HULDA);
    const nlohmann::json& refused = first->timings.at("decide").at("refused");
    REQUIRE(refused.size() == 1);
    CHECK_THAT(refused[0].get<std::string>(), ContainsSubstring("the output is not valid JSON"));

    // Hulda spoke, Saadia never did: Saadia is the least recent.
    const std::optional<core::HostedTurn> second = session.RunTurn(test::Request(id, "decide", "And now?"), {}, {});
    REQUIRE(second);
    CHECK(second->speakerId == test::SAADIA);
    CHECK_THAT(second->timings.at("decide").at("refused")[0].get<std::string>(), ContainsSubstring("/speaker"));
}

TEST_CASE("The decide prompt shows lines whoever witnessed them; the chosen speaker's prompt shows only its own",
          "[hosted][decide]")
{
    test::HostedScript script;
    script.decisions = {Decision(test::SAADIA)};
    script.actions = {NONE, NONE};
    script.lines = {"I will keep it.", "Good evening."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_view", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());

    // Saadia is away while Aela tells Hulda a secret.
    turn::HostedTurnRequest secret = test::Request(id, test::HULDA, "The key is under the mat.");
    secret.observation = Without(test::SAADIA);
    REQUIRE(session.RunTurn(secret, {}, {}));
    REQUIRE(session.RunTurn(test::Request(id, "decide", "Evening, everyone."), {}, {}));

    // The calls of the second turn: decide, then Saadia's resolve and speak.
    const std::vector<test::ScriptedBackend::Call>& calls = backend.Calls();
    REQUIRE(calls.size() == 5);
    REQUIRE(test::IsHostedDecide(calls[2].prompt));
    auto text = [](const inference::Prompt& prompt) {
        std::string all;
        for (const inference::Message& message : prompt.messages) {
            all += message.content + "\n";
        }
        return all;
    };
    CHECK_THAT(text(calls[2].prompt), ContainsSubstring("The key is under the mat."));
    CHECK_THAT(text(calls[3].prompt), !ContainsSubstring("The key is under the mat."));
}

TEST_CASE("A host's decide text replaces the default and fills {player}", "[hosted][decide][prompts]")
{
    test::HostedScript script;
    script.decisions = {Decision(test::HULDA)};
    script.actions = {NONE};
    script.lines = {"Yes?"};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostData data = test::FixtureHostData();
    data.prompts = {{"decide", "Who answers {player} now?"}};
    turn::HostedSession session = OpenRun("hosted_decide_pack", backend, data);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    REQUIRE(session.RunTurn(test::Request(id, "decide", "Anyone?"), {}, {}));
    REQUIRE_FALSE(DecideCalls(backend).empty());
    CHECK(DecideCalls(backend).front().prompt.messages.back().content.ends_with("# Task\nWho answers Aela now?"));
}

TEST_CASE("onSpeaker reports the speaker once, before the action, for a decided and a named speaker",
          "[hosted][decide]")
{
    test::HostedScript script;
    script.decisions = {Decision(test::SAADIA)};
    script.actions = {NONE, NONE};
    script.lines = {"One.", "Two."};
    test::ScriptedBackend backend(16384, script.Responder());
    turn::HostedSession session = OpenRun("hosted_decide_on_speaker", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    std::vector<std::string> order;
    turn::HostedCallbacks callbacks;
    callbacks.onSpeaker = [&order](const std::string& speakerId) { order.push_back("speaker " + speakerId); };
    callbacks.onAction = [&order](const core::Adjudication&) { order.push_back("action"); };
    REQUIRE(session.RunTurn(test::Request(id, "decide", "Hello."), callbacks, {}));
    REQUIRE(session.RunTurn(test::Request(id, test::HULDA, "And you?"), callbacks, {}));
    CHECK(order == std::vector<std::string>{"speaker " + test::SAADIA, "action", "speaker " + test::HULDA, "action"});
}

TEST_CASE("A turn stopped during its decide call commits nothing", "[hosted][decide]")
{
    std::stop_source source;
    test::ScriptedBackend backend(16384, [&source](const inference::Prompt& prompt, const inference::GenerationSpec&) {
        REQUIRE(test::IsHostedDecide(prompt));
        source.request_stop();
        return Decision(test::SAADIA);
    });
    turn::HostedSession session = OpenRun("hosted_decide_stop", backend);
    const std::string id = session.StartConversation({test::HULDA, test::SAADIA, test::AELA}, test::FixtureObservation());
    CHECK_FALSE(session.RunTurn(test::Request(id, "decide", "Hello."), {}, source.get_token()));
    CHECK(backend.Calls().size() == 1);
    CHECK(session.Turns(id).empty());
}
```

- [ ] **Step 3: Run them to see them fail**

Run: `cmake --preset tests && cmake --build --preset tests`
Expected: compile error: `HostedCallbacks` has no `onSpeaker`. (Then, with only that member added, the turns throw `speaker: 'decide' is not a participant ...`.)

- [ ] **Step 4: Implement**

`src/turn/HostedSession.h`, `HostedCallbacks`, first member:

```cpp
    // Once the speaker is known, before the resolve call: the named speaker, or the NPC a "decide" turn chose.
    std::function<void(const std::string&)> onSpeaker = {};
```

`RunTurn`'s comment gains: "With `speakerId` DECIDE_SPEAKER the engine chooses the speaker among the conversation's NPCs present (decide-speaker spec): the last speaker is left out on a turn with no player text and no events while another remains; one candidate is chosen without a call, more with a decide call; a refused decide answer goes to the candidate who spoke least recently, its reasons in `timings.decide.refused`. A DataError "speaker: "decide" needs an NPC participant in the observation" when there is none."

`src/turn/HostedSession.cpp`: include `"context/PromptBuilder.h"` and `<climits>` if not already; add to the anonymous namespace's constants:

```cpp
// The decide call's answer allowance over its longest possible answer, and its seed's attempt (the resolve call's kind,
// apart from the resolve call's attempts).
constexpr int DECIDE_TOKEN_MARGIN = 8;
constexpr int DECIDE_SEED_ATTEMPT = 1 << 22;
```

Replace `RunTurn` from its first line down to (not including) the comment "// The speaker's own memory of this conversation reaches the transcript's cut before it speaks" with:

```cpp
std::optional<core::HostedTurn> HostedSession::RunTurn(const HostedTurnRequest& request,
                                                       const HostedCallbacks& callbacks, std::stop_token stop)
{
    // 0. Check the request: nothing is stored before it passes, and no model call is made before it passes but a
    // "decide" turn's decide call (step 0b). The conversation exists and is open, the observation passes Observe,
    // every event has its text, and the speaker (the host's, or each candidate of a "decide" turn) takes part, is
    // present, is not the player, keeps an allowed action after the actor scopes and has a speak instruction that fits.
    const core::Conversation conversation = OpenConversation(request.conversationId);
    const Observation observation = _world.Observe(request.observation);
    HostedWorld::CheckEvents(request.events);
    const core::Actor& player = observation.Player();
    const std::vector<std::string> allowed = request.allowedActions.value_or(_world.ActionIds());
    // What a speaker must pass before any call: its proposal schema builds (an allowed action is left), and its speak
    // instruction fits narrate_allowance, which the prompt reserves for it; the names come from the host.
    auto checkSpeaker = [&](const core::Actor& actor) {
        _world.ProposalSchema(observation, conversation.participants, allowed, actor.id);
        const std::string speak =
            PromptPack::Fill(_world.Prompts().speak, {.speaker = actor.name, .player = player.name});
        const int speakTokens = _pBackend->CountTokens(speak);
        if (speakTokens > _settings.budget.narrateAllowance) {
            throw core::DataError(std::format("prompts/speak: the speak instruction for '{}' uses {} tokens, over "
                                              "narrate_allowance {}",
                                              actor.name, speakTokens, _settings.budget.narrateAllowance));
        }
    };

    // Player text is cleaned as Session cleans input: control characters other than newline and tab go (they have no
    // place in a prompt), and it is trimmed. Nothing left means no player text.
    std::optional<std::string> playerText;
    if (request.playerText) {
        std::string cleaned = CleanInput(*request.playerText);
        if (!cleaned.empty()) {
            playerText = std::move(cleaned);
        }
    }

    // Run-wide, shared with the summaries' ranges, and never reused: one past every turn stored, hidden ones included.
    const int turn = _store.NextHostedTurn();
    const std::uint64_t seed = _store.Meta().rngSeed;
    const std::vector<core::HostedTurn> played = _store.HostedTurns(conversation.id);
    const int lastTurn = played.empty() ? 0 : played.back().number;

    // The new events: those recorded since the conversation's last turn, then the request's, in order.
    std::vector<std::string> newEvents;
    for (const core::ConversationEvent& recorded : _store.ConversationEvents(conversation.id)) {
        if (recorded.afterTurn == lastTurn) {
            newEvents.push_back(recorded.event.at("text").get<std::string>());
        }
    }
    for (const nlohmann::json& event : request.events) {
        newEvents.push_back(event.at("text").get<std::string>());
    }

    std::string speakerId = request.speakerId;
    nlohmann::json decideTelemetry = nullptr; // The decide call's telemetry, when one ran (step 0b).
    if (speakerId != DECIDE_SPEAKER) {
        // 0a. The host's speaker takes part, is present and is not the player.
        const bool participates =
            std::any_of(conversation.participants.begin(), conversation.participants.end(),
                        [&](const core::Participant& participant) { return participant.id == speakerId; });
        if (!participates) {
            throw core::DataError(
                std::format("speaker: '{}' is not a participant of conversation '{}'", speakerId, conversation.id));
        }
        const core::Actor* pNamed = observation.Find(speakerId);
        if (pNamed == nullptr) {
            throw core::DataError(std::format("speaker: '{}' is not an actor of the observation", speakerId));
        }
        if (pNamed->isPlayer) {
            throw core::DataError(std::format("speaker: '{}' is the player; the host speaks for the player", speakerId));
        }
        checkSpeaker(*pNamed);
    } else {
        // 0b. The engine chooses (decide-speaker spec). The candidates: the NPC participants present, in join order;
        // on a turn with no input the last speaker is left out while another remains.
        std::vector<const core::Actor*> candidates;
        for (const core::Participant& participant : conversation.participants) {
            const core::Actor* pActor = observation.Find(participant.id);
            if (pActor != nullptr && !pActor->isPlayer) {
                candidates.push_back(pActor);
            }
        }
        const bool noInput = !playerText && request.events.empty();
        if (noInput && !played.empty() && candidates.size() > 1) {
            const std::string& last = played.back().speakerId;
            const bool lastIsCandidate = std::any_of(candidates.begin(), candidates.end(),
                                                     [&](const core::Actor* pActor) { return pActor->id == last; });
            if (lastIsCandidate) {
                std::erase_if(candidates, [&](const core::Actor* pActor) { return pActor->id == last; });
            }
        }
        if (candidates.empty()) {
            throw core::DataError("speaker: \"decide\" needs an NPC participant in the observation");
        }
        // Every candidate passes the speaker's checks, so nothing after the decide call fails for the request.
        for (const core::Actor* pCandidate : candidates) {
            checkSpeaker(*pCandidate);
        }

        if (candidates.size() == 1) {
            speakerId = candidates.front()->id;
        } else {
            // The decide prompt: the speakers' system message, every visible line of the conversation from its folded
            // mark whoever witnessed it, and a tail with the observation, the new events, the player's line, who may
            // speak and the pack's decide text. No memories and no retrieval: it is no NPC's view.
            context::HostedInputs inputs;
            inputs.systemText = _world.Prompts().system;
            inputs.lore = _world.Lore();
            inputs.participants = conversation.participants;
            inputs.transcript = Entries(conversation, played, conversation.foldedThrough, _store.LatestHostedTurn(), "");
            inputs.observation = observation.json;
            inputs.events = newEvents;
            inputs.playerLine = playerText ? player.name + ": " + *playerText : std::string();
            for (const core::Actor* pCandidate : candidates) {
                inputs.candidates.push_back(std::format("{} ({})", pCandidate->name, pCandidate->id));
            }
            inputs.instruction = PromptPack::Fill(_world.Prompts().decide, {.player = player.name});
            // The oldest lines leave until it fits: the decide view is no NPC's memory, so nothing folds.
            context::Assembly decideAssembly = context::AssembleHostedPrompt(inputs, _settings.budget, *_pBackend);
            while ((decideAssembly.transcriptOverBudget || decideAssembly.contextOverflow) &&
                   !inputs.transcript.empty()) {
                inputs.transcript.erase(inputs.transcript.begin());
                decideAssembly = context::AssembleHostedPrompt(inputs, _settings.budget, *_pBackend);
            }
            if (!decideAssembly.Fits()) {
                throw TurnError(std::format("the decide prompt does not fit: its tail uses {} tokens, tail_allowance {}",
                                            decideAssembly.tokens.tail, _settings.budget.tailAllowance));
            }

            // The answer's schema: one key, the candidates' ids. Its allowance: the longest answer, and a margin.
            nlohmann::json ids = nlohmann::json::array();
            int longestAnswer = 0;
            for (const core::Actor* pCandidate : candidates) {
                ids.push_back(pCandidate->id);
                longestAnswer =
                    std::max(longestAnswer, _pBackend->CountTokens(R"({"speaker":")" + pCandidate->id + R"("})"));
            }
            const nlohmann::json schema = {{"type", "object"},
                                           {"properties", {{"speaker", {{"enum", ids}}}}},
                                           {"required", nlohmann::json::array({"speaker"})},
                                           {"additionalProperties", false}};
            inference::GenerationSpec decideSpec;
            decideSpec.grammar = grammar::FromSchema(schema);
            decideSpec.maxTokens = longestAnswer + DECIDE_TOKEN_MARGIN;
            decideSpec.sampling = SamplingFrom(_settings.resolveSampling,
                                               core::SamplingSeed(seed, turn, core::CallKind::Resolve, DECIDE_SEED_ATTEMPT));
            const inference::Completion decided = _pBackend->Complete(decideAssembly.prompt, decideSpec, {}, stop);
            if (decided.cancelled) {
                return std::nullopt;
            }
            decideTelemetry = CallTelemetry(decided, decideSpec);

            // A valid answer names the speaker. Any other is the model's mistake, not an error: the candidate who
            // spoke least recently speaks (one who never spoke is the least recent; ties go to candidate order), and
            // the reasons are kept.
            std::vector<std::string> reasons;
            nlohmann::json answer;
            try {
                answer = nlohmann::json::parse(decided.text);
            } catch (const nlohmann::json::exception& error) {
                reasons.push_back(std::format("the output is not valid JSON: {}", error.what()));
            }
            if (reasons.empty()) {
                for (const grammar::ValidationError& error : grammar::Validate(answer, schema)) {
                    reasons.push_back(
                        std::format("{}: {}", error.path.empty() ? "the output" : error.path, error.message));
                }
            }
            if (reasons.empty()) {
                speakerId = answer.at("speaker").get<std::string>();
            } else {
                const core::Actor* pLeast = nullptr;
                int leastAt = 0;
                for (const core::Actor* pCandidate : candidates) {
                    int at = -1; // The index in `played` of its latest turn; -1 when it never spoke.
                    for (std::size_t i = played.size(); i-- > 0;) {
                        if (played[i].speakerId == pCandidate->id) {
                            at = static_cast<int>(i);
                            break;
                        }
                    }
                    if (pLeast == nullptr || at < leastAt) {
                        pLeast = pCandidate;
                        leastAt = at;
                    }
                }
                speakerId = pLeast->id;
                decideTelemetry["refused"] = reasons;
            }
        }
    }

    // The speaker is known: the host hears of it before the resolve call.
    const core::Actor* pSpeaker = observation.Find(speakerId);
    MORPHEUS_REQUIRE(pSpeaker != nullptr, "HostedSession::RunTurn: the speaker passed its checks, so it is present");
    if (callbacks.onSpeaker) {
        callbacks.onSpeaker(speakerId);
    }
    const nlohmann::json proposalSchema =
        _world.ProposalSchema(observation, conversation.participants, allowed, speakerId);
    // The resolve call's grammar is that schema's (HostedWorld::ProposalGrammar), built once here with the schema.
    const std::string proposalGrammar = grammar::FromSchema(proposalSchema);
    // The instructions name the speaker and the player (the run's pack); checkSpeaker checked the speak instruction.
    const PromptVariables names{.speaker = pSpeaker->name, .player = player.name};
    const std::string resolveInstruction = PromptPack::Fill(_world.Prompts().resolve, names);
    const std::string speakMessage = PromptPack::Fill(_world.Prompts().speak, names);

```

In the rest of `RunTurn`, every `request.speakerId` becomes `speakerId` (the `CatchUp` call, `Retrieve`'s witness, `speakerPass`'s `Pass` and `Entries` calls, `Upkeep`, `record.speakerId`, the listeners loop). Delete the old duplicated blocks the replacement already holds (the player-text cleaning, `turn`/`seed`/`played`/`lastTurn`, the new events, the old speak-instruction check). After `timings["speak"] = CallTelemetry(spoken, speakSpec);` add:

```cpp
    if (!decideTelemetry.is_null()) {
        timings["decide"] = decideTelemetry; // The decide call, and why its answer was refused, if it was.
    }
```

If `TurnError` is not visible in HostedSession.cpp, include the header that declares it (grep for `class TurnError`).

- [ ] **Step 5: Run the tests**

Run: `cmake --build --preset tests && ./build/tests/bin/Debug/morpheus_tests.exe "[decide]"`, then `./build/tests/bin/Debug/morpheus_tests.exe "[hosted]"`, then `ctest --preset tests`
Expected: all pass. The checks moved before the speaker checks (events) can change which DataError a request with several faults reports first: if an `invalid_requests.json` entry or an existing test expects the old first fault, report it and update the expectation only if its request has two faults (name each such change in the report).

- [ ] **Step 6: Commit**

```bash
git add src/turn/HostedSession.h src/turn/HostedSession.cpp tests/hosted/HostedTestSupport.h tests/hosted/HostedDecideTests.cpp
git commit -m "HostedSession: a decide turn chooses its speaker among the NPCs present, by a decide call when several may speak"
```

---

