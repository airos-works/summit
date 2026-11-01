#if SUMMIT_MODERN_WEBKIT
// Reading List (#156): pages kept to read later, as in Safari, with unread
// and read items and an offline copy of each. The copy is the page's web
// archive (MHTML, as File › Save Page As… writes it) in the profile's
// ReadingList folder, saved when a page is added from its tab, or, for a
// link added from its menu, the first time it is opened from the list. A
// page that cannot be loaded (no network, the server down) shows its copy
// instead of the error page, with a notice saying so.
#include "BrowserWindow.h"
#include "Chrome.h"
#include "FaviconCache.h"
#include "FeatureMessages.h"
#include "Messages.h"
#include "NoticeBar.h"
#include "SharedProfile.h"
#include "core/Address.h"
#include "core/InternalPages.h"
#include "core/ReadingList.h"
#include <File.h>
#include <Menu.h>
#include <MenuItem.h>
#include <NodeInfo.h>
#include <OS.h>
#include <StringView.h>
#include <WebKit/WebKitView.h>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <optional>
#include <strings.h>

namespace summit {
namespace {
// What the Reading List page's links must carry, as the Bookmarks page's do:
// random, made once per run and shared by every window.
const std::string& ReadingListSecret()
{
    static std::once_flag once;
    static std::string secret;
    std::call_once(once, [] {
        uint8 bytes[16];
        std::ifstream random("/dev/urandom", std::ios::binary);
        if (!random.read(reinterpret_cast<char*>(bytes), sizeof bytes))
            for (auto& byte : bytes) byte = uint8(std::rand() ^ system_time());
        for (uint8 byte : bytes) {
            secret += "0123456789abcdef"[byte >> 4];
            secret += "0123456789abcdef"[byte & 15];
        }
    });
    return secret;
}

std::string ShortTitle(const std::string& title, size_t length = 70)
{
    if (title.size() <= length) return title;
    size_t cut = length;
    while (cut && (static_cast<unsigned char>(title[cut]) & 0xC0) == 0x80) --cut;
    return title.substr(0, cut) + "…";
}

std::string DayText(int64_t seconds)
{
    static const char* months[] = { "January", "February", "March", "April", "May", "June", "July",
        "August", "September", "October", "November", "December" };
    std::time_t time = static_cast<std::time_t>(seconds);
    std::tm local {};
    localtime_r(&time, &local);
    return std::to_string(local.tm_mday) + " " + months[local.tm_mon % 12] + " " + std::to_string(local.tm_year + 1900);
}

// Request identifiers of offline copies, apart from Save Page As's (bit 61).
constexpr uint64 kArchiveRequestBit = uint64(1) << 60;
}

std::filesystem::path BrowserWindow::ReadingListDirectory() const
{
    return fShared->Path().parent_path() / "ReadingList";
}

std::string BrowserWindow::ReadingListCopyItem(const std::string& url) const
{
    // file:///…/ReadingList/<id>.mhtml, or the file:/… form Haiku's URL
    // notification can use before WebKit commits the canonical one.
    const std::string folder = FileURL(ReadingListDirectory().string()) + "/";
    std::string_view rest;
    if (url.rfind(folder, 0) == 0) rest = std::string_view(url).substr(folder.size());
    else if (folder.size() > 7 && url.rfind("file:" + folder.substr(7), 0) == 0) rest = std::string_view(url).substr(folder.size() - 2);
    else return {};
    constexpr std::string_view suffix = ".mhtml";
    if (rest.size() <= suffix.size() || rest.substr(rest.size() - suffix.size()) != suffix) return {};
    const auto id = rest.substr(0, rest.size() - suffix.size());
    return IsReadingListID(id) ? std::string(id) : std::string();
}

void BrowserWindow::ReadingListCommand(const BMessage& message)
{
    const std::string id = message.GetString("id", "");
    switch (message.what) {
        case kAddToReadingList: {
            const char* url = nullptr;
            if (message.FindString("url", &url) == B_OK && url) AddToReadingList(url, message.GetString("title", ""), nullptr);
            else if (auto* tab = ActiveTab()) AddToReadingList(tab->url, tab->title, tab);
            break;
        }
        case kOpenReadingListItem: OpenReadingListItem(id, message.GetBool("offline", false)); break;
        case kMarkReadingListItem: {
            const bool read = message.GetBool("read", true);
            fShared->Change([&](Profile& profile) -> uint32 {
                auto* item = FindReadingListItem(profile.readingList, id);
                if (!item || (item->read != 0) == read) return 0;
                item->read = read ? std::time(nullptr) : 0;
                return SharedProfile::kReadingListChanged;
            }, BMessenger(this));
            break;
        }
        case kRemoveReadingListItem: {
            fShared->Change([&](Profile& profile) -> uint32 {
                auto& list = profile.readingList;
                const auto size = list.size();
                std::erase_if(list, [&](const ReadingListItem& item) { return item.id == id; });
                return list.size() != size ? SharedProfile::kReadingListChanged : 0;
            }, BMessenger(this));
            if (IsReadingListID(id)) std::remove((ReadingListDirectory() / ReadingListArchiveName(id)).c_str());
            fStatus->SetText("Removed from the Reading List");
            break;
        }
        case kShowReadingList: ShowInternalPage(kReadingListPage); break;
        case kLoadLivePage: {
            auto* tab = FindTabByID(message.GetInt64("tab", 0));
            const auto item = tab ? ReadingListCopyItem(tab->url) : std::string();
            const auto url = fShared->Read([&](const Profile& profile) {
                for (const auto& entry : profile.readingList) if (entry.id == item) return entry.url;
                return std::string();
            });
            if (url.empty() || !PrepareTabNavigation(*tab)) break;
            tab->view->LoadURL(url.c_str());
            break;
        }
    }
}

void BrowserWindow::AddToReadingList(const std::string& url, const std::string& title, Tab* archiveFrom)
{
    // A private window's pages are not kept anywhere.
    if (fPrivate) return;
    if (!IsReadingListURL(url)) {
        fStatus->SetText("The Reading List keeps web pages (http and https addresses).");
        return;
    }
    std::string id;
    std::vector<std::string> dropped;
    fShared->Change([&](Profile& profile) -> uint32 {
        id = summit::AddToReadingList(profile.readingList, url, title, std::time(nullptr), &dropped);
        return id.empty() ? 0 : SharedProfile::kReadingListChanged;
    }, BMessenger(this));
    for (const auto& gone : dropped)
        if (IsReadingListID(gone)) std::remove((ReadingListDirectory() / ReadingListArchiveName(gone)).c_str());
    if (id.empty()) return;
    fStatus->SetText("Added to the Reading List");
    if (!archiveFrom) return;
    // The page as it is now (a newer copy for an item added again); one still
    // loading is saved once it has loaded.
    if (archiveFrom->loading || archiveFrom->processExited || archiveFrom->errorView)
        fReadingListOpened[archiveFrom->id] = { id, archiveFrom->loadGeneration };
    else ArchiveForReadingList(*archiveFrom, id);
}

void BrowserWindow::OpenReadingListItem(const std::string& id, bool offline)
{
    auto* tab = ActiveTab();
    std::optional<ReadingListItem> found;
    fShared->Change([&](Profile& profile) -> uint32 {
        auto* item = FindReadingListItem(profile.readingList, id);
        if (!item) return 0;
        found = *item;
        if (item->read) return 0;
        // Opening it is reading it, as in Safari.
        item->read = std::time(nullptr);
        return SharedProfile::kReadingListChanged;
    }, BMessenger(this));
    if (!found || !tab) return;
    const auto copy = ReadingListDirectory() / ReadingListArchiveName(found->id);
    std::error_code error;
    const bool haveCopy = found->archived && std::filesystem::is_regular_file(copy, error);
    if (offline && !haveCopy) {
        ShowError("There is no offline copy of this page.");
        return;
    }
    const int64 tabID = tab->id;
    const uint64 generation = tab->loadGeneration;
    Navigate(haveCopy && (offline || !NetworkAvailable()) ? FileURL(copy.string()) : found->url);
    // Its first load after this one is the item's page (a redirect may give
    // it another address). Until it starts, the Reading List page it leaves
    // is not written again for the item now read (ReadingListPageLeaving).
    if (auto* opened = ActiveTab(); opened && opened->id == tabID) fReadingListOpened[tabID] = { found->id, generation + 1 };
}

bool BrowserWindow::ReadingListPageLeaving(const Tab& tab) const
{
    const auto opened = fReadingListOpened.find(tab.id);
    return opened != fReadingListOpened.end() && tab.loadGeneration < opened->second.second;
}

void BrowserWindow::ArchiveForReadingList(Tab& tab, const std::string& id)
{
    const uint64 request = ++fPrintRequest | kArchiveRequestBit;
    fReadingListArchives[request] = id;
    tab.view->GetPageContents(B_WEBKIT_PAGE_ARCHIVE, BMessenger(this), request);
}

bool BrowserWindow::ReadingListArchiveReceived(const BMessage& message)
{
    auto request = fReadingListArchives.find(message.GetUInt64("identifier", 0));
    if (request == fReadingListArchives.end()) return false;
    const std::string id = request->second;
    fReadingListArchives.erase(request);
    const void* data = nullptr;
    ssize_t size = 0;
    // Only a web page's copy: the tab may have gone on to something else.
    if (message.HasString("error") || message.FindData("data", B_RAW_TYPE, &data, &size) != B_OK || size <= 0
        || !IsReadingListURL(message.GetString("url", "")) || !IsReadingListID(id)) {
        fStatus->SetText("The page could not be saved for reading offline.");
        return true;
    }
    const auto folder = ReadingListDirectory();
    const auto path = folder / ReadingListArchiveName(id);
    const auto temporary = path.string() + ".tmp";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    {
        BFile file(temporary.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
        if (file.InitCheck() != B_OK || file.Write(data, size) != size) {
            std::remove(temporary.c_str());
            fStatus->SetText("The page could not be saved for reading offline.");
            return true;
        }
        BNodeInfo(&file).SetType("multipart/related");
    }
    if (std::rename(temporary.c_str(), path.c_str()) != 0) {
        std::remove(temporary.c_str());
        return true;
    }
    bool kept = false;
    fShared->Change([&](Profile& profile) -> uint32 {
        auto* item = FindReadingListItem(profile.readingList, id);
        if (!item) return 0;
        kept = true;
        item->archived = std::time(nullptr);
        return SharedProfile::kReadingListChanged;
    }, BMessenger(this));
    // Removed while it was being saved.
    if (!kept) std::remove(path.c_str());
    else fStatus->SetText("Saved in the Reading List for reading offline");
    return true;
}

void BrowserWindow::ReadingListTabUpdated(Tab& tab)
{
    // The outcome of the item's load comes after loading has ended: wait
    // for it ("loading" or "idle" until then).
    if (auto opened = fReadingListOpened.find(tab.id); opened != fReadingListOpened.end()
        && tab.loadGeneration >= opened->second.second && !tab.loading && tab.loadOutcome != "loading"
        && tab.loadOutcome != "idle") {
        const std::string id = opened->second.first;
        fReadingListOpened.erase(opened);
        const bool needed = tab.loadOutcome == "succeeded" && !tab.errorView && IsReadingListURL(tab.url)
            && fShared->Read([&](const Profile& profile) {
                for (const auto& item : profile.readingList) if (item.id == id) return item.archived == 0;
                return false;
            });
        if (needed) ArchiveForReadingList(tab, id);
    }
    // The offline copy's notice comes and goes with it.
    const auto copy = ReadingListCopyItem(tab.url);
    auto& shown = fReadingListCopyTabs[tab.id];
    if (shown != copy) {
        shown = copy;
        if (tab.id == fSelected) RefreshNoticeBar();
    }
    if (shown.empty()) fReadingListCopyTabs.erase(tab.id);
}

bool BrowserWindow::ShowReadingListCopy(Tab& tab)
{
    std::string id;
    if (auto opened = fReadingListOpened.find(tab.id); opened != fReadingListOpened.end()) {
        if (tab.loadGeneration >= opened->second.second) id = opened->second.first;
        fReadingListOpened.erase(opened);
    }
    const auto copy = fShared->Read([&](const Profile& profile) -> std::string {
        const ReadingListItem* item = nullptr;
        for (const auto& entry : profile.readingList) if (entry.id == id) item = &entry;
        if (!item) item = FindReadingListURL(profile.readingList, tab.errorURL);
        return item && item->archived ? item->id : std::string();
    });
    if (copy.empty()) return false;
    const auto path = ReadingListDirectory() / ReadingListArchiveName(copy);
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || !PrepareTabNavigation(tab)) return false;
    tab.view->LoadURL(FileURL(path.string()).c_str());
    if (tab.id == fSelected) fStatus->SetText("The page could not be loaded: showing its offline copy from the Reading List");
    return true;
}

bool BrowserWindow::ReadingListNotice(Tab& tab)
{
    const auto id = ReadingListCopyItem(tab.url);
    if (id.empty()) return false;
    const auto item = fShared->Read([&](const Profile& profile) {
        for (const auto& entry : profile.readingList) if (entry.id == id) return std::optional<ReadingListItem>(entry);
        return std::optional<ReadingListItem>();
    });
    if (!item) return false;
    const std::string title = item->title.empty() ? item->url : item->title;
    std::string text = "This is the offline copy of “" + ShortTitle(title, 60) + "” from your Reading List";
    if (item->archived) text += ", saved on " + DayText(item->archived);
    text += ".";
    auto* live = new BMessage(kLoadLivePage);
    live->AddInt64("tab", tab.id);
    fNoticeBar->SetNotice(text, {{"Load Live Page", live}, {"Show Reading List", new BMessage(kShowReadingList)}});
    return true;
}

void BrowserWindow::RefreshReadingListMenu()
{
    auto* tab = ActiveTab();
    if (auto* item = fBookmarksMenu->FindItem(kAddToReadingList)) item->SetEnabled(!fPrivate && tab && IsReadingListURL(tab->url));
    if (!fReadingListMenu) return;
    while (fReadingListMenu->CountItems() > fReadingListMenuFixed) delete fReadingListMenu->RemoveItem(fReadingListMenuFixed);
    // The unread items, newest first, as Safari's sidebar shows them.
    std::vector<ReadingListItem> unread;
    size_t count = 0;
    fShared->Read([&](const Profile& profile) {
        for (const auto& item : profile.readingList) {
            if (item.read) continue;
            if (unread.size() < 30) unread.push_back(item);
            ++count;
        }
        return 0;
    });
    fReadingListMenu->AddSeparatorItem();
    if (unread.empty()) {
        auto* none = new BMenuItem("No Unread Pages", nullptr);
        none->SetEnabled(false);
        fReadingListMenu->AddItem(none);
    }
    for (const auto& item : unread) {
        auto* open = new BMessage(kOpenReadingListItem);
        open->AddString("id", item.id.c_str());
        auto* entry = NewPageMenuItem(ShortTitle(item.title.empty() ? item.url : item.title).c_str(), open, fFavicons->Icon(item.url));
        entry->SetTarget(this);
        fReadingListMenu->AddItem(entry);
    }
    if (count > unread.size()) {
        auto* more = new BMenuItem(("All " + std::to_string(count) + " Unread Pages…").c_str(), new BMessage(kShowReadingList));
        more->SetTarget(this);
        fReadingListMenu->AddItem(more);
    }
}

bool BrowserWindow::ReadingListPageRequest(Tab& tab, const std::string& source, const std::string& url)
{
    const std::string scheme = std::string(kReadingListPageScheme) + ":";
    if (url.size() < scheme.size() || strncasecmp(url.c_str(), scheme.c_str(), scheme.size()) != 0) return false;
    // Only the Reading List page's own links, which carry this run's secret.
    const auto link = ParseReadingListPageLink(url);
    if (!link || link->token != ReadingListSecret() || tab.url != kReadingListPage
        || StoredURL(BString(source.c_str())) != kReadingListPage)
        return true;
    BMessage command;
    command.AddString("id", link->id.c_str());
    if (link->action == "open" || link->action == "offline") {
        command.what = kOpenReadingListItem;
        command.AddBool("offline", link->action == "offline");
    } else if (link->action == "read" || link->action == "unread") {
        command.what = kMarkReadingListItem;
        command.AddBool("read", link->action == "read");
    } else if (link->action == "remove") command.what = kRemoveReadingListItem;
    else if (link->action == "clear-read") {
        std::vector<std::string> gone;
        fShared->Change([&](Profile& profile) -> uint32 {
            auto& list = profile.readingList;
            for (const auto& item : list) if (item.read) gone.push_back(item.id);
            std::erase_if(list, [](const ReadingListItem& item) { return item.read != 0; });
            return gone.empty() ? 0 : SharedProfile::kReadingListChanged;
        }, BMessenger(this));
        for (const auto& id : gone) std::remove((ReadingListDirectory() / ReadingListArchiveName(id)).c_str());
        return true;
    }
    if (command.what) ReadingListCommand(command);
    return true;
}

std::string BrowserWindow::ReadingListPageHTML()
{
    const auto list = fShared->Read([](const Profile& profile) { return profile.readingList; });
    return RenderReadingListPage(list, std::time(nullptr), [this](const std::string& page) { return fFavicons->DataURL(page); },
        ReadingListSecret());
}
}
#endif
