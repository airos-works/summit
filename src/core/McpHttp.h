#pragma once
#include "Mcp.h"
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <string>

// The streamable HTTP transport of the MCP server: POST /mcp carries one
// JSON-RPC message (or batch) and is answered with application/json. Every
// request needs "Authorization: Bearer <token>"; GET /health does not.
// Plain POSIX sockets and threads, so the host tests run it too.
namespace summit::mcp {

struct HttpOptions {
    // "0.0.0.0" accepts connections from other computers, "127.0.0.1" only
    // from this one.
    std::string address = "0.0.0.0";
    uint16_t port = 7790;
    // Required; an empty token refuses every request.
    std::string token;
};

struct HttpRequest {
    std::string method, path, version, body;
    // Lower-case names.
    std::map<std::string, std::string> headers;
    bool keepAlive = true;
};

// Parses one request from the front of buffer (which then holds what came
// after it). Returns 1 when a request was taken, 0 when more bytes are needed,
// -1 when the bytes are not HTTP or are too large.
int ParseHttpRequest(std::string& buffer, HttpRequest& request);

// The complete response to a request, as the transport sends it; also used
// by the tests without a socket.
std::string RespondHttp(Server& server, const HttpOptions& options, const HttpRequest& request, bool& keepAlive);

// Whether the request carries the token (constant time).
bool Authorized(const HttpRequest& request, const std::string& token);

class HttpServer {
public:
    HttpServer(std::shared_ptr<Server> server, HttpOptions options);
    ~HttpServer();
    // Binds and starts listening; an error text when that failed.
    std::string Start();
    // Stops accepting and closes idle connections; requests being answered
    // finish on their own threads. Returns at once.
    void Stop();
    bool IsRunning() const;
    // Requests answered so far, and the peer address of the last one.
    uint64_t RequestCount() const;
    std::string LastPeer() const;
    const HttpOptions& Options() const { return fOptions; }
    // The port actually bound (the options' port, or the system's choice for 0).
    uint16_t BoundPort() const { return fBoundPort; }

    struct State;

private:
    std::shared_ptr<Server> fServer;
    HttpOptions fOptions;
    std::shared_ptr<State> fState;
    uint16_t fBoundPort = 0;
};
}
