// Crash reports of web processes (CrashReporter.h).
#include "CrashReporter.h"
#include "Messages.h"
#include <Application.h>
#include <FindDirectory.h>
#include <Message.h>
#include <Node.h>
#include <OS.h>
#include <Path.h>
#include <WebKit/WebKitDiagnostics.h>
#include <WebKit/WebKitInfo.h>
#include <nlohmann/json.hpp>
#include <sys/utsname.h>
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <unistd.h>

#ifdef B_WEBKIT_HAS_CRASH_REPORTS
namespace {
using nlohmann::json;

// Reports kept in the folder; older ones are removed.
constexpr size_t kKeptReports = 30;
// How deep a stack is read.
constexpr int kStackDepth = 64;

std::string ImagePath(bool application, const char* fileName = nullptr)
{
    int32 cookie = 0;
    image_info image;
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &image) == B_OK) {
        if (application && image.type == B_APP_IMAGE) return image.name;
        const char* slash = std::strrchr(image.name, '/');
        if (fileName && !std::strcmp(slash ? slash + 1 : image.name, fileName)) return image.name;
    }
    return { };
}

// The package a file came from ("summit-0.1.0~git20261110.2123-1"), or
// where it is when it is not from one (a development build).
std::string PackageOf(const std::string& path)
{
    if (path.empty()) return "?";
    std::error_code error;
    const auto real = std::filesystem::canonical(path, error);
    BNode node((error ? path : real.string()).c_str());
    char value[B_PATH_NAME_LENGTH] = "";
    const ssize_t length = node.InitCheck() == B_OK ? node.ReadAttr("SYS:PACKAGE", B_STRING_TYPE, 0, value, sizeof(value) - 1) : -1;
    return length > 0 ? std::string(value, strnlen(value, size_t(length))) : "not from a package: " + path;
}

// Thread names as text: some libraries name theirs with binary bytes.
std::string Printable(std::string text)
{
    for (char& character : text)
        if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) character = '?';
    return text;
}

std::string SignalText(const BWebKitCrashReport& report)
{
    char address[64];
    std::snprintf(address, sizeof(address), "0x%llx", static_cast<unsigned long long>(report.faultAddress));
    switch (report.signal) {
    case 0: return "debugger() call";
    case SIGSEGV: return std::string("SIGSEGV (bad memory access at ") + address + ")";
    case SIGBUS: return std::string("SIGBUS (bad memory access at ") + address + ")";
    case SIGILL: return std::string("SIGILL (illegal instruction at ") + address + ")";
    case SIGFPE: return std::string("SIGFPE (arithmetic error at ") + address + ")";
    case SIGABRT: return "SIGABRT (abort())";
    default: return "signal " + std::to_string(report.signal);
    }
}

std::string SignalName(int signal)
{
    switch (signal) {
    case 0: return "debugger()";
    case SIGSEGV: return "SIGSEGV";
    case SIGBUS: return "SIGBUS";
    case SIGILL: return "SIGILL";
    case SIGFPE: return "SIGFPE";
    case SIGABRT: return "abort()";
    default: return "signal " + std::to_string(signal);
    }
}

// "#3 libWebKit.so.1+0x1a2b3c WebCore::RenderBox::layout()+0x12": where the
// frame is (image and offset, for addr2line on the matching build) and what
// libdebug named it, without the image it repeats.
std::string FrameLine(size_t index, const json& frame)
{
    std::string symbol = frame.value("symbol", "?");
    if (const size_t paren = symbol.rfind(" ("); paren != std::string::npos && symbol.back() == ')') symbol.erase(paren);
    if (symbol.size() > 200) symbol = symbol.substr(0, 197) + "...";
    const std::string location = frame.value("location", "");
    return "#" + std::to_string(index) + " " + (location.empty() ? frame.value("address", "?") : location) + " " + symbol;
}

// The function a crash is filed under: the first frame that is not the
// crash machinery itself (abort(), debugger(), WTFCrash()...).
std::string CrashSite(const json& frames)
{
    static const char* const machinery[] = { "debugger", "abort", "raise", "WTFCrash", "_kern_", "send_signal", "__cxa_",
        "std::terminate", "__gnu_cxx::__verbose_terminate", "commpage", "WTF::jscSignalHandler", "reportCrash",
        "crashSignalHandler", "CRASH_WITH" };
    for (const auto& frame : frames) {
        std::string symbol = frame.value("symbol", "");
        if (symbol.empty() || symbol[0] == '?' || symbol.rfind("0x", 0) == 0) continue;
        bool skip = false;
        for (const char* name : machinery) skip = skip || symbol.find(name) != std::string::npos;
        if (skip) continue;
        // Not a name worth filing under: say where it is instead.
        if (symbol.rfind("unexported code after ", 0) == 0) return frame.value("location", symbol);
        if (const size_t plus = symbol.rfind('+'); plus != std::string::npos) symbol.erase(plus);
        return symbol.size() > 90 ? symbol.substr(0, 87) + "..." : symbol;
    }
    return { };
}

// Runs the debugger helper on the crashed team and returns its JSON.
json ReadStacks(const BWebKitCrashReport& report)
{
    const std::string executable = ImagePath(true);
    BPath temporary;
    std::string output = std::string(find_directory(B_SYSTEM_TEMP_DIRECTORY, &temporary) == B_OK ? temporary.Path() : "/tmp")
        + "/summit-crash-XXXXXX";
    const int fd = mkstemp(output.data());
    if (fd < 0) return {{"error", "no temporary file"}};
    close(fd);
    auto hex = [](uint64 value) {
        char text[32];
        std::snprintf(text, sizeof(text), "0x%llx", static_cast<unsigned long long>(value));
        return std::string(text);
    };
    const std::string team = std::to_string(report.team), thread = std::to_string(report.thread),
        ip = hex(report.instructionPointer), fp = hex(report.framePointer), sp = hex(report.stackPointer),
        depth = std::to_string(kStackDepth);
    const char* argv[] = { executable.c_str(), "--mcp-debug-helper", "crash", team.c_str(), thread.c_str(), ip.c_str(),
        fp.c_str(), sp.c_str(), depth.c_str(), output.c_str(), nullptr };
    // load_image(), not posix_spawn(): spawning forks this large process.
    const thread_id helper = load_image(10, argv, const_cast<const char**>(environ));
    if (helper < 0) {
        unlink(output.c_str());
        return {{"error", std::string("could not start the debugger helper: ") + std::strerror(helper)}};
    }
    resume_thread(helper);
    // The engine's process reaper may collect it before wait_for_thread()
    // could: wait for its main thread to be gone.
    const bigtime_t deadline = system_time() + 20000000;
    thread_info info;
    while (get_thread_info(helper, &info) == B_OK && system_time() < deadline) snooze(20000);
    if (get_thread_info(helper, &info) == B_OK) kill_team(helper);
    std::ifstream in(output, std::ios::binary);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    unlink(output.c_str());
    json stacks = json::parse(text, nullptr, false);
    if (!stacks.is_object()) return {{"error", "the debugger helper gave no stacks"}};
    return stacks;
}

std::filesystem::path ReportFolder()
{
    BPath path;
    if (find_directory(B_USER_LOG_DIRECTORY, &path, true) != B_OK) return "/tmp";
    std::filesystem::path folder = std::filesystem::path(path.Path()) / "Summit";
    std::error_code error;
    std::filesystem::create_directories(folder, error);
    // Only the newest reports are kept.
    std::vector<std::filesystem::directory_entry> reports;
    for (const auto& entry : std::filesystem::directory_iterator(folder, error))
        if (entry.path().extension() == ".txt" && entry.path().filename().string().rfind("crash-", 0) == 0) reports.push_back(entry);
    if (reports.size() >= kKeptReports) {
        std::sort(reports.begin(), reports.end(), [](const auto& a, const auto& b) { return a.path().filename() < b.path().filename(); });
        for (size_t i = 0; i + kKeptReports <= reports.size(); ++i) std::filesystem::remove(reports[i].path(), error);
    }
    return folder;
}

void HandleCrash(BWebKitCrashReport report)
{
    report.process[sizeof(report.process) - 1] = '\0';
    report.message[sizeof(report.message) - 1] = '\0';
    const json stacks = ReadStacks(report);
    // The process has served its purpose: it ends, and its tabs show their
    // crash page.
    kill_team(report.team);

    const time_t now = std::time(nullptr);
    struct tm local { };
    localtime_r(&now, &local);
    char when[64], stamp[64];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S %z", &local);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &local);
    struct utsname system { };
    uname(&system);
    system_info info { };
    get_system_info(&info);

    json crashedFrames = json::array(), crashedScan = json::array();
    std::string threadName = "?";
    for (const auto& thread : stacks.value("threads", json::array()))
        if (thread.value("crashed", false)) {
            crashedFrames = thread.value("frames", json::array());
            crashedScan = thread.value("stackScan", json::array());
            threadName = Printable(thread.value("name", "?"));
        }
    const std::string site = CrashSite(crashedFrames);
    const std::string title = std::string(report.process) + " crash: " + SignalName(report.signal) + (site.empty() ? "" : " in " + site);
    std::string heading = "**Crash:** " + SignalText(report) + " in thread \"" + threadName + "\" of the " + report.process + "\n";
    if (report.message[0]) heading += "**Message:** `" + std::string(report.message) + "`\n";
    heading += "**Summit:** " + PackageOf(ImagePath(true)) + "\n";
    heading += "**Engine:** " + PackageOf(ImagePath(false, "libWebKit.so.1")) + " (WebKit " + BWebKitVersion() + ", Haiku port "
        + BWebKitPortVersion() + ")\n";
    // The revision ("hrev60207+2472"), not the build date after it.
    std::string revision = system.version;
    revision = revision.substr(0, revision.find(' '));
    heading += "**System:** " + std::string(system.sysname) + " " + system.release + " " + revision + ", " + system.machine
        + ", " + std::to_string(info.cpu_count) + " CPUs, " + std::to_string((uint64(info.max_pages) * B_PAGE_SIZE) >> 20) + " MiB\n";
    if (stacks.contains("error")) heading += "**Stacks:** not read: " + stacks["error"].get<std::string>() + "\n";

    // The full report, for the issue's attachment.
    std::string text = "Summit crash report\n" + std::string(when) + "\n\n" + heading + "Team: " + std::to_string(report.team)
        + ", thread " + std::to_string(report.thread) + "\n\nCrashed thread \"" + threadName + "\":\n";
    std::vector<std::string> frames;
    for (size_t i = 0; i < crashedFrames.size(); ++i) {
        frames.push_back(FrameLine(i, crashedFrames[i]));
        text += "  " + frames.back() + "\n";
    }
    if (!crashedScan.empty()) {
        text += "\nCode addresses on its stack (for frames without a frame pointer):\n";
        for (size_t i = 0; i < crashedScan.size(); ++i) text += "  " + FrameLine(i, crashedScan[i]) + "\n";
    }
    for (const auto& thread : stacks.value("threads", json::array())) {
        if (thread.value("crashed", false)) continue;
        text += "\nThread " + std::to_string(thread.value("thread", 0)) + " \"" + Printable(thread.value("name", "")) + "\" ("
            + thread.value("state", "") + "):\n";
        if (thread.contains("error")) text += "  " + thread.value("error", "") + "\n";
        const auto threadFrames = thread.value("frames", json::array());
        for (size_t i = 0; i < threadFrames.size(); ++i) text += "  " + FrameLine(i, threadFrames[i]) + "\n";
    }
    // Their build IDs find the build's symbol tables (tools/symbolize-crash-report.py).
    text += "\nImages (text segments):\n";
    for (const auto& image : stacks.value("images", json::array()))
        text += "  " + image.value("text", "") + " " + image.value("path", "")
            + (image.contains("buildId") ? " build-id " + image.value("buildId", "") : "") + "\n";

    const std::filesystem::path path = ReportFolder() / ("crash-" + std::string(stamp) + "-" + report.process + "-"
        + std::to_string(report.team) + ".txt");
    std::ofstream(path) << text;
    std::fprintf(stderr, "Summit: %s (team %d), report in %s\n", title.c_str(), int(report.team), path.c_str());

    BMessage ready(summit::kCrashReportReady);
    ready.AddInt32("team", report.team);
    ready.AddString("path", path.c_str());
    ready.AddString("title", title.c_str());
    ready.AddString("heading", heading.c_str());
    for (const auto& frame : frames) ready.AddString("frame", frame.c_str());
    be_app_messenger.SendMessage(&ready);
}

status_t Listen(void* data)
{
    const port_id port = static_cast<port_id>(reinterpret_cast<addr_t>(data));
    for (;;) {
        BWebKitCrashReport report { };
        int32 code = 0;
        const ssize_t size = read_port(port, &code, &report, sizeof(report));
        if (size == B_INTERRUPTED) continue;
        if (size < 0) return size;
        if (code != B_WEBKIT_CRASH_REPORT || size_t(size) < sizeof(report) || report.version != 1 || report.team < 0) continue;
        // Each on its own: reading stacks takes a moment, and another
        // process may crash meanwhile.
        std::thread(HandleCrash, report).detach();
    }
}
}
#endif

namespace summit {

void StartCrashReporter()
{
#ifdef B_WEBKIT_HAS_CRASH_REPORTS
    if (const char* setting = std::getenv("SUMMIT_CRASH_REPORTS"); setting && !std::strcmp(setting, "0")) return;
    const port_id port = create_port(16, "summit crash reports");
    if (port < 0) return;
    const thread_id listener = spawn_thread(Listen, "crash reports", B_NORMAL_PRIORITY,
        reinterpret_cast<void*>(static_cast<addr_t>(port)));
    if (listener < 0 || resume_thread(listener) != B_OK) {
        delete_port(port);
        return;
    }
    setenv(B_WEBKIT_CRASH_REPORT_PORT_ENV, std::to_string(port).c_str(), 1);
#endif
}

std::string CrashIssueURL(const std::string& title, const std::string& heading, const std::vector<std::string>& frames,
    const std::string& origin, const std::string& path)
{
    auto encode = [](const std::string& text) {
        static constexpr char digits[] = "0123456789ABCDEF";
        std::string encoded;
        for (unsigned char character : text) {
            if (std::isalnum(character) || character == '-' || character == '_' || character == '.' || character == '~') encoded += char(character);
            else { encoded += '%'; encoded += digits[character >> 4]; encoded += digits[character & 15]; }
        }
        return encoded;
    };
    // GitHub answers a longer address with an error (and signing in first
    // makes it longer still).
    constexpr size_t kLimit = 6000;
    const std::string prefix = "https://github.com/airos-works/summit/issues/new?labels=bug&title=" + encode(title) + "&body=";
    auto body = [&](size_t count, size_t width) {
        std::string text = "**What I was doing:** \n\n" + heading + "**Site:** " + (origin.empty() ? "?" : origin) + "\n\n"
            "**Crashed thread:**\n```\n";
        for (size_t i = 0; i < count && i < frames.size(); ++i)
            text += (frames[i].size() > width ? frames[i].substr(0, width - 3) + "..." : frames[i]) + "\n";
        if (count < frames.size()) text += "(" + std::to_string(frames.size() - count) + " more frames in the full report)\n";
        text += "```\n\nThe full report, with every thread's stack and the loaded libraries, is saved as `" + path
            + "`. Drag that file into this box to attach it.\n";
        return text;
    };
    // As many frames as fit, then shorter ones.
    for (size_t width : { size_t(200), size_t(120), size_t(80) })
        for (size_t count = frames.size(); count > 0; --count) {
            const std::string url = prefix + encode(body(count, width));
            if (url.size() <= kLimit && (count >= std::min<size_t>(frames.size(), 12) || width == 80)) return url;
        }
    return prefix + encode(body(0, 80));
}

}
