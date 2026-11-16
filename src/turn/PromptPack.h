#pragma once

#include <array>
#include <map>
#include <string>
#include <string_view>

#include "turn/EngineText.h"

namespace morpheus::turn {

// The names PromptPack::Fill puts in a hosted text. A variable a text names must have a non-empty value here.
struct PromptVariables {
    std::string speaker = {}; // {speaker}: the speaking NPC's name.
    std::string player = {};  // {player}: the player's character's name.
    std::string actor = {};   // {actor}: the NPC whose memory is folded or condensed.
};

// One text of the pack (prompt pack spec, section 1): its key in the host data and the variables it takes (at most
// two; an empty entry is unused).
struct PromptText {
    std::string_view key = {};
    std::array<std::string_view, 2> variables = {};
};

// The pack's texts in the spec's table order: the order FromHost checks them in and the defaults route lists them in.
inline constexpr std::array<PromptText, 7> PROMPT_TEXTS = {{
    {"system", {}},
    {"resolve", {"speaker", "player"}},
    {"speak", {"speaker", "player"}},
    {"turn_cue", {"speaker", "player"}},
    {"decide", {"player", ""}},
    {"summarize", {"actor", ""}},
    {"condense", {"actor", ""}},
}};

// The texts a hosted run speaks to the model (prompt pack spec): the engine's defaults (EngineText's HOSTED_*), with the
// host's texts over them. The system text takes no variable, so L0 is the same for every turn and speaker of a run.
class PromptPack {
public:
    std::string system = std::string(HOSTED_SYSTEM_TEXT);
    std::string resolve = std::string(HOSTED_RESOLVE_INSTRUCTION);
    std::string speak = std::string(HOSTED_SPEAK_INSTRUCTION);
    std::string turnCue = std::string(HOSTED_TURN_CUE);
    std::string decide = std::string(HOSTED_DECIDE_INSTRUCTION);
    std::string summarize = std::string(HOSTED_SUMMARIZE_INSTRUCTION);
    std::string condense = std::string(HOSTED_CONDENSE_INSTRUCTION);

    // The host's texts (HostData::prompts, by key) over the defaults. Throws DataError for the first fault: an unknown
    // key ("prompts/<key>: unknown text; ..."), checked in key order before any text; then, text by text in
    // PROMPT_TEXTS' order, an empty text ("prompts/<key>: empty") or a variable the text does not take
    // ("prompts/<key>: unknown variable {<name>}; this text takes ..."), in text order. A text that fails is never
    // replaced by its default.
    static PromptPack FromHost(const std::map<std::string, std::string>& texts);

    // `text` with every variable ({name}, name of a-z and _) replaced by its value in `variables`; other braces stay.
    // A variable that is not speaker, player or actor, or has an empty value, is a ContractError: FromHost refused the
    // first, and the engine always gives the names its texts take.
    static std::string Fill(std::string_view text, const PromptVariables& variables);

    // The text whose host-data key is `key` (one of PROMPT_TEXTS'); a ContractError for another key.
    const std::string& Text(std::string_view key) const;
};

} // namespace morpheus::turn
