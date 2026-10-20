#pragma once
#if SUMMIT_MODERN_WEBKIT
#include "Chrome.h"
#include "DownloadList.h"
#include <Window.h>
#include <atomic>
#include <memory>

class BButton;
namespace summit {
class DownloadsView;
// The toolbar's downloads button: while files download, a progress bar
// under the arrow; a dot once one has finished that the panel has not shown
// (red when it failed), as Firefox's button does.
class DownloadsButton final : public ToolButton {
public:
    DownloadsButton();
    void SetSummary(const DownloadList::Summary&);
    void Draw(BRect update) override;
private:
    DownloadList::Summary fSummary;
};

// The panel the downloads button opens: the downloads, newest first, with
// their progress; pause, resume, cancel, retry, open and show them. A window
// of its own under the button, closed when it loses the focus. It tells its
// browser window when it closes (kDownloadsPanelClosed) and asks it to
// download a file again (kDownloadRetry); pausing, resuming and cancelling go
// to the application (kDownloadPause, kDownloadResume, kDownloadCancel),
// which knows the download's engine context.
class DownloadsPanel final : public BWindow {
public:
    // anchor: the downloads button's frame on the screen. menusCancelled
    // closes the panel's context menu; set it before asking the panel to quit.
    DownloadsPanel(BWindow* owner, bool privateBrowsing, BRect anchor, std::shared_ptr<std::atomic<bool>> menusCancelled);
    ~DownloadsPanel() override;
    void MessageReceived(BMessage*) override;
    void WindowActivated(bool active) override;
    bool QuitRequested() override;
private:
    void Refresh();
    void Place();
    void Act(uint32 what, uint64 identifier);
    BMessenger fOwner;
    bool fPrivate;
    BRect fAnchor;
    std::shared_ptr<std::atomic<bool>> fMenusCancelled;
    DownloadsView* fList;
    BButton* fClear;
    bool fWasActive = false;
    bool fDeactivated = false;
};
}
#endif
