#include "SafeBrowsingService.h"
#include "core/BuildTarget.h"
#include <WebKit/WebKitView.h>
#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <MessageRunner.h>
#include <StringView.h>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

using nlohmann::json;

namespace summit {
namespace {
using safebrowsing::Threat;

// How often the lists are brought up to date: Phishing Army's list says it
// expires in 4 hours, malware-filter's in 12. Unchanged lists cost a request
// each (If-None-Match / If-Modified-Since).
constexpr int64 kUpdateInterval = 4 * 60 * 60;
// After a failed update, the next try.
constexpr int64 kRetryInterval = 30 * 60;
// A list with fewer entries than this is not the real one (a captive portal's
// page, a truncated file): it does not replace the list Summit has.
constexpr size_t kMinimumEntries = 1000;
// The lists are a few MB; a response far larger is not one.
constexpr size_t kMaximumDownload = 96 * 1024 * 1024;

int64 Now() { return int64(std::time(nullptr)); }

const char* EngineCheck(const char* url, void*)
{
    return safebrowsing::ThreatName(SafeBrowsingService::Shared().Check(url ? url : ""));
}

struct Download {
    std::string body, etag, lastModified;
    long status = 0;
    bool tooLarge = false;
    const std::atomic<bool>* stopping = nullptr;
};

size_t WriteBody(char* data, size_t size, size_t count, void* context)
{
    auto& download = *static_cast<Download*>(context);
    const size_t bytes = size * count;
    if (download.body.size() + bytes > kMaximumDownload) {
        download.tooLarge = true;
        return 0;
    }
    try {
        download.body.append(data, bytes);
    } catch (...) {
        return 0;
    }
    return bytes;
}

size_t WriteHeader(char* data, size_t size, size_t count, void* context)
{
    auto& download = *static_cast<Download*>(context);
    std::string line(data, size * count);
    const auto colon = line.find(':');
    if (colon == std::string::npos) return size * count;
    std::string name = line.substr(0, colon);
    for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
    std::string value = line.substr(colon + 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) value.erase(0, 1);
    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) value.pop_back();
    if (name == "etag") download.etag = value;
    else if (name == "last-modified") download.lastModified = value;
    return size * count;
}

int Progress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t)
{
    const auto* stopping = static_cast<Download*>(context)->stopping;
    return stopping && stopping->load() ? 1 : 0;
}

// A GET of url, conditional on the copy Summit has; error says why there
// was no answer.
Download Fetch(const std::string& url, const std::string& etag, const std::string& lastModified, bool plainHTTP,
    const std::atomic<bool>& stopping, std::string& error)
{
    static std::once_flag initialized;
    std::call_once(initialized, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    Download download;
    download.stopping = &stopping;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) {
        error = "libcurl did not start";
        return download;
    }
    auto* handle = curl.get();
    const char* protocols = plainHTTP ? "http,https" : "https";
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "Summit (" SUMMIT_TARGET_NAME "; block list update)");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, protocols);
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, protocols);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 20L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_LIMIT, 256L);
    curl_easy_setopt(handle, CURLOPT_LOW_SPEED_TIME, 60L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
    // The lists compress about tenfold.
    curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteBody);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &download);
    curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, WriteHeader);
    curl_easy_setopt(handle, CURLOPT_HEADERDATA, &download);
    curl_easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, Progress);
    curl_easy_setopt(handle, CURLOPT_XFERINFODATA, &download);
    curl_easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
    std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> headers(nullptr, curl_slist_free_all);
    curl_slist* list = nullptr;
    if (!etag.empty()) list = curl_slist_append(list, ("If-None-Match: " + etag).c_str());
    if (!lastModified.empty()) list = curl_slist_append(list, ("If-Modified-Since: " + lastModified).c_str());
    headers.reset(list);
    if (list) curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers.get());
    const CURLcode result = curl_easy_perform(handle);
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &download.status);
    if (stopping.load()) error = "stopped";
    else if (download.tooLarge) error = "the list was larger than expected";
    else if (result != CURLE_OK) error = curl_easy_strerror(result);
    return download;
}

std::filesystem::path ListPath(const std::filesystem::path& directory, const char* name)
{
    return directory / (std::string(name) + ".bin");
}

// "152,158".
std::string Count(size_t value)
{
    std::string text = std::to_string(value);
    for (int i = int(text.size()) - 3; i > 0; i -= 3) text.insert(size_t(i), ",");
    return text;
}

std::string When(int64 when)
{
    static const char* const months[] = {"January", "February", "March", "April", "May", "June", "July", "August",
        "September", "October", "November", "December"};
    const time_t time = static_cast<time_t>(when);
    struct tm local {};
    if (!localtime_r(&time, &local)) return "";
    char text[80];
    std::snprintf(text, sizeof(text), "%d %s %d, %02d:%02d", local.tm_mday, months[local.tm_mon], local.tm_year + 1900,
        local.tm_hour, local.tm_min);
    return text;
}

constexpr uint32 kToggled = 'sbtg', kUpdate = 'sbup', kRefresh = 'sbrf';
}

SafeBrowsingService& SafeBrowsingService::Shared()
{
    // Never destroyed: the engine and the updater thread may ask until the
    // process ends.
    static auto* service = new SafeBrowsingService;
    return *service;
}

bool SafeBrowsingService::EngineChecks()
{
#ifdef B_WEBKIT_HAS_NAVIGATION_CHECK
    return true;
#else
    return false;
#endif
}

void SafeBrowsingService::Start(const std::filesystem::path& directory)
{
    std::unique_lock lock(fMutex);
    if (fStarted) return;
    fStarted = true;
    fDirectory = directory;
    std::error_code code;
    std::filesystem::create_directories(fDirectory, code);
    // SUMMIT_SAFE_BROWSING_PHISHING_FEED / _MALWARE_FEED: another list to
    // download (a test server's; http allowed).
    auto feed = [](Threat threat, const char* name, const char* variable, std::vector<std::string> urls) {
        Feed made { threat, name, std::move(urls) };
        if (const char* custom = std::getenv(variable); custom && *custom) {
            made.urls = { custom };
            made.custom = true;
        }
        return made;
    };
    fFeeds.push_back(feed(Threat::Phishing, "phishing", "SUMMIT_SAFE_BROWSING_PHISHING_FEED",
        { "https://phishing.army/download/phishing_army_blocklist.txt" }));
    fFeeds.push_back(feed(Threat::Malware, "malware", "SUMMIT_SAFE_BROWSING_MALWARE_FEED",
        { "https://malware-filter.gitlab.io/malware-filter/urlhaus-filter-online.txt",
          "https://curbengh.github.io/malware-filter/urlhaus-filter-online.txt" }));
    LoadSettings();
    // SUMMIT_SAFE_BROWSING_TEST_URLS: addresses (or hosts) to treat as
    // listed, separated by spaces or commas, each "malware:" or "phishing:"
    // (the default) first: the warning can be seen without visiting a
    // malicious site.
    if (const char* tests = std::getenv("SUMMIT_SAFE_BROWSING_TEST_URLS")) {
        std::string text(tests);
        for (char& c : text) if (c == ',') c = ' ';
        std::istringstream words(text);
        for (std::string word; words >> word;) {
            Threat threat = Threat::Phishing;
            for (const char* prefix : {"phishing:", "malware:"})
                if (word.starts_with(prefix)) {
                    threat = *safebrowsing::ParseThreat(std::string_view(prefix, std::strlen(prefix) - 1));
                    word.erase(0, std::strlen(prefix));
                }
            if (fDatabase.AddTestEntry(threat, word))
                std::fprintf(stderr, "Summit: safe browsing test entry (%s): %s\n", safebrowsing::ThreatName(threat), word.c_str());
        }
    }
    lock.unlock();
    // The lists are a few MB: read on the updater's thread, not at startup's.
    fThread = std::thread([this] { Run(); });
#ifdef B_WEBKIT_HAS_NAVIGATION_CHECK
    BWebKitSetNavigationCheck(EngineCheck, nullptr);
#else
    (void)EngineCheck;
#endif
}

void SafeBrowsingService::Stop()
{
#ifdef B_WEBKIT_HAS_NAVIGATION_CHECK
    BWebKitSetNavigationCheck(nullptr, nullptr);
#endif
    {
        std::lock_guard lock(fMutex);
        fStopping = true;
    }
    fWake.notify_all();
    if (fThread.joinable()) fThread.join();
}

safebrowsing::Threat SafeBrowsingService::Check(std::string_view url) const
{
    if (!fEnabled || fStopping) return Threat::None;
    return fDatabase.Check(url);
}

void SafeBrowsingService::AllowHost(std::string_view url)
{
    fDatabase.AllowHost(url);
}

void SafeBrowsingService::SetEnabled(bool enabled)
{
    if (fEnabled.exchange(enabled) == enabled) return;
    {
        std::lock_guard lock(fMutex);
        SaveSettings();
    }
    // Turned on: lists that are old get their update now.
    fWake.notify_all();
}

void SafeBrowsingService::UpdateNow()
{
    {
        std::lock_guard lock(fMutex);
        fUpdateRequested = true;
    }
    fWake.notify_all();
}

SafeBrowsingService::Status SafeBrowsingService::CurrentStatus() const
{
    Status status;
    status.enabled = fEnabled;
    status.updating = fUpdating;
    status.phishing = fDatabase.Count(Threat::Phishing);
    status.malware = fDatabase.Count(Threat::Malware);
    std::lock_guard lock(fMutex);
    status.lastUpdate = fLastUpdate;
    status.error = fLastError;
    return status;
}

void SafeBrowsingService::LoadSettings()
{
    std::ifstream in(fDirectory / "settings.json");
    if (!in) return;
    const json settings = json::parse(in, nullptr, false);
    if (!settings.is_object()) return;
    fEnabled = settings.value("enabled", true);
    fLastUpdate = settings.value("lastUpdate", int64(0));
    fLastError = settings.value("lastError", std::string());
    if (const auto feeds = settings.find("feeds"); feeds != settings.end() && feeds->is_object()) {
        for (auto& feed : fFeeds) {
            const auto saved = feeds->find(feed.name);
            // A list from another address (a test feed, or the default after
            // one) is downloaded afresh.
            if (saved == feeds->end() || !saved->is_object() || saved->value("url", std::string()) != feed.urls.front()) continue;
            feed.etag = saved->value("etag", std::string());
            feed.lastModified = saved->value("lastModified", std::string());
            feed.updated = saved->value("updated", int64(0));
        }
    }
}

void SafeBrowsingService::SaveSettings()
{
    json feeds = json::object();
    for (const auto& feed : fFeeds)
        feeds[feed.name] = { {"url", feed.urls.front()}, {"etag", feed.etag}, {"lastModified", feed.lastModified},
            {"updated", feed.updated} };
    const json settings = { {"enabled", fEnabled.load()}, {"lastUpdate", fLastUpdate}, {"lastError", fLastError}, {"feeds", feeds} };
    const auto path = fDirectory / "settings.json";
    {
        std::ofstream out(path.string() + ".new", std::ios::trunc);
        out << settings.dump(2) << "\n";
        if (!out) return;
    }
    std::error_code code;
    std::filesystem::rename(path.string() + ".new", path, code);
}

bool SafeBrowsingService::UpdateFeed(Feed& feed, std::string& error)
{
    const auto path = ListPath(fDirectory, feed.name);
    // Without the file, nothing to be conditional on.
    const bool have = std::filesystem::exists(path);
    std::string reasons;
    for (const auto& url : feed.urls) {
        std::string failure;
        const auto download = Fetch(url, have ? feed.etag : "", have ? feed.lastModified : "", feed.custom, fStopping, failure);
        if (fStopping) return false;
        if (failure.empty() && download.status == 304) {
            std::fprintf(stderr, "Summit: safe browsing %s list unchanged\n", feed.name);
            return true;
        }
        if (failure.empty() && download.status != 200) failure = "HTTP status " + std::to_string(download.status);
        safebrowsing::HashList list;
        if (failure.empty()) {
            list = safebrowsing::HashList(safebrowsing::ParseList(download.body));
            if (list.Size() < (feed.custom ? 1 : kMinimumEntries))
                failure = "only " + std::to_string(list.Size()) + " entries";
        }
        if (failure.empty()) list.Save(path, failure);
        if (!failure.empty()) {
            reasons += (reasons.empty() ? "" : "; ") + url + ": " + failure;
            continue;
        }
        std::fprintf(stderr, "Summit: safe browsing %s list updated from %s: %zu entries\n", feed.name, url.c_str(), list.Size());
        fDatabase.SetList(feed.threat, std::move(list));
        std::lock_guard lock(fMutex);
        // Only the first address's validators mean anything to it.
        feed.etag = url == feed.urls.front() ? download.etag : "";
        feed.lastModified = url == feed.urls.front() ? download.lastModified : "";
        feed.updated = Now();
        return true;
    }
    error = std::string("the ") + feed.name + " list could not be downloaded (" + reasons + ")";
    return false;
}

void SafeBrowsingService::Run()
{
    for (const auto& feed : fFeeds) {
        std::string error;
        if (auto list = safebrowsing::HashList::Load(ListPath(fDirectory, feed.name), error))
            fDatabase.SetList(feed.threat, std::move(*list));
    }
    const bool complete = fDatabase.Count(Threat::Phishing) && fDatabase.Count(Threat::Malware);
    std::unique_lock lock(fMutex);
    // At start, lists that are missing or old are brought up to date soon,
    // but not while the first windows load.
    int64 next = !complete ? Now() + 10 : std::max(fLastUpdate + kUpdateInterval, Now() + 60);
    while (!fStopping) {
        fWake.wait_for(lock, std::chrono::seconds(std::clamp<int64>(next - Now(), 1, 3600)), [&] {
            return fStopping || fUpdateRequested || (fEnabled && Now() >= next);
        });
        if (fStopping) break;
        if (!fUpdateRequested && (!fEnabled || Now() < next)) continue;
        fUpdateRequested = false;
        fUpdating = true;
        lock.unlock();
        std::string error;
        bool succeeded = true;
        for (auto& feed : fFeeds) {
            std::string failure;
            if (!UpdateFeed(feed, failure)) {
                succeeded = false;
                if (!failure.empty()) error += (error.empty() ? "" : " ") + failure;
            }
            if (fStopping) break;
        }
        lock.lock();
        fUpdating = false;
        if (fStopping) break;
        if (succeeded) fLastUpdate = Now();
        fLastError = error;
        if (!error.empty()) std::fprintf(stderr, "Summit: safe browsing: %s\n", error.c_str());
        next = Now() + (succeeded ? kUpdateInterval : kRetryInterval);
        SaveSettings();
    }
}

SecurityPreferences::SecurityPreferences()
    : BGroupView("Security", B_VERTICAL, 8)
{
    fEnabled = new BCheckBox("safe-browsing", "Warn about fraudulent and malicious websites", new BMessage(kToggled));
    fEnabled->SetValue(SafeBrowsingService::Shared().Enabled() ? B_CONTROL_ON : B_CONTROL_OFF);
    auto hint = [](const char* name, const char* text) {
        auto* view = new BStringView(name, text);
        view->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
        return view;
    };
    fStatus = new BStringView("safe-browsing-status", "");
    fCounts = hint("safe-browsing-counts", "");
    fUpdate = new BButton("safe-browsing-update", "Update Now", new BMessage(kUpdate));
    auto* title = new BStringView("security-title", "Fraudulent and malicious websites");
    title->SetFont(be_bold_font);
    BLayoutBuilder::Group<>(this)
        .SetInsets(18)
        .Add(title)
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fEnabled)
            .Add(hint("safe-browsing-hint", "Summit warns before opening a page on a list of phishing or malware sites."))
            .Add(hint("safe-browsing-privacy", "The lists are downloaded every few hours and checked on this computer."))
            .Add(hint("safe-browsing-privacy2", "The addresses you visit are not sent anywhere."))
            .AddStrut(4)
            .AddGroup(B_HORIZONTAL, 8)
                .Add(fStatus)
                .AddGlue()
                .Add(fUpdate)
            .End()
            .Add(fCounts)
            .Add(hint("safe-browsing-sources", "Lists: Phishing Army (phishing.army) and abuse.ch URLhaus (via malware-filter)."))
        .End()
        .AddGlue();
}

SecurityPreferences::~SecurityPreferences() = default;

void SecurityPreferences::AttachedToWindow()
{
    BGroupView::AttachedToWindow();
    fEnabled->SetTarget(this);
    fUpdate->SetTarget(this);
    ShowStatus();
    // The updater works by itself: show what it does while the tab is open.
    BMessage refresh(kRefresh);
    fRefresh = std::make_unique<BMessageRunner>(BMessenger(this), &refresh, 1000000);
}

void SecurityPreferences::DetachedFromWindow()
{
    fRefresh.reset();
    BGroupView::DetachedFromWindow();
}

void SecurityPreferences::MessageReceived(BMessage* message)
{
    switch (message->what) {
    case kToggled:
        SafeBrowsingService::Shared().SetEnabled(fEnabled->Value() == B_CONTROL_ON);
        ShowStatus();
        break;
    case kUpdate:
        SafeBrowsingService::Shared().UpdateNow();
        fUpdate->SetEnabled(false);
        fStatus->SetText("Updating the lists…");
        break;
    case kRefresh:
        ShowStatus();
        break;
    default:
        BGroupView::MessageReceived(message);
    }
}

void SecurityPreferences::ShowStatus()
{
    const auto status = SafeBrowsingService::Shared().CurrentStatus();
    std::string text;
    if (status.updating) text = "Updating the lists…";
    else if (!status.error.empty()) text = status.lastUpdate ? "Could not update the lists; last updated " + When(status.lastUpdate) + "."
        : "Could not download the lists.";
    else if (status.lastUpdate) text = "Lists updated " + When(status.lastUpdate) + ".";
    else text = "The lists have not been downloaded yet.";
    if (text != fStatus->Text()) fStatus->SetText(text.c_str());
    const std::string counts = status.error.empty() || status.updating
        ? Count(status.phishing) + " fraudulent sites and " + Count(status.malware) + " malware addresses."
        : status.error;
    if (counts != fCounts->Text()) fCounts->SetText(counts.c_str());
    fUpdate->SetEnabled(!status.updating);
}
}
