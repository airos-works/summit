#include "DownloadsPanel.h"
#if SUMMIT_MODERN_WEBKIT
#include "Messages.h"
#include <WebKit/WebKitView.h>
#include <Application.h>
#include <Bitmap.h>
#include <Button.h>
#include <Clipboard.h>
#include <Entry.h>
#include <LayoutBuilder.h>
#include <MenuItem.h>
#include <MimeType.h>
#include <NodeInfo.h>
#include <Path.h>
#include <Roster.h>
#include <Screen.h>
#include <String.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <tuple>
#include <utility>

namespace summit {
namespace {
// Actions of a row (to the panel, "identifier").
constexpr uint32 kOpenDownload = 'dpop', kShowDownload = 'dpsh', kCancelDownload = 'dpcn', kRetryDownload = 'dprt',
    kRemoveDownload = 'dprm', kCopyDownloadLink = 'dpcp', kClearDownloads = 'dpcl';
// Rows the panel shows; older downloads are in the Downloads folder.
constexpr size_t kShownRows = 8;

rgb_color Mix(rgb_color a, rgb_color b, float amount)
{
    auto channel = [&](uint8 x, uint8 y) { return static_cast<uint8>(x + (y - x) * amount); };
    return make_color(channel(a.red, b.red), channel(a.green, b.green), channel(a.blue, b.blue));
}
bool Dark(rgb_color color) { return int(color.red) + color.green + color.blue < 384; }
rgb_color Accent(rgb_color background)
{
    return Dark(background) ? rgb_color{96, 165, 250, 255} : rgb_color{26, 107, 214, 255};
}
float Scale() { return std::max(1.0f, be_plain_font->Size() / 12.0f); }

struct Row {
    DownloadList::Entry entry;
    bool missing = false;
    std::shared_ptr<BBitmap> icon;
};

// The file's icon: its type's (guessed from its name while it downloads),
// or Tracker's for the file, or a plain document.
std::shared_ptr<BBitmap> FileIcon(const DownloadList::Entry& entry, int pixels)
{
    auto icon = std::make_shared<BBitmap>(BRect(0, 0, pixels - 1, pixels - 1), B_RGBA32);
    if (icon->InitCheck() != B_OK) return nullptr;
    const auto size = static_cast<icon_size>(pixels);
    BMimeType type;
    if (!entry.filename.empty() && BMimeType::GuessMimeType(entry.filename.c_str(), &type) == B_OK
        && type.Type() && std::strcmp(type.Type(), B_FILE_MIME_TYPE) != 0) {
        if (type.GetIcon(icon.get(), size) == B_OK) return icon;
        BMimeType supertype;
        if (type.GetSupertype(&supertype) == B_OK && supertype.GetIcon(icon.get(), size) == B_OK) return icon;
    }
    entry_ref ref;
    if (entry.state == DownloadStatus::State::Succeeded && !entry.path.empty()
        && get_ref_for_path(entry.path.c_str(), &ref) == B_OK && BNodeInfo::GetTrackerIcon(&ref, icon.get(), size) == B_OK)
        return icon;
    if (BMimeType(B_FILE_MIME_TYPE).GetIcon(icon.get(), size) == B_OK) return icon;
    return nullptr;
}
}

class DownloadsView final : public BView {
public:
    DownloadsView() : BView("downloads-list", B_WILL_DRAW | B_FRAME_EVENTS)
    {
        SetViewUIColor(B_DOCUMENT_BACKGROUND_COLOR);
    }
    void SetRows(std::vector<Row> rows)
    {
        fRows = std::move(rows);
        fHover = -1;
        fHoverButton = false;
        Invalidate();
    }
    const std::vector<Row>& Rows() const { return fRows; }
    float RowHeight() const { return std::ceil(56 * Scale()); }
    float ContentHeight() const { return fRows.empty() ? std::ceil(64 * Scale()) : RowHeight() * fRows.size(); }
    BSize MinSize() override { return BSize(std::ceil(360 * Scale()), ContentHeight()); }
    BSize MaxSize() override { return BSize(B_SIZE_UNLIMITED, ContentHeight()); }
    BSize PreferredSize() override { return MinSize(); }

    void Draw(BRect update) override
    {
        const float s = Scale();
        const rgb_color background = ui_color(B_DOCUMENT_BACKGROUND_COLOR);
        const rgb_color text = ui_color(B_DOCUMENT_TEXT_COLOR);
        const rgb_color dim = Mix(background, text, 0.62f);
        const rgb_color accent = Accent(background);
        SetHighColor(background);
        FillRect(update);
        if (fRows.empty()) {
            SetHighColor(dim);
            const char* empty = "No downloads in this session";
            font_height height;
            GetFontHeight(&height);
            DrawString(empty, BPoint(std::floor((Bounds().Width() - StringWidth(empty)) / 2),
                std::floor((Bounds().Height() + height.ascent - height.descent) / 2)));
            return;
        }
        BFont plain(be_plain_font), small(be_plain_font);
        small.SetSize(be_plain_font->Size() * 0.88f);
        font_height plainHeight, smallHeight;
        plain.GetHeight(&plainHeight);
        small.GetHeight(&smallHeight);
        const float rowHeight = RowHeight(), padding = std::round(10 * s), iconSize = std::round(32 * s);
        for (size_t i = 0; i < fRows.size(); ++i) {
            const auto& row = fRows[i];
            const auto& entry = row.entry;
            const BRect frame(0, i * rowHeight, Bounds().right, (i + 1) * rowHeight - 1);
            if (!frame.Intersects(update)) continue;
            const bool active = entry.state == DownloadStatus::State::Active;
            if (int32(i) == fHover && !fHoverButton && entry.state == DownloadStatus::State::Succeeded && !row.missing) {
                SetHighColor(Mix(background, text, 0.07f));
                FillRect(frame);
            }
            if (i) {
                SetHighColor(Mix(background, text, 0.12f));
                StrokeLine(BPoint(padding, frame.top), BPoint(frame.right - padding, frame.top));
            }
            const BRect iconFrame(padding, frame.top + std::floor((rowHeight - iconSize) / 2),
                padding + iconSize - 1, frame.top + std::floor((rowHeight - iconSize) / 2) + iconSize - 1);
            if (row.icon) {
                SetDrawingMode(B_OP_ALPHA);
                SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
                // Dimmed while the file is not (or no longer) there to open.
                if (active || row.missing || entry.state != DownloadStatus::State::Succeeded) {
                    SetHighColor(0, 0, 0, 120);
                    SetBlendingMode(B_CONSTANT_ALPHA, B_ALPHA_OVERLAY);
                }
                DrawBitmap(row.icon.get(), row.icon->Bounds(), iconFrame, B_FILTER_BITMAP_BILINEAR);
                SetDrawingMode(B_OP_COPY);
            }
            const BRect button = ButtonFrame(i);
            const float textLeft = iconFrame.right + padding;
            const float textRight = button.left - std::round(6 * s);
            // The name, then what is happening, then (while active) a bar.
            const float nameBaseline = frame.top + std::round(8 * s) + std::ceil(plainHeight.ascent);
            SetFont(&plain);
            SetHighColor(row.missing || entry.state == DownloadStatus::State::Cancelled ? dim : text);
            BString name(entry.filename.empty() ? "Download" : entry.filename.c_str());
            TruncateString(&name, B_TRUNCATE_MIDDLE, std::max(0.0f, textRight - textLeft));
            DrawString(name.String(), BPoint(textLeft, nameBaseline));
            SetFont(&small);
            auto status = entry.Status();
            status.missing = row.missing;
            SetHighColor(entry.state == DownloadStatus::State::Failed ? rgb_color{Dark(background) ? uint8(255) : uint8(190),
                Dark(background) ? uint8(120) : uint8(40), Dark(background) ? uint8(110) : uint8(30), 255} : dim);
            BString line(DownloadStatusText(status).c_str());
            TruncateString(&line, B_TRUNCATE_END, std::max(0.0f, textRight - textLeft));
            const float statusBaseline = nameBaseline + std::ceil(plainHeight.descent) + std::round(3 * s) + std::ceil(smallHeight.ascent);
            DrawString(line.String(), BPoint(textLeft, statusBaseline));
            SetFont(be_plain_font);
            if (active) {
                const float top = statusBaseline + std::ceil(smallHeight.descent) + std::round(4 * s);
                const BRect track(textLeft, top, textRight, top + std::round(4 * s) - 1);
                SetHighColor(Mix(background, text, 0.14f));
                FillRoundRect(track, 2 * s, 2 * s);
                const double fraction = DownloadFraction(entry.received, entry.expected);
                SetHighColor(accent);
                if (fraction >= 0) {
                    BRect done = track;
                    done.right = track.left + std::max(4 * s, float(track.Width() * fraction));
                    FillRoundRect(done, 2 * s, 2 * s);
                } else {
                    // Size unknown: a segment that moves as data arrives.
                    const float width = track.Width() * 0.3f;
                    const float position = float((system_time() / 16000) % 100) / 100;
                    BRect moving(track.left + (track.Width() - width) * position, track.top, 0, track.bottom);
                    moving.right = moving.left + width;
                    FillRoundRect(moving, 2 * s, 2 * s);
                }
            }
            DrawButton(i, button, text, background);
        }
    }

    void MouseMoved(BPoint where, uint32 transit, const BMessage*) override
    {
        int32 row = transit == B_EXITED_VIEW || transit == B_OUTSIDE_VIEW ? -1 : RowAt(where);
        const bool button = row >= 0 && ButtonFrame(row).Contains(where);
        if (row == fHover && button == fHoverButton) return;
        fHover = row;
        fHoverButton = button;
        Invalidate();
    }
    void MouseDown(BPoint where) override
    {
        const int32 row = RowAt(where);
        uint32 buttons = 0;
        if (Window()->CurrentMessage()) Window()->CurrentMessage()->FindInt32("buttons", reinterpret_cast<int32*>(&buttons));
        if (row < 0) return;
        if (buttons & B_SECONDARY_MOUSE_BUTTON) { ShowMenu(row, where); return; }
        fPressed = row;
        fPressedButton = ButtonFrame(row).Contains(where);
        SetMouseEventMask(B_POINTER_EVENTS, B_LOCK_WINDOW_FOCUS);
    }
    void MouseUp(BPoint where) override
    {
        const int32 row = RowAt(where), pressed = std::exchange(fPressed, -1);
        if (row < 0 || row != pressed || ButtonFrame(row).Contains(where) != fPressedButton) return;
        const auto& entry = fRows[row].entry;
        uint32 what = 0;
        if (fPressedButton) {
            what = entry.state == DownloadStatus::State::Active ? kCancelDownload
                : entry.state == DownloadStatus::State::Succeeded ? (fRows[row].missing ? kRetryDownload : kShowDownload)
                : kRetryDownload;
        } else if (entry.state == DownloadStatus::State::Succeeded && !fRows[row].missing) what = kOpenDownload;
        if (!what) return;
        BMessage message(what);
        message.AddUInt64("identifier", entry.identifier);
        Window()->PostMessage(&message);
    }
    void SetMenusCancelled(std::shared_ptr<std::atomic<bool>> cancelled) { fMenusCancelled = std::move(cancelled); }

private:
    int32 RowAt(BPoint where) const
    {
        const int32 row = int32(where.y / RowHeight());
        return where.y >= 0 && row < int32(fRows.size()) ? row : -1;
    }
    BRect ButtonFrame(size_t row) const
    {
        const float size = std::round(30 * Scale()), padding = std::round(10 * Scale());
        const float top = row * RowHeight() + std::floor((RowHeight() - size) / 2);
        return BRect(Bounds().right - padding - size + 1, top, Bounds().right - padding, top + size - 1);
    }
    // Cancel (×) while active, show in folder once done, retry after a failure.
    void DrawButton(size_t row, BRect frame, rgb_color text, rgb_color background)
    {
        const auto& entry = fRows[row].entry;
        const float s = Scale();
        if (int32(row) == fHover && fHoverButton) {
            SetHighColor(Mix(background, text, 0.12f));
            FillEllipse(frame);
        }
        SetHighColor(Mix(background, text, 0.8f));
        SetPenSize(std::max(1.0f, 1.5f * s));
        SetDrawingMode(B_OP_ALPHA);
        const BPoint c(frame.left + frame.Width() / 2, frame.top + frame.Height() / 2);
        const float r = 5 * s;
        if (entry.state == DownloadStatus::State::Active) {
            StrokeLine(c + BPoint(-r, -r), c + BPoint(r, r));
            StrokeLine(c + BPoint(r, -r), c + BPoint(-r, r));
        } else if (entry.state == DownloadStatus::State::Succeeded && !fRows[row].missing) {
            // A folder.
            BPoint folder[] = { c + BPoint(-r - 1, -r + 1), c + BPoint(-2 * s, -r + 1), c + BPoint(0, -r + 3 * s),
                c + BPoint(r + 1, -r + 3 * s), c + BPoint(r + 1, r), c + BPoint(-r - 1, r) };
            StrokePolygon(folder, 6);
        } else {
            // Retry: an open circle with an arrow head.
            StrokeArc(c, r, r, 60, 270);
            const BPoint tip = c + BPoint(r * std::cos(M_PI / 3), -r * std::sin(M_PI / 3));
            StrokeLine(tip, tip + BPoint(-3.5f * s, -0.5f * s));
            StrokeLine(tip, tip + BPoint(0.5f * s, 3.5f * s));
        }
        SetPenSize(1);
        SetDrawingMode(B_OP_COPY);
    }
    void ShowMenu(int32 row, BPoint where)
    {
        const auto& entry = fRows[row].entry;
        auto* menu = new CancellableMenu("download", fMenusCancelled);
        auto item = [&](const char* label, uint32 what, bool enabled = true) {
            auto* message = new BMessage(what);
            message->AddUInt64("identifier", entry.identifier);
            auto* menuItem = new BMenuItem(label, message);
            menuItem->SetEnabled(enabled);
            menu->AddItem(menuItem);
        };
        const bool done = entry.state == DownloadStatus::State::Succeeded && !fRows[row].missing;
        const bool active = entry.state == DownloadStatus::State::Active;
        item("Open", kOpenDownload, done);
        item("Show in Folder", kShowDownload, done);
        menu->AddSeparatorItem();
        if (active) item("Cancel", kCancelDownload);
        else item("Retry", kRetryDownload, entry.state != DownloadStatus::State::Succeeded || fRows[row].missing);
        item("Copy Download Link", kCopyDownloadLink, !entry.url.empty());
        item("Remove from List", kRemoveDownload, !active);
        menu->AddSeparatorItem();
        menu->AddItem(new BMenuItem("Clear List", new BMessage(kClearDownloads)));
        menu->SetTargetForItems(Window());
        menu->Go(ConvertToScreen(where), true, false, true);
    }

    std::vector<Row> fRows;
    std::shared_ptr<std::atomic<bool>> fMenusCancelled;
    int32 fHover = -1, fPressed = -1;
    bool fHoverButton = false, fPressedButton = false;
};

DownloadsButton::DownloadsButton() : ToolButton("downloads", "Downloads", Icon::Downloads, kToggleDownloadsPanel) { }

void DownloadsButton::SetSummary(const DownloadList::Summary& summary)
{
    const bool same = summary.active == fSummary.active && summary.unseen == fSummary.unseen
        && summary.unseenFailure == fSummary.unseenFailure && std::fabs(summary.progress - fSummary.progress) < 0.002f;
    fSummary = summary;
    // An unknown size moves the bar with every update.
    if (same && !(summary.active && summary.progress < 0)) return;
    std::string tip = "Downloads";
    if (summary.active) {
        tip += " — " + std::to_string(summary.active) + (summary.active == 1 ? " file" : " files") + " downloading";
        if (summary.progress >= 0) tip += ", " + std::to_string(int(summary.progress * 100)) + "%";
    }
    SetToolTip(tip.c_str());
    Invalidate();
}

void DownloadsButton::Draw(BRect update)
{
    ToolButton::Draw(update);
    const auto colors = ChromeColorsFor(this);
    const rgb_color background = HaikuInterfaceStyle() ? colors.control : colors.panel;
    const rgb_color accent = Accent(background);
    SetDrawingMode(B_OP_ALPHA);
    const BRect bounds = Bounds();
    const float offset = Value() ? 1 : 0;
    // The icon's 20 points are centred; the bar goes in the gap under them.
    const float left = std::floor((bounds.Width() + 1 - 20) / 2) + 1 + offset;
    const float top = std::floor((bounds.Height() + 1 - 20) / 2) + 20 + offset;
    if (fSummary.active) {
        const BRect track(left, top, left + 17, top + 1.6f);
        SetHighColor(Mix(background, colors.controlText, 0.25f));
        FillRect(track);
        SetHighColor(accent);
        if (fSummary.progress >= 0) {
            BRect done = track;
            done.right = track.left + std::max(1.0f, track.Width() * fSummary.progress);
            FillRect(done);
        } else {
            const float position = float((system_time() / 16000) % 100) / 100;
            BRect moving(track.left + (track.Width() - 6) * position, track.top, 0, track.bottom);
            moving.right = moving.left + 6;
            FillRect(moving);
        }
    } else if (fSummary.unseen) {
        SetHighColor(fSummary.unseenFailure ? rgb_color{217, 48, 37, 255} : rgb_color{46, 154, 90, 255});
        FillEllipse(BPoint(bounds.right - 6.5f + offset, bounds.top + 6.5f + offset), 3, 3);
    }
    SetDrawingMode(B_OP_COPY);
}

DownloadsPanel::DownloadsPanel(BWindow* owner, bool privateBrowsing, BRect anchor,
    std::shared_ptr<std::atomic<bool>> menusCancelled)
    : BWindow(BRect(0, 0, 100, 100), "Downloads", B_BORDERED_WINDOW_LOOK, B_FLOATING_SUBSET_WINDOW_FEEL,
        B_NOT_MOVABLE | B_NOT_CLOSABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE
            | B_WILL_ACCEPT_FIRST_CLICK | B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , fOwner(owner), fPrivate(privateBrowsing), fAnchor(anchor), fMenusCancelled(std::move(menusCancelled))
{
    fList = new DownloadsView;
    fList->SetMenusCancelled(fMenusCancelled);
    fClear = new BButton("downloads-clear", "Clear List", new BMessage(kClearDownloads));
    auto* folder = new BButton("downloads-folder", "Show Downloads Folder", new BMessage(kShowDownloads));
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .Add(fList)
        .AddGroup(B_HORIZONTAL, B_USE_SMALL_SPACING)
            .SetInsets(B_USE_SMALL_SPACING)
            .Add(fClear).AddGlue().Add(folder)
        .End();
    AddToSubset(owner);
    DownloadList::Default().AddListener(BMessenger(this));
    Refresh();
}

DownloadsPanel::~DownloadsPanel()
{
    DownloadList::Default().RemoveListener(BMessenger(this));
}

void DownloadsPanel::Place()
{
    const BSize size = GetLayout()->PreferredSize();
    ResizeTo(size.width, size.height);
    const BRect screen = BScreen(this).Frame();
    float left = std::round(fAnchor.right - size.width);
    left = std::clamp(left, screen.left + 4, std::max(screen.left + 4, screen.right - size.width - 4));
    float top = std::round(fAnchor.bottom + 3);
    if (top + size.height > screen.bottom - 4) top = std::max(screen.top + 4, std::round(fAnchor.top - size.height - 3));
    MoveTo(left, top);
}

void DownloadsPanel::Refresh()
{
    auto entries = DownloadList::Default().Entries(fPrivate);
    if (entries.size() > kShownRows) entries.resize(kShownRows);
    // Icons are kept while a row stays, and made again when it finishes or
    // its file name arrives (after the download started).
    std::map<uint64, std::tuple<DownloadStatus::State, std::string, std::shared_ptr<BBitmap>>> icons;
    for (const auto& row : fList->Rows()) icons[row.entry.identifier] = {row.entry.state, row.entry.filename, row.icon};
    float displayScale = BWebKitDisplayScale();
    const int pixels = std::max(16, int(std::round(32 * Scale() * std::max(1.0f, displayScale))));
    std::vector<Row> rows;
    bool finished = false;
    for (auto& entry : entries) {
        Row row;
        row.entry = entry;
        if (entry.state == DownloadStatus::State::Succeeded) {
            BEntry file(entry.path.c_str());
            row.missing = entry.path.empty() || !file.Exists();
        }
        if (entry.state != DownloadStatus::State::Active) finished = true;
        auto known = icons.find(entry.identifier);
        if (known != icons.end() && std::get<0>(known->second) == entry.state && std::get<1>(known->second) == entry.filename
            && std::get<2>(known->second))
            row.icon = std::get<2>(known->second);
        else row.icon = FileIcon(entry, pixels);
        rows.push_back(std::move(row));
    }
    const float before = fList->ContentHeight();
    const size_t countBefore = fList->Rows().size();
    fList->SetRows(std::move(rows));
    fClear->SetEnabled(finished);
    if (fList->ContentHeight() != before || fList->Rows().size() != countBefore || IsHidden()) {
        fList->InvalidateLayout();
        Place();
    }
    // What is shown is no longer news.
    if (!IsHidden()) DownloadList::Default().MarkSeen(fPrivate);
}

void DownloadsPanel::Act(uint32 what, uint64 identifier)
{
    const auto entry = DownloadList::Default().Find(identifier);
    if (!entry) return;
    switch (what) {
    case kOpenDownload: {
        entry_ref ref;
        if (get_ref_for_path(entry->path.c_str(), &ref) == B_OK && be_roster->Launch(&ref) != B_OK) {
            // No application for its type: show it in Tracker instead.
            Act(kShowDownload, identifier);
            return;
        }
        PostMessage(B_QUIT_REQUESTED);
        break;
    }
    case kShowDownload: {
        BEntry file(entry->path.c_str());
        node_ref node;
        BEntry parent;
        entry_ref parentRef;
        if (!file.Exists() || file.GetNodeRef(&node) != B_OK || file.GetParent(&parent) != B_OK
            || parent.GetRef(&parentRef) != B_OK) break;
        // Tracker opens the folder and selects the file in it.
        BMessenger tracker("application/x-vnd.Be-TRAK");
        BMessage message(B_REFS_RECEIVED);
        message.AddRef("refs", &parentRef);
        message.AddData("nodeRefToSelect", B_RAW_TYPE, &node, sizeof(node_ref));
        tracker.SendMessage(&message);
        PostMessage(B_QUIT_REQUESTED);
        break;
    }
    case kCancelDownload: {
        BMessage cancel(kDownloadCancel);
        cancel.AddUInt64("identifier", identifier);
        be_app->PostMessage(&cancel);
        break;
    }
    case kRetryDownload: {
        if (entry->url.empty()) break;
        BMessage retry(kDownloadRetry);
        retry.AddString("url", entry->url.c_str());
        fOwner.SendMessage(&retry);
        DownloadList::Default().Remove(identifier);
        break;
    }
    case kRemoveDownload:
        DownloadList::Default().Remove(identifier);
        break;
    case kCopyDownloadLink:
        if (be_clipboard->Lock()) {
            be_clipboard->Clear();
            if (BMessage* clip = be_clipboard->Data())
                clip->AddData("text/plain", B_MIME_TYPE, entry->url.data(), entry->url.size());
            be_clipboard->Commit();
            be_clipboard->Unlock();
        }
        break;
    }
}

void DownloadsPanel::MessageReceived(BMessage* message)
{
    switch (message->what) {
    case kDownloadsChanged: Refresh(); break;
    case kClearDownloads: DownloadList::Default().ClearFinished(fPrivate); break;
    case kShowDownloads:
        fOwner.SendMessage(kShowDownloads);
        PostMessage(B_QUIT_REQUESTED);
        break;
    case kOpenDownload: case kShowDownload: case kCancelDownload: case kRetryDownload:
    case kRemoveDownload: case kCopyDownloadLink:
        Act(message->what, message->GetUInt64("identifier", 0));
        break;
    default: BWindow::MessageReceived(message);
    }
}

void DownloadsPanel::WindowActivated(bool active)
{
    BWindow::WindowActivated(active);
    if (active) {
        fWasActive = true;
        DownloadList::Default().MarkSeen(fPrivate);
    } else if (fWasActive && !fDeactivated) {
        // Like a menu: a click anywhere else closes it.
        fDeactivated = true;
        PostMessage(B_QUIT_REQUESTED);
    }
}

bool DownloadsPanel::QuitRequested()
{
    fMenusCancelled->store(true);
    BMessage closed(kDownloadsPanelClosed);
    closed.AddMessenger("panel", BMessenger(this));
    closed.AddBool("deactivated", fDeactivated);
    // The window may be waiting for this one to quit.
    fOwner.SendMessage(&closed, static_cast<BHandler*>(nullptr), 0);
    return true;
}
}
#endif
