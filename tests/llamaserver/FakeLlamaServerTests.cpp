#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "fakes/FakeLlamaServer.h"

using namespace morpheus;

TEST_CASE("FakeLlamaServer answers from its script and records each request", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    httplib::Client client(fake.Url());

    const httplib::Result props = client.Get("/props");
    REQUIRE(props);
    CHECK(props->status == 200);
    CHECK(nlohmann::json::parse(props->body).at("default_generation_settings").at("n_ctx") == 16384);

    // One token per 4 bytes, rounded up.
    const httplib::Result tokens =
        client.Post("/tokenize", R"({"content":"abcde","add_special":false,"parse_special":false})", "application/json");
    REQUIRE(tokens);
    CHECK(nlohmann::json::parse(tokens->body).at("tokens").size() == 2);

    const httplib::Result rendered = client.Post(
        "/apply-template", R"({"messages":[{"role":"user","content":"Hi"}],"add_generation_prompt":true})", "application/json");
    REQUIRE(rendered);
    CHECK(nlohmann::json::parse(rendered->body).at("prompt") == "<|im_start|>user\nHi<|im_end|>\n<|im_start|>assistant\n");

    // llama-server's own /embedding answer: one result per text, holding the pooled vector.
    const httplib::Result embedded = client.Post("/embedding", R"({"content":"a lighthouse"})", "application/json");
    REQUIRE(embedded);
    CHECK(nlohmann::json::parse(embedded->body) == nlohmann::json::parse(R"([{"index":0,"embedding":[[3.0,4.0]]}])"));

    // A scripted failure.
    fake.SetStatus("/props", 503);
    const httplib::Result refused = client.Get("/props");
    REQUIRE(refused);
    CHECK(refused->status == 503);

    const std::vector<test::FakeLlamaServer::Request> requests = fake.Requests();
    REQUIRE(requests.size() == 5);
    CHECK(requests[0].method == "GET");
    CHECK(requests[0].path == "/props");
    CHECK(requests[0].body.is_null());
    CHECK(requests[1].method == "POST");
    CHECK(requests[1].path == "/tokenize");
    CHECK(requests[1].body.at("content") == "abcde");
    CHECK(fake.RequestsTo("/props").size() == 2);
}

TEST_CASE("FakeLlamaServer streams a scripted completion as llama-server does", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"Hel", "lo"};
    script.stopType = "limit";
    script.promptN = 1;
    fake.QueueCompletion(script);
    test::FakeLlamaServer::CompletionScript cut;
    cut.pieces = {"Hi"};
    cut.endEarly = true;
    fake.QueueCompletion(cut);

    // The messages of one streamed answer, in order: each "data: {...}" line, without blank lines and pings.
    httplib::Client client(fake.Url());
    auto stream = [&client](const std::string& body) {
        std::string received;
        const httplib::Result result =
            client.Post("/completion", httplib::Headers{}, body, "application/json", [&received](const char* pData, std::size_t length) {
                received.append(pData, length);
                return true;
            });
        REQUIRE(result);
        REQUIRE(result->status == 200);
        std::vector<nlohmann::json> messages;
        std::size_t start = 0;
        for (std::size_t end = received.find('\n'); end != std::string::npos; end = received.find('\n', start)) {
            const std::string line = received.substr(start, end - start);
            start = end + 1;
            if (line.starts_with("data: ")) {
                messages.push_back(nlohmann::json::parse(line.substr(6)));
            }
        }
        return messages;
    };

    // "abcdefgh" is 2 tokens by the default tokenizer: every message carries it as tokens_evaluated.
    const std::vector<nlohmann::json> messages = stream(R"({"prompt":"abcdefgh","id_slot":2})");
    REQUIRE(messages.size() == 3);
    CHECK(messages[0].at("content") == "Hel");
    CHECK(messages[0].at("stop") == false);
    CHECK(messages[0].at("tokens_evaluated") == 2);
    CHECK(messages[0].at("id_slot") == 2);
    CHECK(messages[1].at("content") == "lo");
    CHECK(messages[2].at("content") == "");
    CHECK(messages[2].at("stop") == true);
    CHECK(messages[2].at("stop_type") == "limit");
    CHECK(messages[2].at("tokens_evaluated") == 2);
    CHECK(messages[2].at("tokens_cached") == 4);
    CHECK(messages[2].at("timings").at("prompt_n") == 1);
    CHECK(messages[2].at("timings").at("predicted_n") == 2);
    CHECK(messages[2].at("timings").at("prompt_ms") == 12.5);
    CHECK(messages[2].at("timings").at("predicted_ms") == 40.0);

    // A script that ends early sends its pieces and no last message. A prompt of token ids counts as its length.
    const std::vector<nlohmann::json> cutMessages = stream(R"({"prompt":[5,6,7]})");
    REQUIRE(cutMessages.size() == 1);
    CHECK(cutMessages[0].at("content") == "Hi");
    CHECK(cutMessages[0].at("tokens_evaluated") == 3);

    // With nothing left to answer, the fake fails the request as llama-server fails one.
    const httplib::Result none = client.Post("/completion", R"({"prompt":"abc"})", "application/json");
    REQUIRE(none);
    CHECK(none->status == 500);
}

TEST_CASE("FakeLlamaServer sees a client close a held stream", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    test::FakeLlamaServer::CompletionScript script;
    script.pieces = {"Hel"};
    script.holdOpen = true;
    fake.QueueCompletion(script);
    {
        // The receiver refuses the first bytes, which ends the request; the client closes its connection.
        httplib::Client client(fake.Url());
        const httplib::Result result = client.Post("/completion", httplib::Headers{}, R"({"prompt":"abc"})",
                                                   "application/json", [](const char*, std::size_t) { return false; });
        CHECK_FALSE(result);
        CHECK(result.error() == httplib::Error::Canceled);
    }
    // The fake notices on one of its next pings.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fake.ClientClosedStream() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    CHECK(fake.ClientClosedStream());
}

TEST_CASE("FakeLlamaServer runs its callbacks outside its lock, so they may call it and requests overlap",
          "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    // A tokenizer that reads the fake's own request log would deadlock under the lock; two requests wait for each
    // other inside it, which would serialize (and time out) if the handlers held the lock around the callback.
    std::atomic<int> inside = 0;
    std::atomic<std::size_t> seen = 0;
    fake.SetTokenizer([&](const std::string&, bool, bool) {
        seen = fake.Requests().size();
        ++inside;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (inside < 2 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return std::vector<int>{1};
    });

    std::atomic<int> okCount = 0;
    auto ask = [&fake, &okCount] {
        httplib::Client client(fake.Url());
        const httplib::Result result =
            client.Post("/tokenize", R"({"content":"abc"})", "application/json");
        if (result && result->status == 200) {
            ++okCount;
        }
    };
    std::thread first(ask);
    std::thread second(ask);
    first.join();
    second.join();

    CHECK(okCount == 2);
    CHECK(inside == 2); // Both were inside the callback at the same time.
    CHECK(seen >= 1);
}

TEST_CASE("FakeLlamaServer records a malformed body and answers 400", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    httplib::Client client(fake.Url());
    const httplib::Result result = client.Post("/tokenize", "{not json", "application/json");
    REQUIRE(result);
    CHECK(result->status == 400);

    const std::vector<test::FakeLlamaServer::Request> requests = fake.RequestsTo("/tokenize");
    REQUIRE(requests.size() == 1);
    CHECK(requests[0].body == "{not json");
}

TEST_CASE("FakeLlamaServer records each request's connection, answers a raw body, and listens on a given port",
          "[fake-llama-server]")
{
    std::optional<test::FakeLlamaServer> fake(std::in_place);
    const int port = fake->Port();
    CHECK(fake->Url() == "http://127.0.0.1:" + std::to_string(port));

    // Requests over one kept-alive connection share the client's port; another client has a port of its own.
    {
        httplib::Client client(fake->Url());
        client.set_keep_alive(true);
        REQUIRE(client.Get("/props"));
        REQUIRE(client.Get("/props"));
        httplib::Client other(fake->Url());
        REQUIRE(other.Get("/props"));
    }
    const std::vector<test::FakeLlamaServer::Request> requests = fake->Requests();
    REQUIRE(requests.size() == 3);
    CHECK(requests[0].remotePort > 0);
    CHECK(requests[1].remotePort == requests[0].remotePort);
    CHECK(requests[2].remotePort != requests[0].remotePort);

    // A raw answer: HTTP 200 with a body that is not JSON, the request still recorded.
    fake->SetRawAnswer("/tokenize", "<html>busy</html>");
    {
        httplib::Client client(fake->Url());
        const httplib::Result raw = client.Post("/tokenize", R"({"content":"a"})", "application/json");
        REQUIRE(raw);
        CHECK(raw->status == 200);
        CHECK(raw->body == "<html>busy</html>");
    }
    CHECK(fake->RequestsTo("/tokenize").size() == 1);

    // A server started on the port of one that stopped: a restarted llama-server.
    fake.reset();
    fake.emplace(port);
    CHECK(fake->Port() == port);
    httplib::Client client(fake->Url());
    REQUIRE(client.Get("/props"));
}

TEST_CASE("FakeLlamaServer drops the connection under a path's next requests", "[fake-llama-server]")
{
    test::FakeLlamaServer fake;
    fake.DropNextAnswers("/tokenize", 2);
    httplib::Client client(fake.Url());
    client.set_keep_alive(true);
    // The headers, then the connection closes before the body: a read error, each time.
    for (int drop = 0; drop < 2; ++drop) {
        const httplib::Result dropped = client.Post("/tokenize", R"({"content":"a"})", "application/json");
        CHECK_FALSE(dropped);
        CHECK(dropped.error() == httplib::Error::Read);
    }
    // Then the path answers again, and other paths always did.
    REQUIRE(client.Get("/props"));
    const httplib::Result answered = client.Post("/tokenize", R"({"content":"a"})", "application/json");
    REQUIRE(answered);
    CHECK(answered->status == 200);
    CHECK(fake.RequestsTo("/tokenize").size() == 3);
}
