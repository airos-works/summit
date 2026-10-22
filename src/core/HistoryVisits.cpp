#include "HistoryVisits.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <set>

namespace summit {
int64_t HistoryNowMicroseconds()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
int64_t HistoryMicroseconds(int64_t seconds)
{
    return seconds > 0 && seconds <= INT64_MAX / 1000000 ? seconds * 1000000 : 0;
}
class LocalDays {
public:
    int64_t operator()(int64_t micros)
    {
        std::time_t seconds = micros / 1000000;
        // Several microsecond timestamps often share a second. Haiku's
        // localtime_r refreshes the locale backend on every call.
        if (seconds == fSeconds) return fDay;
        std::tm local {};
        fSeconds = seconds;
        fDay = localtime_r(&seconds, &local) ? (int64_t(local.tm_year) + 1900) * 1000 + local.tm_yday : seconds / 86400;
        return fDay;
    }
private:
    int64_t fSeconds = INT64_MIN, fDay = 0;
};
static void Normalize(std::vector<int64_t>& times)
{
    times.erase(std::remove_if(times.begin(), times.end(), [](int64_t t) { return t <= 0; }), times.end());
    std::sort(times.begin(), times.end(), std::greater<int64_t>());
    times.erase(std::unique(times.begin(), times.end()), times.end());
    if (times.size() <= kRecentHistoryVisits) return;
    LocalDays localDay;
    std::set<int64_t> days {localDay(times.front())};
    size_t kept = 0;
    for (size_t i = 0; i < times.size(); ++i) {
        const auto day = localDay(times[i]);
        const bool daily = days.size() <= kMaxEarlierVisits && days.insert(day).second;
        if (i < kRecentHistoryVisits || daily) times[kept++] = times[i];
    }
    times.resize(kept);
}
std::vector<int64_t> HistoryVisitTimes(const PageRecord& page)
{
    auto times = page.visitTimes;
    // Migrate the summaries when needed, but never add a rounded phantom visit
    // beside an exact timestamp from that second (notably SQLite imports).
    auto addLegacy = [&](int64_t seconds) {
        const auto micros = HistoryMicroseconds(seconds);
        if (micros && std::none_of(times.begin(), times.end(), [&](int64_t t) { return t / 1000000 == seconds; }))
            times.push_back(micros);
    };
    addLegacy(page.visited);
    for (auto seconds : page.earlier) addLegacy(seconds);
    Normalize(times);
    return times;
}
void SetHistoryVisitTimes(PageRecord& page, std::vector<int64_t> times)
{
    Normalize(times);
    page.visitTimes = std::move(times);
    page.earlier.clear();
    if (page.visitTimes.empty()) { page.visited = 0; return; }
    page.visited = page.visitTimes.front() / 1000000;
    LocalDays localDay;
    std::set<int64_t> days {localDay(page.visitTimes.front())};
    for (auto time : page.visitTimes) {
        if (page.earlier.size() >= kMaxEarlierVisits) break;
        if (days.insert(localDay(time)).second) page.earlier.push_back(time / 1000000);
    }
}
}
