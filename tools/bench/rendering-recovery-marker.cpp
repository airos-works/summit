// Timestamp the first visible post-loss marker from rendering-recovery.html.
// Run before loss to establish a zero count, then poll after loss. Compare
// capturedAt with the compositor hook's monotonic timestamp, before PNG
// encoding or transfer can inflate the measurement. This reads app_server's
// framebuffer copy; it never changes the screen or triggers a GPU reset.
// Native build: g++ -O2 rendering-recovery-marker.cpp -lbe -o recovery-marker
#include <Application.h>
#include <Bitmap.h>
#include <Screen.h>
#include <OS.h>
#include <cstdio>
#include <memory>

int main()
{
    BApplication app("application/x-vnd.Summit-RecoveryMarker");
    BScreen screen;
    BBitmap* capturedBitmap = nullptr;
    auto result = screen.GetBitmap(&capturedBitmap, false);
    auto capturedAt = system_time();
    std::unique_ptr<BBitmap> bitmap(capturedBitmap);
    if (result != B_OK || !bitmap)
        return 2;
    if (bitmap->ColorSpace() != B_RGB32 && bitmap->ColorSpace() != B_RGBA32)
        return 3;
    unsigned count = 0;
    for (int y = 0; y <= bitmap->Bounds().IntegerHeight(); ++y) {
        const auto* row = static_cast<const unsigned char*>(bitmap->Bits()) + y * bitmap->BytesPerRow();
        for (int x = 0; x <= bitmap->Bounds().IntegerWidth(); ++x)
            count += row[x * 4] == 214 && row[x * 4 + 1] == 164 && row[x * 4 + 2] == 18;
    }
    std::printf("{\"capturedAt\":%.6f,\"markerPixels\":%u}\n", capturedAt / 1e6, count);
}
