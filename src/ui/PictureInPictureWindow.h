#pragma once
#include <Messenger.h>
#include <Window.h>
#include <memory>
#include <string>

class BMessageRunner;

namespace summit {
class PictureView;
// The floating window of Picture in Picture (#74, #80): above other windows,
// on every workspace, showing the frames the engine sends it
// (B_WEBKIT_PICTURE_IN_PICTURE_FRAME). While the pointer is over it, it shows
// a back-to-tab and a close button and airTime's full screen controller
// (play/pause, rewind and fast forward by 10 seconds, the timeline, volume).
// Its controls and its closing go to the browser window as
// kPictureInPictureCommand ("tab", "command": "toggle", "seek:<s>",
// "skip:<s>", "volume:<0-1>", "mute", "unmute", "back", "size", "closed").
class PictureInPictureWindow : public BWindow {
public:
    // session: numbers this window's commands, so that a window still
    // closing cannot end the next one's video.
    // near: the browser window's frame; the picture starts in its bottom
    // right corner.
    PictureInPictureWindow(BMessenger browser, int64 tab, uint64 session, const std::string& title, BRect near);
    ~PictureInPictureWindow() override;
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;
    void FrameResized(float width, float height) override;
    // The size the pictures should have, in device pixels.
    BSize PictureSize() const;
    void Command(const char* command);
private:
    BMessenger fBrowser;
    int64 fTab;
    uint64 fSession;
    // The engine ended it (the video went away): nothing to tell the page.
    bool fEnded = false;
    PictureView* fView;
    std::unique_ptr<BMessageRunner> fTicks;
    float fAspect = 16.0f / 9.0f;
    bool fAdjusting = false;
};
}
