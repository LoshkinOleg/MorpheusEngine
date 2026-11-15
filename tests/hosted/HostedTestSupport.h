#pragma once

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

namespace morpheus::test {

// The actor ids of observation.json: Hulda and Saadia, and Aela, the player.
inline const std::string HULDA = "0001A6A4";
inline const std::string SAADIA = "0001B07C";
inline const std::string AELA = "00000007";
// Belethor, the third NPC of three_actors.json.
inline const std::string BELETHOR = "0001B07D";

// tests/hosted/fixtures in the source tree: host data and requests written from section 5 of the integration
// requirements. The host's recorded fixtures can replace them later.
inline std::filesystem::path HostedFixturesDir()
{
    return RepoDir() / "tests" / "hosted" / "fixtures";
}

// The fixture `name`, parsed. A fixture that cannot be read is a broken test: it throws.
inline nlohmann::json ReadFixture(std::string_view name)
{
    std::ifstream stream(HostedFixturesDir() / name, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("ReadFixture: cannot open '" + std::string(name) + "'");
    }
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    return nlohmann::json::parse(text);
}

// open.json: the host data a host sends when it opens a run.
inline turn::HostData FixtureHostData()
{
    return turn::HostDataFromJson(ReadFixture("open.json"));
}

// observation.json: Hulda and Saadia at the inn, and Aela, the player.
inline nlohmann::json FixtureObservation()
{
    return ReadFixture("observation.json");
}

// Settings for hosted session tests: a context of 16384 with roomy budgets (resolve_max_tokens 64, narrate_max_tokens
// 256, narrate_allowance 128, tail_allowance 1024, engine_rules_cap 1024, world_cap 1024, summary_cap 512,
// retrieval_cap 256) and no thinking.
inline core::Settings HostedSettings()
{
    core::Settings settings;
    settings.chatModel = "models/chat.gguf"; // Only named in the run meta; never opened.
    settings.embeddingModel = "models/embed.gguf";
    settings.embeddingQueryPrefix = "search_query: ";
    settings.embeddingDocumentPrefix = "search_document: ";
    settings.contextSize = 16384;
    settings.budget = {64, 256, 128, 1024, 1024, 1024, 512, 256};
    settings.resolveSampling = {0.3, 40, 0.9};
    settings.narrateSampling = {0.8, 40, 0.95};
    return settings;
}

// A run file in a fresh folder for test `name`.
inline std::filesystem::path HostedRunFile(std::string_view name)
{
    return FreshTempDir(name) / "run.db";
}

// Whether `prompt` is a hosted fold call: its last message is HOSTED_SUMMARIZE_INSTRUCTION with an NPC's name filled
// in.
inline bool IsHostedFold(const inference::Prompt& prompt)
{
    const std::string_view text = turn::HOSTED_SUMMARIZE_INSTRUCTION;
    return !prompt.messages.empty() && prompt.messages.back().content.starts_with(text.substr(0, text.find("{actor}")));
}

// Whether `prompt` is a hosted condense call: its last message ends with HOSTED_CONDENSE_INSTRUCTION with an NPC's
// name filled in (after the memories it condenses).
inline bool IsHostedCondense(const inference::Prompt& prompt)
{
    const std::string_view text = turn::HOSTED_CONDENSE_INSTRUCTION;
    const std::string_view ending = text.substr(text.rfind("{actor}") + std::string_view("{actor}").size());
    return !prompt.messages.empty() && prompt.messages.back().content.ends_with(ending);
}

// A scripted model for hosted turns. Resolve calls (they carry the proposal grammar) get `actions` in order, speak
// calls (no grammar) get `lines` in order, and fold and condense calls "Summary.". Running out of replies is a test
// bug. The script must outlive the backends that call it.
struct HostedScript {
    std::vector<std::string> actions = {};
    std::vector<std::string> lines = {};
    std::size_t acted = 0;
    std::size_t spoken = 0;

    ScriptedBackend::Responder Responder()
    {
        return [this](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
            const std::string& last = prompt.messages.back().content;
            // Memory calls: the owned game's texts, and the hosted ones (which name the NPC).
            if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION || IsHostedFold(prompt) ||
                IsHostedCondense(prompt)) {
                return "Summary.";
            }
            if (!spec.grammar.empty()) {
                MORPHEUS_REQUIRE(acted < actions.size(), "HostedScript: no action left");
                return actions[acted++];
            }
            MORPHEUS_REQUIRE(spoken < lines.size(), "HostedScript: no line left");
            return lines[spoken++];
        };
    }
};

// A request in conversation `conversationId`: `speakerId` speaks after `playerText` (no player text when it is
// empty), in observation.json, with every action of the catalog allowed.
inline turn::HostedTurnRequest Request(const std::string& conversationId, const std::string& speakerId,
                                       const std::string& playerText)
{
    turn::HostedTurnRequest request;
    request.conversationId = conversationId;
    request.speakerId = speakerId;
    if (!playerText.empty()) {
        request.playerText = playerText;
    }
    request.observation = FixtureObservation();
    return request;
}

// A scripted model for memory tests: resolve and speak calls as HostedScript answers them, and memory calls echo what
// they read, so a memory holds exactly the lines its NPC witnessed. A fold call (IsHostedFold) answers the assistant
// messages of the turns it folds, joined by " "; a condense call (IsHostedCondense) answers its memory lines ("- ..."
// in its last message, without the "- " and any "(told by X) "), joined by " ".
inline ScriptedBackend::Responder MemoryEchoResponder(HostedScript& script)
{
    return [&script](const inference::Prompt& prompt, const inference::GenerationSpec& spec) -> std::string {
        if (IsHostedFold(prompt)) {
            // The folded turns sit between the system message and the instruction.
            std::string echoed;
            for (std::size_t k = 1; k + 1 < prompt.messages.size(); ++k) {
                if (prompt.messages[k].role == inference::Role::Assistant) {
                    echoed += (echoed.empty() ? "" : " ") + prompt.messages[k].content;
                }
            }
            return echoed;
        }
        if (IsHostedCondense(prompt)) {
            // The first memory line follows the heading line (RenderMemories), so every memory line starts after a
            // newline.
            const std::string& last = prompt.messages.back().content;
            std::string echoed;
            std::size_t at = 0;
            while ((at = last.find("\n- ", at)) != std::string::npos) {
                const std::size_t end = last.find('\n', at + 1);
                std::string line = last.substr(at + 3, end - (at + 3));
                if (line.starts_with("(told by ")) {
                    line = line.substr(line.find(") ") + 2);
                }
                echoed += (echoed.empty() ? "" : " ") + line;
                at = end;
            }
            return echoed;
        }
        return script.Responder()(prompt, spec);
    };
}

// The model replies of memory fixture `fixture`, in step order: each step's "model" gives a resolve reply and a line.
inline HostedScript ScriptOf(const nlohmann::json& fixture)
{
    HostedScript script;
    for (const nlohmann::json& step : fixture.at("steps")) {
        if (step.contains("model")) {
            script.actions.push_back(step.at("model").at("resolve").get<std::string>());
            script.lines.push_back(step.at("model").at("line").get<std::string>());
        }
    }
    return script;
}

// Plays memory fixture `name` (format: fixtures/README.md, "Memory fixtures") on a fresh run on `backend` with
// `settings`, checking each step's expectations. With `echoedMemories` true the backend echoes what it witnessed
// (MemoryEchoResponder), so the memories' texts are known and every expectation is checked. With it false a real
// model writes the memories and the lines, and only what cannot depend on its words is checked: every prompt_absent,
// every memories absent and count, a share's pending, the rewind, turns and conversation steps, and that each turn
// commits. prompt_contains, memories contains,
// told_by and a share's shared are skipped, and the fixture's "model" replies are ignored. A prompt_absent or memories
// absent failing then means a real leak, or a model that wrote the text unprompted where its NPC never heard it.
inline void PlayMemoryFixture(const std::string& name, inference::LlmBackend& backend, const core::Settings& settings,
                              bool echoedMemories)
{
    const nlohmann::json fixture = ReadFixture(name);
    const nlohmann::json& observation = fixture.at("observation");
    // A fixture with a "clock" names the pointer of the game time in its observations: the host data's clock; with
    // "prompts", the host data's pack.
    turn::HostData data = FixtureHostData();
    if (fixture.contains("clock")) {
        data.clock = fixture.at("clock").get<std::string>();
    }
    // A fixture with "prompts" sends that pack with the host data.
    if (fixture.contains("prompts")) {
        data.prompts = fixture.at("prompts").get<std::map<std::string, std::string>>();
    }
    turn::HostedSession session =
        turn::HostedSession::Open(HostedRunFile("hosted_memory_fixture_" + name), data, settings, 42, backend);
    // A step's observation: the fixture's, with the step's "clock" (when it has one) written at the clock's pointer.
    auto observationOf = [&](const nlohmann::json& step) {
        nlohmann::json observed = observation;
        if (step.contains("clock")) {
            observed[nlohmann::json::json_pointer(fixture.at("clock").get<std::string>())] = step.at("clock");
        }
        return observed;
    };
    std::string id = session.StartConversation(fixture.at("participants").get<std::vector<std::string>>(), observation);
    const std::string firstId = id; // A share step's default conversation.

    for (std::size_t i = 0; i < fixture.at("steps").size(); ++i) {
        const nlohmann::json& step = fixture.at("steps")[i];
        INFO(name << " step " << i);
        if (step.contains("turn")) {
            const nlohmann::json& turnJson = step.at("turn");
            turn::HostedTurnRequest request;
            request.conversationId = id;
            request.speakerId = turnJson.at("speaker").get<std::string>();
            if (turnJson.contains("player_text")) {
                request.playerText = turnJson.at("player_text").get<std::string>();
            }
            request.observation = observationOf(step);
            REQUIRE(session.RunTurn(request, {}, {}));
            // Every message of the turn's resolve prompt, for the expectations.
            std::string prompt;
            for (const inference::Message& message : session.LastAssembly().prompt.messages) {
                prompt += message.content + "\n";
            }
            const nlohmann::json expect = step.value("expect", nlohmann::json::object());
            for (const nlohmann::json& absent : expect.value("prompt_absent", nlohmann::json::array())) {
                CHECK_THAT(prompt, !Catch::Matchers::ContainsSubstring(absent.get<std::string>()));
            }
            if (echoedMemories) {
                for (const nlohmann::json& present : expect.value("prompt_contains", nlohmann::json::array())) {
                    CHECK_THAT(prompt, Catch::Matchers::ContainsSubstring(present.get<std::string>()));
                }
            }
        } else if (step.contains("leave")) {
            session.SetParticipants(id, {}, step.at("leave").get<std::vector<std::string>>(), observationOf(step));
        } else if (step.contains("join")) {
            session.SetParticipants(id, step.at("join").get<std::vector<std::string>>(), {}, observationOf(step));
        } else if (step.contains("end")) {
            session.EndConversation(id);
        } else if (step.contains("maintain")) {
            REQUIRE(session.Maintain({}));
        } else if (step.contains("memories")) {
            // Every summary's text of the actor, superseded included, one per line.
            const nlohmann::json& check = step.at("memories");
            std::string texts;
            const std::vector<core::Summary> memories = session.Memories(check.at("actor").get<std::string>());
            for (const core::Summary& memory : memories) {
                texts += memory.text + "\n";
            }
            if (echoedMemories) {
                for (const nlohmann::json& present : check.value("contains", nlohmann::json::array())) {
                    CHECK_THAT(texts, Catch::Matchers::ContainsSubstring(present.get<std::string>()));
                }
            }
            for (const nlohmann::json& absent : check.value("absent", nlohmann::json::array())) {
                CHECK_THAT(texts, !Catch::Matchers::ContainsSubstring(absent.get<std::string>()));
            }
            if (check.contains("count")) {
                CHECK(memories.size() == check.at("count").get<std::size_t>());
            }
            // For each substring, the first memory that holds it is told by that teller (known words only). The
            // checks are a local: items() of a temporary would dangle in the range-for.
            const nlohmann::json toldBy = echoedMemories ? check.value("told_by", nlohmann::json::object())
                                                         : nlohmann::json::object();
            for (const auto& item : toldBy.items()) {
                const std::string substring = item.key();
                const auto it = std::find_if(memories.begin(), memories.end(), [&substring](const core::Summary& memory) {
                    return memory.text.find(substring) != std::string::npos;
                });
                REQUIRE(it != memories.end());
                CHECK(it->toldBy == item.value().get<std::string>());
            }
        } else if (step.contains("start")) {
            id = session.StartConversation(step.at("start").get<std::vector<std::string>>(), observationOf(step));
        } else if (step.contains("share")) {
            // A share of the teller's memories of a conversation (the first one by default), its result checked when
            // the step gives it.
            const nlohmann::json& share = step.at("share");
            const turn::ShareResult result =
                session.ShareMemories(share.at("from").get<std::string>(), share.at("to").get<std::string>(),
                                      share.value("conversation", firstId));
            if (share.contains("shared") && echoedMemories) {
                CHECK(result.shared == share.at("shared").get<int>());
            }
            if (share.contains("pending")) {
                CHECK(result.pending == share.at("pending").get<bool>());
            }
        } else if (step.contains("rewind")) {
            // The run as it was at a game time (a save load), the result checked where the step gives it.
            const nlohmann::json rewind = step.at("rewind");
            const turn::RewindResult result = session.Rewind(rewind.at("game_time").get<double>());
            if (rewind.contains("rewound")) {
                CHECK(result.rewound == rewind.at("rewound").get<bool>());
            }
            if (rewind.contains("hidden_turns")) {
                CHECK(result.hiddenTurns == rewind.at("hidden_turns").get<int>());
            }
            if (rewind.contains("restored_turns")) {
                CHECK(result.restoredTurns == rewind.at("restored_turns").get<int>());
            }
        } else if (step.contains("turns")) {
            // The current conversation's visible turns.
            CHECK(session.Turns(id).size() == step.at("turns").at("count").get<std::size_t>());
        } else if (step.contains("conversation")) {
            // The current conversation as it is now: its end, its participants in join order, its visible events.
            const nlohmann::json expected = step.at("conversation");
            const std::optional<core::Conversation> conversation = session.Conversation(id);
            REQUIRE(conversation);
            if (expected.contains("ended")) {
                CHECK(conversation->ended == expected.at("ended").get<bool>());
            }
            if (expected.contains("participants")) {
                std::vector<std::string> ids;
                for (const core::Participant& participant : conversation->participants) {
                    ids.push_back(participant.id);
                }
                CHECK(ids == expected.at("participants").get<std::vector<std::string>>());
            }
            if (expected.contains("events")) {
                CHECK(session.Events(id).size() == expected.at("events").get<std::size_t>());
            }
        } else if (step.contains("events")) {
            // Events recorded between turns of the current conversation.
            session.RecordEvents(id, step.at("events").get<std::vector<nlohmann::json>>());
        } else {
            FAIL("unknown step kind");
        }
    }
}

// Plays memory fixture `name` on a fresh run with the echo responder, checking every expectation.
inline void PlayMemoryFixture(const std::string& name)
{
    HostedScript script = ScriptOf(ReadFixture(name)); // The fixture's model replies, in step order.
    ScriptedBackend backend(16384, MemoryEchoResponder(script));
    PlayMemoryFixture(name, backend, HostedSettings(), true);
}

} // namespace morpheus::test
