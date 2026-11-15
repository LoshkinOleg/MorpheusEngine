#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Errors.h"
#include "core/HostedRecord.h"
#include "fakes/ScriptedBackend.h"
#include "hosted/HostedTestSupport.h"
#include "inference/Llm.h"
#include "server/RunWorker.h"
#include "server/ServerTestSupport.h"
#include "server/SlotBackend.h"
#include "server/SlotPool.h"
#include "turn/EngineText.h"
#include "turn/HostedSession.h"
#include "turn/HostedWorld.h"

using namespace morpheus;
using Catch::Matchers::ContainsSubstring;

namespace {

const std::string NONE = R"({"action":"none","params":{}})";

// A job that does nothing.
void Nothing(turn::HostedSession&, server::SlotBackend&, std::stop_token)
{
}

} // namespace

TEST_CASE("RunWorker opens its run and runs jobs one at a time, in arrival order, on its own thread",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    const std::filesystem::path file = test::HostedRunFile("worker_order");
    std::unique_ptr<server::RunWorker> worker = test::MakeWorker("worker_order", file, script.Responder());
    CHECK(worker->RunId() == "worker_order");
    CHECK(std::filesystem::exists(file));

    std::vector<int> order;
    std::thread::id jobThread;
    std::vector<std::future<void>> done;
    for (int i = 0; i < 5; ++i) {
        done.push_back(worker->Post([&order, &jobThread, i](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
            order.push_back(i);
            jobThread = std::this_thread::get_id();
        }));
    }
    for (std::future<void>& job : done) {
        job.get();
    }
    CHECK(order == std::vector<int>{0, 1, 2, 3, 4});
    CHECK(jobThread != std::this_thread::get_id());
}

TEST_CASE("A job's exception reaches its caller, and the worker goes on", "[hosted-server][worker]")
{
    test::HostedScript script;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_throw", test::HostedRunFile("worker_throw"), script.Responder());
    CHECK_THROWS_AS(worker->Call([](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
                        throw core::DataError("speaker: no such actor");
                    }),
                    core::DataError);
    bool ran = false;
    worker->Call([&ran](turn::HostedSession&, server::SlotBackend&, std::stop_token) { ran = true; });
    CHECK(ran);
}

TEST_CASE("A RunWorker that cannot open its run throws the open's error and leaves no file", "[hosted-server][worker]")
{
    test::HostedScript script;
    const std::filesystem::path file = test::HostedRunFile("worker_bad_data");
    turn::HostData bad = test::FixtureHostData();
    bad.catalog = 42; // Not an action catalog: HostedWorld's DataError, before the file is touched.
    CHECK_THROWS_AS(server::RunWorker("worker_bad_data", file, bad, test::HostedSettings(), 42,
                                      std::make_unique<test::ScriptedBackend>(16384, script.Responder()),
                                      [](const std::string&) {}),
                    core::DataError);
    CHECK_FALSE(std::filesystem::exists(file));
}

TEST_CASE("Between jobs the worker folds ended conversations on the upkeep slot; a job's calls use the slot it sets",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"Welcome."};
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_fold", test::HostedRunFile("worker_fold"), script.Responder(), &pBackend);

    std::string id;
    bool played = false;
    worker->Call([&](turn::HostedSession& session, server::SlotBackend& backend, std::stop_token stop) {
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        backend.SetSlot(2);
        played = session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, stop).has_value();
        session.EndConversation(id);
    });
    REQUIRE(played);

    // Folding happens in the background once the queue is empty: wait until the conversation's turn is folded.
    const bool folded = test::WaitUntil([&] {
        int through = 0;
        worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            through = session.Conversation(id)->foldedThrough;
        });
        return through == 1;
    });
    CHECK(folded);

    // The turn's two calls ran on slot 2, the fold's on the upkeep slot.
    std::vector<int> slots;
    std::vector<bool> folds;
    worker->Call([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
        for (const test::ScriptedBackend::Call& call : pBackend->Calls()) {
            slots.push_back(call.spec.slot);
            folds.push_back(call.prompt.messages.back().content == turn::SUMMARIZE_INSTRUCTION);
        }
    });
    REQUIRE(slots.size() >= 3);
    CHECK(slots[0] == 2);
    CHECK(slots[1] == 2);
    for (std::size_t i = 2; i < slots.size(); ++i) {
        CHECK(slots[i] == server::SlotPool::UPKEEP_SLOT);
        CHECK(folds[i]);
    }
}

namespace {

// A worker whose run has one ended conversation of one turn, and whose fold streams slowly: its summary is 9 pieces,
// each after `pieceDelay`. `foldCalls` counts the fold calls; `*ppBackend` gets the run's backend. The fold starts in
// the background as soon as this returns.
std::unique_ptr<server::RunWorker> SlowFoldWorker(const std::string& name, std::chrono::milliseconds pieceDelay,
                                                  test::HostedScript& script, std::atomic<int>& foldCalls,
                                                  test::ScriptedBackend** ppBackend, std::string& id)
{
    script.actions = {NONE};
    script.lines = {"Welcome."};
    test::ScriptedBackend::Responder responder = [&script, &foldCalls](const inference::Prompt& prompt,
                                                                       const inference::GenerationSpec& spec) {
        if (prompt.messages.back().content == turn::SUMMARIZE_INSTRUCTION) {
            ++foldCalls;
            return std::string("Summary of a long talk at the inn.");
        }
        return script.Responder()(prompt, spec);
    };
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker(name, test::HostedRunFile(name), responder, ppBackend);
    worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token stop) {
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        session.RunTurn(test::Request(id, test::HULDA, "Hello."), {}, stop);
        (*ppBackend)->SetPieceDelay(pieceDelay);
        session.EndConversation(id);
    });
    return worker;
}

} // namespace

TEST_CASE("A job that does not preempt waits for a running fold, which lands first", "[hosted-server][worker]")
{
    // The fold streams for about 900 ms.
    test::HostedScript script;
    std::atomic<int> foldCalls = 0;
    test::ScriptedBackend* pBackend = nullptr;
    std::string id;
    std::unique_ptr<server::RunWorker> worker =
        SlowFoldWorker("worker_fold_wait", std::chrono::milliseconds(100), script, foldCalls, &pBackend, id);

    // While the fold streams, a job arrives: it runs once the fold has landed, which was not started over.
    REQUIRE(test::WaitUntil([&] { return foldCalls.load() >= 1; }));
    int through = 0;
    worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
        through = session.Conversation(id)->foldedThrough;
    });
    CHECK(through == 1);
    CHECK(foldCalls.load() == 1);
}

TEST_CASE("A preempting job stops a running fold at once, also from behind a job that waits; the fold runs again",
          "[hosted-server][worker]")
{
    // The fold streams for about 2 s.
    test::HostedScript script;
    std::atomic<int> foldCalls = 0;
    test::ScriptedBackend* pBackend = nullptr;
    std::string id;
    std::unique_ptr<server::RunWorker> worker =
        SlowFoldWorker("worker_fold_stop", std::chrono::milliseconds(220), script, foldCalls, &pBackend, id);

    // While the fold streams, a job that waits arrives, then a preempting one behind it: the preempting one stops the
    // fold when it is queued, so both run at once, in order.
    REQUIRE(test::WaitUntil([&] { return foldCalls.load() >= 1; }));
    const auto start = std::chrono::steady_clock::now();
    std::vector<int> order;
    std::future<void> waiting =
        worker->Post([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) { order.push_back(1); });
    std::future<void> preempting = worker->Post(
        [&](turn::HostedSession&, server::SlotBackend&, std::stop_token) {
            order.push_back(2);
            pBackend->SetPieceDelay(std::chrono::milliseconds(0));
        },
        server::RunWorker::Preempt::Fold);
    waiting.get();
    preempting.get();
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::milliseconds(1000));
    CHECK(order == std::vector<int>{1, 2});

    // Then the fold runs again, now without the delay, and lands.
    const bool folded = test::WaitUntil([&] {
        int through = 0;
        worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            through = session.Conversation(id)->foldedThrough;
        });
        return through == 1;
    });
    CHECK(folded);
    CHECK(foldCalls.load() >= 2);
}

TEST_CASE("Stopping the worker stops a running turn, which commits nothing, and refuses later jobs",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    script.actions = {NONE};
    script.lines = {"A long answer that streams slowly, piece by piece."};
    const std::filesystem::path file = test::HostedRunFile("worker_stop");
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker = test::MakeWorker("worker_stop", file, script.Responder(), &pBackend);

    std::atomic<bool> streaming = false;
    std::string id;
    std::optional<core::HostedTurn> result;
    std::future<void> turn = worker->Post([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token stop) {
        pBackend->SetPieceDelay(std::chrono::milliseconds(100));
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
        turn::HostedCallbacks callbacks;
        callbacks.onToken = [&streaming](std::string_view) { streaming = true; };
        result = session.RunTurn(test::Request(id, test::HULDA, "Hello."), callbacks, stop);
    });
    REQUIRE(test::WaitUntil([&] { return streaming.load(); }));
    worker->RequestStop();
    turn.get(); // The job returned: the stop cancelled its turn.
    CHECK_FALSE(result.has_value());
    CHECK_THROWS_AS(worker->Call(Nothing), inference::InferenceError);
    CHECK_THROWS_WITH(worker->Call(Nothing), ContainsSubstring("the server is stopping"));
    worker.reset();

    // Nothing was committed.
    test::ScriptedBackend fresh(16384, script.Responder());
    turn::HostedSession reopened =
        turn::HostedSession::Open(file, test::FixtureHostData(), test::HostedSettings(), 42, fresh);
    CHECK(reopened.Turns(id).empty());
}

TEST_CASE("Reopen: the same data changes nothing, changed data applies, and a failed reopen leaves the run as it was",
          "[hosted-server][worker]")
{
    test::HostedScript script;
    test::ScriptedBackend* pBackend = nullptr;
    std::unique_ptr<server::RunWorker> worker =
        test::MakeWorker("worker_reopen", test::HostedRunFile("worker_reopen"), script.Responder(), &pBackend);
    // What the run looks like now: its stored host data's hash, and whether conversation `id` is still there.
    std::string id;
    auto look = [&worker, &id] {
        std::pair<std::string, bool> seen;
        worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
            seen = {session.Meta().worldHash, session.Conversation(id).has_value()};
        });
        return seen;
    };
    worker->Call([&](turn::HostedSession& session, server::SlotBackend&, std::stop_token) {
        id = session.StartConversation({test::HULDA, test::AELA}, test::FixtureObservation());
    });
    const std::string original = turn::HostDataHash(test::FixtureHostData());
    CHECK(look() == std::pair<std::string, bool>{original, true});

    // The same data: nothing changes.
    worker->Reopen(test::FixtureHostData(), 42);
    CHECK(look() == std::pair<std::string, bool>{original, true});

    // Changed data (a mod update): the run reopens with it and keeps its conversations.
    turn::HostData changed = test::FixtureHostData();
    changed.lore.push_back({"The mill", "The mill burned down last winter.", false});
    worker->Reopen(changed, 42);
    CHECK(look() == std::pair<std::string, bool>{turn::HostDataHash(changed), true});

    // Bad data: HostedWorld's DataError, and the open run is untouched.
    turn::HostData bad = changed;
    bad.catalog = 42;
    CHECK_THROWS_AS(worker->Reopen(bad, 42), core::DataError);
    CHECK(look() == std::pair<std::string, bool>{turn::HostDataHash(changed), true});

    // Data whose open fails (its new lore cannot be embedded: Open's DataError naming the entry): the error, and the
    // run opens again as it was.
    worker->Call([&](turn::HostedSession&, server::SlotBackend&, std::stop_token) { pBackend->SetEmbedLimit(64); });
    turn::HostData unembeddable = changed;
    unembeddable.lore.push_back({"The road", std::string(200, 'z'), false});
    CHECK_THROWS_WITH(worker->Reopen(unembeddable, 42), ContainsSubstring("lore entry 'The road' cannot be embedded"));
    CHECK_THROWS_AS(worker->Reopen(unembeddable, 42), core::DataError);
    CHECK(look() == std::pair<std::string, bool>{turn::HostDataHash(changed), true});
}
