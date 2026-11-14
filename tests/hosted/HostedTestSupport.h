#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/Settings.h"
#include "fakes/ScriptedBackend.h"
#include "turn/EngineText.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

namespace morpheus::test {

// The actor ids of observation.json: Hulda and Saadia, and Aela, the player.
inline const std::string HULDA = "0001A6A4";
inline const std::string SAADIA = "0001B07C";
inline const std::string AELA = "00000007";

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
            if (last == turn::SUMMARIZE_INSTRUCTION || last == turn::CONDENSE_INSTRUCTION) {
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

} // namespace morpheus::test
