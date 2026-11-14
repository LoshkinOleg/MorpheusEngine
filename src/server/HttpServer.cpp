#include "server/HttpServer.h"

#include <chrono>
#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN // cpp-httplib includes windows.h: leave out what Morpheus never uses.
#endif
#endif
#include <httplib.h>

#include "core/Errors.h"

namespace morpheus::server {

namespace {

constexpr const char* HOST = "127.0.0.1";
constexpr int MAX_PORT = 65535;
// How long a stream waits for its turn's next event before it lets cpp-httplib check the client again.
constexpr std::chrono::milliseconds STREAM_POLL = std::chrono::milliseconds(50);

} // namespace

HttpServer::HttpServer(Api& api)
    : _pApi(&api)
    , _pServer(std::make_unique<httplib::Server>())
{
}

HttpServer::~HttpServer()
{
    Stop();
}

void HttpServer::Start(int port)
{
    MORPHEUS_REQUIRE(_port == 0, "HttpServer::Start: the server is already started");
    MORPHEUS_REQUIRE(port >= 0 && port <= MAX_PORT, std::format("HttpServer::Start: port {} is out of range", port));

    // A fresh cpp-httplib server for each attempt: one whose bind failed refuses every later bind, and Start may be
    // called again after a DataError. Nothing listens yet, so the old one goes without a thread to join.
    _pServer = std::make_unique<httplib::Server>();

    // Every request goes to the Api, whatever its method: it answers 404 for what it does not know.
    auto handle = [this](const httplib::Request& request, httplib::Response& response) {
        ApiResponse answer = _pApi->Handle(request.method, request.path, request.body);
        response.status = answer.status;
        if (!answer.stream) {
            response.set_content(answer.body.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace),
                                 "application/json");
            return;
        }
        // A turn: its events as Server-Sent Events, as they come. Between events the provider returns without
        // writing, so cpp-httplib checks the client is still there before calling it again. When the client is gone
        // or the server stops, the stream ends unsuccessfully and the releaser cancels the turn.
        std::shared_ptr<TurnStream> stream = std::move(answer.stream);
        response.set_header("Cache-Control", "no-cache");
        response.set_chunked_content_provider(
            "text/event-stream",
            [stream](std::size_t, httplib::DataSink& sink) {
                if (std::optional<StreamEvent> event = stream->Next(STREAM_POLL)) {
                    const std::string text =
                        "event: " + event->name + "\ndata: " +
                        event->data.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace) + "\n\n";
                    return sink.write(text.data(), text.size());
                }
                if (stream->Ended()) {
                    sink.done();
                }
                return true;
            },
            [stream](bool success) {
                if (!success) {
                    stream->Cancel();
                }
            });
    };
    _pServer->Get(".*", handle);
    _pServer->Post(".*", handle);
    _pServer->Put(".*", handle);
    _pServer->Patch(".*", handle);
    _pServer->Delete(".*", handle);

    // Each event goes out as soon as it is written: without TCP_NODELAY, Nagle's algorithm holds a small write back
    // until the previous one is acknowledged, and a delayed acknowledgement then stalls the stream for up to 200 ms.
    _pServer->set_tcp_nodelay(true);

    // The listening socket owns its port alone. cpp-httplib's default (SO_REUSEADDR on Windows, SO_REUSEPORT
    // elsewhere) lets a second server bind a port another one still listens on, and connections then split between
    // them; that must fail instead.
    _pServer->set_socket_options([](socket_t sock) {
#ifdef _WIN32
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
        httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    });
    int bound = -1;
    if (port == 0) {
        bound = _pServer->bind_to_any_port(HOST);
    } else if (_pServer->bind_to_port(HOST, port)) {
        bound = port;
    }
    if (bound <= 0) {
        throw core::DataError(
            std::format("port {} of {} cannot be bound: another server may be listening on it", port, HOST));
    }
    _port = bound;
    _listener = std::jthread([this] { _pServer->listen_after_bind(); });
    _pServer->wait_until_ready();
}

int HttpServer::Port() const
{
    return _port;
}

void HttpServer::Stop()
{
    _pServer->stop();
    if (_listener.joinable()) {
        _listener.join();
    }
}

} // namespace morpheus::server
