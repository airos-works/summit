#pragma once
#include <Messenger.h>
#include <Window.h>
#include <string>

namespace summit {
class PictureView;
// The floating window of Picture in Picture (#74): above other windows, on
// every workspace, showing the frames the engine sends it
// (B_WEBKIT_PICTURE_IN_PICTURE_FRAME). Its buttons and its closing go to the
// browser window as kPictureInPictureCommand ("tab", "command": "toggle",
// "back", "size", "closed").
class PictureInPictureWindow : public BWindow {
public:
    // session: numbers this window's commands, so that a window still
    // closing cannot end the next one's video.
    // near: the browser window's frame; the picture starts in its bottom
    // right corner.
    PictureInPictureWindow(BMessenger browser, int64 tab, uint64 session, const std::string& title, BRect near);
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
    float fAspect = 16.0f / 9.0f;
    bool fAdjusting = false;
    bigtime_t fLastSize = 0;
};
}
