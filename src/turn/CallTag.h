#pragma once

#include <string_view>

namespace morpheus::turn {

// The call tags the turn layer sets on a GenerationSpec (generation-controls spec, section 2). The tags are read back
// by the replay's CallKindOf and the app's SeatLabelOf.
inline constexpr std::string_view LOOKUP_TAG = "lookup";
inline constexpr std::string_view RESOLVE_TAG = "resolve";
inline constexpr std::string_view NARRATE_TAG = "narrate";
inline constexpr std::string_view RENARRATE_TAG = "renarrate";
inline constexpr std::string_view REVIEW_TAG = "review";
inline constexpr std::string_view SUMMARIZE_TAG = "summarize";
inline constexpr std::string_view CONDENSE_TAG = "condense";
inline constexpr std::string_view DECIDE_TAG = "decide";
inline constexpr std::string_view SPEAK_TAG = "speak";
inline constexpr std::string_view RESPEAK_TAG = "respeak";

} // namespace morpheus::turn
