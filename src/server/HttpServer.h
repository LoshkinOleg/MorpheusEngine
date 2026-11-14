#pragma once

#include <memory>
#include <thread>

#include "server/Api.h"

namespace httplib {
class Server; // cpp-httplib, included only by HttpServer.cpp.
}

namespace morpheus::server {

// morpheus_server's HTTP side (spec, sections 1 and 4) on cpp-httplib: every request goes to Api::Handle, whatever
// its method and path. A JSON answer is written as it is; a turn's stream as Server-Sent Events, one "event: <name>"
// and one "data: <JSON>" line each, written as they come. A client that disconnects during a stream (or the server
// stopping under it) cancels its turn. Listens on 127.0.0.1 only, on a port it owns alone.
class HttpServer {
private:
    Api* _pApi = nullptr;                                 // Not owned; outlives this.
    std::unique_ptr<httplib::Server> _pServer = nullptr; // Set by the constructor; replaced by each Start.
    int _port = 0;                                        // 0 until Start succeeds.
    std::jthread _listener = {};                          // Last: joined first on destruction.

public:
    explicit HttpServer(Api& api);
    // Stop.
    ~HttpServer();
    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    // Binds port `port` of 127.0.0.1 (0 picks a free port), alone, and serves on a thread of its own. Throws
    // DataError naming the port when it cannot be bound (another server listens on it, ours or not), and
    // ContractError when already started or for a port out of range. After a DataError, Start may be called again.
    void Start(int port);

    // The port it listens on; 0 before Start.
    int Port() const;

    // Stops accepting and ends open streams (their turns are cancelled), then waits for the handlers. Callers stop the
    // runs first (RunRegistry::StopAll), so no handler waits on a turn. Safe to call twice.
    void Stop();
};

} // namespace morpheus::server
