#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "app/model/AppController.h"
#include "app/model/Catalog.h"
#include "app/model/ModelSeat.h"
#include "app/model/SettingsEdit.h"
#include "inference/Llm.h"

namespace morpheus::app {

// Window names. ImGui identifies windows, and their places in imgui.ini, by them.
constexpr const char* RUNS_WINDOW = "Runs";
constexpr const char* PLAY_WINDOW = "Play";
constexpr const char* STATE_WINDOW = "State";
constexpr const char* CONTEXT_WINDOW = "Context";
constexpr const char* TURN_LOG_WINDOW = "Turn log";
constexpr const char* MEMORY_WINDOW = "Memory";
constexpr const char* SETTINGS_WINDOW = "Settings";
constexpr const char* MODEL_SEAT_WINDOW = "Model seat";

// Text colors.
constexpr ImVec4 ERROR_COLOR = ImVec4(1.0f, 0.45f, 0.4f, 1.0f);
constexpr ImVec4 WARNING_COLOR = ImVec4(1.0f, 0.8f, 0.35f, 1.0f);
constexpr ImVec4 INPUT_COLOR = ImVec4(0.55f, 0.8f, 1.0f, 1.0f);     // The player's input in the transcript.
constexpr ImVec4 MECHANICS_COLOR = ImVec4(0.6f, 0.6f, 0.6f, 1.0f);  // Rolls and changes in the transcript.
constexpr ImVec4 CHANGED_COLOR = ImVec4(0.45f, 1.0f, 0.45f, 1.0f);  // A state node the latest turn changed.
constexpr ImVec4 CONTAINS_COLOR = ImVec4(0.75f, 0.9f, 0.6f, 1.0f);  // A state node holding a change.

// Wrapped text in `color`, drawn as is (no format string), for errors and notices.
void TextWrappedColored(const ImVec4& color, std::string_view text);

// A prompt as the Context window shows it, in the rest of the window: Sections / Raw radio buttons, then one
// collapsing section per message (titled by its role and layer) or the whole text, read-only. The choice is kept in
// `showRaw`. Used by the Context and Model seat windows.
void DrawPrompt(const inference::Prompt& prompt, bool& showRaw);

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

// The state tree, with the latest turn's changes marked.
class StateWindow {
public:
    void Draw(AppController& controller);
};

// Every turn, newest first: input, intent, rolls, applied and rejected changes, notes, lookups, the narration
// review (verdicts, problems, digests and a replaced narration), per-call numbers and the raw resolve output.
class TurnLogWindow {
public:
    void Draw(AppController& controller);
};

// The latest resolve prompt by layer: tokens per layer, cache reuse per call, and the text as sections or raw.
class ContextWindow {
public:
    bool showRaw = false; // The prompt as one raw text instead of sections (the window's toggle).

    void Draw(AppController& controller);
};

// The story summaries, the notes, and the latest turn's retrieval hits with their scores and reasons, and its scene.
class MemoryWindow {
public:
    void Draw(AppController& controller);
};

// The editable settings. Apply saves settings.json and restarts the engine; Revert reads the file again.
class SettingsWindow {
private:
    std::optional<SettingsEdit> _edit = {}; // Read from the file on the first draw and after Revert.
    std::string _problem = {};              // Why the file could not be read, or why Apply refused the edit.
    std::vector<std::string> _models = {};  // The models folder's files, listed again whenever a model list opens.

public:
    void Draw(AppController& controller);
};

// Debug: the owner answers the engine's model calls by hand (the model seat). Always: the "Answer as the model"
// switch and each label's Human / Draft / Model mode. While a call waits: its label and limits, the model's thinking
// (Draft mode) and the grammar, the reply and thinking boxes, Check, Submit and Withdraw, and the prompt as the
// Context window draws it. Withdraw releases the call: a running turn's or regenerate's by cancelling the command,
// a between-turn fold's or condense's at the seat. The texts typed stay until Submit and are reset when a new call
// arrives, unless it is the same call published again (its label and prompt equal).
class ModelSeatWindow {
private:
    std::uint64_t _requestId = 0;         // The request _reply and _thinking belong to (SeatRequest::id); 0: none yet.
    SeatLabel _label = SeatLabel::Resolve; // That request's label and prompt: a new request equal to both is the
    inference::Prompt _prompt = {};        // same call published again, and keeps the texts.
    std::string _reply = {};
    std::string _thinking = {};
    std::optional<SeatCheck> _check = {}; // The last Check of the reply as it is now; cleared when it is edited.
    bool _showRaw = false;                // The prompt as raw text instead of sections.

public:
    void Draw(AppController& controller);
};

} // namespace morpheus::app
