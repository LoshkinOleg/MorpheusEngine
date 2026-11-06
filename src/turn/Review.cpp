#include "turn/Review.h"

#include <algorithm>
#include <format>
#include <utility>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/Text.h"
#include "turn/EngineText.h"
#include "turn/ResolveGrammar.h"

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
        // The grammar's bounds: at most REVIEW_MAX_PROBLEMS, each 1 to REVIEW_PROBLEM_MAX_CHARS long once trimmed.
        if (json.at("problems").size() > static_cast<std::size_t>(REVIEW_MAX_PROBLEMS)) {
            return failed(std::format("'problems' lists more than {} contradictions", REVIEW_MAX_PROBLEMS));
        }
        for (const nlohmann::json& problem : json.at("problems")) {
            std::string trimmed = core::Trim(problem.get<std::string>());
            if (trimmed.empty()) {
                return failed("'problems' holds an empty contradiction");
            }
            // Characters, as the grammar counts them, not bytes.
            if (core::Utf8Length(trimmed) > static_cast<std::size_t>(REVIEW_PROBLEM_MAX_CHARS)) {
                return failed(std::format("'problems' holds a contradiction longer than {} characters",
                                          REVIEW_PROBLEM_MAX_CHARS));
            }
            review.problems.push_back(std::move(trimmed));
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
        if (core::Utf8Length(review.digest) > static_cast<std::size_t>(DIGEST_MAX_CHARS)) {
            return failed(std::format("'digest' is a digest longer than {} characters", DIGEST_MAX_CHARS));
        }
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
