#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
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

TEST_CASE("An undone or abandoned rollback keeps no saved marks", "[store][rollback]")
{
    // The host rewinds on every save load, so a rollback row that kept its marks after it closed would grow the file
    // by every mark and share of the run on every load.
    std::string id;
    const std::filesystem::path file = Run("rollback_marks_cleared", id);
    {
        store::RunStore store = store::RunStore::Open(file);
        store.ApplyRollback(2.7);
        store.UndoRollback();
        store.ApplyRollback(3.5);
        store.AbandonRollback();
    }
    store::Database db = store::Database::Open(file, false);
    store::Statement rows = db.Prepare("SELECT status, saved_marks FROM rollbacks ORDER BY id");
    REQUIRE(rows.Step());
    CHECK(rows.ColumnText(0) == "undone");
    CHECK(rows.ColumnText(1) == "{}");
    REQUIRE(rows.Step());
    CHECK(rows.ColumnText(0) == "abandoned");
    CHECK(rows.ColumnText(1) == "{}");
    CHECK_FALSE(rows.Step());
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

TEST_CASE("A rollback hides a condense that holds a shared copy made after its time", "[store][rollback]")
{
    // c1: turns 1-3, npc-1 folds 1-3, ended. c2: turns 4-5, npc-2 folds 4-5, ended. c3: turns 6-7. At 7, npc-2's memory
    // of c2 is shared with npc-1 (a copy clocked 7); then npc-1 condenses its 1-3 and that copy into one 1-5.
    const std::filesystem::path file = test::FreshTempDir("rollback_condensed_copy") / "run.db";
    store::RunStore store =
        store::RunStore::CreateHosted(file, Meta(), nlohmann::json{{"catalog", nlohmann::json::object()}});
    const std::vector<core::Participant> participants = {{"npc-1", "Hulda", ""}, {"npc-2", "Saadia", ""}, {"player", "Aela", ""}};
    const std::string c1 = store.CreateConversation(participants, 0.5);
    store.CommitHostedTurn(Turn(1, c1, 1.0));
    store.CommitHostedTurn(Turn(2, c1, 2.0));
    store.CommitHostedTurn(Turn(3, c1, 3.0));
    store.EndConversation(c1, 3.0);
    const std::int64_t fold = store.FoldConversation(c1, Fold("npc-1", c1, 1, 3));
    const std::string c2 = store.CreateConversation(participants, 3.5);
    store.CommitHostedTurn(Turn(4, c2, 4.0));
    store.CommitHostedTurn(Turn(5, c2, 5.0));
    store.EndConversation(c2, 5.0);
    const std::int64_t told = store.FoldConversation(c2, Fold("npc-2", c2, 4, 5));
    const std::string c3 = store.CreateConversation(participants, 5.5);
    store.CommitHostedTurn(Turn(6, c3, 6.0));
    store.CommitHostedTurn(Turn(7, c3, 7.0));
    core::Summary copy = Fold("npc-1", c2, 4, 5);
    copy.toldBy = "npc-2";
    copy.sourceId = told;
    copy.clock = 7.0;
    store.AddActorSummary(copy);
    core::Summary condensed;
    condensed.fromTurn = 1;
    condensed.toTurn = 5;
    condensed.text = "npc-1 condensed.";
    condensed.actorId = "npc-1";
    const std::int64_t condense = store.CondenseActorSummaries("npc-1", condensed);
    REQUIRE(store.ActorSummaries("npc-1", false).size() == 1);

    // A rollback to 6.5 hides turn 7 and the copy (its clock), and the condense that holds the copy with it, though
    // the condense ends at turn 5: npc-1 shows its own 1-3 again and nothing of what it was told at 7.
    store.ApplyRollback(6.5);
    const std::vector<core::Summary> shown = store.ActorSummaries("npc-1", false);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].id == fold);
    CHECK(store.ActorSummaries("npc-1", true).size() == 1);

    // Undo: the condense is shown again, over the fold and the copy.
    store.UndoRollback();
    const std::vector<core::Summary> restored = store.ActorSummaries("npc-1", false);
    REQUIRE(restored.size() == 1);
    CHECK(restored[0].id == condense);
    CHECK(store.ActorSummaries("npc-1", true).size() == 3);
}

TEST_CASE("A rollback hides a condense whose range ends before the end of a hidden fold it holds", "[store][rollback]")
{
    // Interleaved conversations: c1 has turns 1, 2, 5 and 6, c2 turns 3 and 4 (clocks are the turn numbers). npc-1
    // folds c1's 1-6; npc-2's fold of c2 (3-4) is shared with npc-1 at 4; npc-1's condense of [1-6, 3-4] ranges 1-4
    // (the condense's range runs from its first summary's fromTurn to its last one's toTurn, by fromTurn).
    const std::filesystem::path file = test::FreshTempDir("rollback_condensed_nested") / "run.db";
    store::RunStore store =
        store::RunStore::CreateHosted(file, Meta(), nlohmann::json{{"catalog", nlohmann::json::object()}});
    const std::vector<core::Participant> participants = {{"npc-1", "Hulda", ""}, {"npc-2", "Saadia", ""}, {"player", "Aela", ""}};
    const std::string c1 = store.CreateConversation(participants, 0.5);
    const std::string c2 = store.CreateConversation(participants, 0.5);
    store.CommitHostedTurn(Turn(1, c1, 1.0));
    store.CommitHostedTurn(Turn(2, c1, 2.0));
    store.CommitHostedTurn(Turn(3, c2, 3.0));
    store.CommitHostedTurn(Turn(4, c2, 4.0));
    const std::int64_t told = store.FoldConversation(c2, Fold("npc-2", c2, 3, 4));
    core::Summary copy = Fold("npc-1", c2, 3, 4);
    copy.toldBy = "npc-2";
    copy.sourceId = told;
    copy.clock = 4.0;
    const std::int64_t copyId = store.AddActorSummary(copy);
    store.CommitHostedTurn(Turn(5, c1, 5.0));
    store.CommitHostedTurn(Turn(6, c1, 6.0));
    store.FoldConversation(c1, Fold("npc-1", c1, 1, 6));
    core::Summary condensed;
    condensed.fromTurn = 1;
    condensed.toTurn = 4;
    condensed.text = "npc-1 condensed.";
    condensed.actorId = "npc-1";
    const std::int64_t condense = store.CondenseActorSummaries("npc-1", condensed);
    REQUIRE(store.ActorSummaries("npc-1", false).size() == 1);

    // A rollback to 4 hides turns 5-6 and npc-1's fold 1-6 (past c1's last visible turn, 2), and the condense that
    // holds that fold with it, though the condense ends at turn 4: npc-1 shows only the copy (3-4, at 4), and its
    // mark in c1 moves before the hidden fold so the catch-up refolds turns 1-2.
    store.ApplyRollback(4.0);
    const std::vector<core::Summary> shown = store.ActorSummaries("npc-1", false);
    REQUIRE(shown.size() == 1);
    CHECK(shown[0].id == copyId);
    CHECK(store.RememberedThrough(c1, "npc-1") == 0);

    // Undo: the condense is shown again, over the fold and the copy, and the mark is back at 6.
    store.UndoRollback();
    const std::vector<core::Summary> restored = store.ActorSummaries("npc-1", false);
    REQUIRE(restored.size() == 1);
    CHECK(restored[0].id == condense);
    CHECK(store.ActorSummaries("npc-1", true).size() == 3);
    CHECK(store.RememberedThrough(c1, "npc-1") == 6);
}

TEST_CASE("An undo deletes a condense made while the rollback was open, with its links and its embedding",
          "[store][rollback]")
{
    std::string id;
    const std::filesystem::path file = Run("rollback_recondense", id);
    std::int64_t firstCondense = 0;
    std::int64_t refold = 0;
    {
        store::RunStore store = store::RunStore::Open(file);
        core::Summary condensed;
        condensed.fromTurn = 1;
        condensed.toTurn = 4;
        condensed.text = "npc-1 condensed.";
        condensed.actorId = "npc-1";
        firstCondense = store.CondenseActorSummaries("npc-1", condensed);
        store.ApplyRollback(2.7);

        // While it is open, npc-1 condenses again: the 1-2 shown again is linked to the new condense, which gets a vector.
        core::Summary again;
        again.fromTurn = 1;
        again.toTurn = 2;
        again.text = "npc-1 condensed again.";
        again.actorId = "npc-1";
        refold = store.CondenseActorSummaries("npc-1", again);
        store.PutEmbedding({store::EmbeddingKind::Summary, std::to_string(refold), "embedder", {1.0f, 0.0f}, "hash"});
        REQUIRE(store.ActorSummaries("npc-1", false).at(0).id == refold);

        // Undo: the refold and its vector are gone; npc-1 shows only the first condense, its mark is back at 4.
        store.UndoRollback();
        CHECK(store.Embeddings(store::EmbeddingKind::Summary, "embedder").empty());
        const std::vector<core::Summary> shown = store.ActorSummaries("npc-1", false);
        REQUIRE(shown.size() == 1);
        CHECK(shown[0].id == firstCondense);
        CHECK(store.RememberedThrough(id, "npc-1") == 4);
        for (const core::Summary& summary : store.ActorSummaries("npc-1", true)) {
            if (summary.id != firstCondense) {
                CHECK(summary.supersededBy == firstCondense);
            }
        }
    }
    // The refold's supersession links went with it; the first condense's links are still there.
    store::Database db = store::Database::Open(file, false);
    store::Statement links = db.Prepare("SELECT COUNT(*) FROM summary_supersessions WHERE condensed_id = ?1");
    links.Bind(1, refold);
    REQUIRE(links.Step());
    CHECK(links.ColumnInt(0) == 0);
    store::Statement kept = db.Prepare("SELECT COUNT(*) FROM summary_supersessions WHERE condensed_id = ?1");
    kept.Bind(1, firstCondense);
    REQUIRE(kept.Step());
    CHECK(kept.ColumnInt(0) == 2);
}

TEST_CASE("A conversation started after the rollback's time is hidden whole and comes back whole on undo",
          "[store][rollback]")
{
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_later_conversation", id));
    const std::vector<core::Participant> participants = {{"npc-1", "Hulda", ""}, {"npc-2", "Saadia", ""}, {"player", "Aela", ""}};
    const std::string later = store.CreateConversation(participants, 5.0);
    store.CommitHostedTurn(Turn(5, later, 5.0));
    store.AddConversationEvents(later, 5, {nlohmann::json{{"text", "A guard walks in."}}}, 5.5);
    // An event after the rollback's time in the conversation that survives it.
    store.AddConversationEvents(id, 4, {nlohmann::json{{"text", "The fire dies down."}}}, 5.5);

    const store::RollbackCounts applied = store.ApplyRollback(4.7);
    CHECK(applied.hiddenTurns == 1);
    const std::vector<core::Conversation> conversations = store.Conversations();
    REQUIRE(conversations.size() == 1);
    CHECK(conversations[0].id == id);
    CHECK_FALSE(store.Conversation(later).has_value());
    CHECK(store.HostedTurns(later).empty());
    CHECK(store.ConversationEvents(later).empty());
    CHECK(store.ConversationEvents(id).size() == 1);
    CHECK(store.HostedTurns(id).size() == 4);

    const store::RollbackCounts undone = store.UndoRollback();
    CHECK(undone.restoredTurns == 1);
    CHECK(store.Conversations().size() == 2);
    REQUIRE(store.Conversation(later).has_value());
    CHECK(store.Conversation(later)->participants.size() == 3);
    CHECK(store.HostedTurns(later).size() == 1);
    CHECK(store.ConversationEvents(later).size() == 1);
    CHECK(store.ConversationEvents(id).size() == 2);
}

TEST_CASE("The writers of new records refuse while a rollback is open", "[store][rollback]")
{
    // An undo is exact only if nothing new was stored while the rollback was open: the session abandons it first.
    std::string id;
    store::RunStore store = store::RunStore::Open(Run("rollback_refuses_new", id));
    store.ApplyRollback(2.7);
    const std::vector<core::Participant> participants = {{"npc-1", "Hulda", ""}, {"player", "Aela", ""}};
    CHECK_THROWS_MATCHES(store.CreateConversation(participants, 2.8), core::ContractError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("CreateConversation")));
    CHECK_THROWS_MATCHES(store.SetParticipants(id, participants, 2.8), core::ContractError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("SetParticipants")));
    CHECK_THROWS_MATCHES(store.EndConversation(id, 2.8), core::ContractError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("EndConversation")));
    CHECK_THROWS_MATCHES(store.AddConversationEvents(id, 2, {nlohmann::json{{"text", "A door opens."}}}, 2.8),
                         core::ContractError, Catch::Matchers::MessageMatches(ContainsSubstring("AddConversationEvents")));
    CHECK_THROWS_MATCHES(store.CommitHostedTurn(Turn(5, id, 2.8)), core::ContractError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("CommitHostedTurn")));
    CHECK_THROWS_MATCHES(store.AddPendingShare(Share(id)), core::ContractError,
                         Catch::Matchers::MessageMatches(ContainsSubstring("AddPendingShare")));
    // Nothing was written: the undo restores the run as it was.
    store.UndoRollback();
    CHECK(store.Conversations().size() == 1);
    CHECK(store.HostedTurns(id).size() == 4);
    CHECK(store.ConversationEvents(id).size() == 1);
    CHECK(store.PendingShares().empty());
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
