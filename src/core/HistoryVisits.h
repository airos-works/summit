#pragma once
#include "PageRecord.h"

namespace summit {
// Keep recent individual visits as well as the last visit of each earlier day.
// The daily samples preserve the existing History retention policy.
constexpr size_t kRecentHistoryVisits = 128;
constexpr size_t kMaxHistoryVisitTimes = kRecentHistoryVisits + kMaxEarlierVisits;
int64_t HistoryNowMicroseconds();
// Zero means an unknown/unrepresentable legacy timestamp.
int64_t HistoryMicroseconds(int64_t seconds);
std::vector<int64_t> HistoryVisitTimes(const PageRecord&);
void SetHistoryVisitTimes(PageRecord&, std::vector<int64_t>);
}
