#include "app/model/RunModel.h"

#include <cstddef>
#include <utility>
#include <variant>

#include "core/Errors.h"

namespace morpheus::app {

void RunModel::OnPosted(const turn::Command& command)
{
    MORPHEUS_REQUIRE(_busy == Busy::None, "RunModel: a command is already running");
    if (std::holds_alternative<turn::StartRun>(command) || std::holds_alternative<turn::LoadRun>(command)) {
        _lastError.clear();
        _busy = Busy::Opening;
        return;
    }
    MORPHEUS_REQUIRE(_open, "RunModel: a turn command needs an open run");
    if (const auto* pInput = std::get_if<turn::SubmitInput>(&command)) {
        _lastError.clear();
        _busy = Busy::Turn;
        _pending = PendingTurn{pInput->text};
        return;
    }
    MORPHEUS_REQUIRE(!_history.empty(), "RunModel: undo and regenerate need a committed turn");
    if (std::holds_alternative<turn::RegenerateTurn>(command)) {
        _lastError.clear();
        _busy = Busy::Regenerate;
        _pending = PendingTurn{_history.back().input}; // The same input, played again.
        return;
    }
    // Only an explicit UndoTurn is left; a new command type must be handled above, not treated as an undo.
    MORPHEUS_REQUIRE(std::holds_alternative<turn::UndoTurn>(command), "RunModel: an unknown command was posted");
    _lastError.clear();
    _busy = Busy::Undo;
}

void RunModel::Apply(const turn::Event& event)
{
    // Progress of the running turn.
    if (const auto* pResolved = std::get_if<turn::TurnResolved>(&event)) {
        MORPHEUS_REQUIRE(_pending.has_value(), "RunModel: TurnResolved with no turn running");
        _pending->resolved = pResolved->record;
        return;
    }
    if (const auto* pPiece = std::get_if<turn::TokenStreamed>(&event)) {
        MORPHEUS_REQUIRE(_pending.has_value(), "RunModel: TokenStreamed with no turn running");
        _pending->narration += pPiece->piece;
        return;
    }
    // Maintenance reports, between commands.
    if (const auto* pUpdated = std::get_if<turn::MemoryUpdated>(&event)) {
        _memory = pUpdated->memory;
        _maintenanceError.clear();
        return;
    }
    if (const auto* pFailed = std::get_if<turn::MaintenanceFailed>(&event)) {
        _maintenanceError = pFailed->message;
        return;
    }

    // Every other event closes the running command.
    MORPHEUS_REQUIRE(_busy != Busy::None, "RunModel: a closing event with no command running");
    const Busy finished = _busy;
    _busy = Busy::None;
    std::optional<PendingTurn> pending = std::move(_pending);
    _pending.reset();

    if (const auto* pOpened = std::get_if<turn::RunOpened>(&event)) {
        MORPHEUS_REQUIRE(finished == Busy::Opening, "RunModel: RunOpened closes only a StartRun or LoadRun");
        _open = true;
        _meta = pOpened->meta;
        _worldChanged = pOpened->worldChanged;
        _history = pOpened->history;
        _state = pOpened->state;
        _memory = pOpened->memory;
        _lastAssembly.reset(); // The prompt shows again once a turn is played.
        _maintenanceError.clear();
        return;
    }
    if (const auto* pCompleted = std::get_if<turn::TurnCompleted>(&event)) {
        if (finished == Busy::Regenerate) {
            MORPHEUS_REQUIRE(!_history.empty() && _history.back().number == pCompleted->record.number,
                             "RunModel: a regenerated turn must replace the latest turn");
            _history.back() = pCompleted->record;
        } else {
            MORPHEUS_REQUIRE(finished == Busy::Turn &&
                                 pCompleted->record.number == static_cast<int>(_history.size()) + 1,
                             "RunModel: a new turn must follow the latest turn");
            _history.push_back(pCompleted->record);
        }
        _state = pCompleted->state;
        _memory = pCompleted->memory;
        _lastAssembly = pCompleted->assembly;
        return;
    }
    if (const auto* pChanged = std::get_if<turn::StateChanged>(&event)) {
        MORPHEUS_REQUIRE(finished == Busy::Undo, "RunModel: StateChanged closes only an UndoTurn");
        _history = pChanged->history;
        _state = pChanged->state;
        _memory = pChanged->memory;
        _lastAssembly.reset(); // It was the prompt of a turn that is gone.
        return;
    }

    // TurnCancelled or Error: nothing was committed. A submitted input goes back to the input box.
    if (pending && finished == Busy::Turn) {
        _restoredInput = std::move(pending->input);
    }
    if (const auto* pError = std::get_if<turn::Error>(&event)) {
        if (finished == Busy::Opening) {
            Close(); // A failed StartRun or LoadRun has already closed the previous run.
        }
        _lastError = pError->message;
    }
}

void RunModel::Close()
{
    *this = RunModel();
}

bool RunModel::Open() const
{
    return _open;
}

const store::RunMeta& RunModel::Meta() const
{
    return _meta;
}

bool RunModel::WorldChanged() const
{
    return _worldChanged;
}

const std::vector<core::TurnRecord>& RunModel::History() const
{
    return _history;
}

std::span<const core::TurnRecord> RunModel::TranscriptTurns() const
{
    // OnPosted guarantees a turn to hide while regenerating.
    const std::size_t shown = _busy == Busy::Regenerate ? _history.size() - 1 : _history.size();
    return std::span<const core::TurnRecord>(_history.data(), shown);
}

const nlohmann::json& RunModel::State() const
{
    return _state;
}

const turn::MemoryView& RunModel::Memory() const
{
    return _memory;
}

const std::optional<context::Assembly>& RunModel::LastAssembly() const
{
    return _lastAssembly;
}

RunModel::Busy RunModel::CurrentBusy() const
{
    return _busy;
}

const std::optional<RunModel::PendingTurn>& RunModel::Pending() const
{
    return _pending;
}

const std::string& RunModel::LastError() const
{
    return _lastError;
}

const std::string& RunModel::MaintenanceError() const
{
    return _maintenanceError;
}

const core::TurnRecord* RunModel::LatestTurn() const
{
    return _history.empty() ? nullptr : &_history.back();
}

std::optional<std::string> RunModel::TakeRestoredInput()
{
    return std::exchange(_restoredInput, std::nullopt);
}

} // namespace morpheus::app
