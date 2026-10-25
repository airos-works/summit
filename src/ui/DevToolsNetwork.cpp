#if SUMMIT_MODERN_WEBKIT
#include "DevToolsPanels.h"
#include <interface/ColumnListView.h>
#include <interface/ColumnTypes.h>
#include <AppFileInfo.h>
#include <Application.h>
#include <Bitmap.h>
#include <Entry.h>
#include <File.h>
#include <FilePanel.h>
#include <Path.h>
#include <Roster.h>
#include <Button.h>
#include <CardLayout.h>
#include <CheckBox.h>
#include <DataIO.h>
#include <GroupView.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <ScrollBar.h>
#include <SplitView.h>
#include <StringView.h>
#include <TabView.h>
#include <TextControl.h>
#include <TranslationUtils.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>
#include <memory>

namespace summit {
using namespace devtools;
namespace {
constexpr uint32 kFormatToggled = 'dbfm', kWrapToggled = 'dbwr', kCopyBody = 'dbcp', kClearRequests = 'dncl',
    kPreserveRequests = 'dnpr', kFilterRequests = 'dnfl', kTypeChosen = 'dnty', kRequestSelected = 'dnsl', kCopyCurl = 'dncu',
    kExportHar = 'dnhx', kHarChosen = 'dnhc', kRequestMenu = 'dnrm', kReplay = 'dnrp', kBlockURL = 'dnbk', kUnblockAll = 'dnub',
    kDisableCache = 'dndc';

// "0.1.0", the browser's version for a HAR's creator.
std::string ApplicationVersion()
{
    app_info info;
    version_info version { };
    if (!be_app || be_app->GetAppInfo(&info) != B_OK) return { };
    BFile file(&info.ref, B_READ_ONLY);
    BAppFileInfo fileInfo(&file);
    if (fileInfo.InitCheck() != B_OK || fileInfo.GetVersionInfo(&version, B_APP_VERSION_KIND) != B_OK) return { };
    return std::to_string(version.major) + "." + std::to_string(version.middle) + "." + std::to_string(version.minor);
}
// Bodies over this are shown as they came: laying them out would hold the window up.
constexpr size_t kLargestFormatted = 32 * 1024 * 1024;

enum { kName, kStatus, kMethod, kType, kDomain, kSize, kTime, kInitiator, kColumns };
enum { kCardNote, kCardSource, kCardImage };

const char* const kCategories[] = {"All types", "Fetch and XHR", "Documents", "Style sheets", "Scripts", "Images", "Fonts", "Other"};

int32 CategoryOf(const std::string& type)
{
    if (type == "XHR" || type == "Fetch" || type == "EventSource" || type == "WebSocket" || type == "Beacon" || type == "Ping") return 1;
    if (type == "Document") return 2;
    if (type == "StyleSheet") return 3;
    if (type == "Script") return 4;
    if (type == "Image") return 5;
    if (type == "Font") return 6;
    return 7;
}

std::string LowerCase(std::string text)
{
    for (auto& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}

std::string HeaderValue(const std::vector<Header>& headers, const std::string& lowerName)
{
    for (const auto& header : headers)
        if (LowerCase(header.name) == lowerName) return header.value;
    return { };
}

// A cell: text that may sort as a number and may be drawn as a failure or as
// something that has not happened yet.
class Field : public BStringField {
public:
    enum Tone { Normal, Failed, Faint };
    Field() : BStringField("") { }
    void Set(const std::string& text, Tone tone, double number = 0)
    {
        if (text != String()) SetString(text.c_str());
        fTone = tone;
        fNumber = number;
    }
    Tone GetTone() const { return fTone; }
    double Number() const { return fNumber; }
private:
    Tone fTone = Normal;
    double fNumber = 0;
};

// A line of text in the middle of the room there is, however much that is.
BView* Centered(BView* text)
{
    auto* group = new BGroupView(B_VERTICAL, 0);
    BLayoutBuilder::Group<>(group).AddGlue().Add(text).AddGlue();
    group->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNLIMITED));
    return group;
}

class Column : public BStringColumn {
public:
    Column(const char* title, float width, bool numeric, uint32 truncate = B_TRUNCATE_END)
        : BStringColumn(title, width, 40, 2000, truncate, numeric ? B_ALIGN_RIGHT : B_ALIGN_LEFT), fNumeric(numeric)
    {
    }
    void DrawField(BField* field, BRect rect, BView* parent) override
    {
        const auto tone = static_cast<Field*>(field)->GetTone();
        const auto saved = parent->HighColor();
        // The row's own text is light when it is selected or the list is dark.
        const bool light = saved.red + saved.green + saved.blue > 384;
        if (tone == Field::Failed) parent->SetHighColor(light ? rgb_color {237, 135, 150, 255} : rgb_color {193, 56, 76, 255});
        else if (tone == Field::Faint) parent->SetHighColor(tint_color(saved, light ? B_DARKEN_2_TINT : B_LIGHTEN_2_TINT));
        BStringColumn::DrawField(field, rect, parent);
        parent->SetHighColor(saved);
    }
    int CompareFields(BField* first, BField* second) override
    {
        if (!fNumeric) return BStringColumn::CompareFields(first, second);
        const double a = static_cast<Field*>(first)->Number(), b = static_cast<Field*>(second)->Number();
        return a < b ? -1 : a > b ? 1 : 0;
    }
    bool AcceptsField(const BField* field) const override { return dynamic_cast<const Field*>(field) != nullptr; }
private:
    bool fNumeric;
};
}

class BodyView::ImageView : public BView {
public:
    ImageView()
        : BView("image", B_WILL_DRAW | B_FULL_UPDATE_ON_RESIZE)
    {
        SetViewColor(B_TRANSPARENT_COLOR);
    }
    bool SetImage(const std::string& bytes)
    {
        BMemoryIO stream(bytes.data(), bytes.size());
        fBitmap.reset(BTranslationUtils::GetBitmap(&stream));
        if (fBitmap && !fBitmap->IsValid()) fBitmap.reset();
        Invalidate();
        return fBitmap != nullptr;
    }
    BSize ImageSize() const { return fBitmap ? BSize(fBitmap->Bounds().Width() + 1, fBitmap->Bounds().Height() + 1) : BSize(0, 0); }
    void Draw(BRect) override
    {
        const auto& palette = CurrentSourcePalette();
        SetLowColor(palette.background);
        FillRect(Bounds(), B_SOLID_LOW);
        if (!fBitmap) return;
        const auto size = ImageSize();
        const BRect area = Bounds().InsetByCopy(12, 12);
        if (area.Width() < 1 || area.Height() < 1) return;
        const float scale = std::min({1.f, (area.Width() + 1) / size.width, (area.Height() + 1) / size.height});
        const float width = std::max(1.f, size.width * scale), height = std::max(1.f, size.height * scale);
        BRect target(0, 0, width - 1, height - 1);
        target.OffsetTo(area.left + std::floor((area.Width() + 1 - width) / 2), area.top + std::floor((area.Height() + 1 - height) / 2));
        // What is transparent in the image shows the squares under it.
        const auto light = palette.Tint(palette.text, .06f), dark = palette.Tint(palette.text, .16f);
        for (float y = target.top; y <= target.bottom; y += 8) {
            for (float x = target.left; x <= target.right; x += 8) {
                SetHighColor((static_cast<int>((x - target.left) / 8) + static_cast<int>((y - target.top) / 8)) % 2 ? dark : light);
                FillRect(BRect(x, y, std::min(x + 7, target.right), std::min(y + 7, target.bottom)));
            }
        }
        SetDrawingMode(B_OP_ALPHA);
        SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
        DrawBitmap(fBitmap.get(), fBitmap->Bounds(), target, B_FILTER_BITMAP_BILINEAR);
        SetDrawingMode(B_OP_COPY);
    }
private:
    std::unique_ptr<BBitmap> fBitmap;
};

BodyView::BodyView(const char* name)
    : BView(name, 0)
{
    fFormat = new BCheckBox("format", "Format", new BMessage(kFormatToggled));
    fFormat->SetValue(B_CONTROL_ON);
    fFormat->SetToolTip("Lay out JSON, XML and HTML for reading");
    fWrap = new BCheckBox("wrap", "Wrap lines", new BMessage(kWrapToggled));
    fCopy = new BButton("copy", "Copy", new BMessage(kCopyBody));
    fCopy->SetToolTip("Copy the text as it is shown");
    fInfo = new BStringView("info", "");
    fInfo->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fInfo->SetTruncation(B_TRUNCATE_END);
    fInfo->SetExplicitMinSize(BSize(40, B_SIZE_UNSET));
    fInfo->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fSource = new SourceView("body");
    fImage = new ImageView();
    fNote = new BStringView("note", "");
    fNote->SetAlignment(B_ALIGN_CENTER);
    fNote->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fNote->SetExplicitMinSize(BSize(40, 40));
    fNote->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    auto* cards = new BView("cards", 0);
    fCards = new BCardLayout();
    cards->SetLayout(fCards);
    fCards->AddView(Centered(fNote));
    fCards->AddView(fSource);
    fCards->AddView(fImage);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 10).SetInsets(8, 4, 8, 4)
            .Add(fFormat)
            .Add(fWrap)
            .Add(fInfo, 1)
            .Add(fCopy)
        .End()
        .Add(cards, 1);
    ShowNote("");
}

void BodyView::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    for (BControl* control : std::initializer_list<BControl*> {fFormat, fWrap, fCopy}) control->SetTarget(this);
}

void BodyView::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kFormatToggled: Update(); break;
        case kWrapToggled: fSource->SetWrap(fWrap->Value() == B_CONTROL_ON); break;
        case kCopyBody: CopyToClipboard(ShownText()); break;
        default: BView::MessageReceived(message);
    }
}

void BodyView::SetFormatted(bool formatted)
{
    fFormat->SetValue(formatted ? B_CONTROL_ON : B_CONTROL_OFF);
    Update();
}

void BodyView::SetWrapped(bool wrapped)
{
    fWrap->SetValue(wrapped ? B_CONTROL_ON : B_CONTROL_OFF);
    fSource->SetWrap(wrapped);
}

void BodyView::CopyShown() { CopyToClipboard(ShownText()); }

std::string BodyView::StateJSON()
{
    static const char* const cards[] = {"note", "text", "image"};
    auto text = ShownText();
    const auto size = text.size();
    if (text.size() > 256 * 1024) text.resize(256 * 1024);
    return nlohmann::json {{"info", fInfo->Text()}, {"note", fHasBody ? "" : fNote->Text()}, {"text", text}, {"textSize", size},
        {"formatted", fFormat->Value() == B_CONTROL_ON}, {"canFormat", fFormat->IsEnabled()}, {"canCopy", fCopy->IsEnabled()},
        {"wraps", fSource->Wraps()}, {"shows", cards[std::clamp<int32>(fCards->VisibleIndex(), 0, 2)]}}
        .dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
}

std::string BodyView::ShownText() { return fHasBody && fCards->VisibleIndex() == kCardSource ? fSource->Text() : std::string(); }

void BodyView::ShowBody(const std::string& bytes, const std::string& mimeType)
{
    if (fHasBody && bytes == fBytes && mimeType == fMimeType) return;
    fBytes = bytes;
    fMimeType = mimeType;
    fHasBody = true;
    Update();
}

void BodyView::ShowNote(const std::string& note)
{
    fHasBody = false;
    fBytes.clear();
    fNote->SetText(note.c_str());
    fInfo->SetText("");
    fCards->SetVisibleItem(static_cast<int32>(kCardNote));
    for (BControl* control : std::initializer_list<BControl*> {fFormat, fWrap, fCopy}) control->SetEnabled(false);
}

void BodyView::Update()
{
    if (!fHasBody) return;
    if (fBytes.empty()) {
        ShowNote("The body is empty.");
        return;
    }
    fKind = DetectBodyKind(fMimeType, fBytes);
    std::string info = BodyKindName(fKind);
    info += " · " + FormatBytes(fBytes.size());
    if (fKind == BodyKind::Image && fImage->SetImage(fBytes)) {
        const auto size = fImage->ImageSize();
        info += " · " + std::to_string(static_cast<int>(size.width)) + " × " + std::to_string(static_cast<int>(size.height));
        fInfo->SetText(info.c_str());
        fCards->SetVisibleItem(static_cast<int32>(kCardImage));
        for (BControl* control : std::initializer_list<BControl*> {fFormat, fWrap, fCopy}) control->SetEnabled(false);
        return;
    }
    const bool text = fKind != BodyKind::Image && fKind != BodyKind::Binary;
    const bool formats = text && CanFormat(fKind);
    if (!text) {
        fSource->SetSource(HexDump(fBytes), "null");
        if (fKind == BodyKind::Image) info += " · cannot be shown as a picture";
    } else {
        auto shown = ValidUTF8(fBytes);
        if (formats && fFormat->Value() == B_CONTROL_ON) {
            if (shown.size() > kLargestFormatted) info += " · too large to format";
            else {
                auto result = FormatBody(fKind, shown);
                info += result.formatted ? " · formatted" : " · " + result.note;
                shown = std::move(result.text);
            }
        }
        fSource->SetSource(shown, BodyKindLexer(fKind));
    }
    fInfo->SetText(info.c_str());
    fCards->SetVisibleItem(static_cast<int32>(kCardSource));
    fFormat->SetEnabled(formats);
    fWrap->SetEnabled(true);
    fCopy->SetEnabled(true);
}

class NetworkPanel::Row : public BRow {
public:
    explicit Row(uint64 serial)
        : serial(serial)
    {
        for (int i = 0; i < kColumns; ++i) SetField(fields[i] = new Field(), i);
    }
    const uint64 serial;
    Field* fields[kColumns];
};

NetworkPanel::NetworkPanel(Session& session, DevToolsHost& host)
    : BView("Network", 0), fSession(session), fHost(host)
{
    fClear = new BButton("clear", "Clear", new BMessage(kClearRequests));
    fClear->SetToolTip("Remove the requests listed");
    fPreserve = new BCheckBox("preserve", "Preserve log", new BMessage(kPreserveRequests));
    fPreserve->SetToolTip("Keep the requests of earlier pages when the page changes");
    fFilter = new BTextControl("filter", "Filter:", "", nullptr);
    fFilter->SetModificationMessage(new BMessage(kFilterRequests));
    fFilter->TextView()->SetExplicitMinSize(BSize(140, B_SIZE_UNSET));
    fFilter->SetToolTip("Show requests whose address contains this");
    auto* types = new BPopUpMenu("types");
    for (int32 i = 0; i < static_cast<int32>(std::size(kCategories)); ++i) {
        auto* message = new BMessage(kTypeChosen);
        message->AddInt32("category", i);
        auto* item = new BMenuItem(kCategories[i], message);
        item->SetMarked(!i);
        types->AddItem(item);
    }
    fTypes = new BMenuField("types", nullptr, types);
    fTypes->SetExplicitMaxSize(BSize(be_plain_font->StringWidth("Fetch and XHR") + 60, B_SIZE_UNSET));
    fRequestMenu = new BButton("request-menu", "Request ▾", new BMessage(kRequestMenu));
    fRequestMenu->SetToolTip("Copy the selected request as a curl command, send it again, or block its address");
    fDisableCache = new BCheckBox("disable-cache", "Disable cache", new BMessage(kDisableCache));
    fDisableCache->SetToolTip("Load everything from the network while the tools are open");
    fExportHar = new BButton("export-har", "Export HAR…", new BMessage(kExportHar));
    fExportHar->SetToolTip("Save the requests listed as an HTTP Archive (.har)");
    fList = new BColumnListView("requests", 0, B_NO_BORDER);
    fList->AddColumn(new Column("Name", 250, false, B_TRUNCATE_MIDDLE), kName);
    fList->AddColumn(new Column("Status", 80, false), kStatus);
    fList->AddColumn(new Column("Method", 70, false), kMethod);
    fList->AddColumn(new Column("Type", 90, false), kType);
    fList->AddColumn(new Column("Domain", 170, false), kDomain);
    fList->AddColumn(new Column("Size", 80, true), kSize);
    fList->AddColumn(new Column("Time", 80, true), kTime);
    fList->AddColumn(new Column("Initiator", 140, false, B_TRUNCATE_MIDDLE), kInitiator);
    fList->SetSelectionMode(B_SINGLE_SELECTION_LIST);
    fList->SetSelectionMessage(new BMessage(kRequestSelected));
    fList->SetSortingEnabled(true);
    fList->SetExplicitMinSize(BSize(200, 80));
    fSummary = new BStringView("summary", "");
    fSummary->SetTruncation(B_TRUNCATE_END);
    fSummary->SetExplicitMinSize(BSize(40, B_SIZE_UNSET));
    fSummary->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    fSummary->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);

    fHeaders = new SourceView("headers", false, false);
    fRequestBody = new BodyView("request");
    fResponseBody = new BodyView("response");
    fDetails = new BTabView("details", B_WIDTH_FROM_LABEL);
    fDetails->SetBorder(B_NO_BORDER);
    const auto tab = [this](BView* view, const char* label) {
        auto* item = new BTab();
        fDetails->AddTab(view, item);
        item->SetLabel(label);
    };
    auto* headers = new BGroupView("Headers", B_VERTICAL, 0);
    headers->AddChild(fHeaders);
    tab(headers, "Headers");
    tab(fRequestBody, "Request");
    tab(fResponseBody, "Response");
    fMessages = new SourceView("messages", false, false);
    auto* messages = new BGroupView("Messages", B_VERTICAL, 0);
    messages->AddChild(fMessages);
    tab(messages, "Messages");
    auto* placeholder = new BStringView("placeholder", "Select a request to see its headers and its response.");
    placeholder->SetAlignment(B_ALIGN_CENTER);
    placeholder->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    placeholder->SetExplicitMinSize(BSize(40, 60));
    placeholder->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    auto* detail = new BView("detail", 0);
    fDetailCards = new BCardLayout();
    detail->SetLayout(fDetailCards);
    fDetailCards->AddView(Centered(placeholder));
    fDetailCards->AddView(fDetails);
    fDetailCards->SetVisibleItem(static_cast<int32>(0));
    detail->SetExplicitMinSize(BSize(200, 120));

    auto* split = new BSplitView(B_VERTICAL, 2);
    split->SetCollapsible(false);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .AddGroup(B_HORIZONTAL, 8).SetInsets(8, 6, 8, 6)
            .Add(fClear)
            .Add(fPreserve)
            .AddStrut(8)
            .Add(fTypes)
            .Add(fFilter, 1)
            .Add(fDisableCache)
            .Add(fRequestMenu)
            .Add(fExportHar)
        .End()
        .AddSplit(split, 1)
            .AddGroup(B_VERTICAL, 0, 1)
                .Add(fList, 1)
                .AddGroup(B_HORIZONTAL, 0).SetInsets(8, 3, 8, 3)
                    .Add(fSummary)
                .End()
            .End()
            .Add(detail, 1)
        .End();
    ShowSummary();
}

void NetworkPanel::AttachedToWindow()
{
    BView::AttachedToWindow();
    AdoptParentColors();
    fClear->SetTarget(this);
    fPreserve->SetTarget(this);
    fFilter->SetTarget(this);
    fTypes->Menu()->SetTargetForItems(this);
    fList->SetTarget(this);
    fRequestMenu->SetTarget(this);
    fDisableCache->SetTarget(this);
    fExportHar->SetTarget(this);
}

void NetworkPanel::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kClearRequests:
            fSession.ClearRequests();
            fHost.Sync();
            break;
        case kPreserveRequests:
            fSession.SetPreserveRequests(fPreserve->Value() == B_CONTROL_ON);
            fHost.Sync();
            break;
        case kFilterRequests: {
            const auto text = LowerCase(fFilter->Text());
            if (text == fFilterText) break;
            fFilterText = text;
            Rebuild();
            break;
        }
        case kTypeChosen:
            fCategory = message->GetInt32("category", 0);
            Rebuild();
            break;
        case kCopyCurl:
            if (const auto* request = fSession.FindRequest(fSelected)) CopyToClipboard(CurlCommand(*request));
            break;
        case kRequestMenu: {
            const auto* request = fSession.FindRequest(fSelected);
            auto* menu = new BPopUpMenu("request", false, false);
            const auto add = [&](const std::string& label, uint32 what, bool enabled) {
                auto* item = new BMenuItem(label.c_str(), new BMessage(what));
                item->SetEnabled(enabled);
                menu->AddItem(item);
            };
            add("Copy as cURL", kCopyCurl, request);
            add("Replay", kReplay, request && !request->webSocket && request->url.rfind("http", 0) == 0);
            menu->AddSeparatorItem();
            // Blocked by its address without the query, so its variants are too.
            const auto address = request ? request->url.substr(0, request->url.find_first_of("?#")) : std::string();
            const auto scheme = address.find("://");
            auto shown = scheme == std::string::npos ? address : address.substr(scheme + 3);
            if (shown.size() > 60) shown = shown.substr(0, 59) + "…";
            add(shown.empty() ? std::string("Block This Address") : "Block " + shown, kBlockURL, !address.empty());
            const auto blocked = fSession.BlockedURLs().size();
            add(blocked ? "Unblock All (" + std::to_string(blocked) + ")" : std::string("Unblock All"), kUnblockAll, blocked);
            menu->SetTargetForItems(this);
            menu->SetAsyncAutoDestruct(true);
            const auto frame = fRequestMenu->ConvertToScreen(fRequestMenu->Bounds());
            menu->Go(frame.LeftBottom() + BPoint(0, 1), true, false, frame, true);
            break;
        }
        case kReplay:
            if (fSession.Replay(fSelected)) fHost.Sync();
            break;
        case kBlockURL:
            if (const auto* request = fSession.FindRequest(fSelected)) {
                fSession.BlockURL(request->url.substr(0, request->url.find_first_of("?#")));
                fHost.Sync();
                ShowSummary();
            }
            break;
        case kUnblockAll:
            fSession.UnblockAll();
            fHost.Sync();
            ShowSummary();
            break;
        case kDisableCache:
            fSession.SetCacheDisabled(fDisableCache->Value() == B_CONTROL_ON);
            fHost.Sync();
            break;
        case kExportHar: {
            if (!fHarPanel) {
                BMessenger target(this);
                fHarPanel = std::make_unique<BFilePanel>(B_SAVE_PANEL, &target, nullptr, 0, false, new BMessage(kHarChosen));
            }
            // Named after the page's host, as other browsers do.
            Request page;
            page.url = fSession.PageURL();
            std::string name = page.Host();
            if (name.empty()) name = "requests";
            fHarPanel->SetSaveText((name + ".har").c_str());
            fHarPanel->Window()->SetTitle("Summit: Export HAR");
            fHarPanel->Show();
            break;
        }
        case kHarChosen: {
            entry_ref directory;
            const char* name = nullptr;
            if (message->FindRef("directory", &directory) != B_OK || message->FindString("name", &name) != B_OK) break;
            BPath path(&directory);
            if (path.InitCheck() != B_OK || path.Append(name) != B_OK) break;
            std::string error;
            if (!ExportHar(path.Path(), error)) fSummary->SetText(error.c_str());
            break;
        }
        case kRequestSelected: Select(static_cast<Row*>(fList->CurrentSelection())); break;
        default: BView::MessageReceived(message);
    }
}

std::vector<const Request*> NetworkPanel::ShownRequests() const
{
    std::vector<const Request*> requests;
    for (int32 i = 0; i < fList->CountRows(); ++i)
        if (const auto* request = fSession.FindRequest(static_cast<Row*>(fList->RowAt(i))->serial)) requests.push_back(request);
    return requests;
}

bool NetworkPanel::ExportHar(const std::string& path, std::string& error) const
{
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << HarArchive(ShownRequests(), ApplicationVersion());
    file.close();
    if (!file) error = "The archive could not be written to " + path;
    return !!file;
}

void NetworkPanel::Select(Row* row)
{
    const uint64 selected = row ? row->serial : 0;
    if (selected == fSelected) return;
    fSelected = selected;
    ShowSelection();
    fHost.Sync();
}

bool NetworkPanel::Command(const std::string& action, const std::string& argument, std::string& error)
{
    const bool on = argument == "on";
    if (action == "clear-network") {
        fSession.ClearRequests();
        fHost.Sync();
    } else if (action == "preserve-network") {
        fPreserve->SetValue(on ? B_CONTROL_ON : B_CONTROL_OFF);
        fSession.SetPreserveRequests(on);
        fHost.Sync();
    } else if (action == "filter-network") {
        fFilter->SetText(argument.c_str());
        fFilterText = LowerCase(argument);
        Rebuild();
    } else if (action == "types") {
        const int32 category = std::atoi(argument.c_str());
        auto* item = fTypes->Menu()->ItemAt(category);
        if (!item) {
            error = "There is no such type";
            return true;
        }
        item->SetMarked(true);
        fCategory = category;
        Rebuild();
    } else if (action == "select") {
        // The last request shown whose address contains the argument.
        Row* chosen = nullptr;
        for (const auto& request : fSession.Requests()) {
            const auto found = fRows.find(request.serial);
            if (found != fRows.end() && request.url.find(argument) != std::string::npos) chosen = found->second;
        }
        fList->DeselectAll();
        if (chosen) {
            fList->AddToSelection(chosen);
            fList->ScrollTo(chosen);
        } else error = "No request shown has that in its address";
        Select(chosen);
    } else if (action == "detail") {
        const int32 tab = argument == "headers" ? 0 : argument == "request" ? 1 : argument == "response" ? 2
            : argument == "messages" ? 3 : -1;
        if (tab < 0) error = "The details are headers, request, response and messages";
        else fDetails->Select(tab);
    } else if (action == "format") fResponseBody->SetFormatted(on);
    else if (action == "wrap") fResponseBody->SetWrapped(on);
    else if (action == "copy") fResponseBody->CopyShown();
    else if (action == "disable-cache") {
        fDisableCache->SetValue(on ? B_CONTROL_ON : B_CONTROL_OFF);
        fSession.SetCacheDisabled(on);
        fHost.Sync();
    } else if (action == "block-url") {
        fSession.BlockURL(argument);
        fHost.Sync();
        ShowSummary();
    } else if (action == "unblock-all") {
        fSession.UnblockAll();
        fHost.Sync();
        ShowSummary();
    } else if (action == "replay") {
        if (!fSession.Replay(fSelected)) error = "No request that can be sent again is selected";
        fHost.Sync();
    } else if (action == "copy-curl") {
        if (const auto* request = fSession.FindRequest(fSelected)) CopyToClipboard(CurlCommand(*request));
        else error = "No request is selected";
    } else if (action == "export-har") ExportHar(argument, error);
    else return false;
    return true;
}

std::string NetworkPanel::StateJSON()
{
    using nlohmann::json;
    json shown = json::array();
    for (int32 i = 0; i < fList->CountRows(); ++i) shown.push_back(static_cast<Row*>(fList->RowAt(i))->serial);
    json state = {{"preserve", fPreserve->Value() == B_CONTROL_ON}, {"filter", fFilter->Text()}, {"types", fCategory},
        {"shown", shown}, {"selected", fSelected}, {"summary", fSummary->Text()}, {"detail", fDetails->Selection()},
        {"detailShown", fDetailCards->VisibleIndex() == 1}, {"headers", fHeaders->Text()}, {"messages", fMessages->Text()},
        {"curl", [&] { const auto* request = fSession.FindRequest(fSelected); return request ? CurlCommand(*request) : std::string(); }()},
        {"cacheDisabled", fSession.CacheDisabled()}, {"blocked", fSession.BlockedURLs()},
        {"request", json::parse(fRequestBody->StateJSON())}, {"response", json::parse(fResponseBody->StateJSON())}};
    return state.dump(-1, ' ', false, json::error_handler_t::replace);
}

bool NetworkPanel::Shows(const Request& request) const
{
    if (fCategory && CategoryOf(request.type) != fCategory) return false;
    return fFilterText.empty() || LowerCase(request.url).find(fFilterText) != std::string::npos;
}

bool NetworkPanel::AtEnd() const
{
    auto* bar = dynamic_cast<BScrollBar*>(fList->FindView("vertical_scroll_bar"));
    if (!bar) return false;
    float least = 0, most = 0;
    bar->GetRange(&least, &most);
    return bar->Value() >= most - 2;
}

void NetworkPanel::Fill(Row& row, const Request& request)
{
    const auto tone = request.IsError() ? Field::Failed : Field::Normal;
    const auto faint = tone == Field::Failed ? tone : Field::Faint;
    row.fields[kName]->Set(request.Name(), tone);
    row.fields[kStatus]->Set(request.StatusLabel(), request.Pending() || request.canceled || request.redirected ? faint : tone, request.status);
    row.fields[kMethod]->Set(request.method, tone);
    row.fields[kType]->Set(request.type, tone);
    row.fields[kDomain]->Set(request.Host(), tone);
    const bool cached = request.source == "memory-cache" || request.source == "disk-cache";
    if (request.hasResponse || request.size)
        row.fields[kSize]->Set(FormatBytes(request.size), cached ? faint : tone, static_cast<double>(request.size));
    else row.fields[kSize]->Set("—", faint, -1);
    if (request.Duration() >= 0) row.fields[kTime]->Set(FormatDuration(request.Duration()), tone, request.Duration());
    else row.fields[kTime]->Set("—", faint, -1);
    row.fields[kInitiator]->Set(request.initiator, faint);
}

void NetworkPanel::Rebuild()
{
    fList->Clear();
    fRows.clear();
    Row* selected = nullptr;
    for (const auto& request : fSession.Requests()) {
        if (!Shows(request)) continue;
        auto* row = new Row(request.serial);
        Fill(*row, request);
        fList->AddRow(row);
        fRows[request.serial] = row;
        if (request.serial == fSelected) selected = row;
    }
    if (selected) {
        fList->AddToSelection(selected);
        fList->ScrollTo(selected);
    } else if (fSelected) {
        fSelected = 0;
        ShowSelection();
    }
    ShowSummary();
}

void NetworkPanel::Apply(const Changes& changes)
{
    if (changes.requestsReset) Rebuild();
    else if (!changes.requests.empty()) {
        const bool follow = !fSelected && AtEnd();
        Row* last = nullptr;
        bool selectedChanged = false;
        for (const auto serial : changes.requests) {
            const auto* request = fSession.FindRequest(serial);
            if (!request) continue;
            if (serial == fSelected) selectedChanged = true;
            if (const auto found = fRows.find(serial); found != fRows.end()) {
                Fill(*found->second, *request);
                fList->UpdateRow(found->second);
            } else if (Shows(*request)) {
                auto* row = new Row(serial);
                Fill(*row, *request);
                fList->AddRow(row);
                fRows[serial] = last = row;
            }
        }
        if (follow && last) fList->ScrollTo(last);
        if (selectedChanged) ShowSelection();
        ShowSummary();
    }
    if (std::find(changes.bodies.begin(), changes.bodies.end(), fSelected) != changes.bodies.end()) {
        if (const auto* request = fSession.FindRequest(fSelected)) {
            ShowBody(*request);
            ShowFrames(*request);
        }
    }
}

void NetworkPanel::ShowSelection()
{
    const auto* request = fSelected ? fSession.FindRequest(fSelected) : nullptr;
    if (!request) {
        fDetailCards->SetVisibleItem(static_cast<int32>(0));
        return;
    }
    // Setting the same text again would lose the place it was read at.
    const auto described = DescribeHeaders(*request);
    if (described.text != fHeaders->Text()) fHeaders->SetStyled(described);
    if (request->hasRequestBody) fRequestBody->ShowBody(request->requestBody, HeaderValue(request->requestHeaders, "content-type"));
    else fRequestBody->ShowNote("This request has no body.");
    ShowBody(*request);
    ShowFrames(*request);
    fDetailCards->SetVisibleItem(static_cast<int32>(1));
}

void NetworkPanel::ShowFrames(const Request& request)
{
    if (!request.webSocket) {
        if (!fMessages->Text().empty()) fMessages->SetStyled(StyledText { });
        return;
    }
    // The latest message in view, as it arrives.
    const auto described = DescribeFrames(request);
    if (described.text == fMessages->Text()) return;
    fMessages->SetStyled(described);
    fMessages->SendMessage(SCI_SCROLLTOEND);
}

void NetworkPanel::ShowBody(const Request& request)
{
    if (request.webSocket) {
        fResponseBody->ShowNote("A WebSocket: its messages are in Messages.");
        return;
    }
    switch (request.bodyState) {
        case BodyState::Loaded: fResponseBody->ShowBody(request.body, request.mimeType); break;
        case BodyState::Waiting: fResponseBody->ShowNote("Loading…"); break;
        case BodyState::Failed: fResponseBody->ShowNote(request.bodyError); break;
        case BodyState::None:
            if (request.failed) fResponseBody->ShowNote(request.canceled ? "The request was canceled." : "The request failed.");
            else if (request.Pending()) fResponseBody->ShowNote("The response has not arrived yet.");
            else {
                // The host sends the question when the message in hand is done with.
                fSession.RequestBody(request.serial);
                fResponseBody->ShowNote("Loading…");
            }
            break;
    }
}

void NetworkPanel::ShowSummary()
{
    auto summary = SummarizeRequests(fSession.Requests(), fRows.size());
    if (const auto blocked = fSession.BlockedURLs().size())
        summary += " · " + std::to_string(blocked) + (blocked == 1 ? " address blocked" : " addresses blocked");
    fSummary->SetText(summary.c_str());
}
}
#endif
