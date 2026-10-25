// Team-targeted scripting helper for Summit benchmarks and stress tests.
//
// Unlike tests/BrowserProbe.cpp this never addresses Summit by signature alone:
// other engineers run their own Summit instances in the same VM, so every
// command requires the team id (== pid) of the instance the caller launched.
//
//   summitctl --team ID activate|escape     activate the browser window, or send Escape
//   summitctl --team ID state                 one JSON line with window/tab state
//   summitctl --team ID navigate URL
//   summitctl --team ID newtab URL
//   summitctl --team ID closetab [TABID]
//   summitctl --team ID selecttab TABID
//   summitctl --team ID back|forward|reload
//   summitctl --team ID sidebar               hides the bookmarks bar, for a full-height viewport
//                                             (the sidebar this once toggled is gone; the name is
//                                             kept for the benchmark scripts)
//   summitctl --team ID frame LEFT TOP RIGHT BOTTOM   standard BWindow "Frame" scripting property
//   summitctl --team ID framestats            frame counts since the targeted scroll burst began
//   summitctl --team ID quit                  B_QUIT_REQUESTED to that team only
//   summitctl --team ID windows               one JSON line per browser window: index, frame, tabs
//   summitctl --team ID newwindow [frame=L,T,R,B] URL...
//                                             a new browser window with one tab per URL
//   summitctl --team ID newprivatewindow [frame=L,T,R,B] URL...   the same, private
//   summitctl --team ID zoomin|zoomout|zoomreset   the window's current page (View menu)
//   summitctl --team ID devtools [network|console]  View > Developer Tools for the current tab
//   summitctl --team ID devtools-do ACTION [ARGUMENT]
//                                             drives the current tab's open Developer Tools and
//                                             prints what they show as JSON (docs/developer-tools.md;
//                                             needs SUMMIT_ENABLE_INPUT_SYNTHESIS=1): state,
//                                             panel, select, detail, format, wrap, copy,
//                                             clear-network, preserve-network, filter-network,
//                                             types, clear-console, preserve-console, levels,
//                                             filter-console, evaluate, inspect, expand
//   summitctl --team ID closewindow           closes the window (--window N) like its close box
//   summitctl --team ID search ENGINE          Preferences' search engine (duckduckgo, google, bing)
//   summitctl --team ID style haiku|safari     Preferences' appearance
//   summitctl --team ID useragent PRESET       Preferences' compatibility identity
//   summitctl --team ID clearhistory|clearcache|clearsitedata|preferences
//                                             Preferences' History and Data buttons (without
//                                             asking), or open the Preferences window
//   summitctl --team ID defaultbrowser        prints {"is_default", "current"}: the system's browser
//   summitctl --team ID makedefault           Preferences' Make Summit the Default Browser; prints the same
//   summitctl --team ID installext PACKAGE    adds an extension through the Extensions window
//   summitctl --team ID pin EXTENSION on|off  whether the extension's action has a toolbar button
//   summitctl --team ID privateext EXTENSION on|off
//                                             whether the extension works in private windows (needs
//                                             SUMMIT_ENABLE_INPUT_SYNTHESIS=1 in the browser)
//   --window N                                address the Nth browser window (default 0) instead
//                                             of the first; windows are counted in the
//                                             application's window list order
//   summitctl find NAME                       every team whose application image is called NAME
//                                             (Summit is single-launch: a second instance forwards
//                                             its URL to the first, whatever --profile says)
//   summitctl group TEAM                      teams in TEAM's process group with full image paths
//                                             (ps and team_info.args truncate long bundle paths)
//   summitctl sample INTERVAL_MS [TEAM...]    system CPU/memory + per-team CPU, threads and
//                                             (for the listed teams) area memory; needs no Summit
//
// Exit codes: 0 ok, 2 usage, 3 team is not a running Summit, 4 no reply in time
// (hang indicator), 5 send failed.
#include "ui/Messages.h"
#include <Entry.h>
#include <Application.h>
#include <Message.h>
#include <Messenger.h>
#include <Rect.h>
#include <OS.h>
#include <image.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

static std::string Escape(const char* text)
{
    std::string out;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(text ? text : ""); *p; ++p) {
        char buffer[8];
        switch (*p) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (*p < 0x20) { std::snprintf(buffer, sizeof buffer, "\\u%04x", *p); out += buffer; }
                else out += static_cast<char>(*p);
        }
    }
    return out;
}

static const char* String(const BMessage& message, const char* name)
{
    const char* value = "";
    message.FindString(name, &value);
    return value;
}

static void PrintFrameStatsFields(const BMessage& stats)
{
    int64 firstFrameDelay = 0;
    int64 longestInterframeGap = 0;
    const bool splitGapAvailable = stats.FindInt64("first_frame_delay_us", &firstFrameDelay) == B_OK
        && stats.FindInt64("longest_interframe_gap_us", &longestInterframeGap) == B_OK;
    std::printf("\"elapsedMicros\":%lld,\"frames\":%ld,\"longestGapMicros\":%lld,\"splitGapAvailable\":%s,\"firstFrameDelayMicros\":%lld,\"longestInterframeGapMicros\":%lld,\"longGaps\":%ld,\"pendingGapMicros\":%lld,\"queueMaxMicros\":%lld,\"queueOver33\":%ld",
        static_cast<long long>(stats.GetInt64("elapsed_us", 0)), long(stats.GetInt32("frames", 0)),
        static_cast<long long>(stats.GetInt64("longest_gap_us", 0)), splitGapAvailable ? "true" : "false",
        static_cast<long long>(firstFrameDelay), static_cast<long long>(longestInterframeGap),
        long(stats.GetInt32("long_gaps", 0)), static_cast<long long>(stats.GetInt64("pending_gap_us", 0)),
        static_cast<long long>(stats.GetInt64("queue_max_us", 0)), long(stats.GetInt32("queue_over_33", 0)));
}

static std::string AppImage(team_id team)
{
    int32 cookie = 0; image_info image;
    while (get_next_image_info(team, &cookie, &image) == B_OK)
        if (image.type == B_APP_IMAGE) return image.name;
    return {};
}

static int Find(int argc, char** argv)
{
    if (argc < 3) return 2;
    const std::string wanted = std::string("/") + argv[2];
    int32 cookie = 0; team_info info; bool comma = false;
    std::printf("[");
    while (get_next_team_info(&cookie, &info) == B_OK) {
        if (info.team <= 1) continue;
        const std::string image = AppImage(info.team);
        if (image.size() < wanted.size() || image.compare(image.size() - wanted.size(), wanted.size(), wanted)) continue;
        std::printf("%s{\"team\":%ld,\"group\":%ld,\"image\":\"%s\"}", comma ? "," : "", long(info.team),
            long(getpgid(info.team)), Escape(image.c_str()).c_str());
        comma = true;
    }
    std::puts("]");
    return 0;
}

static int Group(int argc, char** argv)
{
    if (argc < 3) return 2;
    const pid_t group = static_cast<pid_t>(std::strtol(argv[2], nullptr, 10));
    int32 cookie = 0; team_info info; bool comma = false;
    std::printf("[");
    while (get_next_team_info(&cookie, &info) == B_OK) {
        if (info.team <= 1 || getpgid(info.team) != group) continue;
        std::printf("%s{\"team\":%ld,\"image\":\"%s\",\"threads\":%ld}", comma ? "," : "", long(info.team),
            Escape(AppImage(info.team).c_str()).c_str(), long(info.thread_count));
        comma = true;
    }
    std::puts("]");
    return 0;
}

// One JSON line: whole-system CPU utilisation over the interval, memory totals,
// every team that used more than 2% of a core, and detailed rows for TEAM...
static int Sample(int argc, char** argv)
{
    if (argc < 3) return 2;
    const bigtime_t interval = bigtime_t(std::max(50L, std::strtol(argv[2], nullptr, 10))) * 1000;
    std::set<team_id> wanted;
    for (int i = 3; i < argc; ++i) wanted.insert(static_cast<team_id>(std::strtol(argv[i], nullptr, 10)));
    system_info system;
    get_system_info(&system);
    const uint32 cpus = system.cpu_count;
    std::vector<cpu_info> before(cpus), after(cpus);
    auto teamTimes = [] {
        std::map<team_id, bigtime_t> times;
        int32 cookie = 0; team_info info;
        while (get_next_team_info(&cookie, &info) == B_OK) {
            team_usage_info usage;
            if (get_team_usage_info(info.team, B_TEAM_USAGE_SELF, &usage) == B_OK)
                times[info.team] = usage.user_time + usage.kernel_time;
        }
        return times;
    };
    auto kernelTimes = [] {
        std::map<thread_id, thread_info> threads;
        int32 cookie = 0; thread_info info;
        while (get_next_thread_info(B_SYSTEM_TEAM, &cookie, &info) == B_OK) {
            // Kernel team totals include idle threads. Keep actual workers
            // separate so driver CPU work is observable without counting idle.
            if (std::strncmp(info.name, "idle thread ", 12) != 0)
                threads.emplace(info.thread, info);
        }
        return threads;
    };
    get_cpu_info(0, cpus, before.data());
    const auto first = teamTimes();
    const auto kernelFirst = kernelTimes();
    const bigtime_t start = system_time();
    snooze(interval);
    get_cpu_info(0, cpus, after.data());
    const auto second = teamTimes();
    const auto kernelSecond = kernelTimes();
    const double elapsed = double(system_time() - start);
    double busy = 0;
    for (uint32 i = 0; i < cpus; ++i) busy += double(after[i].active_time - before[i].active_time);
    get_system_info(&system);
    std::printf("{\"cpuCount\":%u,\"intervalMicros\":%.0f,\"busyCores\":%.3f,\"usedBytes\":%llu,"
        "\"cachedBytes\":%llu,\"maxBytes\":%llu,\"teamCount\":%u,\"threadCount\":%u,\"teams\":[",
        cpus, elapsed, busy / elapsed, (unsigned long long)system.used_pages * B_PAGE_SIZE,
        (unsigned long long)system.cached_pages * B_PAGE_SIZE, (unsigned long long)system.max_pages * B_PAGE_SIZE,
        system.used_teams, system.used_threads);
    bool comma = false;
    for (const auto& [id, time] : second) {
        const auto old = first.find(id);
        const double cores = old == first.end() ? 0 : double(time - old->second) / elapsed;
        const bool detailed = wanted.count(id) != 0;
        if (!detailed && cores < 0.02) continue;
        team_info info;
        if (get_team_info(id, &info) != B_OK) continue;
        std::printf("%s{\"team\":%ld,\"cores\":%.3f,\"threads\":%ld,\"areas\":%ld,\"cpuMicros\":%lld,\"args\":\"%s\"",
            comma ? "," : "", long(id), cores, long(info.thread_count), long(info.area_count),
            static_cast<long long>(time), Escape(info.args).c_str());
        comma = true;
        std::printf(",\"image\":\"%s\"", Escape(AppImage(id).c_str()).c_str());
        if (detailed) {
            ssize_t cookie = 0; area_info area; unsigned long long ram = 0, size = 0;
            while (get_next_area_info(id, &cookie, &area) == B_OK) { ram += area.ram_size; size += area.size; }
            std::printf(",\"ramBytes\":%llu,\"virtualBytes\":%llu", ram, size);
        }
        std::printf("}");
    }
    std::printf("],\"kernelWorkers\":[");
    comma = false;
    for (const auto& [id, info] : kernelSecond) {
        auto old = kernelFirst.find(id);
        if (old == kernelFirst.end()) continue;
        const bigtime_t delta = info.user_time + info.kernel_time
            - old->second.user_time - old->second.kernel_time;
        const double cores = double(delta) / elapsed;
        if (cores < 0.005) continue;
        std::printf("%s{\"thread\":%ld,\"name\":\"%s\",\"cores\":%.3f}", comma ? "," : "",
            long(id), Escape(info.name).c_str(), cores);
        comma = true;
    }
    std::puts("]}");
    return 0;
}

int main(int argc, char** argv)
{
    if (argc > 1 && !std::strcmp(argv[1], "sample")) return Sample(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "group")) return Group(argc, argv);
    if (argc > 1 && !std::strcmp(argv[1], "find")) return Find(argc, argv);
    team_id team = -1;
    bigtime_t timeout = 5000000;
    long windowIndex = 0;
    int index = 1;
    while (index + 1 < argc && argv[index][0] == '-') {
        char* end = nullptr;
        errno = 0;
        long parsed = std::strtol(argv[index + 1], &end, 10);
        const bool zeroAllowed = !std::strcmp(argv[index], "--window");
        if (errno || *end || parsed < (zeroAllowed ? 0 : 1) || parsed > INT_MAX) return 2;
        if (!std::strcmp(argv[index], "--team")) team = static_cast<team_id>(parsed);
        else if (!std::strcmp(argv[index], "--timeout-ms")) timeout = bigtime_t(parsed) * 1000;
        else if (!std::strcmp(argv[index], "--window")) windowIndex = parsed;
        else return 2;
        index += 2;
    }
    if (team < 0 || index >= argc) {
        std::fputs("usage: summitctl --team ID [--timeout-ms N] state|navigate URL|newtab URL|closetab [ID]|selecttab ID|back|forward|reload|scroll N MS DELTA [X Y]|framestats|permission allow|block|notnow [N]|quit\n", stderr);
        return 2;
    }
    const std::string command = argv[index++];
    const char* argument = index < argc ? argv[index] : nullptr;

    status_t status = B_NO_INIT;
    BApplication application("application/x-vnd.Kunanyi-Summit-benchctl", &status);
    if (status != B_OK) { std::fprintf(stderr, "BApplication: %s\n", std::strerror(status)); return 5; }
    BMessenger app("application/x-vnd.Kunanyi-Summit", team, &status);
    if (status != B_OK || !app.IsValid() || app.Team() != team) {
        std::fprintf(stderr, "team %ld is not a running Summit: %s\n", long(team), std::strerror(status));
        return 3;
    }
    if (command == "quit") {
        BMessage quit(B_QUIT_REQUESTED);
        return app.SendMessage(&quit, static_cast<BHandler*>(nullptr), timeout) == B_OK ? 0 : 5;
    }
    if (command == "defaultbrowser" || command == "makedefault") {
        BMessage message(command == "makedefault" ? summit::kMakeDefaultBrowser : summit::kDefaultBrowserState);
        BMessage reply;
        if (app.SendMessage(&message, &reply, timeout, timeout) != B_OK) return 5;
        const char* error = nullptr;
        reply.FindString("error", &error);
        std::printf("{\"is_default\": %s, \"current\": \"%s\", \"error\": \"%s\"}\n",
            reply.GetBool("is_default", false) ? "true" : "false", reply.GetString("current", ""), error ? error : "");
        return 0;
    }
    if (command == "installext") {
        // Adds an extension package through the Extensions window, as its
        // Add extension… button and Install do (approving what it asks).
        if (!argument) return 2;
        BEntry entry(argument, true);
        entry_ref ref;
        if (entry.GetRef(&ref) != B_OK || !entry.Exists()) { std::fprintf(stderr, "no such package: %s\n", argument); return 2; }
        BMessage show(summit::kShowExtensions);
        app.SendMessage(&show, static_cast<BHandler*>(nullptr), timeout);
        BMessenger manager;
        for (int attempt = 0; attempt < 50 && !manager.IsValid(); ++attempt) {
            snooze(100000);
            for (int32 i = 0; i < 64; ++i) {
                BMessage request(B_GET_PROPERTY), reply;
                request.AddSpecifier("Window", i);
                BMessenger candidate;
                if (app.SendMessage(&request, &reply, timeout, timeout) != B_OK || reply.FindMessenger("result", &candidate) != B_OK) break;
                BMessage title(B_GET_PROPERTY), answer;
                title.AddSpecifier("Title");
                const char* text = nullptr;
                if (candidate.SendMessage(&title, &answer, timeout, timeout) == B_OK && answer.FindString("result", &text) == B_OK
                    && text && !std::strncmp(text, "Extensions", 10)) { manager = candidate; break; }
            }
        }
        if (!manager.IsValid()) { std::fputs("no Extensions window\n", stderr); return 5; }
        snooze(1000000);
        BMessage selected(summit::kExtensionSelected);
        selected.AddRef("refs", &ref);
        if (manager.SendMessage(&selected, static_cast<BHandler*>(nullptr), timeout) != B_OK) return 5;
        // Preparation takes a moment; approving before it is ready is ignored.
        for (int attempt = 0; attempt < 4; ++attempt) {
            snooze(2500000);
            BMessage approve(summit::kExtensionApprove);
            manager.SendMessage(&approve, static_cast<BHandler*>(nullptr), timeout);
        }
        return 0;
    }
    if (command == "pin" || command == "privateext") {
        if (index + 1 >= argc) return 2;
        const bool on = !std::strcmp(argv[index + 1], "on");
        BMessage message(command == "pin" ? summit::kExtensionSetPinned : summit::kExtensionSetPrivate);
        message.AddString("extension_identifier", argument);
        message.AddBool(command == "pin" ? "pinned" : "allowed", on);
        return app.SendMessage(&message, static_cast<BHandler*>(nullptr), timeout) == B_OK ? 0 : 5;
    }
    if (command == "permission") {
        // permission allow|block|notnow [DEVICE_INDEX]: answers the open prompt.
        const std::string answer = argument ? argument : "";
        const int32 which = answer == "allow" ? 2 : answer == "block" ? 0 : answer == "notnow" ? 1 : -1;
        if (which < 0) return 2;
        BMessage message(summit::kAnswerPermissionPrompt);
        message.AddInt32("which", which);
        if (index + 1 < argc) message.AddInt32("device_index", std::atoi(argv[index + 1]));
        BMessage reply;
        if (app.SendMessage(&message, &reply, timeout, timeout) != B_OK) return 5;
        std::printf("{\"answered\":%s}\n", reply.GetBool("answered", false) ? "true" : "false");
        return reply.GetBool("answered", false) ? 0 : 4;
    }
    if (command == "style" || command == "useragent") {
        if (!argument) return 2;
        BMessage message(summit::kPreferencesChanged);
        message.AddString(command == "style" ? "interface_style" : "user_agent", argument);
        return app.SendMessage(&message, static_cast<BHandler*>(nullptr), timeout) == B_OK ? 0 : 5;
    }
    if (command == "search" || command == "clearhistory" || command == "clearcache" || command == "clearsitedata"
        || command == "preferences") {
        BMessage message(command == "search" ? summit::kPreferencesChanged : command == "clearhistory" ? summit::kClearHistoryRequest
            : command == "clearcache" ? summit::kClearCacheRequest : command == "clearsitedata" ? summit::kClearSiteDataRequest
            : summit::kShowPreferences);
        if (command == "search") { if (!argument) return 2; message.AddString("search_engine", argument); }
        return app.SendMessage(&message, static_cast<BHandler*>(nullptr), timeout) == B_OK ? 0 : 5;
    }
    if (command == "newwindow" || command == "newprivatewindow") {
        BMessage open(summit::kNewWindow);
        open.AddBool("private", command == "newprivatewindow");
        for (int i = index; i < argc; ++i) {
            float left, top, right, bottom;
            if (std::sscanf(argv[i], "frame=%f,%f,%f,%f", &left, &top, &right, &bottom) == 4)
                open.AddRect("frame", BRect(left, top, right, bottom));
            else
                open.AddString("url", argv[i]);
        }
        return app.SendMessage(&open, static_cast<BHandler*>(nullptr), timeout) == B_OK ? 0 : 5;
    }
    // The application's window list also holds windows that are not browser
    // windows (preferences, extension popups...); a browser window is one that
    // answers the state request.
    std::vector<std::pair<BMessenger, BMessage>> browsers;
    for (int32 i = 0; i < 64; ++i) {
        BMessage request(B_GET_PROPERTY), reply;
        request.AddSpecifier("Window", i);
        BMessenger candidate;
        status = app.SendMessage(&request, &reply, timeout, timeout);
        if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("application did not reply\n", stderr); return 4; }
        if (status != B_OK || reply.FindMessenger("result", &candidate) != B_OK || !candidate.IsValid()) break;
        BMessage ask(summit::kBrowserState), state;
        if (candidate.SendMessage(&ask, &state, timeout, timeout) == B_OK && state.HasInt32("count"))
            browsers.emplace_back(candidate, state);
        if (command != "windows" && long(browsers.size()) > windowIndex) break;
    }
    if (command == "windows") {
        std::printf("[");
        for (size_t i = 0; i < browsers.size(); ++i) {
            const BMessage& state = browsers[i].second;
            BRect frame = state.FindRect("frame");
            std::printf("%s{\"window\":%zu,\"tabs\":%ld,\"selected\":%lld,\"private\":%s,\"frame\":[%.0f,%.0f,%.0f,%.0f]}",
                i ? "," : "", i, long(state.GetInt32("count", 0)), static_cast<long long>(state.GetInt64("selected", -1)),
                state.GetBool("private", false) ? "true" : "false", frame.left, frame.top, frame.right, frame.bottom);
        }
        std::puts("]");
        return 0;
    }
    if (windowIndex < 0 || long(browsers.size()) <= windowIndex) {
        std::fprintf(stderr, "no browser window %ld (%zu found)\n", windowIndex, browsers.size());
        return 5;
    }
    BMessenger window = browsers[windowIndex].first;
    if (command == "activate") {
        BMessage request(B_SET_PROPERTY), reply;
        request.AddSpecifier("Active");
        request.AddBool("data", true);
        return browsers[windowIndex].first.SendMessage(&request, &reply, timeout, timeout) == B_OK ? 0 : 4;
    }
    if (command == "escape") {
        BMessage request(B_KEY_DOWN);
        request.AddString("bytes", "\033");
        request.AddInt32("raw_char", B_ESCAPE);
        request.AddInt32("key", 1);
        request.AddInt32("modifiers", 0);
        request.AddInt64("when", system_time());
        return browsers[windowIndex].first.SendMessage(&request) == B_OK ? 0 : 4;
    }
    if (command == "typeaddress" || command == "keyaddress") {
        // typeaddress TEXT [select]: types into the address field (select: over
        // its text); keyaddress down|up|enter|escape|backspace. Prints the
        // field and its suggestions after a short pause.
        if (!argument) return 2;
        BMessage type(summit::kTypeInAddress), reply;
        if (command == "typeaddress") {
            type.AddString("text", argument);
            if (index + 1 < argc && !std::strcmp(argv[index + 1], "select")) type.AddBool("select_all", true);
        } else type.AddString("key", argument);
        status = window.SendMessage(&type, &reply, timeout, timeout);
        if (status != B_OK) { std::fprintf(stderr, "typeaddress: %s\n", std::strerror(status)); return 5; }
        if (const char* error = nullptr; reply.FindString("error", &error) == B_OK) { std::fprintf(stderr, "%s\n", error); return 6; }
        snooze(400000);
        BMessage ask(summit::kBrowserState), state;
        if (window.SendMessage(&ask, &state, timeout, timeout) != B_OK) return 5;
        std::printf("{\"address\":\"%s\",\"selection\":[%ld,%ld],\"focused\":%s,\"typed\":\"%s\",\"autofill\":\"%s\","
            "\"showing\":%s,\"selected\":%ld,\"suggestions\":[",
            Escape(String(state, "address")).c_str(), long(state.GetInt32("address_selection_start", 0)),
            long(state.GetInt32("address_selection_end", 0)), state.GetBool("address_focused", false) ? "true" : "false",
            Escape(String(state, "address_typed")).c_str(), Escape(String(state, "autofill_url")).c_str(),
            state.GetBool("suggestions_showing", false) ? "true" : "false", long(state.GetInt32("suggestion_selected", -1)));
        const char* title = nullptr;
        const char* url = nullptr;
        for (int32 i = 0; state.FindString("suggestion_title", i, &title) == B_OK && state.FindString("suggestion_url", i, &url) == B_OK; ++i)
            std::printf("%s{\"title\":\"%s\",\"url\":\"%s\"}", i ? "," : "", Escape(title).c_str(), Escape(url).c_str());
        std::puts("]}");
        return 0;
    }
    if (command == "state") {
        BMessage ask(summit::kBrowserState), state;
        const bigtime_t before = system_time();
        status = window.SendMessage(&ask, &state, timeout, timeout);
        const bigtime_t elapsed = system_time() - before;
        if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("window did not reply\n", stderr); return 4; }
        if (status != B_OK) { std::fprintf(stderr, "state: %s\n", std::strerror(status)); return 5; }
        int32 count = -1; int64 selected = -1; bool closing = false, scrollActive = false;
        state.FindInt32("count", &count); state.FindInt64("selected", &selected); state.FindBool("closing", &closing);
        state.FindBool("scroll_active", &scrollActive);
        std::printf("{\"team\":%ld,\"now\":%lld,\"replyMicros\":%lld,\"count\":%ld,\"selected\":%lld,\"closing\":%s,"
            "\"scrollActive\":%s,\"scrollRequested\":%ld,\"scrollSent\":%ld,\"scrollStatus\":%ld,\"scrollDurationMicros\":%lld,"
            "\"address\":\"%s\",\"status\":\"%s\",\"backend\":\"%s\",\"webkit\":\"%s\",\"haikuWebkit\":\"%s\","
            "\"webkitRevision\":\"%s\",\"private\":%s,\"fullscreen\":%s,\"tabs\":[",
            long(team), static_cast<long long>(state.GetInt64("now", 0)), static_cast<long long>(elapsed), long(count), static_cast<long long>(selected),
            closing ? "true" : "false", scrollActive ? "true" : "false",
            long(state.GetInt32("scroll_requested", 0)), long(state.GetInt32("scroll_sent", 0)),
            long(state.GetInt32("scroll_status", 0)), static_cast<long long>(state.GetInt64("scroll_duration_us", 0)),
            Escape(String(state, "address")).c_str(), Escape(String(state, "status")).c_str(),
            Escape(String(state, "backend")).c_str(), Escape(String(state, "webkit")).c_str(),
            Escape(String(state, "haiku_webkit")).c_str(), Escape(String(state, "webkit_revision")).c_str(),
            state.GetBool("private", false) ? "true" : "false",
            state.GetBool("fullscreen", false) ? "true" : "false");
        BMessage tab;
        for (int32 i = 0; state.FindMessage("tab", i, &tab) == B_OK; ++i) {
            int64 id = -1; bool loading = false, loadError = false;
            tab.FindInt64("id", &id); tab.FindBool("loading", &loading); tab.FindBool("loadError", &loadError);
            std::printf("%s{\"id\":%lld,\"url\":\"%s\",\"title\":\"%s\",\"loading\":%s,\"loadError\":%s,"
                "\"loadErrorText\":\"%s\",\"loadOutcome\":\"%s\",\"loadStartedAt\":%lld,\"loadFinishedAt\":%lld,\"shownAt\":%lld,\"firstFrameAfterShow\":%lld,"
                "\"zoom\":%.2f,\"zoomKey\":\"%s\"}",
                i ? "," : "", static_cast<long long>(id), Escape(String(tab, "url")).c_str(),
                Escape(String(tab, "title")).c_str(), loading ? "true" : "false", loadError ? "true" : "false",
                Escape(String(tab, "loadErrorText")).c_str(), Escape(String(tab, "loadOutcome")).c_str(),
                static_cast<long long>(tab.GetInt64("loadStartedAt", 0)), static_cast<long long>(tab.GetInt64("loadFinishedAt", 0)),
                static_cast<long long>(tab.GetInt64("shownAt", 0)), static_cast<long long>(tab.GetInt64("firstFrameAfterShow", 0)),
                tab.GetDouble("pageZoom", 1), Escape(String(tab, "zoomKey")).c_str());
        }
        std::puts("]}");
        return 0;
    }
    if (command == "devtools") {
        BMessage show(summit::kShowDeveloperTools);
        if (argument) show.AddString("panel", argument);
        return window.SendMessage(&show, static_cast<BHandler*>(nullptr), timeout) == B_OK ? 0 : 5;
    }
    if (command == "devtools-do") {
        if (!argument) return 2;
        BMessage ask(summit::kBrowserState), state, tab;
        if (window.SendMessage(&ask, &state, timeout, timeout) != B_OK) return 4;
        BMessenger tools;
        for (int32 i = 0; state.FindMessage("tab", i, &tab) == B_OK; ++i)
            if (tab.GetInt64("id", -1) == state.GetInt64("selected", -2)) tab.FindMessenger("devtools", &tools);
        if (!tools.IsValid()) { std::fputs("the current tab's Developer Tools are not open\n", stderr); return 5; }
        BMessage request(summit::kDeveloperToolsCommand), reply;
        request.AddString("action", argument);
        if (index + 1 < argc) request.AddString("argument", argv[index + 1]);
        status = tools.SendMessage(&request, &reply, timeout, 4 * timeout);
        if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("the Developer Tools did not reply\n", stderr); return 4; }
        if (status != B_OK) return 5;
        if (const char* error = reply.GetString("error", nullptr)) std::fprintf(stderr, "%s\n", error);
        std::puts(reply.GetString("json", "{}"));
        return reply.HasString("error") ? 5 : 0;
    }
    if (command == "frame") {
        if (index + 3 >= argc) return 2;
        BMessage set(B_SET_PROPERTY), done;
        set.AddSpecifier("Frame");
        set.AddRect("data", BRect(std::atof(argv[index]), std::atof(argv[index + 1]),
            std::atof(argv[index + 2]), std::atof(argv[index + 3])));
        status = window.SendMessage(&set, &done, timeout, timeout);
        if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) return 4;
        return status == B_OK ? 0 : 5;
    }
    if (command == "scroll") {
        // scroll COUNT INTERVAL_MS DELTA [X Y] - one wheel notch every INTERVAL_MS.
        if (index + 2 >= argc) return 2;
        BMessage burst(summit::kSimulateScroll), done;
        burst.AddInt32("count", int32(std::strtol(argv[index], nullptr, 10)));
        burst.AddInt32("interval_ms", int32(std::strtol(argv[index + 1], nullptr, 10)));
        burst.AddFloat("delta", float(std::atof(argv[index + 2])));
        if (index + 4 < argc)
            burst.AddPoint("at", BPoint(std::atof(argv[index + 3]), std::atof(argv[index + 4])));
        status = window.SendMessage(&burst, &done, timeout, timeout);
        if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("window did not reply\n", stderr); return 4; }
        if (status != B_OK) { std::fprintf(stderr, "scroll: %s\n", std::strerror(status)); return 5; }
        const char* error = nullptr;
        if (done.FindString("error", &error) == B_OK && error) {
            std::fprintf(stderr, "scroll refused: %s\n", error);
            return 6;
        }
        std::printf("{\"count\":%ld,\"intervalMs\":%ld}\n",
            long(done.GetInt32("count", 0)), long(done.GetInt32("interval_ms", 0)));
        return 0;
    }
    if (command == "framestats") {
        BMessage ask(summit::kFrameStats), stats;
        status = window.SendMessage(&ask, &stats, timeout, timeout);
        if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("window did not reply\n", stderr); return 4; }
        if (status != B_OK) { std::fprintf(stderr, "framestats: %s\n", std::strerror(status)); return 5; }
        int64 elapsed = -1;
        if (stats.FindInt64("elapsed_us", &elapsed) != B_OK) { std::fputs("frame stats unavailable\n", stderr); return 6; }
        std::printf("{");
        PrintFrameStatsFields(stats);
        BMessage atCompletion;
        if (stats.FindMessage("at_scroll_completion", &atCompletion) == B_OK) {
            std::printf(",\"atScrollCompletion\":{");
            PrintFrameStatsFields(atCompletion);
            std::printf("}");
        }
        std::puts("}");
        return 0;
    }
    uint32 what = 0;
    bool wantsURL = false, wantsID = false;
    if (command == "navigate") { what = summit::kNavigate; wantsURL = true; }
    else if (command == "newtab") { what = summit::kNewTab; wantsURL = true; }
    else if (command == "closetab") { what = summit::kCloseTab; wantsID = argument != nullptr; }
    else if (command == "selecttab") { what = summit::kSelectTab; wantsID = true; }
    else if (command == "back") what = summit::kBack;
    else if (command == "forward") what = summit::kForward;
    else if (command == "reload") what = summit::kReload;
    else if (command == "sidebar" || command == "hide-bookmarks-bar") what = summit::kPreferencesChanged;
    else if (command == "closewindow") what = summit::kCloseWindow;
    else if (command == "zoomin") what = summit::kZoomIn;
    else if (command == "zoomout") what = summit::kZoomOut;
    else if (command == "zoomreset") what = summit::kZoomReset;
    // Edit › Copy and Paste on the current page, as the menu does (tests of
    // clipboard images need no keyboard).
    else if (command == "copy") what = B_COPY;
    else if (command == "paste") what = B_PASTE;
    else return 2;
    BMessage message(what);
    if (what == B_COPY || what == B_PASTE) message.AddBool("page", true);
    if (what == summit::kPreferencesChanged) message.AddBool("show_bookmarks_bar", false);
    if (wantsURL) { if (!argument) return 2; message.AddString("url", argument); }
    if (wantsID) { if (!argument) return 2; message.AddInt64("id", std::strtoll(argument, nullptr, 10)); }
    status = window.SendMessage(&message, static_cast<BHandler*>(nullptr), timeout);
    if (status == B_TIMED_OUT || status == B_WOULD_BLOCK) { std::fputs("window port is full\n", stderr); return 4; }
    return status == B_OK ? 0 : 5;
}
