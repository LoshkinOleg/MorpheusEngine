#pragma once

#include <optional>
#include <span>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "context/PromptBuilder.h"
#include "core/TurnRecord.h"
#include "store/RunStore.h"
#include "turn/Worker.h"

namespace morpheus::app {

// The open run as the windows show it, kept up to date from the worker's events (UI thread only). The UI runs one
// command at a time: OnPosted records it, and its closing event (turn::IsClosing) ends it. MemoryUpdated and
// MaintenanceFailed arrive between commands.
class RunModel {
public:
    // The command the worker is running for the UI.
    enum class Busy { None, Opening, Turn, Regenerate, Undo };

    // The turn being played: its input, then its resolved record (checks and changes), then the narration so far.
    struct PendingTurn {
        std::string input = {};
        std::optional<core::TurnRecord> resolved = {};
        std::string narration = {};
    };

private:
    bool _open = false;
    store::RunMeta _meta = {};
    bool _worldChanged = false;
    std::vector<core::TurnRecord> _history = {};
    nlohmann::json _state = nullptr;
    turn::MemoryView _memory = {};
    std::optional<context::Assembly> _lastAssembly = {}; // The latest turn's resolve prompt, if played this session.
    Busy _busy = Busy::None;
    std::optional<PendingTurn> _pending = {};
    std::string _lastError = {};        // Why the last command failed; cleared when the next one is posted.
    std::string _maintenanceError = {}; // Why maintenance last failed; cleared when it next finishes.
    std::optional<std::string> _restoredInput = {};

public:
    // Records `command` as posted to the worker. Throws ContractError while another command runs, for a turn
    // command with no run open, and for UndoTurn or RegenerateTurn with no turn.
    void OnPosted(const turn::Command& command);

    // Applies one worker event. Throws ContractError for an event the running command cannot produce (a bug).
    void Apply(const turn::Event& event);

    // Forgets the run, as when the engine restarts: the worker that had it open is gone.
    void Close();

    bool Open() const;
    const store::RunMeta& Meta() const;
    bool WorldChanged() const;
    const std::vector<core::TurnRecord>& History() const;
    // The committed turns the transcript shows: all of them, except the latest while it is being regenerated.
    std::span<const core::TurnRecord> TranscriptTurns() const;
    const nlohmann::json& State() const;
    const turn::MemoryView& Memory() const;
    const std::optional<context::Assembly>& LastAssembly() const;
    Busy CurrentBusy() const;
    const std::optional<PendingTurn>& Pending() const;
    const std::string& LastError() const;
    const std::string& MaintenanceError() const;
    // The latest committed turn, or nullptr before the first.
    const core::TurnRecord* LatestTurn() const;
    // The input of a turn that was cancelled or failed, once, so the input box can offer it again.
    std::optional<std::string> TakeRestoredInput();
};

} // namespace morpheus::app
