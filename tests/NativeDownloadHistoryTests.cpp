#include "ui/DownloadList.h"
#include "ui/DownloadListener.h"
#include "core/Profile.h"
#include "core/HistoryVisits.h"
#include <Application.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>
using namespace summit;
static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __LINE__ << ": " #x "\n"; } } while (0)
int main()
{
    status_t status;
    BApplication app("application/x-vnd.Kunanyi-Summit-download-history-tests", &status);
    CHECK(status == B_OK);
    const auto root = std::filesystem::temp_directory_path() / ("summit-native-download-history-" + std::to_string(getpid()));
    CHECK(std::filesystem::create_directory(root));
    DownloadList list;
    list.Open(root / "downloads.json");
    Profile profile;
    int visits = 0, titles = 0, retired = 0;
    auto receive = [&](const BMessage& message, bool privateBrowsing, bool active) {
        if (!active) { ++retired; return; }
        if (auto change = list.Update(message, privateBrowsing)) {
            if (change->newVisit) { ++visits; profile.VisitDownload({change->url, change->title}, change->started); }
            else { ++titles; profile.SetDownloadTitle(change->url, change->started, change->title); }
        }
    };
    DownloadListener normal(false, {}, receive), privateSession(true, {}, receive), newPrivateSession(true, {}, receive);
    BMessage start(B_WEBKIT_DOWNLOAD_STARTED);
    start.AddUInt64("identifier", 1);
    start.AddString("url", "https://downloads.test/original");
    start.AddString("request_url", "https://downloads.test/original");
    normal.MessageReceived(&start);
    CHECK(visits == 1 && titles == 0 && profile.history.size() == 1);
    const auto when = profile.history[0].visitTimes[0];
    CHECK(when > 0 && HistoryVisitCount(profile.history[0]) == 0);
    normal.MessageReceived(&start);
    CHECK(visits == 1 && profile.history[0].visitTimes.size() == 1);
    auto progress = start;
    progress.what = B_WEBKIT_DOWNLOAD_PROGRESS;
    progress.ReplaceString("url", "https://downloads.test/redirected?token=test");
    progress.AddString("filename", "archive.zip");
    normal.MessageReceived(&progress);
    CHECK(titles == 1 && visits == 1 && profile.history[0].title == "archive.zip");
    CHECK(profile.history[0].url == "https://downloads.test/original" && profile.history[0].visitTimes[0] == when);
    normal.MessageReceived(&progress);
    CHECK(titles == 1 && visits == 1);
    auto finished = progress;
    finished.what = B_WEBKIT_DOWNLOAD_FINISHED;
    finished.AddUInt32("result", B_WEBKIT_DOWNLOAD_PAUSED);
    normal.MessageReceived(&finished);
    CHECK(visits == 1 && list.Entries(false).size() == 1);
    const auto id = list.Entries(false)[0].identifier;
    list.Resumed(id, 2);
    start.ReplaceUInt64("identifier", 2);
    normal.MessageReceived(&start);
    CHECK(visits == 1 && list.Entries(false).size() == 1);
    finished.ReplaceUInt64("identifier", 2);
    finished.ReplaceUInt32("result", B_WEBKIT_DOWNLOAD_FAILED);
    normal.MessageReceived(&finished);
    normal.MessageReceived(&finished);
    CHECK(visits == 1 && list.Entries(false)[0].state == DownloadStatus::State::Failed);
    CHECK(profile.history[0].visitTransitions.size() == 1);
    // A freshly loaded entry has no session-only history state. Its resume
    // records one visit, matching Firefox's saver after a process restart.
    DownloadList reopened;
    reopened.Open(root / "downloads.json");
    CHECK(reopened.Entries(false).size() == 1);
    reopened.Resumed(reopened.Entries(false)[0].identifier, 3);
    start.ReplaceUInt64("identifier", 3);
    const auto again = reopened.Update(start, false);
    CHECK(again && again->newVisit && again->started >= when);
    CHECK(!reopened.Update(start, false));
    // Closing a private window cannot turn its context's queued start public.
    start.ReplaceUInt64("identifier", 4);
    privateSession.MessageReceived(&start);
    CHECK(list.Entries(true).size() == 1 && visits == 1);
    privateSession.Deactivate();
    list.ForgetPrivate();
    start.ReplaceUInt64("identifier", 5);
    privateSession.MessageReceived(&start);
    CHECK(retired == 1 && list.Entries(true).empty() && list.Entries(false).size() == 1);
    start.ReplaceUInt64("identifier", 6);
    newPrivateSession.MessageReceived(&start);
    CHECK(list.Entries(true).size() == 1 && visits == 1);
    privateSession.MessageReceived(&start);
    CHECK(retired == 2 && list.Entries(true).size() == 1);
    // A private context never depends on a message field or a current window.
    start.ReplaceUInt64("identifier", 7);
    start.AddBool("private", false);
    newPrivateSession.MessageReceived(&start);
    CHECK(list.Entries(true).size() == 2 && visits == 1);
    // History clearing during a transfer must survive late filename/finish.
    start.ReplaceUInt64("identifier", 8);
    start.ReplaceString("url", "https://downloads.test/cleared");
    start.ReplaceString("request_url", "https://downloads.test/cleared");
    normal.MessageReceived(&start);
    CHECK(visits == 2);
    profile.history.clear();
    start.what = B_WEBKIT_DOWNLOAD_FINISHED;
    start.AddString("filename", "late.zip");
    start.AddUInt32("result", B_WEBKIT_DOWNLOAD_SUCCEEDED);
    normal.MessageReceived(&start);
    CHECK(titles == 2 && profile.history.empty());
    list.ForgetPrivate();
    DownloadList saved;
    saved.Open(root / "downloads.json");
    CHECK(saved.Entries(true).empty() && saved.Entries(false).size() == 2);
    std::filesystem::remove_all(root);
    std::cout << checks << " native download history checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
