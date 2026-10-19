#include "McpHttp.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

namespace summit::mcp {
namespace {
constexpr size_t kMaximumHeader = 64 * 1024;
constexpr size_t kMaximumBody = 32 * 1024 * 1024;
// A connection with no request for this long is closed.
constexpr int kIdleMilliseconds = 120 * 1000;

std::string Lower(std::string text)
{
    for (char& character : text) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return text;
}

std::string Trim(const std::string& text)
{
    const size_t start = text.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return { };
    return text.substr(start, text.find_last_not_of(" \t\r\n") - start + 1);
}

const char* StatusText(int status)
{
    switch (status) {
        case 200: return "OK";
        case 202: return "Accepted";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        default: return "Error";
    }
}

std::string Response(int status, const std::string& type, const std::string& body, bool keepAlive,
    const std::string& extra = { })
{
    std::string response = "HTTP/1.1 " + std::to_string(status) + " " + StatusText(status) + "\r\n";
    if (!type.empty()) response += "Content-Type: " + type + "\r\n";
    response += "Content-Length: " + std::to_string(body.size()) + "\r\n";
    response += "Cache-Control: no-store\r\n";
    response += extra;
    response += keepAlive ? "Connection: keep-alive\r\n" : "Connection: close\r\n";
    response += "\r\n";
    response += body;
    return response;
}

bool SendAll(int fd, const std::string& data)
{
    size_t done = 0;
    while (done < data.size()) {
        const ssize_t written = send(fd, data.data() + done, data.size() - done, 0);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return false;
        done += static_cast<size_t>(written);
    }
    return true;
}
}

int ParseHttpRequest(std::string& buffer, HttpRequest& request)
{
    const size_t headerEnd = buffer.find("\r\n\r\n");
    if (headerEnd == std::string::npos) return buffer.size() > kMaximumHeader ? -1 : 0;
    if (headerEnd > kMaximumHeader) return -1;
    HttpRequest parsed;
    size_t lineStart = 0;
    bool first = true;
    while (lineStart < headerEnd) {
        size_t lineEnd = buffer.find("\r\n", lineStart);
        if (lineEnd == std::string::npos || lineEnd > headerEnd) lineEnd = headerEnd;
        const std::string line = buffer.substr(lineStart, lineEnd - lineStart);
        lineStart = lineEnd + 2;
        if (first) {
            first = false;
            const size_t space1 = line.find(' '), space2 = line.rfind(' ');
            if (space1 == std::string::npos || space2 == space1) return -1;
            parsed.method = line.substr(0, space1);
            parsed.path = line.substr(space1 + 1, space2 - space1 - 1);
            parsed.version = line.substr(space2 + 1);
            if (parsed.version.rfind("HTTP/1.", 0) != 0) return -1;
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        parsed.headers[Lower(Trim(line.substr(0, colon)))] = Trim(line.substr(colon + 1));
    }
    if (const size_t query = parsed.path.find('?'); query != std::string::npos) parsed.path.erase(query);
    parsed.keepAlive = parsed.version == "HTTP/1.1";
    if (auto connection = parsed.headers.find("connection"); connection != parsed.headers.end()) {
        const std::string value = Lower(connection->second);
        if (value == "close") parsed.keepAlive = false;
        else if (value == "keep-alive") parsed.keepAlive = true;
    }
    size_t offset = headerEnd + 4;
    if (auto encoding = parsed.headers.find("transfer-encoding");
        encoding != parsed.headers.end() && Lower(encoding->second).find("chunked") != std::string::npos) {
        std::string body;
        while (true) {
            const size_t sizeEnd = buffer.find("\r\n", offset);
            if (sizeEnd == std::string::npos) return 0;
            const unsigned long chunk = std::strtoul(buffer.substr(offset, sizeEnd - offset).c_str(), nullptr, 16);
            if (body.size() + chunk > kMaximumBody) return -1;
            offset = sizeEnd + 2;
            if (chunk == 0) {
                // Trailers end with an empty line.
                const size_t trailerEnd = buffer.find("\r\n", offset);
                if (trailerEnd == std::string::npos) return 0;
                offset = trailerEnd + 2;
                break;
            }
            if (buffer.size() < offset + chunk + 2) return 0;
            body.append(buffer, offset, chunk);
            offset += chunk + 2;
        }
        parsed.body = std::move(body);
    } else {
        size_t length = 0;
        if (auto field = parsed.headers.find("content-length"); field != parsed.headers.end()) {
            char* end = nullptr;
            length = std::strtoul(field->second.c_str(), &end, 10);
            if (end == field->second.c_str() || length > kMaximumBody) return -1;
        }
        if (buffer.size() < offset + length) return 0;
        parsed.body = buffer.substr(offset, length);
        offset += length;
    }
    buffer.erase(0, offset);
    request = std::move(parsed);
    return 1;
}

bool Authorized(const HttpRequest& request, const std::string& token)
{
    if (token.empty()) return false;
    std::string offered;
    if (auto field = request.headers.find("authorization"); field != request.headers.end()) {
        const std::string& value = field->second;
        if (value.size() > 7 && Lower(value.substr(0, 7)) == "bearer ") offered = Trim(value.substr(7));
    }
    if (offered.size() != token.size()) return false;
    unsigned char difference = 0;
    for (size_t i = 0; i < token.size(); ++i) difference |= static_cast<unsigned char>(offered[i] ^ token[i]);
    return difference == 0;
}

std::string RespondHttp(Server& server, const HttpOptions& options, const HttpRequest& request, bool& keepAlive)
{
    keepAlive = request.keepAlive;
    if (request.path == "/health" && request.method == "GET") {
        const Json health = {{"ok", true}, {"server", "Summit"}, {"tools", server.ToolCount()}};
        return Response(200, "application/json", health.dump() + "\n", keepAlive);
    }
    if (request.path != "/mcp" && request.path != "/") return Response(404, "text/plain", "Summit: POST JSON-RPC to /mcp\n", keepAlive);
    // A web page cannot reach the server: browsers send an Origin header that
    // the server refuses (DNS rebinding), and the token is required anyway.
    if (auto origin = request.headers.find("origin"); origin != request.headers.end() && !origin->second.empty()
        && origin->second != "null")
        return Response(403, "text/plain", "Summit: requests from web pages are refused\n", keepAlive);
    if (!Authorized(request, options.token))
        return Response(401, "application/json", "{\"error\":\"bearer token required\"}\n", keepAlive,
            "WWW-Authenticate: Bearer\r\n");
    if (request.method == "GET")
        return Response(405, "text/plain", "Summit: no event stream; POST requests\n", keepAlive, "Allow: POST, DELETE\r\n");
    if (request.method == "DELETE") return Response(200, { }, { }, keepAlive);
    if (request.method != "POST") return Response(405, "text/plain", "Summit: use POST\n", keepAlive, "Allow: POST\r\n");
    const std::string answer = server.Handle(request.body);
    const std::string session = "Mcp-Session-Id: summit\r\n";
    if (answer.empty()) return Response(202, { }, { }, keepAlive, session);
    return Response(200, "application/json", answer, keepAlive, session);
}

struct HttpServer::State {
    std::atomic<bool> running { false };
    std::atomic<int> listener { -1 };
    std::atomic<uint64_t> requests { 0 };
    mutable std::mutex lock;
    std::string lastPeer;
};

HttpServer::HttpServer(std::shared_ptr<Server> server, HttpOptions options)
    : fServer(std::move(server)), fOptions(std::move(options))
{
}

HttpServer::~HttpServer()
{
    Stop();
}

std::string HttpServer::Start()
{
    if (IsRunning()) return { };
    if (fOptions.token.empty()) return "no access token";
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return std::strerror(errno);
    // Processes Summit starts (the engine's, a restarted Summit) must not
    // keep the port.
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address {};
    address.sin_family = AF_INET;
    address.sin_port = htons(fOptions.port);
    if (inet_pton(AF_INET, fOptions.address.c_str(), &address.sin_addr) != 1) {
        close(fd);
        return "not an IPv4 address: " + fOptions.address;
    }
    // A server just stopped closes its socket within a quarter of a second.
    int bound = -1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        bound = bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
        if (bound == 0 || errno != EADDRINUSE) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (bound < 0 || listen(fd, 16) < 0) {
        const std::string error = errno == EADDRINUSE ? "port " + std::to_string(fOptions.port) + " is in use"
            : std::strerror(errno);
        close(fd);
        return error;
    }
    socklen_t length = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) == 0) fBoundPort = ntohs(address.sin_port);
    auto state = std::make_shared<State>();
    state->listener = fd;
    state->running = true;
    fState = state;
    std::thread([state, server = fServer, options = fOptions] {
        while (state->running) {
            pollfd wait { state->listener, POLLIN, 0 };
            const int ready = poll(&wait, 1, 250);
            if (!state->running) break;
            if (ready <= 0) continue;
            sockaddr_in peer {};
            socklen_t peerLength = sizeof(peer);
            const int connection = accept(state->listener, reinterpret_cast<sockaddr*>(&peer), &peerLength);
            if (connection < 0) continue;
            fcntl(connection, F_SETFD, FD_CLOEXEC);
            int on = 1;
            setsockopt(connection, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
            char name[INET_ADDRSTRLEN] = "?";
            inet_ntop(AF_INET, &peer.sin_addr, name, sizeof(name));
            const std::string peerName = std::string(name) + ":" + std::to_string(ntohs(peer.sin_port));
            std::thread([state, server, options, connection, peerName] {
                std::string buffer;
                bool keepAlive = true;
                while (keepAlive && state->running) {
                    HttpRequest request;
                    int parsed = ParseHttpRequest(buffer, request);
                    while (parsed == 0 && state->running) {
                        pollfd wait { connection, POLLIN, 0 };
                        // Short polls notice Stop(); the idle limit is counted in them.
                        int waited = 0, ready = 0;
                        while (state->running && (ready = poll(&wait, 1, 250)) == 0 && (waited += 250) < kIdleMilliseconds) { }
                        if (ready <= 0) { parsed = -1; break; }
                        char chunk[65536];
                        const ssize_t got = recv(connection, chunk, sizeof(chunk), 0);
                        if (got <= 0) { parsed = -1; break; }
                        buffer.append(chunk, static_cast<size_t>(got));
                        parsed = ParseHttpRequest(buffer, request);
                    }
                    if (parsed != 1) {
                        if (parsed < 0 && !buffer.empty())
                            SendAll(connection, Response(400, "text/plain", "Summit: bad request\n", false));
                        break;
                    }
                    {
                        std::lock_guard lock(state->lock);
                        state->lastPeer = peerName;
                    }
                    ++state->requests;
                    if (!SendAll(connection, RespondHttp(*server, options, request, keepAlive))) break;
                }
                close(connection);
            }).detach();
        }
        const int listener = state->listener.exchange(-1);
        if (listener >= 0) close(listener);
    }).detach();
    return { };
}

void HttpServer::Stop()
{
    if (!fState) return;
    fState->running = false;
    fState.reset();
}

bool HttpServer::IsRunning() const
{
    return fState && fState->running;
}

uint64_t HttpServer::RequestCount() const
{
    return fState ? fState->requests.load() : 0;
}

std::string HttpServer::LastPeer() const
{
    if (!fState) return { };
    std::lock_guard lock(fState->lock);
    return fState->lastPeer;
}
}
