#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "TestPaths.h"
#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "core/TurnRecord.h"
#include "store/RunStore.h"
#include "store/Sqlite.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

// A hosted run's meta and host data, as the hosted store tests make them.
store::RunMeta Meta()
{
    store::RunMeta meta;
    meta.kind = store::RunKind::Hosted;
    meta.worldId = "hosted";
    meta.worldHash = "0123456789abcdef";
    meta.modelId = "model";
    meta.rngSeed = 42;
    meta.settings = nlohmann::json::object();
    return meta;
}

// Turn `number` of conversation `id` at `clock`: npc-1 speaks to npc-2 and the player.
core::HostedTurn Turn(int number, const std::string& id, double clock)
{
    core::HostedTurn turn;
    turn.number = number;
    turn.conversationId = id;
    turn.speakerId = "npc-1";
    turn.listenerIds = {"npc-2", "player"};
    turn.observation = {{"actors",
                         {{{"id", "npc-1"}, {"name", "Hulda"}, {"is_player", false}},
                          {{"id", "npc-2"}, {"name", "Saadia"}, {"is_player", false}},
                          {{"id", "player"}, {"name", "Aela"}, {"is_player", true}}}}};
    turn.line = "Line " + std::to_string(number) + ".";
    turn.clock = clock;
    return turn;
}

// npc-`actor`'s fold of conversation `id` over `from`..`to`.
core::Summary Fold(const std::string& actor, const std::string& id, int from, int to)
{
    core::Summary summary;
    summary.fromTurn = from;
    summary.toTurn = to;
    summary.text = actor + " remembers " + std::to_string(from) + "-" + std::to_string(to) + ".";
    summary.actorId = actor;
    summary.conversationId = id;
    return summary;
}

// A run with conversation c1: turns 1-4 at clocks 1-4, an event at 2.5, a leave at 3.5, ended at 4.5; npc-1 folded
// turns 1-2 and 3-4 (marks at 4), npc-2 folded 1-4 in one summary.
std::filesystem::path Run(const char* name, std::string& id)
{
    const std::filesystem::path file = test::FreshTempDir(name) / "run.db";
    store::RunStore store = store::RunStore::CreateHosted(file, Meta(), nlohmann::json{{"catalog", nlohmann::json::object()}});
    const std::vector<core::Participant> participants = {{"npc-1", "Hulda", ""}, {"npc-2", "Saadia", ""}, {"player", "Aela", ""}};
    id = store.CreateConversation(participants, 0.5);
    store.CommitHostedTurn(Turn(1, id, 1.0));
    store.CommitHostedTurn(Turn(2, id, 2.0));
    store.AddConversationEvents(id, 2, {nlohmann::json{{"text", "A dragon roars."}}}, 2.5);
    store.CommitHostedTurn(Turn(3, id, 3.0));
    store.SetParticipants(id, {participants[0], participants[2]}, 3.5);
    store.CommitHostedTurn(Turn(4, id, 4.0));
    store.EndConversation(id, 4.5);
    store.FoldConversation(id, Fold("npc-1", id, 1, 2));
    store.FoldConversation(id, Fold("npc-1", id, 3, 4));
    store.RememberFold(Fold("npc-2", id, 1, 4));
    return file;
}

// npc-1's share of conversation `id` with npc-3, created at clock 2.0.
core::PendingShare Share(const std::string& id)
{
    core::PendingShare share;
    share.fromActor = "npc-1";
    share.toActor = "npc-3";
    share.conversationId = id;
    share.clock = 2.0;
    return share;
}

// npc-3's copy of npc-1's fold of turns 1-2, made when the share was applied at `clock`.
core::Summary Copy(const std::string& id, double clock)
{
    core::Summary copy = Fold("npc-3", id, 1, 2);
    copy.toldBy = "npc-1";
    copy.clock = clock;
    return copy;
}

} // namespace

TEST_CASE("A rollback hides what came after its time, moves the marks back, and an undo restores it exactly",
          "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_apply_undo", id));
    const store::RollbackCounts applied = store.ApplyRollback(2.7);
    CHECK(applied.hiddenTurns == 2);
    REQUIRE(store.OpenRollback().has_value());
    CHECK(store.OpenRollback()->gameTime == 2.7);

    // Turns 3-4, the leave and the end are hidden; the event at 2.5 stays.
    CHECK(store.HostedTurns(id).size() == 2);
    CHECK(store.LatestHostedTurn() == 2);
    CHECK(store.ConversationEvents(id).size() == 1);
    CHECK(store.Conversation(id)->participants.size() == 3);
    CHECK_FALSE(store.Conversation(id)->ended);
    CHECK(store.LatestClock() == 2.5);

    // npc-1's 3-4 is hidden, its 1-2 stays; npc-2's 1-4 straddles: hidden, and its mark moves back before it.
    const std::vector<core::Summary> npc1 = store.ActorSummaries("npc-1", false);
    REQUIRE(npc1.size() == 1);
    CHECK(npc1[0].toTurn == 2);
    CHECK(store.ActorSummaries("npc-2", true).empty());
    CHECK(store.Conversation(id)->foldedThrough == 2);
    CHECK(store.RememberedThrough(id, "npc-1") == 2);
    CHECK(store.RememberedThrough(id, "npc-2") == 0);

    // A summary written while the rollback is open belongs to it.
    store.RememberFold(Fold("npc-2", id, 1, 2));
    CHECK(store.ActorSummaries("npc-2", false).at(0).madeIn == store.OpenRollback()->id);

    // Undo: everything back as it was; the refold is gone.
    const store::RollbackCounts undone = store.UndoRollback();
    CHECK(undone.restoredTurns == 2);
    CHECK_FALSE(store.OpenRollback().has_value());
    CHECK(store.HostedTurns(id).size() == 4);
    CHECK(store.Conversation(id)->ended);
    CHECK(store.Conversation(id)->participants.size() == 2);
    CHECK(store.Conversation(id)->foldedThrough == 4);
    CHECK(store.RememberedThrough(id, "npc-2") == 4);
    const std::vector<core::Summary> npc2 = store.ActorSummaries("npc-2", true);
    REQUIRE(npc2.size() == 1);
    CHECK(npc2[0].toTurn == 4);
    CHECK(store.LatestClock() == 4.5);
}

TEST_CASE("A rollback shows again what a hidden condense superseded", "[store][rollback]")
{
    std::string id;
    const std::filesystem::path file = Run("rollback_condense", id);
    {
        store::RunStore store = store::RunStore::Open(file);
        core::Summary condensed;
        condensed.fromTurn = 1;
        condensed.toTurn = 4;
        condensed.text = "npc-1 condensed.";
        condensed.actorId = "npc-1";
        store.CondenseActorSummaries("npc-1", condensed);
        REQUIRE(store.ActorSummaries("npc-1", false).size() == 1);
    }
    store::RunStore store = store::RunStore::Open(file);
    store.ApplyRollback(2.7);
    // The condense covers hidden turns: hidden; the 1-2 it superseded is shown again, the 3-4 stays hidden.
    const std::vector<core::Summary> shown = store.ActorSummaries("npc-1", false);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].text == "npc-1 remembers 1-2.");
    CHECK(store.RememberedThrough(id, "npc-1") == 2);
}

TEST_CASE("Abandoning keeps the hidden rows hidden for good and the refolds as ordinary memories", "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_abandon", id));
    store.ApplyRollback(2.7);
    store.RememberFold(Fold("npc-2", id, 1, 2));
    store.AbandonRollback();
    CHECK_FALSE(store.OpenRollback().has_value());
    CHECK(store.HostedTurns(id).size() == 2);
    CHECK(store.ActorSummaries("npc-2", false).at(0).madeIn == 0);
    CHECK(store.NextHostedTurn() == 5);
    CHECK_THROWS_AS(store.UndoRollback(), core::ContractError);
}

TEST_CASE("A share applied after the rollback's time is pending again; one made after it is hidden", "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_shares", id));
    core::PendingShare early;
    early.fromActor = "npc-1";
    early.toActor = "npc-3";
    early.conversationId = id;
    early.clock = 2.0;
    const std::int64_t earlyId = store.AddPendingShare(early);
    store.MarkShareApplied(earlyId, 4.6);
    core::Summary copy = Fold("npc-3", id, 1, 2);
    copy.toldBy = "npc-1";
    copy.clock = 4.6;
    store.AddActorSummary(copy);
    core::PendingShare late = early;
    late.clock = 4.7;
    store.AddPendingShare(late);
    CHECK(store.PendingShares().size() == 1); // The late one.

    store.ApplyRollback(2.7);
    const std::vector<core::PendingShare> pending = store.PendingShares();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].id == earlyId);
    CHECK(store.ActorSummaries("npc-3", true).empty());

    store.UndoRollback();
    CHECK(store.PendingShares().size() == 1);
    CHECK(store.ActorSummaries("npc-3", true).size() == 1);
}

TEST_CASE("A share pending at the rollback and applied while it is open is pending again after an undo",
          "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_share_pending", id));
    const std::int64_t shareId = store.AddPendingShare(Share(id));
    store.ApplyRollback(2.7);
    REQUIRE(store.PendingShares().size() == 1);

    // Maintain applies it while the rollback is open: its copy is made in the rollback.
    store.AddActorSummary(Copy(id, 2.5));
    store.MarkShareApplied(shareId, 2.5);
    CHECK(store.PendingShares().empty());
    CHECK(store.ActorSummaries("npc-3", true).at(0).madeIn == store.OpenRollback()->id);

    // Undo: pending again, as it was, and the copy is gone.
    store.UndoRollback();
    const std::vector<core::PendingShare> pending = store.PendingShares();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].id == shareId);
    CHECK_FALSE(pending[0].appliedClock.has_value());
    CHECK(pending[0].unappliedBy == 0);
    CHECK(store.ActorSummaries("npc-3", true).empty());
}

TEST_CASE("A share unapplied by a rollback and applied again while it is open keeps its first application after an undo",
          "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_share_reapplied", id));
    const std::int64_t shareId = store.AddPendingShare(Share(id));
    store.MarkShareApplied(shareId, 4.6);
    store.AddActorSummary(Copy(id, 4.6));
    store.ApplyRollback(2.7);
    REQUIRE(store.PendingShares().size() == 1);

    // Maintain applies it again while the rollback is open.
    store.AddActorSummary(Copy(id, 2.5));
    store.MarkShareApplied(shareId, 2.5);
    CHECK(store.PendingShares().empty());

    // Undo: applied as it was, its first copy shown again and the second gone.
    store.UndoRollback();
    CHECK(store.PendingShares().empty());
    const std::vector<core::Summary> copies = store.ActorSummaries("npc-3", true);
    REQUIRE(copies.size() == 1);
    CHECK(copies[0].clock == 4.6);

    // Its applied clock is 4.6 again: a rollback to 4.0 unapplies it and hides that copy.
    store.ApplyRollback(4.0);
    const std::vector<core::PendingShare> pending = store.PendingShares();
    REQUIRE(pending.size() == 1);
    CHECK(pending[0].id == shareId);
    CHECK(pending[0].appliedClock == 4.6);
    CHECK(pending[0].unappliedBy == store.OpenRollback()->id);
    CHECK(store.ActorSummaries("npc-3", true).empty());
}

TEST_CASE("Rollback transactions refuse a second open rollback and a missing one", "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_contracts", id));
    CHECK_THROWS_AS(store.UndoRollback(), core::ContractError);
    CHECK_THROWS_AS(store.AbandonRollback(), core::ContractError);
    store.ApplyRollback(2.7);
    CHECK_THROWS_AS(store.ApplyRollback(1.5), core::ContractError);
}

TEST_CASE("Damaged rollback rows are StoreErrors naming the file", "[store][rollback]")
{
    // Saved marks that are not JSON, or JSON of the wrong shape in any part.
    const std::vector<std::string> damaged = {
        "nope",
        "[]",
        R"({"remembered": [], "shares": []})",
        R"({"folded_through": [], "remembered": [], "shares": []})",
        R"({"folded_through": {"c1": "4"}, "remembered": [], "shares": []})",
        R"({"folded_through": {}, "shares": []})",
        R"({"folded_through": {}, "remembered": {}, "shares": []})",
        R"({"folded_through": {}, "remembered": [["c1", "npc-1"]], "shares": []})",
        R"({"folded_through": {}, "remembered": [["c1", 1, 2]], "shares": []})",
        R"({"folded_through": {}, "remembered": []})",
        R"({"folded_through": {}, "remembered": [], "shares": {}})",
        R"({"folded_through": {}, "remembered": [], "shares": [[1, 1, 4.6]]})",
        R"({"folded_through": {}, "remembered": [], "shares": [[1, 1, "4.6", 0]]})",
        R"({"folded_through": {}, "remembered": [], "shares": [[1.5, 1, null, 0]]})",
    };
    for (const std::string& marks : damaged) {
        CAPTURE(marks);
        std::string id;
        const std::filesystem::path file = Run("rollback_damaged", id);
        {
            store::RunStore store = store::RunStore::Open(file);
            store.ApplyRollback(2.7);
        }
        {
            store::Database db = store::Database::Open(file, false);
            db.Prepare("UPDATE rollbacks SET saved_marks = ?1").Bind(1, marks).Run();
        }
        store::RunStore store = store::RunStore::Open(file);
        CHECK_THROWS_WITH(store.UndoRollback(), ContainsSubstring("run.db") && ContainsSubstring("saved_marks"));
        // Nothing changed: the rollback is still open and its rows still hidden.
        CHECK(store.OpenRollback().has_value());
        CHECK(store.HostedTurns(id).size() == 2);
    }
}

TEST_CASE("A hidden row naming no rollback, or two open rollbacks, is a StoreError on open", "[store][rollback]")
{
    std::string id;
    const std::filesystem::path dangling = Run("rollback_dangling", id);
    {
        store::Database db = store::Database::Open(dangling, false);
        db.Exec("UPDATE hosted_turns SET hidden_by = 7 WHERE number = 4");
    }
    CHECK_THROWS_WITH(store::RunStore::Open(dangling),
                      ContainsSubstring("run.db") && ContainsSubstring("hosted_turns") && ContainsSubstring("hidden_by"));

    const std::filesystem::path twoOpen = Run("rollback_two_open", id);
    {
        store::Database db = store::Database::Open(twoOpen, false);
        db.Exec("INSERT INTO rollbacks (game_time, status, saved_marks) VALUES (1.0, 'open', '{}'), "
                "(2.0, 'open', '{}')");
    }
    CHECK_THROWS_WITH(store::RunStore::Open(twoOpen), ContainsSubstring("run.db") && ContainsSubstring("rollbacks"));
}
