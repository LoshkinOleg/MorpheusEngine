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
        _commands.push_back(std::move(command));
        if (_maintaining) {
            _jobStop.request_stop(); // A command preempts between-turn maintenance.
        }
    }
    _commandPosted.notify_one();
}

void Worker::Cancel()
{
    std::scoped_lock lock(_mutex);
    _jobStop.request_stop();
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
    bool maintenanceDue = false;
    for (;;) {
        std::optional<Command> command;
        std::stop_token stop;
        {
            std::unique_lock lock(_mutex);
            if (!maintenanceDue && !_commandPosted.wait(lock, shutdown, [this] { return !_commands.empty(); })) {
                return; // Shutdown while idle.
            }
            if (shutdown.stop_requested()) {
                return;
            }
            if (!_commands.empty()) {
                command = std::move(_commands.front());
                _commands.pop_front();
            }
            _jobStop = std::stop_source();
            _maintaining = !command.has_value();
            stop = _jobStop.get_token();
        }
        if (command) {
            maintenanceDue = execute(*command, stop);
        } else {
            try {
                if (_session) {
                    _session->Maintain(stop);
                }
            } catch (const std::exception& error) {
                Emit(MaintenanceFailed{error.what()});
            }
            maintenanceDue = false;
        }
    }
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
