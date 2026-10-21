// Compare the document's stripes with the rAF-driven stripes in scroll-snap.html.
// Run on the test desktop while real wheel input scrolls the fixture. Coordinates
// are logical screen pixels; the capture checks what app_server actually paints.
#include <Application.h>
#include <Bitmap.h>
#include <Screen.h>
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <map>
#include <vector>

int main(int argc, char** argv)
{
    if (argc != 7) {
        std::fprintf(stderr, "Usage: %s ACTUAL_X MIRROR_X TOP HEIGHT FRAMES INTERVAL_US\n", argv[0]);
        return 2;
    }
    int values[6];
    for (int i = 0; i < 6; ++i) {
        char* end = nullptr; errno = 0;
        long value = std::strtol(argv[i + 1], &end, 10);
        if (errno || !*argv[i + 1] || *end || value < 0 || value > INT_MAX) return 2;
        values[i] = static_cast<int>(value);
    }
    BApplication app("application/x-vnd.Summit-ScrollScreenProbe");
    const int x1 = values[0], x2 = values[1], y = values[2], height = values[3], count = values[4], interval = values[5];
    BScreen screen;
    if (x2 <= x1 || height < 140 || height > 16384 || !count || count > 100000 || interval < 1000
        || y > 16384 || !screen.Frame().Contains(BRect(x1, y, x2, y + height - 1))) return 2;
    BRect area(x1, y, x2, y + height - 1);
    BBitmap bitmap(BRect(0, 0, x2 - x1, height - 1), B_RGBA32);
    if (bitmap.InitCheck() != B_OK) return 3;
    auto stripe = [&](int x, int row) {
        const auto* p = static_cast<const uint8*>(bitmap.Bits()) + row * bitmap.BytesPerRow() + x * 4;
        return std::abs(p[2] - 221) <= 8 && std::abs(p[1] - 102) <= 8 && std::abs(p[0] - 136) <= 8;
    };
    const auto start = system_time();
    int failures = 0, maximumOffset = 0;
    std::puts("time_us,offset,reference_pixels,mirror_pixels,mismatches");
    for (int i = 0; i < count; ++i) {
        snooze_until(start + bigtime_t(i) * interval, B_SYSTEM_TIMEBASE);
        if (screen.ReadBitmap(&bitmap, false, &area) != B_OK) return 4;
        const auto when = system_time();
        int best = height * 2, bestOffset = 0, a = 0, b = 0;
        for (int row = 20; row < height - 20; ++row) { a += stripe(0, row); b += stripe(x2 - x1, row); }
        for (int offset = -16; offset <= 16; ++offset) {
            int errors = 0;
            for (int row = 20; row < height - 20; ++row) errors += stripe(0, row) != stripe(x2 - x1, row + offset);
            if (errors < best || (errors == best && std::abs(offset) < std::abs(bestOffset))) { best = errors; bestOffset = offset; }
        }
        // Allow one pixel of raster rounding between a transformed layer and
        // the scroll position. Missing stripes must not count as alignment.
        failures += !a || !b || best > 2 || std::abs(bestOffset) > 1;
        if (!a || !b) {
            for (int column : { 0, x2 - x1 }) {
                std::map<uint32, int> colors;
                for (int row = 20; row < height - 20; ++row) {
                    const auto* p = static_cast<const uint8*>(bitmap.Bits()) + row * bitmap.BytesPerRow() + column * 4;
                    ++colors[(uint32(p[2]) << 16) | (uint32(p[1]) << 8) | p[0]];
                }
                std::vector<std::pair<int, uint32>> counts;
                for (auto [color, count] : colors) counts.emplace_back(count, color);
                std::sort(counts.rbegin(), counts.rend());
                std::fprintf(stderr, "Missing stripe at sample %d column %d:", i, column);
                for (size_t j = 0; j < std::min<size_t>(8, counts.size()); ++j)
                    std::fprintf(stderr, " #%06lx:%d", static_cast<unsigned long>(counts[j].second), counts[j].first);
                std::fputc('\n', stderr);
            }
        }
        maximumOffset = std::max(maximumOffset, std::abs(bestOffset));
        std::printf("%lld,%d,%d,%d,%d\n", static_cast<long long>(when), bestOffset, a, b, best);
        std::fflush(stdout);
    }
    std::fprintf(stderr, "SCROLL_POSITION_RESULT %s samples=%d failures=%d max_offset=%d\n",
        failures ? "FAIL" : "PASS", count, failures, maximumOffset);
    return failures ? 1 : 0;
}
