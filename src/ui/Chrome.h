#pragma once
#include <Button.h>
#include <Messenger.h>
#include <PopUpMenu.h>
#include <View.h>
#include <atomic>
#include <string>
#include <vector>
#include <memory>

class BBitmap;
class BMenu;
class BMenuItem;
class BMessageRunner;

namespace summit {
enum class Icon { Back, Forward, Reload, Stop, Plus, Bookmark, BookmarkFilled, Downloads, Home, More, Go, Lock, LockWarning, Reader, ReaderActive };
// The look of the browser's own controls, for every window: true draws them
// like other Haiku applications (BControlLook buttons and tabs), false keeps
// the flat, Safari-like look.
void SetInterfaceStyle(bool haiku);
bool HaikuInterfaceStyle();
// Sent to the window by a secondary click on a tab: "id" (int64), "where" (screen point).
constexpr uint32 kTabMenu = 'tbmn';
// Private windows draw their bars in purple, so they cannot be mistaken for
// ordinary ones. The window registers itself; its views look it up.
void SetPrivateWindow(const BWindow* window, bool privateBrowsing);
bool IsPrivateWindow(const BWindow* window);
// The colours the browser's own bars and buttons use in a view's window.
struct ChromeColors {
    rgb_color panel, text, control, controlText;
    bool privateBrowsing;
};
ChromeColors ChromeColorsFor(const BView* view);
ChromeColors ChromeColorsFor(bool privateBrowsing);
class ToolButton : public BButton {
public:
    ToolButton(const char* name, const char* tooltip, Icon icon, uint32 message);
    ~ToolButton() override;
    void Draw(BRect update) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    BSize MinSize() override;
    BSize MaxSize() override;
    BSize PreferredSize() override;
    void SetIcon(Icon icon) { if (fIcon != icon) { fIcon = icon; Invalidate(); } }
private:
    void UpdateIconBitmap(rgb_color ink);
    std::unique_ptr<BBitmap> fBitmap;
    rgb_color fBitmapInk { 0, 0, 0, 0 };
    int fBitmapSize = 0;
    Icon fBitmapIcon = Icon::More;
    Icon fIcon;
    bool fHover = false;
};
// The page's zoom, shown in the toolbar while it is not 100%; clicking it
// resets the zoom, as in Firefox.
class ZoomButton : public BButton {
public:
    ZoomButton();
    void SetZoom(double zoom);
    void Draw(BRect update) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    BSize MinSize() override;
    BSize MaxSize() override;
    BSize PreferredSize() override;
private:
    bool fHover = false;
};
// "Private" with a mask, at the end of a private window's toolbar.
class PrivateBadge : public BView {
public:
    PrivateBadge();
    void Draw(BRect update) override;
    BSize MinSize() override;
    BSize MaxSize() override;
    BSize PreferredSize() override;
};
#if SUMMIT_MODERN_WEBKIT
class ExtensionActionButton : public BButton {
public:
    explicit ExtensionActionButton(const char* identifier);
    ~ExtensionActionButton() override;
    void SetAction(const BMessage&, uint64 snapshot);
    void Draw(BRect update) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void MouseDown(BPoint where) override;
    BSize MinSize() override;
    BSize MaxSize() override;
    BSize PreferredSize() override;
private:
    void Preload();
    BMessage fPreload;
    std::unique_ptr<BBitmap> fBitmap;
    std::string fBadge;
    rgb_color fBadgeBackground { 217, 0, 0, 255 };
    rgb_color fBadgeTextColor { 255, 255, 255, 255 };
};
#endif
// A pop-up menu its window can dismiss. An open menu holds its window's close
// lock, so a window that closes (or quits with Summit) must end its menus
// first: set the flag and the menu closes within 50 ms.
class CancellableMenu final : public BPopUpMenu {
public:
    CancellableMenu(const char* name, std::shared_ptr<std::atomic<bool>> cancelled);
    ~CancellableMenu() override;
    void AttachedToWindow() override;
    void DetachedFromWindow() override;
    void MessageReceived(BMessage* message) override;
private:
    std::shared_ptr<std::atomic<bool>> fCancelled;
    std::unique_ptr<BMessageRunner> fTimer;
};
// capturing: the page uses the camera, microphone or a screen (a red dot).
// url: the page's address, for dragging the tab onto the bookmarks bar.
struct TabLabel {
    int64 id;
    std::string title;
    bool loading;
    const BBitmap* icon = nullptr;
    bool capturing = false;
    std::string url;
};
// Draws a site icon, or a neutral globe when there is none.
void DrawSiteIcon(BView* view, const BBitmap* icon, BPoint leftTop);
class TabStrip : public BView {
public:
    TabStrip();
    void SetTabs(std::vector<TabLabel> tabs, int64 selected);
    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void MessageReceived(BMessage* message) override;
    void FrameResized(float width, float height) override;
    BSize MinSize() override;
    BSize MaxSize() override;
    BSize PreferredSize() override;
private:
    void DrawHaiku(BRect update);
    void DrawSafari(BRect update);
    BRect TabRect(size_t index) const;
    BRect CloseRect(size_t index) const;
    BRect NewTabRect() const;
    float Height() const;
    size_t FirstVisible() const;
    size_t VisibleCount() const;
    std::vector<TabLabel> fTabs;
    int64 fSelected = 0;
    int64 fHoverClose = 0;
    bool fHoverNewTab = false;
    // A tab pressed with the primary button, which can be dragged out (onto
    // the bookmarks bar, or to Tracker as a bookmark file).
    int64 fPressedTab = 0;
    BPoint fPressPoint;
    std::string fDraggedURL, fDraggedTitle;
};
// A bookmark or a folder as the bookmarks bar and the Bookmarks menu show it:
// a copy of the profile's, which menus read from their own threads.
struct BookmarkButton {
    int64 id = 0;
    std::string url, title;
    // The bar's own buttons only; owned by the window's icon cache.
    const BBitmap* icon = nullptr;
    bool folder = false;
    std::vector<BookmarkButton> children;
};
// Adds a folder's contents to a menu: bookmarks, folders as submenus that
// are filled when they open, and Open All in Tabs. Their messages go to target.
void AddBookmarkItems(BMenu* menu, std::shared_ptr<const BookmarkButton> root, const BookmarkButton& folder,
    const BMessenger& target, size_t limit = 300);
// A folder as an item with its submenu (filled when it opens).
BMenuItem* NewBookmarkFolderItem(std::shared_ptr<const BookmarkButton> root, const BookmarkButton& folder,
    const BMessenger& target);
// Safari-style favourites under the toolbar. Clicking opens a bookmark in the
// current tab; a middle click or Command-click opens it in a new tab; a
// folder opens as a menu; the secondary button offers the rest. Buttons can
// be dragged to another place or into a folder, and pages (tabs, links from
// other applications, Tracker's bookmark files) dropped on the bar are
// bookmarked there. Messages go to the window.
class BookmarksBar : public BView {
public:
    BookmarksBar();
    // The bar's folder: its children are the buttons.
    void SetBookmarks(std::shared_ptr<const BookmarkButton> bar);
    // The window's current menu-cancel flag (it is replaced when a close is cancelled).
    void SetMenusCancelled(const std::shared_ptr<std::atomic<bool>>* flag) { fMenusCancelled = flag; }
    void Draw(BRect update) override;
    void MouseDown(BPoint where) override;
    void MouseUp(BPoint where) override;
    void MouseMoved(BPoint where, uint32 transit, const BMessage* drag) override;
    void MessageReceived(BMessage* message) override;
    void FrameResized(float width, float height) override;
private:
    const std::vector<BookmarkButton>& Items() const;
    void LayoutItems();
    int32 ItemAt(BPoint where) const;
    void ShowOverflow();
    void ShowFolder(int32 index);
    void ShowContextMenu(int32 index, BPoint where);
    void Open(int32 index, bool newTab);
    // Where a drop at where goes: before the button at the index (the
    // number of buttons: after the last), or into the folder there.
    int32 DropIndex(BPoint where, bool& into) const;
    void SetDropTarget(int32 index, bool into);
    std::shared_ptr<const BookmarkButton> fBar;
    std::vector<BRect> fRects;
    size_t fVisible = 0;
    BRect fOverflow;
    int32 fHover = -1, fPressed = -1;
    uint32 fPressedButtons = 0;
    BPoint fPressPoint;
    int32 fDropIndex = -1;
    bool fDropInto = false;
    // The button dragged last, for Tracker's request to make its file.
    std::string fDraggedURL, fDraggedTitle;
    const std::shared_ptr<std::atomic<bool>>* fMenusCancelled = nullptr;
    BPopUpMenu* NewMenu(const char* name);
};
class ProgressLine : public BView {
public:
    ProgressLine();
    void SetProgress(float progress);
    void Draw(BRect update) override;
private:
    float fProgress = 0;
};
}
