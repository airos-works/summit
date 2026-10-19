#if SUMMIT_MODERN_WEBKIT
#include "McpService.h"
#include <OS.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <deque>
#include <thread>

// The log: Summit's stderr goes through a pipe that a thread copies to where
// it went before (a terminal, a launcher's log file) and into memory. Engine
// processes started afterwards inherit the pipe, so their traces and warnings
// are kept too.
namespace summit::mcplog {
namespace {
constexpr size_t kMaximumLines = 40000;
constexpr size_t kMaximumBytes = 8 * 1024 * 1024;
constexpr size_t kMaximumLine = 16 * 1024;

std::mutex sLock;
std::deque<Line> sLines;
size_t sBytes = 0;
uint64 sNext = 1;
std::atomic<bool> sCapturing { false };
std::atomic<bigtime_t> sSince { 0 };
// Where stderr went before the capture (a copy, closed on exec).
std::atomic<int> sOriginal { -1 };
std::string sPartial;

void Append(std::string text)
{
    if (text.size() > kMaximumLine) text.resize(kMaximumLine);
    std::lock_guard lock(sLock);
    sBytes += text.size();
    sLines.push_back({sNext++, system_time(), std::move(text)});
    while (!sLines.empty() && (sLines.size() > kMaximumLines || sBytes > kMaximumBytes)) {
        sBytes -= sLines.front().text.size();
        sLines.pop_front();
    }
}

void Copy(int input, int output)
{
    // A write to a closed original stderr must not end the browser.
    sigset_t pipe;
    sigemptyset(&pipe);
    sigaddset(&pipe, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &pipe, nullptr);
    rename_thread(find_thread(nullptr), "Summit log capture");
    char buffer[16384];
    bool teeing = output >= 0;
    while (true) {
        const ssize_t got = read(input, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) break;
        if (teeing) {
            ssize_t done = 0;
            while (done < got) {
                const ssize_t written = write(output, buffer + done, size_t(got - done));
                if (written < 0 && errno == EINTR) continue;
                if (written <= 0) { teeing = false; break; }
                done += written;
            }
        }
        sPartial.append(buffer, size_t(got));
        size_t start = 0, newline;
        while ((newline = sPartial.find('\n', start)) != std::string::npos) {
            Append(sPartial.substr(start, newline - start));
            start = newline + 1;
        }
        sPartial.erase(0, start);
        if (sPartial.size() > kMaximumLine) {
            Append(sPartial);
            sPartial.clear();
        }
    }
}
}

void Start()
{
    bool expected = false;
    if (!sCapturing.compare_exchange_strong(expected, true)) return;
    int fds[2];
    if (pipe(fds) != 0) {
        sCapturing = false;
        return;
    }
    const int original = dup(STDERR_FILENO);
    if (original >= 0) fcntl(original, F_SETFD, FD_CLOEXEC);
    fcntl(fds[0], F_SETFD, FD_CLOEXEC);
    // Line-sized writes keep the engine processes' lines whole (writes up to
    // PIPE_BUF are atomic).
    std::fflush(stderr);
    if (dup2(fds[1], STDERR_FILENO) < 0) {
        close(fds[0]);
        close(fds[1]);
        if (original >= 0) close(original);
        sCapturing = false;
        return;
    }
    close(fds[1]);
    sSince = system_time();
    sOriginal = original;
    std::thread(Copy, fds[0], original).detach();
}

void RestoreStderr()
{
    if (!sCapturing) return;
    std::fflush(stderr);
    if (const int original = sOriginal; original >= 0) dup2(original, STDERR_FILENO);
}

bool Capturing()
{
    return sCapturing;
}

bigtime_t Since()
{
    return sSince;
}

void Note(const std::string& text)
{
    Append(text);
}

std::vector<Line> Read(uint64 after, const std::string& pattern, size_t limit, uint64& last)
{
    std::vector<Line> lines;
    std::lock_guard lock(sLock);
    last = sNext - 1;
    const std::string glob = pattern.empty() ? std::string() : (pattern.find('*') == std::string::npos
        && pattern.find('?') == std::string::npos ? "*" + pattern + "*" : pattern);
    for (auto line = sLines.rbegin(); line != sLines.rend() && lines.size() < limit; ++line) {
        if (line->sequence <= after) break;
        if (glob.empty() || mcp::Matches(glob, line->text)) lines.push_back(*line);
    }
    std::reverse(lines.begin(), lines.end());
    return lines;
}
}
#endif
