#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "app/model/AppController.h"
#include "app/model/Catalog.h"

namespace morpheus::app {

// Window names. ImGui identifies windows, and their places in imgui.ini, by them.
constexpr const char* RUNS_WINDOW = "Runs";
constexpr const char* PLAY_WINDOW = "Play";
constexpr const char* STATE_WINDOW = "State";
constexpr const char* CONTEXT_WINDOW = "Context";
constexpr const char* TURN_LOG_WINDOW = "Turn log";
constexpr const char* MEMORY_WINDOW = "Memory";
constexpr const char* SETTINGS_WINDOW = "Settings";

// Text colors.
constexpr ImVec4 ERROR_COLOR = ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
constexpr ImVec4 WARNING_COLOR = ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
constexpr ImVec4 INPUT_COLOR = ImVec4(0.55f, 0.8f, 1.0f, 1.0f);     // The player's input in the transcript.
constexpr ImVec4 MECHANICS_COLOR = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);  // Rolls and changes in the transcript.
constexpr ImVec4 CHANGED_COLOR = ImVec4(0.45f, 1.0f, 0.45f, 1.0f);  // A state node the latest turn changed.
constexpr ImVec4 CONTAINS_COLOR = ImVec4(0.75f, 0.9f, 0.6f, 1.0f);  // A state node holding a change.

// Wrapped text in `color`, drawn as is (no format string), for errors and notices.
void TextWrappedColored(const ImVec4& color, std::string_view text);

// Worlds, their saved runs, and starting or continuing one.
class RunsWindow {
private:
    std::size_t _selectedWorld = 0; // Index into AppController::Worlds().
    bool _suggestName = true;       // Suggest a fresh run name on the next draw.
    std::string _runName = {};
    std::string _seedText = {};     // Blank: a random seed.
    std::vector<RunEntry> _runs = {};
    std::string _runsKey = {};      // What _runs was listed for; listing again when it changes.

public:
    void Draw(AppController& controller);
};

// The story: the transcript with inline rolls, the streaming narration, the input box, and undo, regenerate and
// cancel.
class PlayWindow {
private:
    std::string _input = {};
    bool _couldPost = false; // Last frame's CanPost: the input takes focus when the engine is ready again.

public:
    void Draw(AppController& controller);
};

} // namespace morpheus::app
