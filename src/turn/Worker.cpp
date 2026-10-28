#include "turn/Worker.h"

#include <exception>
#include <string_view>
#include <utility>

#include "core/Errors.h"
#include "core/World.h"

namespace morpheus::turn {

bool IsClosing(const Event& event)
{
    return std::holds_alternative<RunOpened>(event) || std::holds_alternative<TurnCompleted>(event) ||
           std::holds_alternative<TurnCancelled>(event) || std::holds_alternative<StateChanged>(event) ||
           std::holds_alternative<Error>(event);
}

Worker::Worker(core::Settings settings, inference::LlmBackend& backend)
    : _settings(std::move(settings))
    , _pBackend(&backend)
    , _thread([this](std::stop_token shutdown) { Run(shutdown); })
{
}

void Worker::Post(Command command)
{
    {
        std::scoped_lock lock(_mutex);
        _commands.push_back({++_postedThrough, std::move(command)});
        if (_maintaining) {
            _jobStop.request_stop(); // A command preempts between-turn maintenance.
        }
    }
    _commandPosted.notify_one();
}

void Worker::Cancel()
{
    std::scoped_lock lock(_mutex);
    _cancelledThrough = _postedThrough; // Commands still queued close without running when the worker reaches them.
    _jobStop.request_stop();            // The running job, if any.
}

std::vector<Event> Worker::PollEvents()
{
    std::scoped_lock lock(_mutex);
    std::vector<Event> events(std::make_move_iterator(_events.begin()), std::make_move_iterator(_events.end()));
    _events.clear();
    return events;
}

std::optional<Event> Worker::WaitEvent(std::chrono::milliseconds timeout)
{
    std::unique_lock lock(_mutex);
    if (!_eventPosted.wait_for(lock, timeout, [this] { return !_events.empty(); })) {
        return std::nullopt;
    }
    Event event = std::move(_events.front());
    _events.pop_front();
    return event;
}

void Worker::Run(std::stop_token shutdown)
{
    // Runs one command and emits its events. Returns true when it committed a turn (maintenance is due).
    auto execute = [this](const Command& command, std::stop_token stop) {
        try {
            if (const auto* pStart = std::get_if<StartRun>(&command)) {
                _session.reset(); // Close the previous run first.
                _session.emplace(Session::Start(core::LoadWorld(pStart->worldFolder), _settings, pStart->runFile,
                                                pStart->seed, *_pBackend));
                EmitOpened();
                return false;
            }
            if (const auto* pLoad = std::get_if<LoadRun>(&command)) {
                _session.reset();
                _session.emplace(
                    Session::Load(core::LoadWorld(pLoad->worldFolder), _settings, pLoad->runFile, *_pBackend));
                EmitOpened();
                return false;
            }
            if (!_session) {
                throw TurnError("no run is open");
            }
            if (const auto* pInput = std::get_if<SubmitInput>(&command)) {
                return FinishTurn(_session->RunTurn(pInput->text, Callbacks(), stop));
            }
            if (std::holds_alternative<UndoTurn>(command)) {
                _session->Undo();
                Emit(StateChanged{_session->History(), _session->State()});
                return false;
            }
            return FinishTurn(_session->Regenerate(Callbacks(), stop));
        } catch (const core::ContractError& error) {
            Emit(Error{std::string("engine bug: ") + error.what()});
        } catch (const std::exception& error) {
            Emit(Error{error.what()});
        }
        return false;
    };

    // Shutdown cancels the running job too, so destroying the worker never waits for a whole turn.
    std::stop_callback cancelOnShutdown(shutdown, [this] { Cancel(); });
    // Set when a turn commits; cleared only when maintenance finishes (or cannot), or the run is replaced.
    bool maintenanceDue = false;
    for (;;) {
        // Pick the next job under the lock: a cancelled command to close, a command to run, or maintenance.
        std::optional<Command> command;
        bool cancelledEarly = false; // The command was cancelled before it started.
        std::stop_token stop;
        {
            std::unique_lock lock(_mutex);
            if (!maintenanceDue && !_commandPosted.wait(lock, shutdown, [this] { return !_commands.empty(); })) {
                break; // Shutdown while idle.
            }
            if (shutdown.stop_requested()) {
                break;
            }
            if (!_commands.empty()) {
                const Posted& next = _commands.front();
                const bool retrieves = std::holds_alternative<SubmitInput>(next.command) ||
                                       std::holds_alternative<RegenerateTurn>(next.command);
                if (next.number <= _cancelledThrough) {
                    cancelledEarly = true;
                    _commands.pop_front();
                } else if (!maintenanceDue || !retrieves) {
                    command = std::move(_commands.front().command);
                    _commands.pop_front();
                }
                // Otherwise maintenance runs first: the retrieving command must see the last turn indexed.
            }
            _jobStop = std::stop_source();
            // Only maintenance run while nothing waits is preemptible. Before a retrieving command, preempting it
            // would only restart it, since every later command waits behind that one anyway.
            _maintaining = !cancelledEarly && !command && _commands.empty();
            stop = _jobStop.get_token();
        }

        if (cancelledEarly) {
            Emit(TurnCancelled{}); // Its closing event; it never ran.
        } else if (command) {
            // A StartRun or LoadRun replaces the session (or leaves none when it fails): the run that had
            // maintenance due is closed and the new run has nothing due, so the flag is cleared. A committed turn
            // sets it; any other command keeps it.
            const bool opensRun =
                std::holds_alternative<StartRun>(*command) || std::holds_alternative<LoadRun>(*command);
            maintenanceDue = execute(*command, stop) || (maintenanceDue && !opensRun);
        } else {
            bool finished = true; // A throw counts as finished: rerunning would only throw again.
            try {
                MORPHEUS_REQUIRE(_session.has_value(), "Worker: maintenance is due with no run open");
                finished = _session->Maintain(stop);
            } catch (const core::ContractError& error) {
                Emit(MaintenanceFailed{std::string("engine bug: ") + error.what()});
            } catch (const std::exception& error) {
                Emit(MaintenanceFailed{error.what()});
            }
            if (finished) {
                maintenanceDue = false;
            } else {
                // Stopped early. A command preempted it, or Cancel stopped it while a command waits: it stays due
                // and runs again before the next retrieving command. A Cancel with nothing queued drops it (until
                // the next commit), so cancelling cannot restart it forever. Shutdown leaves the loop next.
                std::scoped_lock lock(_mutex);
                maintenanceDue = !_commands.empty();
            }
        }
    }
    _session.reset(); // Close the run here: only the worker thread touches it.
}

bool Worker::FinishTurn(const std::optional<core::TurnRecord>& record)
{
    if (!record) {
        Emit(TurnCancelled{});
        return false;
    }
    Emit(TurnCompleted{*record, _session->State(), _session->LastAssembly()});
    return true;
}

void Worker::EmitOpened()
{
    Emit(RunOpened{_session->Meta(), _session->History(), _session->State(), _session->WorldChanged()});
}

TurnCallbacks Worker::Callbacks()
{
    TurnCallbacks callbacks;
    callbacks.onResolved = [this](const core::TurnRecord& record) { Emit(TurnResolved{record}); };
    callbacks.onNarrationToken = [this](std::string_view piece) { Emit(TokenStreamed{std::string(piece)}); };
    return callbacks;
}

void Worker::Emit(Event event)
{
    {
        std::scoped_lock lock(_mutex);
        _events.push_back(std::move(event));
    }
    _eventPosted.notify_all();
}

} // namespace morpheus::turn
