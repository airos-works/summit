#include "PictureInPictureWindow.h"
#include "FeatureMessages.h"
#include <WebKit/WebKitView.h>
#include <Bitmap.h>
#include <Font.h>
#include <GradientLinear.h>
#include <MessageRunner.h>
#include <Screen.h>
#include <Shape.h>
#include <View.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

namespace summit {
namespace {
constexpr uint32 kTick = 'piti';
// airTime's full screen controller goes after 2.8 seconds without the mouse.
constexpr bigtime_t kControlsTimeout = 2800000;
// Rewind and fast forward held down skip again and again.
constexpr bigtime_t kRepeatDelay = 450000, kRepeatInterval = 200000;
constexpr double kSkip = 10;

rgb_color Color(uint8 red, uint8 green, uint8 blue, uint8 alpha = 255)
{
    return rgb_color{red, green, blue, alpha};
}

std::string FormatTime(double seconds, bool hours)
{
    const long long total = std::isfinite(seconds) && seconds > 0 ? (long long)seconds : 0;
    char text[32];
    if (hours) std::snprintf(text, sizeof text, "%lld:%02lld:%02lld", total / 3600, total / 60 % 60, total % 60);
    else std::snprintf(text, sizeof text, "%lld:%02lld", total / 60, total % 60);
    return text;
}

// app_server draws a shape where the pen is (as R5 did), and StrokeLine and
// DrawString move the pen: shapes here are always in view coordinates. It
// also scales a shape by the scale of the current state alone, so nothing
// here uses PushState: in a pushed state, shapes would miss the SetScale of
// the composing bitmap's view.
void ShapeFill(BView* view, BShape& shape)
{
    view->MovePenTo(B_ORIGIN);
    view->FillShape(&shape);
}

void ShapeFill(BView* view, BShape& shape, const BGradient& gradient)
{
    view->MovePenTo(B_ORIGIN);
    view->FillShape(&shape, gradient);
}

void ShapeStroke(BView* view, BShape& shape)
{
    view->MovePenTo(B_ORIGIN);
    view->StrokeShape(&shape);
}

BPoint At(BRect rect, float x, float y)
{
    return BPoint(rect.left + x * rect.Width(), rect.top + y * rect.Height());
}

void FillPolygon(BView* view, BRect rect, const float* points, int count)
{
    BShape shape;
    shape.MoveTo(At(rect, points[0], points[1]));
    for (int i = 1; i < count; i++) shape.LineTo(At(rect, points[i * 2], points[i * 2 + 1]));
    shape.Close();
    ShapeFill(view, shape);
}

enum class Glyph { Play, Pause, Rewind, FastForward, Speaker, SpeakerMuted };

// airTime's glyphs (Look.cpp), in the unit square of rect.
void DrawGlyph(BView* view, Glyph glyph, BRect rect, rgb_color color)
{
    view->SetDrawingMode(B_OP_ALPHA);
    view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
    view->SetHighColor(color);
    switch (glyph) {
    case Glyph::Play: {
        const float points[] = {0.32f, 0.20f, 0.32f, 0.80f, 0.82f, 0.50f};
        FillPolygon(view, rect, points, 3);
        break;
    }
    case Glyph::Pause:
        view->FillRect(BRect(At(rect, 0.27f, 0.22f), At(rect, 0.43f, 0.78f)));
        view->FillRect(BRect(At(rect, 0.57f, 0.22f), At(rect, 0.73f, 0.78f)));
        break;
    case Glyph::Rewind: {
        const float a[] = {0.50f, 0.26f, 0.50f, 0.74f, 0.13f, 0.50f};
        const float b[] = {0.87f, 0.26f, 0.87f, 0.74f, 0.50f, 0.50f};
        FillPolygon(view, rect, a, 3);
        FillPolygon(view, rect, b, 3);
        break;
    }
    case Glyph::FastForward: {
        const float a[] = {0.13f, 0.26f, 0.13f, 0.74f, 0.50f, 0.50f};
        const float b[] = {0.50f, 0.26f, 0.50f, 0.74f, 0.87f, 0.50f};
        FillPolygon(view, rect, a, 3);
        FillPolygon(view, rect, b, 3);
        break;
    }
    case Glyph::Speaker:
    case Glyph::SpeakerMuted: {
        view->FillRect(BRect(At(rect, 0.10f, 0.38f), At(rect, 0.26f, 0.62f)));
        const float cone[] = {0.24f, 0.38f, 0.48f, 0.16f, 0.48f, 0.84f, 0.24f, 0.62f};
        FillPolygon(view, rect, cone, 4);
        view->SetPenSize(std::max(1.2f, rect.Width() / 12.0f));
        view->SetLineMode(glyph == Glyph::Speaker ? B_BUTT_CAP : B_ROUND_CAP, B_ROUND_JOIN);
        if (glyph == Glyph::Speaker) {
            const BPoint center = At(rect, 0.46f, 0.50f);
            for (int wave = 0; wave < 2; wave++) {
                const float radius = rect.Width() * (0.20f + wave * 0.17f);
                view->StrokeArc(center, radius, radius, -45, 90);
            }
        } else {
            view->StrokeLine(At(rect, 0.60f, 0.34f), At(rect, 0.88f, 0.66f));
            view->StrokeLine(At(rect, 0.60f, 0.66f), At(rect, 0.88f, 0.34f));
        }
        break;
    }
    }
}

// airTime's dark timeline track (look::timeline_track), played to playedTo.
void DrawTrack(BView* view, BRect track, float playedTo)
{
    view->SetDrawingMode(B_OP_ALPHA);
    view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
    const float radius = track.Height() / 2;
    view->SetHighColor(Color(20, 20, 20, 230));
    view->FillRoundRect(track, radius, radius);
    view->SetHighColor(Color(255, 255, 255, 40));
    view->SetPenSize(1);
    view->StrokeRoundRect(track.OffsetByCopy(0, 1), radius, radius);
    if (playedTo > track.left + 1) {
        BRect played = track;
        played.right = std::min(playedTo, track.right);
        BGradientLinear fill(played.LeftTop(), played.LeftBottom());
        fill.AddColor(Color(230, 230, 234), 0);
        fill.AddColor(Color(170, 170, 176), 255);
        view->FillRoundRect(played, radius, radius, fill);
    }
}

// airTime's playhead: a pale pentagon pointing down at the track.
void DrawPlayhead(BView* view, BPoint tip, float height)
{
    const float width = std::round(height * 0.8f), shoulder = height * 0.52f;
    auto outline = [&](BShape& shape, float dx, float dy) {
        shape.MoveTo(BPoint(tip.x - width / 2 + dx, tip.y - height + dy));
        shape.LineTo(BPoint(tip.x + width / 2 + dx, tip.y - height + dy));
        shape.LineTo(BPoint(tip.x + width / 2 + dx, tip.y - height + shoulder + dy));
        shape.LineTo(BPoint(tip.x + dx, tip.y + dy));
        shape.LineTo(BPoint(tip.x - width / 2 + dx, tip.y - height + shoulder + dy));
        shape.Close();
    };
    BShape shape, shadow;
    outline(shape, 0, 0);
    outline(shadow, 0.8f, 1.2f);
    view->SetDrawingMode(B_OP_ALPHA);
    view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
    view->SetHighColor(Color(0, 0, 0, 60));
    ShapeFill(view, shadow);
    BGradientLinear face(BPoint(0, tip.y - height), BPoint(0, tip.y));
    face.AddColor(Color(250, 250, 250), 0);
    face.AddColor(Color(200, 200, 204), 255);
    ShapeFill(view, shape, face);
    view->SetHighColor(Color(40, 40, 44, 220));
    view->SetPenSize(1);
    view->SetLineMode(B_BUTT_CAP, B_MITER_JOIN);
    ShapeStroke(view, shape);
}
}

// The picture, and while the pointer is over it, the controls: back to the
// tab and close at the top, airTime's controller at the bottom. With the
// controls showing, picture and controls are put together in a bitmap
// first, so the video never shows through them between two drawing steps.
class PictureView : public BView {
public:
    explicit PictureView(BRect frame)
        : BView(frame, "picture", B_FOLLOW_ALL, B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE)
    {
        SetViewColor(B_TRANSPARENT_COLOR);
        // Shown when the window opens, so the user learns they are there.
        fLastActivity = system_time();
        fShown = true;
        Layout();
    }
    void AttachedToWindow() override
    {
        BView::AttachedToWindow();
        MakeFocus(true);
    }
    void SetFrame(const BMessage& message, const void* pixels, int32 width, int32 height)
    {
        if (!fBitmap || fBitmap->Bounds().IntegerWidth() + 1 != width || fBitmap->Bounds().IntegerHeight() + 1 != height)
            fBitmap = std::make_unique<BBitmap>(BRect(0, 0, width - 1, height - 1), B_RGB32);
        if (fBitmap->InitCheck() != B_OK) return;
        const int32 rowBytes = width * 4;
        auto* bits = static_cast<uint8*>(fBitmap->Bits());
        for (int32 y = 0; y < height; ++y)
            std::memcpy(bits + y * fBitmap->BytesPerRow(), static_cast<const uint8*>(pixels) + y * rowBytes, rowBytes);
        const bigtime_t now = system_time();
        // What the user just did wins over frames sent before the engine saw it.
        if (now > fPausedHeld) fPaused = message.GetBool("paused", false);
        if (now > fVolumeHeld) {
            fVolume = std::clamp(message.GetDouble("volume", 1.0), 0.0, 1.0);
            fMuted = message.GetBool("muted", false);
        }
        fTime = message.GetDouble("time", 0);
        fDuration = message.GetDouble("duration", 0);
        // A live stream, or one without its metadata yet, has no timeline.
        fHasTimeline = std::isfinite(fDuration) && fDuration > 0;
        if (fSeekHeld && (now > fSeekHeld || std::fabs(fTime - fSeekTarget) < 1.0)) fSeekHeld = 0;
        Invalidate();
    }
    void FrameResized(float width, float height) override
    {
        BView::FrameResized(width, height);
        Layout();
        Invalidate();
    }
    void Draw(BRect) override
    {
        if (!fShown) {
            DrawPicture(this);
            return;
        }
        if (BView* view = Offscreen()) {
            fOffscreen->Lock();
            view->SetScale(fOffscreenScale);
            DrawPicture(view);
            DrawControls(view);
            view->Sync();
            fOffscreen->Unlock();
            DrawBitmap(fOffscreen.get(), fOffscreen->Bounds(), Bounds());
            return;
        }
        DrawPicture(this);
        DrawControls(this);
    }
    void MouseMoved(BPoint where, uint32 transit, const BMessage*) override
    {
        const bool inside = transit != B_EXITED_VIEW && transit != B_OUTSIDE_VIEW;
        if (inside) Activity();
        if (fPressed == kTimeline && fHasTimeline) {
            fDragTime = TimeAt(where.x);
            const bigtime_t now = system_time();
            if (now - fLastDragSent > 120000) {
                Seek(fDragTime);
                fLastDragSent = now;
            }
            Invalidate();
            return;
        }
        if (fPressed == kVolume) {
            SetVolume(VolumeAt(where.x));
            return;
        }
        if (fPressed != kNone) {
            const bool over = PartAt(where) == fPressed;
            if (over != fPressedInside) { fPressedInside = over; Invalidate(); }
            return;
        }
        const Part hover = inside ? PartAt(where) : kNone;
        if (hover != fHover) { fHover = hover; Invalidate(); }
        // The controls go with the pointer, as Firefox's do.
        if (!inside && fShown) ShowControls(false);
    }
    void MouseDown(BPoint where) override
    {
        MakeFocus(true);
        Activity();
        auto* window = static_cast<PictureInPictureWindow*>(Window());
        const Part part = PartAt(where);
        if (part == kNone) {
            // The picture moves the window, as a title bar would; a click
            // without moving plays or pauses.
            const BPoint start = ConvertToScreen(where);
            const BPoint origin = window->Frame().LeftTop();
            bool moved = false;
            uint32 buttons;
            BPoint now;
            do {
                snooze(10000);
                GetMouse(&now, &buttons, false);
                const BPoint offset = ConvertToScreen(now) - start;
                if (std::fabs(offset.x) > 3 || std::fabs(offset.y) > 3) moved = true;
                if (moved) window->MoveTo(origin + offset);
            } while (buttons);
            if (!moved) TogglePlay();
            Activity();
            return;
        }
        fPressed = part;
        fPressedInside = true;
        fPressedAt = system_time();
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS | B_NO_POINTER_HISTORY);
        switch (part) {
        case kTimeline:
            if (!fHasTimeline) break;
            fDragTime = TimeAt(where.x);
            Seek(fDragTime);
            fLastDragSent = fPressedAt;
            break;
        case kVolume:
            SetVolume(VolumeAt(where.x));
            break;
        case kRewind:
        case kFastForward:
            Skip(part == kRewind ? -kSkip : kSkip);
            fNextRepeat = fPressedAt + kRepeatDelay;
            break;
        default:
            break;
        }
        Invalidate();
    }
    void MouseUp(BPoint where) override
    {
        const Part part = fPressed;
        const bool inside = fPressedInside && PartAt(where) == part;
        fPressed = kNone;
        fPressedInside = false;
        auto* window = static_cast<PictureInPictureWindow*>(Window());
        switch (part) {
        case kTimeline:
            if (fHasTimeline) {
                fDragTime = TimeAt(where.x);
                Seek(fDragTime);
            }
            break;
        case kVolume:
            SetVolume(VolumeAt(where.x));
            break;
        case kPlay:
            if (inside) TogglePlay();
            break;
        case kSpeaker:
            if (inside) SetMuted(!fMuted);
            break;
        case kLCD:
            if (inside) fRemaining = !fRemaining;
            break;
        case kBack:
            if (inside) window->Command("back");
            break;
        case kClose:
            if (inside) window->PostMessage(B_QUIT_REQUESTED);
            break;
        default:
            break;
        }
        fHover = PartAt(where);
        Activity();
        Invalidate();
    }
    void KeyDown(const char* bytes, int32 count) override
    {
        if (count != 1) { BView::KeyDown(bytes, count); return; }
        switch (bytes[0]) {
        case B_SPACE: TogglePlay(); break;
        case B_LEFT_ARROW: Skip(-kSkip); break;
        case B_RIGHT_ARROW: Skip(kSkip); break;
        case B_UP_ARROW: SetVolume(std::min(1.0, fVolume + 0.1)); break;
        case B_DOWN_ARROW: SetVolume(std::max(0.0, fVolume - 0.1)); break;
        case 'm': case 'M': SetMuted(!fMuted); break;
        default: BView::KeyDown(bytes, count); return;
        }
        Activity();
    }
    // Ten times a second: rewind and fast forward held down, and the
    // controls going when the pointer rests.
    void Tick()
    {
        const bigtime_t now = system_time();
        if ((fPressed == kRewind || fPressed == kFastForward) && fPressedInside && now >= fNextRepeat) {
            Skip(fPressed == kRewind ? -kSkip : kSkip);
            fNextRepeat = now + kRepeatInterval;
        }
        if (!fShown || fPressed != kNone || now - fLastActivity < kControlsTimeout) return;
        // Not while the pointer rests on them, or on a paused picture.
        BPoint where;
        uint32 buttons;
        GetMouse(&where, &buttons, false);
        const bool inside = Bounds().Contains(where);
        if (inside && (fPaused || PartAt(where) != kNone || fPanel.Contains(where))) return;
        ShowControls(false);
    }
private:
    enum Part { kNone, kBack, kClose, kTimeline, kSpeaker, kVolume, kRewind, kPlay, kFastForward, kLCD, kBar };
    void Activity()
    {
        fLastActivity = system_time();
        if (!fShown) ShowControls(true);
    }
    void ShowControls(bool show)
    {
        if (show == fShown) return;
        fShown = show;
        if (!show) fHover = kNone;
        Invalidate();
    }
    void Send(const char* command)
    {
        static_cast<PictureInPictureWindow*>(Window())->Command(command);
    }
    void TogglePlay()
    {
        Send("toggle");
        fPaused = !fPaused;
        fPausedHeld = system_time() + 600000;
        Invalidate();
    }
    void Seek(double time)
    {
        char command[48];
        std::snprintf(command, sizeof command, "seek:%.3f", time);
        Send(command);
        fSeekTarget = time;
        fSeekHeld = system_time() + 1500000;
        Invalidate();
    }
    void Skip(double delta)
    {
        char command[48];
        std::snprintf(command, sizeof command, "skip:%g", delta);
        Send(command);
        double target = ShownTime() + delta;
        if (fHasTimeline) target = std::clamp(target, 0.0, fDuration);
        fSeekTarget = std::max(0.0, target);
        fSeekHeld = system_time() + 1500000;
        Invalidate();
    }
    void SetVolume(double volume)
    {
        volume = std::clamp(volume, 0.0, 1.0);
        char command[48];
        std::snprintf(command, sizeof command, "volume:%.3f", volume);
        Send(command);
        // A volume chosen while muted is meant to be heard.
        if (fMuted && volume > 0) Send("unmute");
        fVolume = volume;
        fMuted = fMuted && volume == 0;
        fVolumeHeld = system_time() + 600000;
        Invalidate();
    }
    void SetMuted(bool muted)
    {
        Send(muted ? "mute" : "unmute");
        fMuted = muted;
        fVolumeHeld = system_time() + 600000;
        Invalidate();
    }
    double ShownTime() const
    {
        if (fPressed == kTimeline && fHasTimeline) return fDragTime;
        return fSeekHeld ? fSeekTarget : fTime;
    }
    // The layout of airTime's full screen controller (ControlBar::_Layout
    // with hud), scaled down for small windows.
    void Layout()
    {
        const float width = Bounds().Width() + 1, height = Bounds().Height() + 1;
        const float k = std::clamp(std::min(width / 520.0f, height / 292.0f), 0.55f, 1.0f);
        auto r = [k](float value) { return std::round(value * k); };
        fScale = k;
        const float margin = r(8), panelHeight = r(74);
        fPanel.Set(margin, height - margin - panelHeight, width - margin - 1, height - margin - 1);
        const float inner = r(18), trackHeight = std::max(4.0f, r(6));
        fTrack.Set(fPanel.left + inner, fPanel.top + r(18), fPanel.right - inner, fPanel.top + r(18) + trackHeight);
        fTimelineHit.Set(fTrack.left - r(7), fTrack.top - r(11), fTrack.right + r(7), fTrack.bottom + r(6));
        const float row = fPanel.top + r(50);
        fSpeaker.Set(fPanel.left + inner, row - r(8), fPanel.left + inner + r(16), row + r(8));
        fVolumeTrack.Set(fSpeaker.right + r(5), row - r(3), fSpeaker.right + r(5) + r(80), row + r(3));
        const float scan = r(28), play = r(34), gap = r(10);
        float x = std::floor((fPanel.left + fPanel.right) / 2 - (scan * 2 + play + gap * 2) / 2);
        auto place = [&](BRect& rect, float size) {
            rect.Set(x, std::round(row - size / 2), x + size, std::round(row - size / 2) + size);
            x += size + gap;
        };
        place(fRewind, scan);
        place(fPlay, play);
        place(fFastForward, scan);
        const float right = fPanel.right - inner;
        const float lcdWidth = std::min(right - (fFastForward.right + r(12)), r(150));
        fShowLCD = lcdWidth >= r(44);
        fLCD = fShowLCD ? BRect(right - lcdWidth, row - r(11), right, row + r(11)) : BRect();
        // The volume slider shrinks, then goes, rather than run into the buttons.
        const float leftLimit = fRewind.left - r(10);
        fShowSpeaker = fSpeaker.right <= leftLimit;
        fVolumeTrack.right = std::min(fVolumeTrack.right, leftLimit);
        fShowVolume = fShowSpeaker && fVolumeTrack.Width() >= r(30);
        // The two buttons at the top.
        const float button = std::max(18.0f, r(28)), inset = r(8);
        fBack.Set(inset, inset, inset + button, inset + button);
        fClose.Set(width - inset - button - 1, inset, width - inset - 1, inset + button);
    }
    Part PartAt(BPoint where) const
    {
        if (!fShown) return kNone;
        if (fBack.Contains(where)) return kBack;
        if (fClose.Contains(where)) return kClose;
        for (Part part : { kPlay, kRewind, kFastForward }) {
            // Round buttons: the circle.
            const BRect frame = PartFrame(part);
            const BPoint center((frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2);
            const float radius = frame.Width() / 2 + 1, dx = where.x - center.x, dy = where.y - center.y;
            if (dx * dx + dy * dy <= radius * radius) return part;
        }
        if (fShowLCD && fLCD.Contains(where)) return kLCD;
        if (fShowSpeaker && fSpeaker.Contains(where)) return kSpeaker;
        if (fShowVolume && fVolumeTrack.InsetByCopy(-6 * fScale, -6 * fScale).Contains(where)) return kVolume;
        if (fTimelineHit.Contains(where)) return kTimeline;
        // The rest of the controller is not the picture: no window drag there.
        if (fPanel.Contains(where)) return kBar;
        return kNone;
    }
    BRect PartFrame(Part part) const
    {
        switch (part) {
        case kRewind: return fRewind;
        case kPlay: return fPlay;
        case kFastForward: return fFastForward;
        default: return BRect();
        }
    }
    float TimelineX(double time) const
    {
        if (!fHasTimeline) return fTrack.left;
        const double fraction = std::clamp(time / fDuration, 0.0, 1.0);
        return fTrack.left + 1 + (fTrack.Width() - 2) * fraction;
    }
    double TimeAt(float x) const
    {
        if (!fHasTimeline || fTrack.Width() <= 2) return 0;
        return std::clamp((x - fTrack.left - 1) / (fTrack.Width() - 2), 0.0f, 1.0f) * fDuration;
    }
    double VolumeAt(float x) const
    {
        if (fVolumeTrack.Width() <= 0) return fVolume;
        return std::clamp((x - fVolumeTrack.left) / fVolumeTrack.Width(), 0.0f, 1.0f);
    }
    // A bitmap of the view's size in device pixels, drawn in the view's own
    // coordinates, so that it reaches a 200% screen one to one.
    BView* Offscreen()
    {
        const float scale = std::max(1.0f, BWebKitDisplayScale());
        const int32 width = int32(std::ceil((Bounds().Width() + 1) * scale)), height = int32(std::ceil((Bounds().Height() + 1) * scale));
        if (!fOffscreen || fOffscreen->Bounds().IntegerWidth() + 1 != width || fOffscreen->Bounds().IntegerHeight() + 1 != height
            || fOffscreenScale != scale) {
            fOffscreenView = nullptr;
            fOffscreen = std::make_unique<BBitmap>(BRect(0, 0, width - 1, height - 1), B_BITMAP_ACCEPTS_VIEWS, B_RGB32);
            if (fOffscreen->InitCheck() != B_OK) { fOffscreen.reset(); return nullptr; }
            fOffscreenView = new BView(fOffscreen->Bounds(), "composite", B_FOLLOW_NONE, 0);
            fOffscreen->AddChild(fOffscreenView);
            fOffscreenScale = scale;
        }
        return fOffscreenView;
    }
    void DrawPicture(BView* view)
    {
        const BRect bounds = Bounds();
        view->SetDrawingMode(B_OP_COPY);
        view->SetHighColor(0, 0, 0);
        if (!fBitmap) { view->FillRect(bounds); return; }
        const BRect source = fBitmap->Bounds();
        const float fit = std::min((bounds.Width() + 1) / (source.Width() + 1), (bounds.Height() + 1) / (source.Height() + 1));
        const float width = (source.Width() + 1) * fit, height = (source.Height() + 1) * fit;
        BRect target(0, 0, width - 1, height - 1);
        target.OffsetTo(std::floor((bounds.Width() + 1 - width) / 2), std::floor((bounds.Height() + 1 - height) / 2));
        // Black bars around the picture, drawn without flicker.
        view->FillRect(BRect(bounds.left, bounds.top, bounds.right, target.top - 1));
        view->FillRect(BRect(bounds.left, target.bottom + 1, bounds.right, bounds.bottom));
        view->FillRect(BRect(bounds.left, target.top, target.left - 1, target.bottom));
        view->FillRect(BRect(target.right + 1, target.top, bounds.right, target.bottom));
        view->DrawBitmap(fBitmap.get(), source, target, B_FILTER_BITMAP_BILINEAR);
    }
    void DrawControls(BView* view)
    {
        view->SetDrawingMode(B_OP_ALPHA);
        view->SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        DrawTopButton(view, kBack);
        DrawTopButton(view, kClose);

        // airTime's dark glass (look::hud_panel), a little see-through.
        const float radius = std::round(10 * fScale);
        BGradientLinear glass(fPanel.LeftTop(), fPanel.LeftBottom());
        glass.AddColor(Color(62, 62, 66, 224), 0);
        glass.AddColor(Color(34, 34, 38, 224), 120);
        glass.AddColor(Color(22, 22, 26, 224), 255);
        view->FillRoundRect(fPanel, radius, radius, glass);
        view->SetHighColor(Color(255, 255, 255, 50));
        view->SetPenSize(1);
        view->StrokeRoundRect(fPanel.InsetByCopy(1, 1), radius - 1, radius - 1);
        view->SetHighColor(Color(0, 0, 0, 200));
        view->StrokeRoundRect(fPanel, radius, radius);

        const double shown = ShownTime();
        const float playheadX = TimelineX(shown);
        DrawTrack(view, fTrack, fHasTimeline ? playheadX : fTrack.left);
        if (fHasTimeline) DrawPlayhead(view, BPoint(std::round(playheadX) + 0.5f, fTrack.bottom - 1), std::max(8.0f, std::round(12 * fScale)));

        DrawRoundButton(view, kRewind, Glyph::Rewind);
        DrawRoundButton(view, kPlay, fPaused ? Glyph::Play : Glyph::Pause);
        DrawRoundButton(view, kFastForward, Glyph::FastForward);

        if (fShowSpeaker) {
            rgb_color color = Color(232, 232, 236);
            if (fPressed == kSpeaker && fPressedInside) color = Color(150, 170, 200);
            else if (fHover == kSpeaker && fPressed == kNone) color = Color(255, 255, 255);
            DrawGlyph(view, fMuted ? Glyph::SpeakerMuted : Glyph::Speaker, fSpeaker, color);
        }
        if (fShowVolume) {
            const float fraction = fMuted ? 0 : fVolume;
            DrawTrack(view, fVolumeTrack, fVolumeTrack.left + fVolumeTrack.Width() * fraction);
            const float knobX = std::round(fVolumeTrack.left + fVolumeTrack.Width() * fraction);
            const float knobY = (fVolumeTrack.top + fVolumeTrack.bottom) / 2, knob = std::round(6 * fScale);
            const BRect knobRect(knobX - knob, knobY - knob, knobX + knob, knobY + knob);
            BGradientLinear face(knobRect.LeftTop(), knobRect.LeftBottom());
            face.AddColor(Color(250, 250, 250), 0);
            face.AddColor(Color(190, 190, 196), 255);
            view->FillEllipse(knobRect, face);
        }
        if (fShowLCD) DrawLCD(view, shown);
    }
    void DrawRoundButton(BView* view, Part part, Glyph glyph)
    {
        const BRect frame = PartFrame(part);
        const bool pressed = (fPressed == part && fPressedInside), hover = fHover == part && fPressed == kNone;
        rgb_color color = Color(232, 232, 236);
        if (hover) color = Color(255, 255, 255);
        else if (pressed) color = Color(150, 170, 200);
        if (hover || pressed) {
            view->SetHighColor(Color(255, 255, 255, 28));
            view->FillEllipse(frame.InsetByCopy(-2, -2));
        }
        DrawGlyph(view, glyph, frame.InsetByCopy(frame.Width() * 0.12f, frame.Height() * 0.12f), color);
    }
    void DrawLCD(BView* view, double shown)
    {
        BFont font(be_bold_font);
        font.SetSize(std::round(13 * fScale));
        view->SetFont(&font);
        font_height height;
        font.GetHeight(&height);
        const float baseline = std::round((fLCD.top + fLCD.bottom) / 2 + (height.ascent - height.descent) / 2);
        const bool hours = (fHasTimeline ? fDuration : shown) >= 3600;
        std::string time;
        if (fRemaining && fHasTimeline) time = "-" + FormatTime(fDuration - shown, hours);
        else {
            time = FormatTime(shown, hours);
            if (fHasTimeline) time += " / " + FormatTime(fDuration, hours);
        }
        const float padding = std::round(7 * fScale);
        if (font.StringWidth(time.c_str()) + padding * 2 > fLCD.Width() && fHasTimeline && !fRemaining)
            time = FormatTime(shown, hours);
        view->SetDrawingMode(B_OP_OVER);
        view->SetHighColor(Color(240, 240, 244));
        view->DrawString(time.c_str(), BPoint(fLCD.right - padding - font.StringWidth(time.c_str()), baseline));
        view->SetDrawingMode(B_OP_ALPHA);
    }
    // The buttons at the top: dark glass squares with white line icons.
    void DrawTopButton(BView* view, Part part)
    {
        const BRect frame = part == kBack ? fBack : fClose;
        const bool pressed = fPressed == part && fPressedInside, hover = fHover == part && fPressed == kNone;
        const float radius = std::round(frame.Width() * 0.22f);
        view->SetHighColor(Color(20, 20, 24, hover || pressed ? 224 : 176));
        view->FillRoundRect(frame, radius, radius);
        view->SetHighColor(Color(255, 255, 255, 46));
        view->SetPenSize(1);
        view->StrokeRoundRect(frame, radius, radius);
        const rgb_color color = pressed ? Color(150, 170, 200) : hover ? Color(255, 255, 255) : Color(232, 232, 236);
        view->SetHighColor(color);
        const float size = frame.Width();
        const float line = std::max(1.0f, std::round(size / 14));
        const BPoint center((frame.left + frame.right) / 2, (frame.top + frame.bottom) / 2);
        if (part == kClose) {
            const float arm = std::round(size * 0.17f);
            view->SetPenSize(line);
            view->SetLineMode(B_ROUND_CAP, B_ROUND_JOIN);
            view->StrokeLine(center + BPoint(-arm, -arm), center + BPoint(arm, arm));
            view->StrokeLine(center + BPoint(arm, -arm), center + BPoint(-arm, arm));
            return;
        }
        // Back to the tab (the usual "leave picture in picture" icon): the
        // page's frame, the picture in its bottom right corner and an arrow
        // from the picture back up into the frame. Drawn on a grid of whole
        // points as shapes, which cover exactly their area at 100% and 200%.
        const float unit = size / 26;
        auto u = [unit](float value) { return std::round(value * unit); };
        const float width = u(20), height = u(15), t = std::max(1.0f, u(2));
        const float x = std::round(center.x - width / 2), y = std::round(center.y - height / 2);
        auto rect = [&](BShape& shape, float left, float top, float right, float bottom) {
            shape.MoveTo(BPoint(x + left, y + top));
            shape.LineTo(BPoint(x + right, y + top));
            shape.LineTo(BPoint(x + right, y + bottom));
            shape.LineTo(BPoint(x + left, y + bottom));
            shape.Close();
        };
        const float pictureLeft = u(11), pictureTop = u(8);
        BShape icon;
        // The frame, open where the picture sits.
        rect(icon, 0, 0, width, t);
        rect(icon, 0, 0, t, height);
        rect(icon, 0, height - t, pictureLeft - t, height);
        rect(icon, width - t, 0, width, pictureTop - t);
        rect(icon, pictureLeft, pictureTop, width, height);
        // The arrow's head, pointing to the frame's top left corner.
        const float head = u(4), arm = u(5);
        rect(icon, head, head, head + arm, head + t);
        rect(icon, head, head, head + t, head + arm);
        ShapeFill(view, icon);
        // Its shaft, from the head's corner towards the picture.
        const BPoint from(x + head + t / 2, y + head + t / 2), to(x + pictureLeft - u(1.5f), y + pictureTop + u(0.5f));
        const BPoint along = to - from;
        const float length = std::sqrt(along.x * along.x + along.y * along.y);
        const BPoint normal(-along.y / length * t / 2, along.x / length * t / 2);
        BShape shaft;
        shaft.MoveTo(from + normal);
        shaft.LineTo(to + normal);
        shaft.LineTo(to - normal);
        shaft.LineTo(from - normal);
        shaft.Close();
        ShapeFill(view, shaft);
    }

    std::unique_ptr<BBitmap> fBitmap;
    std::unique_ptr<BBitmap> fOffscreen;
    BView* fOffscreenView = nullptr;
    float fOffscreenScale = 1;
    bool fPaused = false, fMuted = false, fHasTimeline = false, fShown = false, fRemaining = false;
    double fTime = 0, fDuration = 0, fVolume = 1, fSeekTarget = 0, fDragTime = 0;
    bigtime_t fLastActivity = 0, fPausedHeld = 0, fVolumeHeld = 0, fSeekHeld = 0, fPressedAt = 0, fLastDragSent = 0, fNextRepeat = 0;
    Part fHover = kNone, fPressed = kNone;
    bool fPressedInside = false;
    float fScale = 1;
    bool fShowLCD = true, fShowVolume = true, fShowSpeaker = true;
    BRect fPanel, fTrack, fTimelineHit, fSpeaker, fVolumeTrack, fRewind, fPlay, fFastForward, fLCD, fBack, fClose;
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
    BMessage tick(kTick);
    fTicks = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 100000);
}

PictureInPictureWindow::~PictureInPictureWindow() = default;

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
        fView->SetFrame(*message, pixels, width, height);
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
    case kTick:
        fView->Tick();
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
