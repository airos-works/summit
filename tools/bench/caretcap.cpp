// caretcap X Y W H FRAMES INTERVAL_MS: reads a screen region FRAMES times,
// INTERVAL_MS apart, and prints per frame the number of red pixels (the
// caret-color the caret test pages set, so a blinking caret shows as counts
// alternating between 0 and ~20 every 500 ms) and of dark pixels. Reads through
// BScreen::ReadBitmap, so it sees what app_server composed, like a screenshot.
// Cross-build for the X399 or the laptop with the x86_64 cross tools, or in the
// VM with g++ -o caretcap caretcap.cpp -lbe. Used for issue #53 (docs/issue-fixes-2026-10-15.md).
#include <Application.h>
#include <Bitmap.h>
#include <Screen.h>
#include <OS.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
int main(int argc, char** argv)
{
    if (argc < 7) { std::fprintf(stderr, "usage: caretcap X Y W H FRAMES INTERVAL_MS\n"); return 2; }
    BApplication app("application/x-vnd.claude-caretcap");
    BScreen screen;
    BRect area(atof(argv[1]), atof(argv[2]), atof(argv[1]) + atof(argv[3]) - 1, atof(argv[2]) + atof(argv[4]) - 1);
    int frames = atoi(argv[5]);
    bigtime_t interval = (bigtime_t)atoi(argv[6]) * 1000;
    BBitmap bitmap(BRect(0, 0, area.Width(), area.Height()), B_RGB32);
    bigtime_t first = system_time();
    for (int f = 0; f < frames; ++f) {
        bigtime_t start = system_time();
        status_t status = screen.ReadBitmap(&bitmap, false, &area);
        const uint8* bits = (const uint8*)bitmap.Bits();
        int red = 0, dark = 0;
        int w = bitmap.Bounds().IntegerWidth() + 1, h = bitmap.Bounds().IntegerHeight() + 1;
        for (int32 y = 0; y < h; ++y) {
            const uint8* row = bits + y * bitmap.BytesPerRow();
            for (int32 x = 0; x < w; ++x) {
                int b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2];
                if (r > 180 && g < 90 && b < 90) ++red;
                if (r < 90 && g < 90 && b < 90) ++dark;
            }
        }
        std::printf("frame %d at %lld ms: red %d dark %d (%s)\n", f, (long long)((start - first) / 1000), red, dark, strerror(status));
        std::fflush(stdout);
        bigtime_t next = start + interval;
        if (system_time() < next) snooze(next - system_time());
    }
    return 0;
}
