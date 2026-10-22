#include "core/Address.h"
#include "core/UserAgent.h"
#include "core/Profile.h"
#include "core/Favicon.h"
#include "core/Spelling.h"
#include "core/InternalPages.h"
#include "core/Zoom.h"
#include "core/Suggest.h"
#include "core/DownloadText.h"
#include "core/DownloadHistory.h"
#include "core/LoadError.h"
#include "core/ProfileLock.h"
#include <set>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sys/stat.h>
#include <unistd.h>

static int checks = 0, failures = 0;
#define CHECK(x) do { ++checks; if (!(x)) { ++failures; std::cerr << __FILE__ << ':' << __LINE__ << ": " #x "\n"; } } while (0)

int main()
{
    using namespace summit;
    CHECK(std::string(FindUserAgentPreset("unknown").id) == "default");
    CHECK(std::string(FindUserAgentPreset("").agent).empty());
    std::set<std::string> userAgentIDs;
    for (const auto& preset : UserAgentPresets()) {
        CHECK(userAgentIDs.insert(preset.id).second);
        CHECK(&FindUserAgentPreset(preset.id) == &preset);
        CHECK(std::string(preset.agent).find_first_of("\r\n") == std::string::npos);
        if (std::string(preset.id) == "default") continue;
        const std::string agent = preset.agent, id = preset.id;
        CHECK(agent.rfind("Mozilla/5.0 ", 0) == 0);
        CHECK(agent.find(id.find("windows") != std::string::npos ? "Windows NT" : "Macintosh") != std::string::npos);
        CHECK(agent.find(id.rfind("edge", 0) == 0 ? "Edg/" : id.rfind("firefox", 0) == 0 ? "Firefox/"
            : id.rfind("chrome", 0) == 0 ? "Chrome/" : "Version/") != std::string::npos);
    }
    CHECK(FormatByteSize(0) == "0 bytes");
    CHECK(FormatByteSize(1) == "1 byte");
    CHECK(FormatByteSize(1023) == "1023 bytes");
    CHECK(FormatByteSize(1536) == "1.5 KiB");
    CHECK(FormatByteSize(150 * 1024) == "150 KiB");
    CHECK(FormatByteSize(3 * 1024 * 1024 + 104858) == "3.1 MiB");
    CHECK(FormatByteSize(1342177280ull) == "1.25 GiB");
    CHECK(FormatByteProgress(512 * 1024, 15 * 1024 * 1024) == "0.5 of 15.0 MiB");
    CHECK(FormatTimeLeft(4.2) == "5 seconds left");
    CHECK(FormatTimeLeft(0.2) == "1 second left");
    CHECK(FormatTimeLeft(125) == "2 minutes left");
    CHECK(FormatTimeLeft(3599) == "1 hour left");
    CHECK(FormatTimeLeft(3900) == "1 hour 5 minutes left");
    CHECK(FormatTimeLeft(7200) == "2 hours left");
    CHECK(FormatTimeLeft(-1).empty());
    CHECK(DownloadFraction(5, 10) == 0.5 && DownloadFraction(5, -1) == -1 && DownloadFraction(20, 10) == 1);
    {
        DownloadStatus status;
        CHECK(DownloadStatusText(status) == "Starting…");
        status.received = 3 * 1024 * 1024;
        status.expected = 15 * 1024 * 1024;
        status.rate = 1024 * 1024;
        CHECK(DownloadStatusText(status) == "3.0 of 15.0 MiB — 1.0 MiB/s, 12 seconds left");
        status.expected = -1;
        CHECK(DownloadStatusText(status) == "3.0 MiB — 1.0 MiB/s");
        status.state = DownloadStatus::State::Succeeded;
        status.host = "example.com";
        CHECK(DownloadStatusText(status) == "3.0 MiB — example.com");
        status.missing = true;
        CHECK(DownloadStatusText(status) == "File moved or missing");
        status.state = DownloadStatus::State::Failed;
        status.error = "Connection reset";
        CHECK(DownloadStatusText(status) == "Failed — Connection reset");
        status.state = DownloadStatus::State::Cancelled;
        CHECK(DownloadStatusText(status) == "Cancelled");
        status = DownloadStatus();
        status.state = DownloadStatus::State::Paused;
        CHECK(DownloadStatusText(status) == "Paused");
        status.received = 400 * 1024 * 1024;
        status.expected = 1000 * 1024 * 1024;
        CHECK(DownloadStatusText(status) == "Paused — 400 of 1000 MiB");
        status.interrupted = true;
        CHECK(DownloadStatusText(status) == "Interrupted — 400 of 1000 MiB");
        status.resumable = false;
        CHECK(DownloadStatusText(status) == "Interrupted — will start over");
        status = DownloadStatus();
        status.restarted = true;
        CHECK(DownloadStatusText(status) == "Starting over…");
        status.received = 1024 * 1024;
        status.expected = 4 * 1024 * 1024;
        CHECK(DownloadStatusText(status) == "Starting over — 1.0 of 4.0 MiB");
    }
    {
        DownloadRecord paused;
        paused.url = "https://cdn.example.com/a.iso?sig=1";
        paused.requestURL = "https://example.com/a.iso";
        paused.path = "/boot/home/Downloads/a.iso";
        paused.filename = "a.iso";
        paused.state = DownloadStatus::State::Paused;
        paused.received = 5000;
        paused.expected = 9000;
        paused.validator = "\"abc\"";
        paused.acceptsRanges = true;
        paused.partial = true;
        paused.started = 1700000000;
        CHECK(paused.CanResume());
        DownloadRecord active = paused;
        active.state = DownloadStatus::State::Active;
        active.partial = false;
        active.requestURL = active.url;
        DownloadRecord done;
        done.url = done.requestURL = "https://example.com/b.zip";
        done.path = "/boot/home/Downloads/b.zip";
        done.state = DownloadStatus::State::Succeeded;
        done.received = 12;
        done.finished = 1700000100;
        DownloadRecord failed = done;
        failed.state = DownloadStatus::State::Failed;
        failed.error = "Transferred a partial file";
        const auto records = DecodeDownloads(EncodeDownloads({paused, active, done, failed}));
        CHECK(records.size() == 4);
        if (records.size() == 4) {
            CHECK(records[0].requestURL == paused.requestURL && records[0].url == paused.url && records[0].path == paused.path
                && records[0].state == DownloadStatus::State::Paused && records[0].received == 5000 && records[0].expected == 9000
                && records[0].validator == "\"abc\"" && records[0].acceptsRanges && records[0].partial && !records[0].interrupted
                && records[0].started == 1700000000);
            // Still active when Summit stopped: a crash. It comes back paused, with its file.
            CHECK(records[1].state == DownloadStatus::State::Paused && records[1].interrupted && records[1].partial
                && records[1].requestURL == records[1].url);
            CHECK(records[2].state == DownloadStatus::State::Succeeded && !records[2].partial && records[2].finished == 1700000100
                && records[2].expected == -1 && !records[2].CanResume());
            CHECK(records[3].state == DownloadStatus::State::Failed && records[3].error == "Transferred a partial file");
        }
        DownloadRecord unresumable = paused;
        unresumable.acceptsRanges = false;
        CHECK(!unresumable.CanResume());
        CHECK(DecodeDownloads("").empty() && DecodeDownloads("{\"version\":2,\"downloads\":[]}").empty()
            && DecodeDownloads("[1,2]").empty());
        const auto mixed = DecodeDownloads("{\"version\":1,\"downloads\":[{\"state\":\"bogus\",\"url\":\"x\"},"
            "{\"state\":\"failed\"},{\"state\":\"succeeded\",\"url\":\"https://e.com/f\",\"received\":-5},7]}");
        CHECK(mixed.size() == 1 && mixed[0].received == 0 && mixed[0].url == "https://e.com/f");
    }
    CHECK(ResolveAddress("").url == "summit:home");
    CHECK(ResolveAddress("  example.com  ").url == "https://example.com");
    CHECK(ResolveAddress("localhost:8080/test").url == "http://localhost:8080/test");
    CHECK(ResolveAddress("[::1]:8000/test").url == "http://[::1]:8000/test");
    CHECK(ResolveAddress("example.com:8443/a").url == "https://example.com:8443/a");
    CHECK(ResolveAddress("HTTPS://example.com/a").url == "https://example.com/a");
    CHECK(ResolveAddress("http://10.0.2.2:8765/").url == "http://10.0.2.2:8765/");
    CHECK(ResolveAddress("about:blank").url == "about:blank");
    CHECK(ResolveAddress("webkit-extension://12345678-1234-4234-8234-123456789abc/view.html?kind=tab").url
        == "webkit-extension://12345678-1234-4234-8234-123456789abc/view.html?kind=tab");
    CHECK(ResolveAddress("WEBKIT-EXTENSION://12345678-1234-4234-8234-123456789abc/options.html").url
        == "webkit-extension://12345678-1234-4234-8234-123456789abc/options.html");
    CHECK(ResolveAddress("/boot/home/test.html").url == "file:///boot/home/test.html");
    CHECK(ResolveAddress("/").url == "file:///");
    CHECK(ResolveAddress("/boot/home/a #?%/café.html").url == "file:///boot/home/a%20%23%3F%25/caf%C3%A9.html");
    CHECK(FileURL("/boot/home/ report .html ") == "file:///boot/home/%20report%20.html%20");
    CHECK(ResolveAddress("file:///boot/home/a%20%23.html").url == "file:///boot/home/a%20%23.html");
    CHECK(FileURL("relative.html").empty());
    CHECK(ResolveAddress("kunanyi walking tracks").search);
    CHECK(ResolveAddress("site:example.com").url == "site:example.com");
    CHECK(ResolveAddress("tel:12345").url == "tel:12345");
    CHECK(ResolveAddress("MAILTO:person@example.com").url == "mailto:person@example.com");
    CHECK(ResolveAddress("web+demo:123").url == "web+demo:123");
    CHECK(ResolveAddress("hello & goodbye").url == "https://duckduckgo.com/?q=hello%20%26%20goodbye");
    CHECK(ResolveAddress("example.com query").search);
    CHECK(!ResolveAddress("javascript:alert(1)").error.empty());
    CHECK(!ResolveAddress("https://example.com\r\nother").error.empty());
    CHECK(!ResolveAddress(std::string("https://host\0evil", 17)).error.empty());
    CHECK(!ResolveAddress(std::string(65537, 'x')).error.empty());
    CHECK(PercentEncode("café") == "caf%C3%A9");
    CHECK(EscapeHTML("<&\"'>") == "&lt;&amp;&quot;&#39;&gt;");
    CHECK(ResolveAddress("summit:history").url == "summit:history");
    CHECK(ResolveAddress("Summit:Bookmarks").url == "summit:bookmarks");
    CHECK(!ResolveAddress("summit:nothing").error.empty());

    // External link dispatch: allowed registration schemes, trusted templates,
    // encoding one complete URL (never shell arguments), and credential removal.
    CHECK(URLScheme("MAILTO:hello@example.com") == "mailto");
    CHECK(URLScheme("1mail:bad").empty() && URLScheme("mail to:bad").empty());
    CHECK(URLScheme("mailto:bad\naddress").empty());
    CHECK(!IsExternalScheme("javascript") && !IsExternalScheme("https") && !IsExternalScheme("file"));
    CHECK(IsExternalScheme("haiku-app") && IsWebHandlerScheme("mailto") && IsWebHandlerScheme("web+demo"));
    CHECK(!IsWebHandlerScheme("https") && !IsWebHandlerScheme("web+") && !IsWebHandlerScheme("web+demo1"));
    CHECK(ProtocolOrigin("HTTPS://MAIL.Example:443/path?q=%s") == "https://mail.example");
    CHECK(ProtocolOrigin("http://[::1]:080/path") == "http://[::1]");
    CHECK(ProtocolOrigin("https://mail.example:8443/path") == "https://mail.example:8443");
    CHECK(ProtocolOrigin("https://good@evil/path").empty());
    CHECK(ProtocolOrigin("https://good:99999/path").empty());
    CHECK(ProtocolOrigin("https://good:abc/path").empty());
    CHECK(ProtocolOrigin("https://good\\evil/path").empty());
    const ProtocolHandler mail {"web", "https://mail.example/compose?url=%s&other=%s", "https://mail.example"};
    CHECK(ValidProtocolHandler("mailto", mail));
    CHECK(!ValidProtocolHandler("http", mail));
    CHECK(!ValidProtocolHandler("mailto", {"web", "https://evil.example/?url=%s", mail.origin}));
    CHECK(!ValidProtocolHandler("mailto", {"web", "javascript:alert('%s')", mail.origin}));
    CHECK(!ValidProtocolHandler("mailto", {"web", "https://mail.example/", mail.origin}));
    CHECK(!ValidProtocolHandler("mailto", {"app", "relative", ""}));
    CHECK(!ValidProtocolHandler("mailto", {"app", "/boot/apps/Mail\n", ""}));
    CHECK(ProtocolHandlerURL(mail, "mailto:café@example.com?subject=A&B")
        == "https://mail.example/compose?url=mailto%3Acaf%C3%A9%40example.com%3Fsubject%3DA%26B&other=%s");
    const ProtocolHandler ftp {"web", "https://files.example/open?url=%s", "https://files.example"};
    CHECK(ProtocolHandlerURL(ftp, "ftp://user:password@host/file@name")
        == "https://files.example/open?url=ftp%3A%2F%2Fhost%2Ffile%40name");
    CHECK(ProtocolHandlerURL(ftp, "ftp://host/file@name")
        == "https://files.example/open?url=ftp%3A%2F%2Fhost%2Ffile%40name");
    CHECK(ProtocolHandlerURL(mail, "javascript:alert(1)").empty());

    // Search engines.
    CHECK(CurrentSearchEngine().id == std::string("duckduckgo") && SearchEngines().size() == 3);
    SetSearchEngine("google");
    CHECK(ResolveAddress("hello & goodbye").url == "https://www.google.com/search?q=hello%20%26%20goodbye");
    CHECK(ResolveAddress("example.com").url == "https://example.com");
    SetSearchEngine("bing");
    CHECK(SearchURL("  café  ") == "https://www.bing.com/search?q=caf%C3%A9" && CurrentSearchEngine().name == std::string("Bing"));
    SetSearchEngine("altavista");
    CHECK(CurrentSearchEngine().id == std::string("duckduckgo"));
    CHECK(SearchURL("example.com") == "https://duckduckgo.com/?q=example.com");
    {
        const auto page = RenderPrivateStartPage("Google", "https://www.google.com/search?q=");
        CHECK(page.find("action=\"https://www.google.com/search\"") != std::string::npos);
        CHECK(page.find("Search with Google") != std::string::npos && page.find("name=\"q\"") != std::string::npos);
        CHECK(RenderPrivateStartPage("<x>", "https://a.example/?q=").find("Search with &lt;x&gt;") != std::string::npos);
    }

    // Zoom.
    CHECK(NextZoomLevel(1, 1) == 1.1 && NextZoomLevel(1, -1) == 0.9);
    CHECK(NextZoomLevel(1.05, 1) == 1.1 && NextZoomLevel(1.05, -1) == 1.0);
    CHECK(NextZoomLevel(5, 1) == 5 && NextZoomLevel(0.3, -1) == 0.3 && NextZoomLevel(9, -1) == 5);
    CHECK(NextZoomLevel(1.2, 1) == 1.33 && NextZoomLevel(1.33, 1) == 1.5);
    CHECK(IsDefaultZoom(1.0001) && !IsDefaultZoom(1.1));
    CHECK(ZoomLabel(1.1) == "110%" && ZoomLabel(0.67) == "67%" && ZoomLabel(1.33) == "133%");
    CHECK(ZoomKey("https://WWW.Example.com:8443/a?b") == "example.com:8443");
    CHECK(ZoomKey("http://user@news.example.org/") == "news.example.org");
    CHECK(ZoomKey("file:///boot/home/a.html") == "file" && ZoomKey("summit:history") == "summit:history");
    CHECK(ZoomKey("about:blank").empty() && ZoomKey("webkit-extension://abc/page.html").empty());

    // Favicons.
    CHECK(FaviconKey("https://WWW.Example.com:8443/a?b") == "www.example.com");
    CHECK(FaviconKey("http://user@[::1]:80/") == "___1_");
    CHECK(FaviconKey("file:///boot/home").empty() && FaviconKey("summit:history").empty());
    CHECK(Base64("") == "" && Base64("f") == "Zg==" && Base64("fo") == "Zm8=" && Base64("foobar") == "Zm9vYmFy");
    {
        // A 2x2 32-bit icon (bottom-up rows) and a 1-bit one, both in one .ico.
        auto le16 = [](std::string& s, unsigned v) { s += char(v & 255); s += char(v >> 8); };
        auto le32 = [&](std::string& s, unsigned v) { le16(s, v & 0xffff); le16(s, v >> 16); };
        auto dib = [&](int width, int bpp, const std::string& pixels, const std::string& palette) {
            std::string d;
            le32(d, 40); le32(d, width); le32(d, width * 2); le16(d, 1); le16(d, bpp); le32(d, 0);
            le32(d, 0); le32(d, 0); le32(d, 0); le32(d, palette.size() / 4); le32(d, 0);
            return d + palette + pixels;
        };
        // BGRA rows, bottom row first: bottom = blue, green; top = red, transparent.
        std::string rgba32 = std::string("\xff\0\0\xff\0\xff\0\xff", 8) + std::string("\0\0\xff\xff\0\0\0\0", 8);
        std::string first = dib(2, 32, rgba32 + std::string(8, '\0'), { });
        // 1-bit 16x16: palette black/white, all white, mask transparent on the top row.
        std::string bits(16 * 4, '\xff'), mask(16 * 4, '\0');
        mask[15 * 4] = mask[15 * 4 + 1] = '\xff';
        std::string second = dib(16, 1, bits + mask, std::string("\0\0\0\0\xff\xff\xff\0", 8));
        std::string ico;
        le16(ico, 0); le16(ico, 1); le16(ico, 2);
        const unsigned base = 6 + 32;
        ico += char(2); ico += char(2); ico += char(0); ico += char(0); le16(ico, 1); le16(ico, 32);
        le32(ico, first.size()); le32(ico, base);
        ico += char(16); ico += char(16); ico += char(0); ico += char(0); le16(ico, 1); le16(ico, 1);
        le32(ico, second.size()); le32(ico, base + first.size());
        ico += first + second;
        CHECK(IsIco(ico));
        auto entry = SelectIcoEntry(ico, 32);
        CHECK(entry.kind == IcoEntryKind::DIB && entry.width == 16);
        Image image;
        CHECK(DecodeDIB(entry.bytes, image) && image.width == 16 && image.height == 16);
        CHECK(image.rgba[3] == 0 && image.rgba[0 + 16 * 4 * 1 + 3] == 255 && image.rgba[16 * 4 + 0] == 255);
        entry = SelectIcoEntry(ico, 2);
        CHECK(entry.width == 2 && DecodeDIB(entry.bytes, image));
        CHECK(image.rgba[0] == 255 && image.rgba[3] == 255);        // top-left red
        CHECK(image.rgba[7] == 0);                                   // top-right transparent
        CHECK(image.rgba[8 + 2] == 255 && image.rgba[12 + 1] == 255); // blue, green
        auto scaled = ScaleImage(image, 1);
        CHECK(scaled.width == 1 && scaled.rgba[3] > 180 && scaled.rgba[3] < 200);
        CHECK(scaled.rgba[0] > 80 && scaled.rgba[1] > 80 && scaled.rgba[2] > 80);
        auto wide = ScaleImage(Image{4, 2, std::vector<uint8_t>(32, 255)}, 4);
        CHECK(wide.rgba[3] == 0 && wide.rgba[(1 * 4) * 4 + 3] == 255 && wide.rgba[(3 * 4) * 4 + 3] == 0);
        auto png = EncodePNG(scaled);
        CHECK(png.size() > 8 && png.substr(1, 3) == "PNG" && png.find("IEND") != std::string::npos);
        CHECK(!IsIco("\x89PNG") && SelectIcoEntry("junk").kind == IcoEntryKind::None);
        // SVG favicons: a red square on the left half of a 2:1 picture, centred.
        const std::string svg = "<?xml version=\"1.0\"?>\n<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 64 32\">"
            "<rect x=\"0\" y=\"0\" width=\"32\" height=\"32\" fill=\"#ff0000\"/></svg>";
        CHECK(IsSvg(svg) && !IsSvg("\x89PNG") && !IsSvg("<html><body></body></html>"));
        Image vector;
        CHECK(RasterizeSvg(svg, 32, vector) && vector.width == 32 && vector.height == 32);
        const auto at = [&](int x, int y) { return &vector.rgba[(size_t(y) * 32 + x) * 4]; };
        CHECK(at(4, 4)[3] == 0);                                   // above the picture
        CHECK(at(4, 16)[0] == 255 && at(4, 16)[3] == 255);           // red
        CHECK(at(24, 16)[3] == 0);                                  // right half empty
        CHECK(!RasterizeSvg("<svg xmlns=\"http://www.w3.org/2000/svg\"></svg>", 32, vector) && vector.Empty());
        CHECK(!RasterizeSvg("<svg", 32, vector) && !RasterizeSvg(svg, 0, vector));
        // Damaged input must not crash.
        for (size_t cut = 0; cut < svg.size(); cut += 7) RasterizeSvg(svg.substr(0, cut), 16, vector);
    }

    // Spelling dictionaries and the languages given to the engine.
    {
        const auto root = std::filesystem::temp_directory_path() / ("summit-spelling-" + std::to_string(::getpid()));
        std::filesystem::create_directories(root / "a");
        std::filesystem::create_directories(root / "b");
        for (const char* name : {"a/de_DE.aff", "a/de_DE.dic", "a/en_US.aff", "a/en_US.dic", "b/en_AU.aff", "b/en_AU.dic",
                "b/fr_FR.aff", "b/de_DE.aff", "b/de_DE.dic"})
            std::ofstream(root / name) << "x";
        const auto found = SpellingDictionaries({(root / "a").string(), (root / "b").string(), (root / "missing").string()});
        CHECK((found == std::vector<std::string>{"de_DE", "en_AU", "en_US"}));
        CHECK(SpellingLanguages("", {"de"}, found) == "de_DE,en_US");
        CHECK(SpellingLanguages("", {"en-AU", "de_CH"}, found) == "en_AU,de_DE,en_US");
        CHECK(SpellingLanguages("", {"fr"}, found) == "en_US");
        CHECK(SpellingLanguages("en_AU", {"de"}, found) == "en_AU,en_US");
        CHECK(SpellingLanguages("it_IT", {"de"}, found) == "de_DE,en_US");
        CHECK(SpellingLanguages("", {}, {}) == "en_US");
        std::filesystem::remove_all(root);
    }

    // History grouped by day, and the built-in pages.
    setenv("TZ", "UTC", 1);
    tzset();
    const int64_t now = 1790380800 + 15 * 3600; // Saturday, 26 September 2026, 15:00 UTC
    std::vector<PageRecord> visits = {
        {"https://a.example/", "A <b>", now - 60}, {"https://b.example/", "", now - 3600},
        {"https://c.example/", "C", now - 86400}, {"https://d.example/", "D", now - 2 * 86400},
        {"https://e.example/", "E", 0}};
    auto days = GroupHistoryByDay(visits, now);
    CHECK(days.size() == 4 && days[0].label == "Today" && days[0].visits.size() == 2);
    CHECK(days[1].label == "Yesterday" && days[2].label == "Thursday, 24 September 2026" && days[3].label == "Earlier");
    auto html = RenderHistoryPage(visits, now, [](const std::string& url) {
        return url == "https://a.example/" ? std::string("data:image/png;base64,QUJD") : std::string();
    });
    CHECK(html.find("<title>History</title>") != std::string::npos && html.find("A &lt;b&gt;") != std::string::npos);
    CHECK(html.find("href=\"https://b.example/\"") != std::string::npos && html.find(">14:59<") != std::string::npos);
    CHECK(html.find(".i0{background-image:url(data:image/png;base64,QUJD)}") != std::string::npos);
    CHECK(html.find("icon letter\">B<") != std::string::npos);
    CHECK(RenderHistoryPage({}, now, nullptr).find("Pages you visit") != std::string::npos);
    // A page visited on three days is under each of them, and a time range
    // can be forgotten.
    {
        Profile profile;
        CHECK(profile.Visit({"https://daily.example/", "Daily"}, now - 2 * 86400) && profile.Visit({"https://daily.example/", "Daily"}, now - 86400));
        CHECK(profile.Visit({"https://daily.example/", "Daily"}, now - 7200) && profile.Visit({"https://daily.example/", "Daily"}, now - 60));
        CHECK(profile.Visit({"https://once.example/", "Once"}, now - 600));
        CHECK(profile.history.size() == 2 && profile.history[0].earlier.size() == 2 && profile.history[0].visits == 4);
        auto grouped = GroupHistoryByDay(profile.history, now);
        CHECK(grouped.size() == 3 && grouped[0].visits.size() == 2 && grouped[1].label == "Yesterday" && grouped[2].visits.size() == 1);
        CHECK(grouped[1].visits[0].page->url == "https://daily.example/" && grouped[1].visits[0].time == now - 86400);
        // Saved and read back.
        const auto folder = std::filesystem::temp_directory_path() / ("summit-visits-" + std::to_string(::getpid()));
        std::filesystem::create_directories(folder);
        std::string error;
        CHECK(profile.Save(folder / "profile.json", error));
        auto loaded = Profile::Load(folder / "profile.json", error);
        CHECK(error.empty() && loaded.history.size() == 2 && loaded.history[0].earlier == profile.history[0].earlier);
        std::filesystem::remove_all(folder);
        // The last hour: once.example goes, daily.example falls back to its
        // earlier visit today; the older days remain too.
        CHECK(profile.ForgetVisitsSince(now - 3600) == 2);
        CHECK(profile.history.size() == 1 && profile.history[0].visited == now - 7200 && profile.history[0].earlier.size() == 2);
        CHECK(profile.ForgetVisitsSince(now - 30 * 86400) == 3 && profile.history.empty());
        // Pages from before visits had times stay.
        profile.history.push_back({"https://old.example/", "Old"});
        CHECK(profile.ForgetVisitsSince(now - 3600) == 0 && profile.history.size() == 1);
    }
    Bookmarks marks;
    CHECK(marks.Add({"https://x.example/", "X"}, kBookmarksBarFolder) && !marks.Add({"data:text/html,bad", "J"}, kOtherBookmarksFolder));
    CHECK(marks.AddFolder(kOtherBookmarksFolder, "Reading"));
    auto bookmarksHTML = RenderBookmarksPage(marks,
        [](const std::string&) { return std::string("data:image/png;base64,\"evil"); }, "abc123");
    CHECK(bookmarksHTML.find("Bookmarks Bar") != std::string::npos && bookmarksHTML.find("Other Bookmarks") != std::string::npos);
    CHECK(bookmarksHTML.find("href=\"javascript") == std::string::npos && bookmarksHTML.find("evil") == std::string::npos);

    // Address field type-ahead.
    {
        std::vector<PageRecord> history = {
            {"https://www.pulsetasmania.com/events/", "Pulse Tasmania", now - 3600, false, 6},
            {"https://pulsetasmania.com/about", "About", now - 7200, false, 1},
            {"https://publicholidays.com.au/", "Public holidays", now - 30 * 86400, false, 2},
            {"https://github.com/jmgasper/summit/issues", "Issues · jmgasper/summit", now - 60, false, 9},
            {"https://github.com/jmgasper/haiku", "Haiku fork", now - 600, false, 2},
            {"https://github.com/WebKit/WebKit", "WebKit", now - 86400, false, 1},
            {"http://192.168.1.1/login", "UniFi OS", now - 86400, false, 3},
            {"http://localhost:8790/index.html", "Test", now - 100, false, 1},
            {"https://user@secret.example/", "Credentials", now, false, 50}};
        std::vector<PageRecord> bookmarks = {{"https://pubs.example.org/menu", "Pub menu"}};
        auto fill = AutofillAddress(history, bookmarks, "pul", now);
        CHECK(fill.text == "pulsetasmania.com/" && fill.url == "https://www.pulsetasmania.com/");
        CHECK(AutofillAddress(history, bookmarks, "PuL", now).text == "PuLsetasmania.com/");
        CHECK(AutofillAddress(history, bookmarks, "pu", now).text == "pulsetasmania.com/");
        CHECK(AutofillAddress(history, bookmarks, "pubs", now).url == "https://pubs.example.org/");
        CHECK(AutofillAddress(history, bookmarks, "www.pul", now).text == "www.pulsetasmania.com/");
        CHECK(AutofillAddress(history, bookmarks, "https://pul", now).text == "https://pulsetasmania.com/");
        CHECK(AutofillAddress(history, bookmarks, "http://pul", now).Empty());
        CHECK(AutofillAddress(history, bookmarks, "pulsetasmania.com", now).text == "pulsetasmania.com/");
        CHECK(AutofillAddress(history, bookmarks, "pulsetasmania.com/", now).Empty() == false);
        CHECK(AutofillAddress(history, bookmarks, "pulsetasmania.com/", now).text == "pulsetasmania.com/events/");
        fill = AutofillAddress(history, bookmarks, "github.com/jm", now);
        CHECK(fill.text == "github.com/jmgasper/" && fill.url == "https://github.com/jmgasper/");
        fill = AutofillAddress(history, bookmarks, "github.com/jmgasper/s", now);
        CHECK(fill.text == "github.com/jmgasper/summit/" && fill.url == "https://github.com/jmgasper/summit/");
        CHECK(AutofillAddress(history, bookmarks, "github.com/jmgasper/summit/i", now).text == "github.com/jmgasper/summit/issues");
        CHECK(AutofillAddress(history, bookmarks, "github.com/webk", now).url == "https://github.com/WebKit/");
        CHECK(AutofillAddress(history, bookmarks, "192.168", now).url == "http://192.168.1.1/");
        CHECK(AutofillAddress(history, bookmarks, "localhost:87", now).text == "localhost:8790/");
        CHECK(AutofillAddress(history, bookmarks, "pul tas", now).Empty());
        CHECK(AutofillAddress(history, bookmarks, "secret", now).Empty());
        CHECK(AutofillAddress(history, bookmarks, "zzz", now).Empty());
        CHECK(AutofillAddress(history, bookmarks, "", now).Empty());
        CHECK(AutofillAddress(history, bookmarks, "/boot", now).Empty());

        auto pages = SuggestPages(history, bookmarks, "pu", now, 10);
        CHECK(pages.size() == 4 && pages[0].url == "https://www.pulsetasmania.com/events/");
        CHECK(pages.back().url == "https://pubs.example.org/menu" || pages.back().url == "https://publicholidays.com.au/");
        pages = SuggestPages(history, bookmarks, "jmgasper issues", now, 10);
        CHECK(pages.size() == 1 && pages[0].title == "Issues · jmgasper/summit");
        pages = SuggestPages(history, bookmarks, "WEBKIT", now, 10);
        CHECK(pages.size() == 1 && pages[0].url == "https://github.com/WebKit/WebKit");
        CHECK(SuggestPages(history, bookmarks, "unifi", now, 10).size() == 1);
        CHECK(SuggestPages(history, bookmarks, "   ", now, 10).empty());
        CHECK(SuggestPages(history, bookmarks, "github", now, 2).size() == 2);
        std::vector<PageRecord> marked = {{"https://pubs.example.org/menu", "Pub menu"}};
        pages = SuggestPages(history, marked, "menu", now, 10);
        CHECK(pages.size() == 1 && pages[0].kind == Suggestion::Kind::Bookmark);
        history.push_back({"https://pubs.example.org/menu", "Pub menu", now, false, 3});
        pages = SuggestPages(history, marked, "menu", now, 10);
        CHECK(pages.size() == 1 && pages[0].kind == Suggestion::Kind::Bookmark);
        CHECK(ShortAddress("https://www.pulsetasmania.com/") == "pulsetasmania.com");
        CHECK(ShortAddress("https://GitHub.com/jmgasper/Summit") == "github.com/jmgasper/Summit");
        CHECK(ShortAddress("summit:history") == "summit:history");
        CHECK(Frecency({"https://a/", "", now, false, 3}, now) > Frecency({"https://a/", "", now - 100 * 86400, false, 10}, now));
    }

    char directory[] = "/tmp/summit-core-XXXXXX";
    auto* created = mkdtemp(directory);
    if (!created) return 2;
    const auto path = std::filesystem::path(created) / "profile/profile.json";
    std::string error;
    CHECK(Profile::Load(path, error).windows.empty() && error.empty());
    CHECK(Profile::Load(path, error).interfaceStyle == "haiku");
    Profile profile;
    profile.windows = {{{{"https://example.com/", "Example"}, {"summit:home", "Start Page"}}, 1, {10, 20, 810, 620}},
        {{{"https://second.example/", "Second"}}, 0}};
    profile.interfaceStyle = "safari";
    profile.searchEngine = "bing";
    profile.userAgent = "firefox-macos";
    profile.siteZoom = {{"example.com", 1.5}, {"file", 0.8}};
    profile.unpinnedExtensions = {"ext-1", "ext-2"};
    const std::string routerCertificate(64, 'a');
    CHECK(profile.TrustCertificate({"Router.LAN", std::string(64, 'A'), "/CN=UniFi", 100}));
    CHECK(!profile.TrustCertificate({"router.lan", routerCertificate, "", 200}));
    CHECK(profile.TrustCertificate({"router.lan", std::string(64, 'b'), "", 300}));
    CHECK(profile.trustedCertificates.size() == 2 && profile.trustedCertificates[0].host == "router.lan"
        && profile.trustedCertificates[0].sha256 == routerCertificate);
    profile.AddBookmark({"https://webkit.org", "WebKit — 浏览器"}, false);
    profile.Visit({"https://example.com/", "First title"});
    profile.Visit({"https://webkit.org/", "WebKit"});
    profile.Visit({"https://example.com/", "Updated title"});
    CHECK(!profile.Visit({"file:///boot/home/private", "Local file"}));
    CHECK(profile.history.size() == 2);
    CHECK(profile.history.front().title == "Updated title");
    CHECK(profile.history.front().visited > 0);
    CHECK(profile.history.front().visits == 2 && profile.history.back().visits == 1);
    profile.AddBookmark({"https://bar.example/", "Bar"}, true);
    profile.AddBookmark({"https://webkit.org", "ignored"}, true);
    CHECK(profile.bookmarks.Count() == 2 && profile.bookmarks.OnBar("https://webkit.org")
        && profile.bookmarks.FindURL("https://webkit.org")->title == "WebKit — 浏览器");
    CHECK(profile.SetSitePermission("notifications", "https://github.com", true));
    CHECK(!profile.SetSitePermission("notifications", "https://github.com", true));
    CHECK(profile.SetSitePermission("geolocation", "https://maps.example:8443", false));
    CHECK(!profile.SetSitePermission("unknown-permission", "https://github.com", true));
    CHECK(profile.SetSitePermission("camera", "https://github.com", true));
    CHECK(profile.SetSitePermission("microphone", "https://github.com", false));
    CHECK(!profile.SetSitePermission("notifications", "https://github.com/path", true));
    CHECK(!profile.SetSitePermission("notifications", "javascript:alert(1)", true));
    CHECK(!profile.SetSitePermission("notifications", "https://", true));
    CHECK(profile.SetSitePermission("notifications", "http://old.example", true));
    CHECK(profile.SetSitePermission("notifications", "http://old.example", std::nullopt));
    CHECK(!profile.SetSitePermission("notifications", "http://old.example", std::nullopt));
    CHECK(profile.SetProtocolHandler("mailto", mail));
    CHECK(!profile.SetProtocolHandler("mailto", mail));
    CHECK(profile.SetProtocolHandler("irc", {"app", "/boot/apps/Vision/Vision", ""}));
    CHECK(!profile.SetProtocolHandler("https", mail));
    profile.declinedProtocolHandlers["web+demo"].insert("https://demo.example/?url=%s");
    profile.homeURL = "https://home.example/";
    profile.showBookmarksBar = false;
    CHECK(profile.restoreSession);
    profile.restoreSession = false;
    CHECK(profile.Save(path, error) && error.empty());
    auto loaded = Profile::Load(path, error);
    CHECK(error.empty() && loaded.windows.size() == 2 && loaded.windows[0].tabs.size() == 2 && loaded.windows[0].selected == 1);
    CHECK(loaded.windows[0].HasFrame() && loaded.windows[0].frame[2] == 810 && !loaded.windows[1].HasFrame());
    CHECK(loaded.windows[1].tabs.at(0).url == "https://second.example/" && loaded.interfaceStyle == "safari");
    CHECK(loaded.bookmarks.Pages().at(1).title == "WebKit — 浏览器");
    CHECK(loaded.history.size() == 2 && loaded.history.front().title == "Updated title");
    CHECK(loaded.history.front().visited == profile.history.front().visited);
    CHECK(loaded.history.front().visits == 2 && loaded.history.back().visits == 0);
    CHECK(loaded.bookmarks.Count() == 2 && loaded.bookmarks.Pages()[0].bar && loaded.bookmarks.Pages()[0].url == "https://bar.example/");
    CHECK(loaded.homeURL == "https://home.example/" && !loaded.showBookmarksBar && !loaded.restoreSession);
    CHECK(loaded.sitePermissions.size() == 4 && loaded.sitePermissions["notifications"].size() == 1
        && loaded.sitePermissions["notifications"]["https://github.com"]
        && !loaded.sitePermissions["geolocation"]["https://maps.example:8443"]
        && loaded.sitePermissions.at("camera").at("https://github.com")
        && !loaded.sitePermissions.at("microphone").at("https://github.com"));
    CHECK(loaded.protocolHandlers == profile.protocolHandlers);
    CHECK(loaded.declinedProtocolHandlers == profile.declinedProtocolHandlers);
    CHECK(loaded.RemoveProtocolHandler("mailto") && !loaded.RemoveProtocolHandler("mailto"));
    CHECK(loaded.RemoveProtocolHandler("web+demo") && loaded.declinedProtocolHandlers.empty());
    CHECK(loaded.searchEngine == "bing" && loaded.siteZoom.size() == 2 && loaded.siteZoom["example.com"] == 1.5);
    CHECK(loaded.userAgent == "firefox-macos");
    for (const auto& preset : UserAgentPresets()) {
        profile.userAgent = preset.id;
        CHECK(profile.Save(path, error));
        CHECK(Profile::Load(path, error).userAgent == preset.id && error.empty());
    }
    CHECK(loaded.unpinnedExtensions == std::set<std::string>({"ext-1", "ext-2"}));
    CHECK(loaded.trustedCertificates.size() == 2 && loaded.trustedCertificates[0].subject == "/CN=UniFi"
        && loaded.trustedCertificates[0].added == 100 && loaded.trustedCertificates[1].sha256 == std::string(64, 'b'));
    CHECK(loaded.RemoveBookmark("https://bar.example/") && !loaded.RemoveBookmark("https://bar.example/"));
    CHECK(!loaded.bookmarks.Contains("https://bar.example/") && loaded.bookmarks.Contains("https://webkit.org"));
    struct stat mode{};
    CHECK(stat(path.c_str(), &mode) == 0 && (mode.st_mode & 0777) == 0600);
    profile.windows = {{{{"https://different.example/", "Replacement"}}, 999}};
    CHECK(profile.Save(path, error));
    loaded = Profile::Load(path, error);
    CHECK(loaded.windows.size() == 1 && loaded.windows[0].tabs[0].title == "Replacement" && loaded.windows[0].selected == 0);
    // A profile written by a build without windows restores as one window.
    { std::ofstream out(path); out << R"({"version":1,"tabs":[{"url":"https://old.example/","title":"Old"}],"selected":0,)"
        R"("bookmarks":[],"history":[],"interfaceStyle":"nonsense"})"; }
    loaded = Profile::Load(path, error);
    CHECK(error.empty() && loaded.windows.size() == 1 && loaded.windows[0].tabs[0].url == "https://old.example/");
    CHECK(loaded.userAgent == "default");
    CHECK(loaded.interfaceStyle == "haiku" && loaded.searchEngine == "duckduckgo" && loaded.siteZoom.empty()
        && loaded.unpinnedExtensions.empty());
    // Unknown engines and unusable zoom entries fall back instead of failing the profile.
    { std::ofstream out(path); out << R"({"version":1,"tabs":[],"selected":0,"bookmarks":[],"history":[],)"
        R"("searchEngine":"altavista","userAgent":"unrecognised","siteZoom":{"a.example":7,"b.example":1.2,"":1.1},)"
        R"("trustedCertificates":[{"host":"","sha256":"00"},{"host":"nas.lan","sha256":"not hex"},)"
        R"({"host":"nas.lan","sha256":")" << std::string(64, 'c') << R"("}]})"; }
    loaded = Profile::Load(path, error);
    CHECK(error.empty() && loaded.searchEngine == "duckduckgo" && loaded.siteZoom.size() == 1 && loaded.siteZoom["b.example"] == 1.2);
    CHECK(loaded.userAgent == "default");
    CHECK(loaded.trustedCertificates.size() == 1 && loaded.trustedCertificates[0].host == "nas.lan"
        && loaded.trustedCertificates[0].subject.empty());
    { std::ofstream out(path); out << "{broken"; }
    loaded = Profile::Load(path, error);
    CHECK(!error.empty() && loaded.windows.empty());
    CHECK(std::filesystem::file_size(path) == 7);
    CHECK(!profile.Save(path / "impossible.json", error) && !error.empty());
    CHECK(std::filesystem::file_size(path) == 7);

    // #93: every save keeps the file it replaces, and the day's first save a
    // dated copy; a damaged profile loads from the newest readable copy.
    {
        const auto folder = std::filesystem::path(created) / "backups";
        const auto file = folder / "profile.json";
        std::tm noon { }; // 19 October 2026 at noon, local time
        noon.tm_year = 126; noon.tm_mon = 9; noon.tm_mday = 19; noon.tm_hour = 12; noon.tm_isdst = -1;
        const int64_t monday = static_cast<int64_t>(std::mktime(&noon));
        Profile first;
        first.homeURL = "https://first.example/";
        CHECK(first.Save(file, error, monday) && error.empty());
        CHECK(!std::filesystem::exists(folder / "profile.json.1"));
        CHECK(Profile::Backups(file).size() == 1);
        Profile second = first;
        second.homeURL = "https://second.example/";
        CHECK(second.Save(file, error, monday + 60));
        CHECK(Profile::Load(folder / "profile.json.1", error).homeURL == "https://first.example/" && error.empty());
        auto copies = Profile::Backups(file);
        CHECK(copies.size() == 2 && copies[0].filename() == "profile.json.1");
        CHECK(Profile::Load(copies[1], error).homeURL == "https://first.example/");
        // One dated copy a day, for the last kDailyBackups days.
        for (int day = 1; day <= 4; ++day) {
            Profile later = second;
            later.homeURL = "https://day" + std::to_string(day) + ".example/";
            CHECK(later.Save(file, error, monday + day * 86400));
        }
        copies = Profile::Backups(file);
        CHECK(copies.size() == 1 + Profile::kDailyBackups);
        CHECK(Profile::Load(copies[1], error).homeURL == "https://day4.example/");
        CHECK(Profile::Load(copies.back(), error).homeURL == "https://day2.example/");
        CHECK(Profile::Load(file, error).homeURL == "https://day4.example/");
        // Readable: nothing to restore.
        auto load = Profile::LoadOrRestore(file, monday);
        CHECK(load.error.empty() && load.restoredFrom.empty() && load.writable && load.profile.homeURL == "https://day4.example/");
        // Zero-filled, as BFS leaves a file whose blocks never reached the disk.
        { std::ofstream out(file, std::ios::binary); out << std::string(4096, '\0'); }
        load = Profile::LoadOrRestore(file, monday + 5 * 86400);
        CHECK(!load.error.empty() && load.writable && load.restoredFrom == folder / "profile.json.1");
        CHECK(load.profile.homeURL == "https://day3.example/" && load.restoredTime > 0);
        CHECK(!std::filesystem::exists(file) && std::filesystem::exists(load.damagedCopy)
            && load.damagedCopy.filename().string().rfind("profile.damaged-", 0) == 0
            && std::filesystem::file_size(load.damagedCopy) == 4096);
        // Saving goes on.
        CHECK(load.profile.Save(file, error, monday + 5 * 86400));
        CHECK(Profile::Load(file, error).homeURL == "https://day3.example/" && error.empty());
        // Truncated, with the newest copy damaged too: an older one.
        { std::ofstream out(file, std::ios::binary | std::ios::trunc); out << R"({"version":1,"tabs":[)"; }
        { std::ofstream out(folder / "profile.json.1", std::ios::binary | std::ios::trunc); out << "{"; }
        load = Profile::LoadOrRestore(file, monday + 5 * 86400 + 1);
        CHECK(!load.error.empty() && load.writable && load.restoredFrom.filename().string().rfind("profile-backup-", 0) == 0);
        CHECK(load.profile.homeURL == "https://day3.example/");
        // A save cut short between its renames leaves only the copies.
        std::filesystem::remove(file);
        load = Profile::LoadOrRestore(file, monday + 6 * 86400);
        CHECK(!load.error.empty() && !load.restoredFrom.empty() && load.damagedCopy.empty() && load.writable);
        // Nothing readable at all: a fresh profile, the damaged file kept.
        for (const auto& copy : Profile::Backups(file)) std::filesystem::remove(copy);
        { std::ofstream out(file); out << "garbage"; }
        load = Profile::LoadOrRestore(file, monday + 6 * 86400 + 2);
        CHECK(!load.error.empty() && load.restoredFrom.empty() && load.writable && !load.damagedCopy.empty());
        CHECK(load.profile.homeURL.empty() && !std::filesystem::exists(file));
        // A newer Summit's profile is left as it is, and not written.
        { std::ofstream out(file); out << R"({"version":2,"tabs":[],"selected":0,"bookmarks":[],"history":[]})"; }
        load = Profile::LoadOrRestore(file, monday + 7 * 86400);
        CHECK(!load.error.empty() && !load.writable && load.damagedCopy.empty() && std::filesystem::exists(file));
        // A first start.
        load = Profile::LoadOrRestore(folder / "new" / "profile.json", monday);
        CHECK(load.error.empty() && load.writable && load.restoredFrom.empty());
    }
    // #93: one Summit per profile folder.
    {
        const auto folder = std::filesystem::path(created) / "locked";
        std::filesystem::create_directories(folder);
        ProfileLock first, second;
        int64_t holder = -1;
        CHECK(first.Take(folder, 1234, holder, error) == ProfileLock::Result::Taken && first.Holds() && error.empty());
        CHECK(second.Take(folder, 99, holder, error) == ProfileLock::Result::Held && holder == 1234 && !second.Holds());
        first.Release();
        CHECK(second.Take(folder, 99, holder, error) == ProfileLock::Result::Taken && second.Holds());
        CHECK(first.Take(folder, 1234, holder, error) == ProfileLock::Result::Held && holder == 99);
        CHECK(first.Take(folder / "missing", 1, holder, error) == ProfileLock::Result::Unsupported && !error.empty());
    }
    // #89, #95: what the error page says, and what is no error.
    CHECK(ClassifyLoadFailure("CurlErrorDomain", 6) == LoadFailure::HostNotFound);
    CHECK(ClassifyLoadFailure("CurlErrorDomain", 7) == LoadFailure::Refused);
    CHECK(ClassifyLoadFailure("CurlErrorDomain", 56) == LoadFailure::Reset);
    CHECK(ClassifyLoadFailure("CurlErrorDomain", 28) == LoadFailure::TimedOut);
    CHECK(ClassifyLoadFailure("CurlErrorDomain", 35) == LoadFailure::Secure);
    CHECK(ClassifyLoadFailure("CurlErrorDomain", 3) == LoadFailure::Other);
    CHECK(ClassifyLoadFailure("WebKitErrorDomain", 204) == LoadFailure::None && IsMediaTakeover("WebKitErrorDomain", 204));
    CHECK(!IsMediaTakeover("CurlErrorDomain", 204) && !IsMediaTakeover("WebKitErrorDomain", 102));
    CHECK(ClassifyLoadFailure("WebKitErrorDomain", 102) == LoadFailure::None);
    CHECK(ClassifyLoadFailure("WebKitNetworkError", 302) == LoadFailure::None);
    CHECK(ClassifyLoadFailure("WebKitErrorDomain", 101) == LoadFailure::Other);
    CHECK(LoadFailureHost("https://user@Example.com:8443/path?q") == "Example.com");
    CHECK(LoadFailureHost("http://[::1]:8080/") == "::1" && LoadFailureHost("about:blank") == "about:blank");
    CHECK(DescribeLoadFailure(LoadFailure::HostNotFound, "https://nowhere.invalid/x", "").title == "Can't find nowhere.invalid");
    CHECK(DescribeLoadFailure(LoadFailure::Offline, "https://a.example/", "").title == "You're offline");
    CHECK(DescribeLoadFailure(LoadFailure::Other, "file:///x", "File does not exist").explanation == "File does not exist");
    std::filesystem::remove_all(created);
    std::cout << checks << " checks, " << failures << " failures\n";
    return failures ? 1 : 0;
}
