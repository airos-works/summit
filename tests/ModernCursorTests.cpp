// Run against the real engine and app_server. This moves the native pointer;
// use a test desktop. The final captures include the actual server cursor.
#include <WebKit/WebKitContext.h>
#include <WebKit/WebKitView.h>
#include <Application.h>
#include <Bitmap.h>
#include <Message.h>
#include <Screen.h>
#include <Window.h>
#include <WindowScreen.h>
#include <image.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

static constexpr uint32 kObservedCursor = 'ctob';
static constexpr const char* kCases[] = { "pointer", "png", "svg", "retina", "outside",
    "crosshair", "oversized", "missing", "hand", "none" };

class CursorView final : public BWebKitView {
public:
    CursorView(BRect frame, BMessenger target, std::shared_ptr<BWebKitContext> context)
        : BWebKitView(frame, "cursor-test", target, B_FOLLOW_ALL, context), m_target(target) { }
    void MessageReceived(BMessage* message) override
    {
        BWebKitView::MessageReceived(message);
        if (message->what == 'wvcu') {
            BMessage copy(*message);
            copy.what = kObservedCursor;
            m_target.SendMessage(&copy);
        }
    }
private:
    BMessenger m_target;
};

class CursorTests final : public BApplication {
public:
    CursorTests(const char* base, const char* output, const char* profile)
        : BApplication("application/x-vnd.Summit-CursorTests")
        , m_base(base), m_output(output), m_profile(profile) { }
    void ReadyToRun() override
    {
        if (BWebKitInitialize() != B_OK) { Fail("initialize engine"); return; }
        auto context = std::make_shared<BWebKitContext>(m_profile.c_str());
        if (context->InitCheck() != B_OK) { Fail("create isolated profile"); return; }
        m_window = new BWindow(BRect(100, 100, 739, 579), "Summit cursor tests",
            B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS | B_NOT_CLOSABLE);
        m_view = new CursorView(m_window->Bounds(), BMessenger(this), context);
        m_window->AddChild(m_view);
        if (m_view->InitCheck() != B_OK) { Fail("create native view"); return; }
        m_view->GetMouse(&m_originalPointer, &m_buttons, false);
        m_view->ConvertToScreen(&m_originalPointer);
        m_window->Show();
        SetPulseRate(100000);
        Next();
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == kObservedCursor) {
            m_cursor = *message;
            m_received = true;
        } else if (message->what == B_WEBKIT_STATE_CHANGED && !m_closing) {
            const char* title = message->FindString("title");
            const std::string expected = std::string("CURSOR ") + kCases[m_case] + " dpr=";
            if (title && std::string(title).starts_with(expected) && !message->GetBool("loading", true)) {
                m_ready = true;
                std::printf("%s\n", title);
            }
        } else if (message->what == B_WEBKIT_PROCESS_EXITED)
            Fail("web process exited");
        else
            BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if (m_closing) { PostMessage(B_QUIT_REQUESTED); return; }
        if (system_time() - m_started > 30000000) { Fail("cursor case timed out"); return; }
        if (!m_ready) return;
        if (!m_moved) {
            if (!m_window->Lock()) return;
            m_pointer = m_view->ConvertToScreen(BPoint(300, 220));
            m_window->Unlock();
            m_received = false;
            set_mouse_position(m_pointer.x, m_pointer.y);
            m_moved = system_time();
            return;
        }
        if (!m_received || system_time() - m_moved < 500000) return;
        if (!Check()) return;
        if (++m_case == sizeof(kCases) / sizeof(kCases[0])) {
            m_result = 0;
            std::puts("CURSOR_RESULT PASS cases=10 (transport, pixels, hot spots, alpha, fallbacks, keywords)");
            m_closing = true;
            PostMessage(B_QUIT_REQUESTED);
        } else Next();
    }
    bool QuitRequested() override
    {
        m_closing = true;
        if (m_window) {
            if (!m_window->Lock()) return false;
            auto* window = m_window; m_window = nullptr; m_view = nullptr;
            window->Quit();
            m_closedAt = system_time();
            set_mouse_position(m_originalPointer.x, m_originalPointer.y);
            PostMessage(B_QUIT_REQUESTED);
            return false;
        }
        // View/context destruction queues work on the application run loop.
        // Keep it alive through another pulse before quitting the application.
        if (m_closedAt && system_time() - m_closedAt < 1000000) return false;
        if (BWebKitHasPendingNativeUI()) return false;
        team_info team; int32 cookie = 0;
        while (get_next_team_info(&cookie, &team) == B_OK) {
            if (team.parent != getpid()) continue;
            int32 images = 0; image_info info;
            while (get_next_image_info(team.team, &images, &info) == B_OK) {
                if (info.type != B_APP_IMAGE) continue;
                std::string name(info.name);
                if (name.ends_with("/WebProcess") || name.ends_with("/NetworkProcess")) {
                    if (system_time() - m_closedAt > 30000000) {
                        std::fputs("CURSOR_RESULT FAIL helper shutdown timed out\n", stderr);
                        std::fflush(nullptr); std::_Exit(1);
                    }
                    return false;
                }
                break;
            }
        }
        return true;
    }
    int Result() const { return m_result; }
private:
    void Next()
    {
        m_ready = false; m_received = false; m_moved = 0; m_started = system_time();
        set_mouse_position(50, 50);
        m_view->LoadURL((m_base + "?case=" + kCases[m_case]).c_str());
    }
    bool Fail(const char* reason)
    {
        std::fprintf(stderr, "CURSOR_RESULT FAIL case=%s reason=%s\n", kCases[m_case], reason);
        m_closing = true; PostMessage(B_QUIT_REQUESTED); return false;
    }
    bool Check()
    {
        BRect region(m_pointer.x - 40, m_pointer.y - 40, m_pointer.x + 55, m_pointer.y + 55);
        BBitmap* raw = nullptr;
        if (BScreen().GetBitmap(&raw, true, &region) != B_OK) return Fail("capture native cursor");
        std::unique_ptr<BBitmap> bitmap(raw);
        if (!bitmap || !bitmap->IsValid() || (bitmap->ColorSpace() != B_RGB32 && bitmap->ColorSpace() != B_RGBA32))
            return Fail("invalid native capture");
        std::vector<uint8> rgb;
        for (int y = 0; y < 96; ++y) for (int x = 0; x < 96; ++x) {
            const auto* p = static_cast<const uint8*>(bitmap->Bits()) + y * bitmap->BytesPerRow() + 4 * x;
            rgb.insert(rgb.end(), { p[2], p[1], p[0] });
        }
        std::ofstream output(m_output + "/" + kCases[m_case] + ".ppm", std::ios::binary);
        output << "P6\n96 96\n255\n";
        output.write(reinterpret_cast<const char*>(rgb.data()), rgb.size());
        output.close();
        if (!output) return Fail("save native cursor capture");
        const bool custom = m_case >= 1 && m_case <= 4;
        const void* data = nullptr; ssize_t length = 0;
        if (custom) {
            // WebCore resolves an out-of-image spot to the intrinsic spot,
            // or (0, 0) for this PNG, before the native port receives it.
            const int hotspot = m_case == 4 ? 0 : 8;
            BPoint point;
            if (m_cursor.GetInt32("width", 0) != 32 || m_cursor.GetInt32("height", 0) != 32
                || m_cursor.FindPoint("hotspot", &point) != B_OK || point != BPoint(hotspot, hotspot)
                || m_cursor.FindData("pixels", B_RAW_TYPE, &data, &length) != B_OK || length != 32 * 32 * 4) {
                std::fprintf(stderr, "cursor payload: size=%ldx%ld hotspot=%.0f,%.0f bytes=%ld\n",
                    static_cast<long>(m_cursor.GetInt32("width", 0)), static_cast<long>(m_cursor.GetInt32("height", 0)),
                    point.x, point.y, static_cast<long>(length));
                return Fail("image dimensions, hot spot or pixel transport");
            }
            const auto* bytes = static_cast<const uint8*>(data);
            const auto* red = bytes + (5 * 32 + 5) * 4;
            const auto* alpha = bytes + (14 * 32 + 5) * 4;
            if (bytes[0] || bytes[1] || bytes[2] || bytes[3]
                || red[0] != 64 || red[1] != 32 || red[2] != 240 || red[3] != 255
                || std::abs(alpha[0] - 120) > 1 || std::abs(alpha[1] - 32) > 1
                || std::abs(alpha[2] - 16) > 1 || std::abs(alpha[3] - 128) > 1)
                return Fail("premultiplied BGRA pixels and transparency");
            auto pixel = [&](int x, int y, int r, int g, int b) {
                const size_t offset = ((40 - hotspot + y) * 96 + 40 - hotspot + x) * 3;
                return std::abs(rgb[offset] - r) <= 3 && std::abs(rgb[offset + 1] - g) <= 3
                    && std::abs(rgb[offset + 2] - b) <= 3;
            };
            if (!pixel(5, 5, 240, 32, 64) || !pixel(14, 5, 32, 192, 64)
                || !pixel(5, 14, 143, 159, 247) || !pixel(1, 1, 255, 255, 255)
                || !pixel(30, 30, 255, 255, 255))
                return Fail("app_server cursor pixels, alpha or hot spot");
        } else {
            if (m_cursor.FindData("pixels", B_RAW_TYPE, &data, &length) == B_OK)
                return Fail("keyword unexpectedly carried image pixels");
            if (!m_case) m_default = rgb;
            else if (m_case == 5) {
                if (rgb == m_default) return Fail("crosshair still shows the default cursor");
                m_cross = rgb;
            } else if (m_case == 6 || m_case == 7) {
                if (rgb != m_cross) return Fail("invalid image did not use CSS fallback");
            } else if (m_case == 8 && (rgb == m_default || rgb == m_cross))
                return Fail("hand cursor did not change");
            else if (m_case == 9) {
                // app_server's integer cursor blend can round white down by
                // one even for a fully transparent native cursor.
                for (auto value : rgb) if (value < 254) return Fail("cursor:none remained visible");
            }
        }
        std::printf("PASS %s native cursor\n", kCases[m_case]); std::fflush(stdout);
        return true;
    }
    std::string m_base, m_output, m_profile;
    BWindow* m_window { nullptr };
    CursorView* m_view { nullptr };
    BMessage m_cursor;
    BPoint m_pointer, m_originalPointer;
    uint32 m_buttons { 0 };
    size_t m_case { 0 };
    bool m_ready { false }, m_received { false }, m_closing { false };
    bigtime_t m_started { 0 }, m_moved { 0 }, m_closedAt { 0 };
    std::vector<uint8> m_default, m_cross;
    int m_result { 1 };
};

int main(int argc, char** argv)
{
    if (argc != 4) {
        std::fprintf(stderr, "Usage: %s FIXTURE_URL EXISTING_OUTPUT_DIRECTORY NEW_PROFILE_DIRECTORY\n", argv[0]);
        return 2;
    }
    int result = 1;
    {
        CursorTests application(argv[1], argv[2], argv[3]);
        if (application.InitCheck() != B_OK) return 1;
        application.Run();
        result = application.Result();
    }
    // Match Summit's exit path: worker TLS teardown must not race libbe's
    // global handler-token table destructor after the application is gone.
    std::fflush(nullptr);
    std::_Exit(result);
}
