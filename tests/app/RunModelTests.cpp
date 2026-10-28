#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "app/model/RunModel.h"
#include "core/Errors.h"
#include "turn/Worker.h"

using namespace morpheus;

namespace {

core::TurnRecord Record(int number, std::string input, std::string narration)
{
    core::TurnRecord record;
    record.number = number;
    record.input = std::move(input);
    record.narration = std::move(narration);
    return record;
}

// A model with a run open, as after LoadRun and its RunOpened.
app::RunModel OpenModel(std::vector<core::TurnRecord> history = {})
{
    app::RunModel model;
    model.OnPosted(turn::LoadRun{});
    turn::RunOpened opened;
    opened.meta.worldId = "mini";
    opened.history = std::move(history);
    opened.state = nlohmann::json::object();
    model.Apply(opened);
    return model;
}

} // namespace

TEST_CASE("RunModel follows a turn from input to narration", "[run_model]")
{
    app::RunModel model = OpenModel();
    REQUIRE(model.Open());
    CHECK(model.Meta().worldId == "mini");
    CHECK(model.LatestTurn() == nullptr);

    model.OnPosted(turn::SubmitInput{"I walk."});
    CHECK(model.CurrentBusy() == app::RunModel::Busy::Turn);
    REQUIRE(model.Pending());
    CHECK(model.Pending()->input == "I walk.");
    CHECK_FALSE(model.Pending()->resolved);

    model.Apply(turn::TurnResolved{Record(1, "I walk.", "")});
    CHECK(model.Pending()->resolved);
    model.Apply(turn::TokenStreamed{"You "});
    model.Apply(turn::TokenStreamed{"walk."});
    CHECK(model.Pending()->narration == "You walk.");

    turn::TurnCompleted completed;
    completed.record = Record(1, "I walk.", "You walk.");
    completed.state = {{"player", 1}};
    completed.memory.notes = {{1, "A note."}};
    completed.assembly.tokens.total = 123;
    model.Apply(completed);
    CHECK(model.CurrentBusy() == app::RunModel::Busy::None);
    CHECK_FALSE(model.Pending());
    REQUIRE(model.History().size() == 1);
    CHECK(model.LatestTurn()->narration == "You walk.");
    CHECK(model.TranscriptTurns().size() == 1);
    CHECK(model.State().at("player") == 1);
    CHECK(model.Memory().notes.size() == 1);
    REQUIRE(model.LastAssembly());
    CHECK(model.LastAssembly()->tokens.total == 123);
}

TEST_CASE("A regenerate replaces the latest turn and hides it while it runs", "[run_model]")
{
    app::RunModel model = OpenModel({Record(1, "I walk.", "Old.")});
    model.OnPosted(turn::RegenerateTurn{});
    CHECK(model.CurrentBusy() == app::RunModel::Busy::Regenerate);
    CHECK(model.Pending()->input == "I walk."); // The latest turn's input, played again.
    CHECK(model.TranscriptTurns().empty());     // The old text is hidden while the new one streams.

    core::TurnRecord again = Record(1, "I walk.", "New.");
    again.attempt = 1;
    model.Apply(turn::TurnCompleted{again, nlohmann::json::object(), {}, {}});
    REQUIRE(model.History().size() == 1);
    CHECK(model.History()[0].narration == "New.");
    CHECK(model.History()[0].attempt == 1);
    CHECK(model.TranscriptTurns().size() == 1);
}

TEST_CASE("A cancelled or failed turn commits nothing and offers its input again", "[run_model]")
{
    app::RunModel model = OpenModel();
    model.OnPosted(turn::SubmitInput{"I jump."});
    model.Apply(turn::TokenStreamed{"You"});
    model.Apply(turn::TurnCancelled{});
    CHECK(model.History().empty());
    CHECK(model.CurrentBusy() == app::RunModel::Busy::None);
    CHECK_FALSE(model.Pending());
    CHECK(model.TakeRestoredInput() == std::optional<std::string>("I jump."));
    CHECK_FALSE(model.TakeRestoredInput()); // Offered once.

    model.OnPosted(turn::SubmitInput{"I fly."});
    model.Apply(turn::Error{"input too long"});
    CHECK(model.LastError() == "input too long");
    CHECK(model.TakeRestoredInput() == std::optional<std::string>("I fly."));
    CHECK(model.Open()); // A failed turn leaves the run open.

    model.OnPosted(turn::SubmitInput{"I walk."});
    CHECK(model.LastError().empty()); // Cleared by the next command.
}

TEST_CASE("A failed open closes the previous run", "[run_model]")
{
    app::RunModel model = OpenModel({Record(1, "I walk.", "You walk.")});
    model.OnPosted(turn::LoadRun{});
    CHECK(model.CurrentBusy() == app::RunModel::Busy::Opening);
    model.Apply(turn::Error{"run belongs to another world"});
    CHECK_FALSE(model.Open());
    CHECK(model.History().empty());
    CHECK(model.LastError() == "run belongs to another world");
}

TEST_CASE("Undo takes the history and state the worker reports", "[run_model]")
{
    app::RunModel model = OpenModel({Record(1, "I walk.", "You walk.")});
    model.OnPosted(turn::UndoTurn{});
    CHECK(model.CurrentBusy() == app::RunModel::Busy::Undo);
    model.Apply(turn::StateChanged{{}, nlohmann::json::object(), {}});
    CHECK(model.History().empty());
    CHECK(model.CurrentBusy() == app::RunModel::Busy::None);
    CHECK_FALSE(model.LastAssembly());
}

TEST_CASE("Maintenance reports update the memory between commands", "[run_model]")
{
    app::RunModel model = OpenModel();
    turn::MemoryUpdated updated;
    updated.memory.summaries = {{1, 2, "The keeper's story."}};
    model.Apply(updated);
    CHECK(model.Memory().summaries.size() == 1);
    CHECK(model.CurrentBusy() == app::RunModel::Busy::None);

    model.Apply(turn::MaintenanceFailed{"summary_cap too small"});
    CHECK(model.MaintenanceError() == "summary_cap too small");
    model.Apply(updated);
    CHECK(model.MaintenanceError().empty()); // It finished since.
}

TEST_CASE("RunModel rejects commands and events the UI cannot produce", "[run_model]")
{
    app::RunModel closed;
    CHECK_THROWS_AS(closed.OnPosted(turn::SubmitInput{"I walk."}), core::ContractError); // No run open.
    CHECK_THROWS_AS(closed.Apply(turn::TurnCancelled{}), core::ContractError);           // No command running.

    app::RunModel model = OpenModel();
    CHECK_THROWS_AS(model.OnPosted(turn::UndoTurn{}), core::ContractError); // No turn to undo.
    CHECK_THROWS_AS(model.Apply(turn::TokenStreamed{"x"}), core::ContractError);
    model.OnPosted(turn::SubmitInput{"I walk."});
    CHECK_THROWS_AS(model.OnPosted(turn::SubmitInput{"I run."}), core::ContractError); // One at a time.
}
