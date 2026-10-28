#include "turn/Resolve.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <string>
#include <utility>
#include <vector>

#include "context/Transcript.h"
#include "core/Dice.h"
#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"

namespace morpheus::turn {

ResolveProposal ParseResolveOutput(std::string_view text)
{
    // Every unreadable output ends here: nothing is kept, only the reason.
    auto failed = [](std::string why) {
        ResolveProposal proposal;
        proposal.error = std::move(why);
        return proposal;
    };
    // Parse without exceptions: a cut-off or malformed output is a model mistake, not an engine bug.
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded()) {
        return failed("the output is not complete JSON; it may have hit resolve_max_tokens");
    }
    if (!json.is_object() || !json.contains("intent") || !json.at("intent").is_string()) {
        return failed("'intent' must be a string");
    }
    if (!json.contains("checks") || !json.at("checks").is_array()) {
        return failed("'checks' must be an array");
    }
    if (!json.contains("changes") || !json.at("changes").is_array()) {
        return failed("'changes' must be an array");
    }

    // The top level is well-formed; read the intent and changes, then check each check's shape.
    ResolveProposal proposal;
    proposal.intent = json.at("intent").get<std::string>();
    proposal.changes = json.at("changes");
    const nlohmann::json& checks = json.at("checks");
    for (std::size_t i = 0; i < checks.size(); ++i) {
        const nlohmann::json& check = checks[i];
        auto has = [&check](const char* key, bool string) {
            return check.contains(key) && (string ? check.at(key).is_string() : check.at(key).is_array());
        };
        if (!check.is_object() || !has("stat", true) || !has("difficulty", true) || !has("stakes", true) ||
            !has("on_success", false) || !has("on_failure", false)) {
            return failed(std::format("check {} needs strings 'stat', 'difficulty' and 'stakes' and arrays "
                                      "'on_success' and 'on_failure'",
                                      i + 1));
        }
        proposal.checks.push_back({check.at("stat").get<std::string>(), check.at("difficulty").get<std::string>(),
                                   check.at("stakes").get<std::string>(), check.at("on_success"),
                                   check.at("on_failure")});
    }
    // Notes are optional (assisted mode only).
    if (json.contains("notes")) {
        const nlohmann::json& notes = json.at("notes");
        if (!notes.is_array() || !std::all_of(notes.begin(), notes.end(), [](const auto& n) { return n.is_string(); })) {
            return failed("'notes' must be an array of strings");
        }
        for (const nlohmann::json& note : notes) {
            proposal.notes.push_back(note.get<std::string>());
        }
    }
    return proposal;
}

Resolution ApplyProposal(const core::World& world, const nlohmann::json& state, const ResolveProposal& proposal,
                         std::uint64_t runSeed, int turn)
{
    MORPHEUS_REQUIRE(state.contains(world.checkEntity), "ApplyProposal: the state has no check entity");
    const nlohmann::json& statFields = state.at(world.checkEntity).at("fields");

    Resolution resolution;
    nlohmann::json changes = proposal.changes;
    std::vector<core::RejectedChange> badChecks;
    for (std::size_t i = 0; i < proposal.checks.size(); ++i) {
        const ProposedCheck& check = proposal.checks[i];
        const bool knownStat = std::find(world.stats.begin(), world.stats.end(), check.stat) != world.stats.end();
        const auto difficulty = world.difficulties.find(check.difficulty);
        if (!knownStat || difficulty == world.difficulties.end()) {
            const nlohmann::json raw = {{"stat", check.stat}, {"difficulty", check.difficulty}, {"stakes", check.stakes}};
            badChecks.push_back({raw, !knownStat ? std::format("unknown stat '{}'", check.stat)
                                                 : std::format("unknown difficulty '{}'", check.difficulty)});
            continue;
        }
        // World loading guarantees every stat is an integer field of the check entity; a miss is an engine bug.
        MORPHEUS_REQUIRE(statFields.contains(check.stat) && statFields.at(check.stat).is_number_integer(),
                         "ApplyProposal: the check entity has no integer field for the stat");
        // The seed index is the check's position in the proposal, so one rejected check never shifts the others.
        core::CheckOutcome outcome = {check.stat, check.difficulty, check.stakes,
                                      core::RollCheck(world.dice, statFields.at(check.stat).get<int>(),
                                                      difficulty->second,
                                                      core::CheckSeed(runSeed, turn, static_cast<int>(i)))};
        const nlohmann::json& branch = outcome.result.success ? check.onSuccess : check.onFailure;
        changes.insert(changes.end(), branch.begin(), branch.end());
        resolution.checks.push_back(std::move(outcome));
    }

    core::ApplyResult applied = core::ApplyChanges(world.schema, state, changes);
    resolution.state = std::move(applied.state);
    resolution.applied = std::move(applied.applied);
    resolution.rejected = std::move(badChecks);
    resolution.rejected.insert(resolution.rejected.end(), applied.rejected.begin(), applied.rejected.end());
    return resolution;
}

std::string RenderNarrateMessage(const ResolveProposal& proposal, const Resolution& resolution, int allowance,
                                 inference::LlmBackend& backend)
{
    const std::string task = "\n# Task\n" + std::string(NARRATE_INSTRUCTION);
    if (!proposal.error.empty()) {
        return std::format("# Resolution\nThe action could not be resolved ({}). Nothing changed.\n{}", proposal.error,
                           task);
    }

    // One line per rolled check; the stakes are model-written text and can be shed.
    auto renderChecks = [&resolution](bool withStakes) {
        std::string checks;
        for (const core::CheckOutcome& check : resolution.checks) {
            std::string rolls;
            for (const int roll : check.result.rolls) {
                rolls += (rolls.empty() ? "" : " + ") + std::to_string(roll);
            }
            // A stat of 0 adds nothing, so its "+N" is left out.
            const std::string stat = check.result.statValue != 0 ? std::format(" {:+}", check.result.statValue) : "";
            checks += std::format("Check: {} vs {} ({}): rolled {}{} = {}, {}", check.stat, check.difficulty,
                                  check.result.target, rolls, stat, check.result.total,
                                  check.result.success ? "success" : "failure");
            checks += withStakes ? std::format(". Stakes: {}\n", check.stakes) : "\n";
        }
        return checks;
    };
    std::vector<std::string> applied;
    for (const core::AppliedChange& change : resolution.applied) {
        applied.push_back(context::RenderChangePart(change));
    }
    std::vector<std::string> rejected;
    for (const core::RejectedChange& change : resolution.rejected) {
        const nlohmann::json& raw = change.raw;
        const bool named = raw.is_object() && raw.contains("op") && raw.at("op").is_string() && raw.contains("path") &&
                           raw.at("path").is_string();
        rejected.push_back(named ? std::format("{} {} ({})", raw.at("op").get<std::string>(),
                                               raw.at("path").get<std::string>(), change.reason)
                                 : change.reason);
    }
    auto join = [](const std::vector<std::string>& parts, std::size_t count) {
        std::string text;
        for (std::size_t i = 0; i < count; ++i) {
            text += (i == 0 ? "" : "; ") + parts[i];
        }
        return text;
    };

    // Render, then shed detail one step at a time until the message fits the narrate allowance: the rejected
    // changes, applied changes from the end, the stakes, then the intent's tail. Each step runs only while over.
    bool showRejected = !rejected.empty();
    std::size_t appliedShown = applied.size();
    bool showStakes = true;
    bool cutIntent = false;
    for (;;) {
        std::string intent = proposal.intent;
        if (cutIntent) {
            intent = core::CutUtf8(intent, INTENT_SHORT_CHARS) + (intent.size() > INTENT_SHORT_CHARS ? "..." : "");
        }
        std::string text = "# Resolution\nIntent: " + intent + "\n" + renderChecks(showStakes);
        if (applied.empty()) {
            text += "Applied: none\n";
        } else {
            text += "Applied: " + join(applied, appliedShown);
            if (appliedShown < applied.size()) {
                text += std::format("{}and {} more", appliedShown == 0 ? "" : "; ", applied.size() - appliedShown);
            }
            text += "\n";
        }
        if (showRejected) {
            text += "Rejected: " + join(rejected, rejected.size()) + "\n";
        }
        text += task;
        if (backend.CountTokens(text) <= allowance) {
            return text;
        }
        if (showRejected) {
            showRejected = false;
        } else if (appliedShown > 0) {
            --appliedShown;
        } else if (showStakes) {
            showStakes = false;
        } else if (!cutIntent) {
            cutIntent = true;
        } else {
            // Everything model-written is shed: the allowance itself is too small.
            throw core::DataError(std::format("narrate_allowance {} is too small for the resolution record ({} tokens)",
                                              allowance, backend.CountTokens(text)));
        }
    }
}

std::vector<Lookup> ParseLookupOutput(std::string_view text, std::string& error)
{
    // A reused string must not carry an old error into a successful read.
    error.clear();
    // Parse without exceptions: unreadable output is a model mistake, reported through `error`.
    const nlohmann::json json = nlohmann::json::parse(text, nullptr, false);
    if (json.is_discarded() || !json.is_object() || !json.contains("lookups") || !json.at("lookups").is_array()) {
        error = "the lookup output is not {\"lookups\": [...]}";
        return {};
    }
    // Each entry names a source and a query; one bad entry rejects the whole output.
    std::vector<Lookup> lookups;
    for (const nlohmann::json& entry : json.at("lookups")) {
        if (!entry.is_object() || !entry.contains("source") || !entry.at("source").is_string() ||
            !entry.contains("query") || !entry.at("query").is_string()) {
            error = "each lookup needs a string 'source' and 'query'";
            return {};
        }
        // The model's source names map onto the store's embedding kinds.
        const std::string source = entry.at("source").get<std::string>();
        Lookup lookup;
        if (source == "lore") {
            lookup.source = store::EmbeddingKind::Lore;
        } else if (source == "history") {
            lookup.source = store::EmbeddingKind::Turn;
        } else if (source == "notes") {
            lookup.source = store::EmbeddingKind::Note;
        } else {
            error = std::format("unknown lookup source '{}'", source);
            return {};
        }
        lookup.query = entry.at("query").get<std::string>();
        lookups.push_back(std::move(lookup));
    }
    return lookups;
}

} // namespace morpheus::turn
