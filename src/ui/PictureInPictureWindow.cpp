#include "PictureInPictureWindow.h"
#include "FeatureMessages.h"
#include <WebKit/WebKitView.h>
#include <Bitmap.h>
#include <Screen.h>
#include <View.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>

namespace summit {
// The picture, and while the pointer is over it, three buttons: back to the
// tab, play or pause, and close.
class PictureView : public BView {
public:
    explicit PictureView(BRect frame)
        : BView(frame, "picture", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE)
    {
        SetViewColor(B_TRANSPARENT_COLOR);
    }
    void SetPicture(const void* pixels, int32 width, int32 height, bool paused)
    {
        if (!fBitmap || fBitmap->Bounds().IntegerWidth() + 1 != width || fBitmap->Bounds().IntegerHeight() + 1 != height)
            fBitmap = std::make_unique<BBitmap>(BRect(0, 0, width - 1, height - 1), B_RGB32);
        if (fBitmap->InitCheck() != B_OK) return;
        const int32 rowBytes = width * 4;
        auto* bits = static_cast<uint8*>(fBitmap->Bits());
        for (int32 y = 0; y < height; ++y)
            std::memcpy(bits + y * fBitmap->BytesPerRow(), static_cast<const uint8*>(pixels) + y * rowBytes, rowBytes);
        fPaused = paused;
        Invalidate();
    }
    void Draw(BRect) override
    {
        const BRect bounds = Bounds();
        SetHighColor(0, 0, 0);
        if (!fBitmap) { FillRect(bounds); DrawControls(); return; }
        const BRect source = fBitmap->Bounds();
        const float fit = std::min((bounds.Width() + 1) / (source.Width() + 1), (bounds.Height() + 1) / (source.Height() + 1));
        const float width = (source.Width() + 1) * fit, height = (source.Height() + 1) * fit;
        BRect target(0, 0, width - 1, height - 1);
        target.OffsetTo(std::floor((bounds.Width() + 1 - width) / 2), std::floor((bounds.Height() + 1 - height) / 2));
        // Black bars around the picture, drawn without flicker.
        FillRect(BRect(bounds.left, bounds.top, bounds.right, target.top - 1));
        FillRect(BRect(bounds.left, target.bottom + 1, bounds.right, bounds.bottom));
        FillRect(BRect(bounds.left, target.top, target.left - 1, target.bottom));
        FillRect(BRect(target.right + 1, target.top, bounds.right, target.bottom));
        DrawBitmap(fBitmap.get(), source, target, B_FILTER_BITMAP_BILINEAR);
        DrawControls();
    }
    void MouseMoved(BPoint, uint32 transit, const BMessage*) override
    {
        const bool inside = transit != B_EXITED_VIEW && transit != B_OUTSIDE_VIEW;
        if (inside != fHover) { fHover = inside; Invalidate(); }
    }
    void MouseDown(BPoint where) override
    {
        auto* window = static_cast<PictureInPictureWindow*>(Window());
        if (CloseRect().Contains(where)) { window->PostMessage(B_QUIT_REQUESTED); return; }
        if (BackRect().Contains(where)) { window->Command("back"); return; }
        if (PlayRect().Contains(where) || !fHover) { window->Command("toggle"); return; }
        // Elsewhere the picture moves the window, as a title bar would.
        BPoint start = ConvertToScreen(where);
        const BPoint origin = window->Frame().LeftTop();
        uint32 buttons;
        BPoint now;
        do {
            snooze(10000);
            GetMouse(&now, &buttons, false);
            const BPoint screen = ConvertToScreen(now);
            window->MoveTo(origin + (screen - start));
        } while (buttons);
    }
private:
    BRect CloseRect() const { return BRect(Bounds().right - 30, 6, Bounds().right - 6, 30); }
    BRect BackRect() const { return BRect(6, 6, 30, 30); }
    BRect PlayRect() const
    {
        const BPoint c((Bounds().left + Bounds().right) / 2, (Bounds().top + Bounds().bottom) / 2);
        return BRect(c.x - 22, c.y - 22, c.x + 22, c.y + 22);
    }
    void DrawControls()
    {
        if (!fHover) return;
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        SetHighColor(0, 0, 0, 140);
        FillEllipse(CloseRect());
        FillEllipse(BackRect());
        FillEllipse(PlayRect());
        SetHighColor(255, 255, 255, 230);
        SetPenSize(2);
        const BRect close = CloseRect().InsetByCopy(8, 8);
        StrokeLine(close.LeftTop(), close.RightBottom());
        StrokeLine(close.RightTop(), close.LeftBottom());
        // Back to the tab: an arrow out of a box.
        const BRect back = BackRect().InsetByCopy(7, 7);
        StrokeRect(BRect(back.left, back.top + 3, back.right - 3, back.bottom));
        StrokeLine(BPoint(back.left + 4, back.bottom - 4), back.RightTop());
        const BRect play = PlayRect().InsetByCopy(14, 13);
        if (fPaused) {
            BPoint triangle[] = { play.LeftTop() + BPoint(3, 0), play.LeftBottom() + BPoint(3, 0),
                BPoint(play.right + 2, (play.top + play.bottom) / 2) };
            FillPolygon(triangle, 3);
        } else {
            FillRect(BRect(play.left + 2, play.top, play.left + 6, play.bottom));
            FillRect(BRect(play.right - 6, play.top, play.right - 2, play.bottom));
        }
        SetPenSize(1);
        SetDrawingMode(B_OP_COPY);
    }
    std::unique_ptr<BBitmap> fBitmap;
    bool fPaused = false, fHover = false;
};

PictureInPictureWindow::PictureInPictureWindow(BMessenger browser, int64 tab, uint64 session, const std::string& title, BRect near)
    : BWindow(BRect(0, 0, 479, 269), title.empty() ? "Picture in Picture" : title.c_str(), B_FLOATING_WINDOW_LOOK,
        B_FLOATING_ALL_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_ASYNCHRONOUS_CONTROLS | B_WILL_ACCEPT_FIRST_CLICK,
        B_ALL_WORKSPACES),
      fBrowser(browser), fTab(tab), fSession(session)
{
    fView = new PictureView(Bounds());
    AddChild(fView);
    SetSizeLimits(160, 1920, 90, 1080);
    // The bottom right corner of the browser window, on the screen.
    const BRect screen = BScreen(this).Frame();
    const float left = std::clamp(near.right - Frame().Width() - 30, screen.left + 10, screen.right - Frame().Width() - 10);
    const float top = std::clamp(near.bottom - Frame().Height() - 40, screen.top + 30, screen.bottom - Frame().Height() - 10);
    MoveTo(left, top);
}

BSize PictureInPictureWindow::PictureSize() const
{
    const float scale = BWebKitDisplayScale();
    return BSize(std::ceil((Bounds().Width() + 1) * scale), std::ceil((Bounds().Height() + 1) * scale));
}

void PictureInPictureWindow::Command(const char* command)
{
    BMessage message(kPictureInPictureCommand);
    message.AddInt64("tab", fTab);
    message.AddUInt64("session", fSession);
    message.AddString("command", command);
    message.AddBool("ended", fEnded);
    if (!std::strcmp(command, "size")) message.AddSize("size", PictureSize());
    fBrowser.SendMessage(&message);
}

void PictureInPictureWindow::MessageReceived(BMessage* message)
{
    switch (message->what) {
    case B_WEBKIT_PICTURE_IN_PICTURE_FRAME: {
        const void* pixels = nullptr;
        ssize_t size = 0;
        const int32 width = message->GetInt32("width", 0), height = message->GetInt32("height", 0);
        if (message->FindData("pixels", B_RAW_TYPE, &pixels, &size) != B_OK || width <= 0 || height <= 0
            || size < ssize_t(width) * height * 4)
            return;
        fView->SetPicture(pixels, width, height, message->GetBool("paused", false));
        // The window takes the video's shape once.
        const float aspect = float(width) / height;
        if (std::fabs(aspect - fAspect) > 0.02f) {
            fAspect = aspect;
            fAdjusting = true;
            ResizeTo(Bounds().Width(), std::round((Bounds().Width() + 1) / fAspect) - 1);
            fAdjusting = false;
        }
        return;
    }
    case B_WEBKIT_PICTURE_IN_PICTURE_ENDED:
        fEnded = true;
        PostMessage(B_QUIT_REQUESTED);
        return;
    }
    BWindow::MessageReceived(message);
}

void PictureInPictureWindow::FrameResized(float width, float height)
{
    BWindow::FrameResized(width, height);
    if (!fAdjusting) {
        // Keep the video's shape when the user resizes the window.
        const float wanted = std::round((width + 1) / fAspect) - 1;
        if (std::fabs(wanted - height) > 2) {
            fAdjusting = true;
            ResizeTo(width, wanted);
            fAdjusting = false;
        }
    }
    Command("size");
}

bool PictureInPictureWindow::QuitRequested()
{
    Command("closed");
    return true;
}
}
