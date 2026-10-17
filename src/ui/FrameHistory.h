#pragma once
#include <MessageFilter.h>
#include <OS.h>
#include <array>
#include <cstdint>
#include <vector>

namespace summit {
// When the engine's frames reached a page view: one 'wvfr' message per frame
// (copied, shared or put straight into the screen). Kept for every tab, for
// the MCP server's frame_stats tool; a few words of memory and no work beyond
// a store per frame. The view owns it (a BHandler deletes its filters); read
// it on the window's thread or with the window locked.
class FrameHistory final : public BMessageFilter {
public:
    FrameHistory() : BMessageFilter('wvfr') { }
    filter_result Filter(BMessage* message, BHandler**) override
    {
        const bigtime_t now = system_time();
        fTimes[fTotal % fTimes.size()] = now;
        bool direct = false;
        if (message->FindBool("direct", &direct) == B_OK && direct) ++fDirect;
        int64 queued = 0;
        fQueue[fTotal % fQueue.size()] = message->FindInt64("summit:queuedMicros", &queued) == B_OK && queued > 0
            && queued <= now ? now - queued : -1;
        ++fTotal;
        return B_DISPATCH_MESSAGE;
    }
    uint64_t Total() const { return fTotal; }
    uint64_t Direct() const { return fDirect; }
    // The arrival times kept (at most the last 1024 frames), oldest first.
    std::vector<bigtime_t> Times() const
    {
        std::vector<bigtime_t> times;
        const uint64_t kept = fTotal < fTimes.size() ? fTotal : fTimes.size();
        for (uint64_t i = fTotal - kept; i < fTotal; ++i) times.push_back(fTimes[i % fTimes.size()]);
        return times;
    }
    // How long each kept frame waited in the window's queue (-1: unknown).
    std::vector<bigtime_t> QueueDelays() const
    {
        std::vector<bigtime_t> delays;
        const uint64_t kept = fTotal < fQueue.size() ? fTotal : fQueue.size();
        for (uint64_t i = fTotal - kept; i < fTotal; ++i) delays.push_back(fQueue[i % fQueue.size()]);
        return delays;
    }

private:
    std::array<bigtime_t, 1024> fTimes { };
    std::array<bigtime_t, 1024> fQueue { };
    uint64_t fTotal = 0;
    uint64_t fDirect = 0;
};
}
