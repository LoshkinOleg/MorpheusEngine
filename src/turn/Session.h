#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "core/MemoryMode.h"
#include "core/Settings.h"
#include "core/TurnRecord.h"
#include "core/World.h"
#include "inference/Llm.h"
#include "store/RunStore.h"
#include "turn/Retrieval.h"

namespace morpheus::turn {

// A turn that cannot run as asked: nothing to undo or regenerate, an empty input, or an input too long for the
// tail allowance. Nothing changed; the message is for the player.
class TurnError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

constexpr std::size_t RETRIEVAL_MAX_HITS = 6; // Retrieval hits in the tail (T2), before retrieval_cap applies.
constexpr std::size_t LOOKUP_MAX_HITS = 2;    // Hits each assisted-mode lookup adds.
constexpr int LOOKUP_MAX_TOKENS = 192;        // Generation cap for the lookup call.
constexpr int FOLD_SUMMARY_DIVISOR = 4;       // A fold summary gets summary_cap / 4 tokens: several fit before a condense.
constexpr int CONDENSE_SUMMARY_DIVISOR = 2;   // A condensed summary gets summary_cap / 2 tokens, so it fits its cap.

struct TurnCallbacks {
    // After apply, before narrate: the record has its checks and applied and rejected changes, no narration yet.
    std::function<void(const core::TurnRecord&)> onResolved = {};
    inference::TokenCallback onNarrationToken = {};
};

// One open run: its world, settings, database and memory index, and the turn loop (architecture spec, "Turn flow").
// Every backend call happens inside these methods, on the caller's thread (the engine worker). Not thread-safe.
class Session {
private:
    core::World _world;     // Set by the constructor.
    core::Settings _settings;
    core::MemoryMode _mode = core::MemoryMode::Engine;
    store::RunStore _store; // Set by the constructor.
    inference::LlmBackend* _pBackend = nullptr; // Not owned; outlives the session.
    MemoryIndex _index;     // Set by the constructor.
    context::Assembly _lastAssembly = {};
    bool _worldChanged = false;

public:
    // Starts a new run in `file` from the world's initial state, with `seed` for dice and sampling, and indexes the
    // lore. Throws StoreError if the file exists, and DataError if lore cannot be embedded (then no file is left).
    static Session Start(core::World world, core::Settings settings, const std::filesystem::path& file,
                         std::uint64_t seed, inference::LlmBackend& backend);

    // Opens a run of `world`. Throws DataError if the run belongs to another world or its latest state no longer
    // validates against the world's schema. Edited world files set WorldChanged(); edited lore is re-embedded.
    static Session Load(core::World world, core::Settings settings, const std::filesystem::path& file,
                        inference::LlmBackend& backend);

    // Plays one turn: retrieve, (assisted mode: look up), resolve, apply, narrate, commit. Control characters other
    // than newline and tab are stripped from `input` and it is trimmed. Returns the committed record, or nullopt
    // when `stop` cancelled the turn (nothing is committed). Throws TurnError for an empty input or one that does
    // not fit, and InferenceError from the backend (nothing is committed).
    std::optional<core::TurnRecord> RunTurn(std::string_view input, const TurnCallbacks& callbacks,
                                            std::stop_token stop);

    // Removes the latest turn. Throws TurnError when there is none.
    void Undo();

    // Plays the latest turn's input again as a new attempt: same dice, newly sampled text. If it is cancelled or
    // fails, the previous turn is put back. Throws TurnError when there is no turn.
    std::optional<core::TurnRecord> Regenerate(const TurnCallbacks& callbacks, std::stop_token stop);

    // Between-turn upkeep: folds or condenses while the transcript or the summary is over budget, then indexes
    // folded turns and notes. Returns false if `stop` cancelled it (it can simply run again later).
    bool Maintain(std::stop_token stop);

    const store::RunMeta& Meta() const;
    bool WorldChanged() const;
    nlohmann::json State();                  // After the latest turn.
    std::vector<core::TurnRecord> History(); // Every committed turn, oldest first.
    std::vector<core::Summary> Summaries();
    const context::Assembly& LastAssembly() const; // The last resolve prompt, for the context inspector.

private:
    Session(core::World world, core::Settings settings, store::RunStore store, MemoryIndex index,
            inference::LlmBackend& backend);

    // RunTurn and Regenerate: plays turn LatestTurn()+1 for a cleaned input.
    std::optional<core::TurnRecord> PlayTurn(const std::string& input, int attempt, const TurnCallbacks& callbacks,
                                             std::stop_token stop);

    // The layers for one call: summaries and unfolded transcript from the store, then the given tail parts.
    context::ContextInputs Inputs(const nlohmann::json& state, const std::vector<RetrievalHit>& hits,
                                  const std::string& input, std::string_view instruction);

    // Assembles a prompt that fits, shedding hits, condensing and folding as needed (see the loop). nullopt when a
    // memory call was cancelled.
    std::optional<context::Assembly> AssembleFitting(const nlohmann::json& state, std::vector<RetrievalHit>& hits,
                                                     const std::string& input, std::string_view instruction, int turn,
                                                     int attempt, std::stop_token stop);

    // Folds the oldest half (at least one) of `transcript` into a new summary. False when cancelled.
    bool FoldOldest(const inference::Message& systemMessage, const std::vector<core::TurnRecord>& transcript, int turn,
                    int attempt, std::stop_token stop);

    // Replaces every summary with one condensed summary. False when cancelled.
    bool Condense(const inference::Message& systemMessage, int turn, int attempt, std::stop_token stop);

    // The last turn covered by a summary, 0 when none.
    int FoldedThrough();
};

} // namespace morpheus::turn
