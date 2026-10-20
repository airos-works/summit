// Native PDF integration: real viewer frames, mouse input, and byte-exact saving.
#include <WebKit/WebKitView.h>
#include <WebKit/WebKitInspector.h>
#include <Application.h>
#include <Bitmap.h>
#include <Screen.h>
#include <Window.h>
#include <image.h>
#include <unistd.h>
#include "nlohmann/json.hpp"
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <stdexcept>
#include <thread>

using JSON = nlohmann::json;
namespace {
unsigned checks;
void Require(bool value, const char* description)
{
    ++checks;
    std::printf("%s %s\n", value ? "PASS" : "FAIL", description);
    std::fflush(stdout);
    if (!value) throw std::runtime_error(description);
}
std::string Read(const std::string& path)
{
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}
bool HasHelpers(const std::string& bundle)
{
    team_info team; int32 cookie = 0;
    while (get_next_team_info(&cookie, &team) == B_OK) {
        if (team.parent != getpid()) continue;
        int32 images = 0; image_info image;
        while (get_next_image_info(team.team, &images, &image) == B_OK) {
            if (image.type != B_APP_IMAGE) continue;
            if (image.name == bundle + "/WebProcess" || image.name == bundle + "/NetworkProcess") return true;
            break;
        }
    }
    return false;
}
void Capture(const std::string& path)
{
    BScreen screen; BRect frame = screen.Frame(); BBitmap bitmap(frame, B_RGBA32);
    Require(bitmap.InitCheck() == B_OK && screen.ReadBitmap(&bitmap, false, &frame) == B_OK,
        "capture the rendered native PDF viewer");
    std::ofstream stream(path, std::ios::binary);
    const int width = frame.IntegerWidth() + 1, height = frame.IntegerHeight() + 1;
    stream << "P6\n" << width << ' ' << height << "\n255\n";
    for (int y = 0; y < height; ++y) for (int x = 0; x < width; ++x) {
        auto* pixel = static_cast<uint8*>(bitmap.Bits()) + y * bitmap.BytesPerRow() + x * 4;
        const char rgb[] = {char(pixel[2]), char(pixel[1]), char(pixel[0])}; stream.write(rgb, 3);
    }
    Require(stream.good(), "save the native PDF screenshot");
}

// Public inspector API, used only by this external test executable. The
// protocol selects the PDF iframe's context without bypassing web isolation.
class Inspector {
public:
    explicit Inspector(std::shared_ptr<BWebKitInspectorSession> session) : m_session(std::move(session)) { }
    void Drain()
    {
        std::vector<std::string> messages;
        m_session->Take(messages);
        for (const auto& text : messages) {
            if (std::getenv("SUMMIT_PDF_INSPECTOR_TRACE")) std::printf("PDF_PROTOCOL %s\n", text.c_str());
            auto message = JSON::parse(text);
            auto method = message.value("method", "");
            auto parameters = message.value("params", JSON::object());
            if (method == "Target.targetCreated") {
                auto info = parameters.at("targetInfo");
                if (info.value("type", "") != "page") continue;
                auto target = info.at("targetId").get<std::string>();
                if (!info.value("isProvisional", false)) m_target = target;
                Send(target, "Runtime.enable", JSON::object());
                Send(target, "Console.enable", JSON::object());
                Send(target, "Page.enable", JSON::object());
                if (info.value("isPaused", false)) Outer("Target.resume", {{"targetId", target}});
            } else if (method == "Target.didCommitProvisionalTarget") {
                m_target = parameters.at("newTargetId").get<std::string>();
            } else if (method == "Target.targetDestroyed") {
                m_contexts.erase(parameters.at("targetId").get<std::string>());
            } else if (method == "Target.dispatchMessageFromTarget") {
                auto target = parameters.at("targetId").get<std::string>();
                auto inner = JSON::parse(parameters.at("message").get<std::string>());
                if (inner.contains("id")) m_answers[inner.at("id").get<unsigned>()] = inner;
                else if (inner.value("method", "") == "Runtime.executionContextCreated") {
                    auto context = inner.at("params").at("context");
                    m_contexts[target].insert(context.at("id").get<unsigned>());
                } else if (inner.value("method", "") == "Console.messageAdded") {
                    std::printf("PDF_CONSOLE %s\n", inner.at("params").dump().c_str());
                }
            }
        }
    }
    JSON Evaluate(const std::string& expression, unsigned context = 0)
    {
        Drain();
        if (m_target.empty()) return nullptr;
        JSON parameters {{"expression", expression}, {"returnByValue", true}, {"doNotPauseOnExceptionsAndMuteConsole", true}};
        if (context) parameters["contextId"] = context;
        unsigned id = Send(m_target, "Runtime.evaluate", parameters);
        auto deadline = system_time() + 5000000;
        while (system_time() < deadline) {
            Drain();
            if (auto it = m_answers.find(id); it != m_answers.end()) {
                auto response = std::move(it->second); m_answers.erase(it);
                if (response.contains("error")) {
                    if (context && response["error"].value("message", "").starts_with("Missing injected script")) m_contexts[m_target].erase(context);
                    return nullptr;
                }
                const auto result = response.value("result", JSON::object());
                if (result.value("wasThrown", false)) return nullptr;
                return result.value("result", JSON::object()).value("value", JSON());
            }
            snooze(10000);
        }
        throw std::runtime_error("Inspector evaluation timed out");
    }
    unsigned Viewer()
    {
        Drain();
        auto contexts = m_contexts[m_target];
        for (unsigned context : contexts) {
            if (Evaluate("typeof PDFViewerApplication === 'object' && location.protocol === 'webkit-pdfjs-viewer:'", context) == true)
                return context;
        }
        return 0;
    }
private:
    void Outer(const char* method, const JSON& params)
    {
        auto text = JSON{{"id", ++m_outer}, {"method", method}, {"params", params}}.dump();
        m_session->Send(text.c_str());
    }
    unsigned Send(const std::string& target, const char* method, const JSON& params)
    {
        unsigned id = ++m_inner;
        Outer("Target.sendMessageToTarget", {{"targetId", target},
            {"message", JSON{{"id", id}, {"method", method}, {"params", params}}.dump()}});
        return id;
    }
    std::shared_ptr<BWebKitInspectorSession> m_session;
    std::string m_target;
    std::map<std::string, std::set<unsigned>> m_contexts;
    std::map<unsigned, JSON> m_answers;
    unsigned m_outer {0}, m_inner {0};
};
}

#ifndef SUMMIT_PDF_HELPERS_ONLY
class PDFTests final : public BApplication {
public:
    PDFTests(char** argv, status_t& status)
        : BApplication("application/x-vnd.Summit-PDFTests", &status), m_base(argv[1]),
          m_directory(argv[2]), m_bundle(argv[3]), m_golden(argv[4]), m_capture(argv[5]) { }
    int Result() const { return m_failed ? 1 : 0; }
    void ReadyToRun() override
    {
        try {
            Require(BWebKitInitialize() == B_OK, "initialize native WebKit");
            m_context = std::make_shared<BWebKitContext>(nullptr, true);
            Require(m_context->InitCheck() == B_OK, "create an isolated private PDF context");
            Require(m_context->SetDownloadDirectory(m_directory.c_str()) == B_OK
                && m_context->SetDownloadListener(BMessenger(this)) == B_OK, "configure owned download directory");
            m_window = new BWindow(BRect(60, 60, 1139, 909), "Summit PDF tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            m_view = new BWebKitView(m_window->Bounds(), "pdf-page", BMessenger(this), B_FOLLOW_ALL, m_context);
            m_window->AddChild(m_view);
            Require(m_view->InitCheck() == B_OK, "create PDF native view");
            m_window->Show();
            SetPulseRate(100000);
            m_worker = std::thread([this] {
                try { RunTests(); }
                catch (const std::exception& error) { std::fprintf(stderr, "PDF_FAILURE %s\n", error.what()); m_failed = true; }
                m_done = true;
            });
        } catch (const std::exception& error) {
            std::fprintf(stderr, "PDF_FAILURE %s\n", error.what()); m_failed = true; m_done = true; SetPulseRate(100000);
        }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_INSPECTOR_MESSAGES) return;
        if (message->what == B_WEBKIT_STATE_CHANGED) { std::lock_guard lock(m_mutex); m_state = *message; return; }
        if (message->what == B_WEBKIT_DOWNLOAD_FINISHED) {
            std::lock_guard lock(m_mutex); m_downloads.push_back(*message); return;
        }
        BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if (!m_done) return;
        if (!m_closing) {
            m_closing = system_time();
            if (m_worker.joinable()) m_worker.join();
            m_session.reset();
            if (m_window && m_window->Lock()) { m_window->Quit(); m_window = nullptr; m_view = nullptr; }
            if (m_context) m_context->CancelAllDownloads();
            m_context.reset();
        }
        if (!HasHelpers(m_bundle) && !BWebKitHasPendingNativeUI()) {
            std::printf("PDF_RESULT %s checks=%u\n", m_failed ? "FAIL" : "PASS", checks);
            m_finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - m_closing > 30000000) {
            std::fputs("PDF_FAILURE helper teardown deadline\n", stderr); _Exit(1);
        }
    }
    bool QuitRequested() override { return m_finished; }
private:
    void Wait(const std::function<bool()>& predicate, const char* description)
    {
        auto deadline = system_time() + 45000000;
        while (system_time() < deadline) { if (predicate()) { Require(true, description); return; } snooze(100000); }
        Require(false, description);
    }
    void Navigate(const std::string& url)
    {
        Require(m_window->Lock(), "lock native PDF view for navigation");
        m_view->LoadURL(url.c_str()); m_window->Unlock(); snooze(300000);
    }
    void Click(Inspector& inspector, unsigned context, const char* element)
    {
        auto point = inspector.Evaluate("(()=>{const e=document.getElementById(" + JSON(element).dump()
            + "); if(!e || e.disabled)return null; const r=e.getBoundingClientRect(); return [r.x+r.width/2,r.y+r.height/2,r.width,r.height]})()", context);
        Require(point.is_array() && point.size() == 4 && point[2].get<double>() > 0 && point[3].get<double>() > 0,
            "find visible enabled PDF control");
        BPoint where(point[0].get<float>(), point[1].get<float>());
        BMessenger view(m_view);
        for (uint32 what : {B_MOUSE_DOWN, B_MOUSE_UP}) {
            BMessage event(what); event.AddPoint("where", where); event.AddPoint("be:view_where", where);
            event.AddInt32("buttons", what == B_MOUSE_DOWN ? B_PRIMARY_MOUSE_BUTTON : 0);
            event.AddInt32("clicks", 1); event.AddInt32("modifiers", 0); event.AddInt64("when", system_time());
            Require(view.SendMessage(&event) == B_OK, "send real native PDF pointer event");
        }
    }
    unsigned Ready(Inspector& inspector, const std::string& url, int page = 1)
    {
        unsigned context = 0;
        Wait([&] {
            if (inspector.Evaluate("location.href") != url) return false;
            context = inspector.Viewer(); if (!context) return false;
            // PDF.js remembers the last page for a fingerprint. Repeated fixture
            // visits select the first page explicitly before pixel checks.
            inspector.Evaluate("(()=>{const a=PDFViewerApplication;if(a.pdfDocument?.numPages===3 && a.page!==" + std::to_string(page) + ") a.page=" + std::to_string(page) + ";return true})()", context);
            return Rendered(inspector, context, page);
        }, "PDF page renders three-page document with selectable marker text");
        return context;
    }
    bool Rendered(Inspector& inspector, unsigned context, int page)
    {
        return inspector.Evaluate("(()=>{const a=PDFViewerApplication; const v=a.pdfViewer?.getPageView(" + std::to_string(page - 1)
            + ");return a.pdfDocument?.numPages===3 && a.page===" + std::to_string(page)
            + " && v?.renderingState===3 && v.div.querySelector('canvas')?.width>0 && v.div.innerText.includes('PDF_PAGE_"
            + std::to_string(page) + "_MARKER')})()", context) == true;
    }
    void Download(Inspector& inspector, unsigned context)
    {
        size_t previous;
        { std::lock_guard lock(m_mutex); previous = m_downloads.size(); }
        Click(inspector, context, "downloadButton");
        VerifyDownload(previous);
    }
    void VerifyDownload(size_t previous)
    {
        BMessage result;
        Wait([&] { std::lock_guard lock(m_mutex); if (m_downloads.size() <= previous) return false; result = m_downloads.back(); return true; },
            "PDF download finishes through the native download manager");
        uint32 status = ~0U; uint64 bytes = 0; const char* path = nullptr;
        Require(result.FindUInt32("result", &status) == B_OK && status == B_WEBKIT_DOWNLOAD_SUCCEEDED
            && result.FindString("path", &path) == B_OK && result.FindUInt64("current_size", &bytes) == B_OK,
            "PDF save succeeds with a native file and byte count");
        auto golden = Read(m_golden);
        Require(!golden.empty() && std::filesystem::path(path).parent_path() == m_directory
            && Read(path) == golden && bytes == golden.size(), "saved PDF exactly matches the original authorized response bytes");
    }
    void RunTests()
    {
        const auto first = m_base + "/fixture.pdf";
        Navigate(first);
        Wait([&] { std::lock_guard lock(m_mutex); return std::string(m_state.GetString("url", "")) == first && !m_state.GetBool("loading", true); }, "initial PDF navigation finishes before Inspector attachment");
        Require(m_window->Lock(), "lock view for Inspector attachment");
        m_session = BWebKitInspectorSession::Create(*m_view, BMessenger(this)); m_window->Unlock();
        Require(bool(m_session), "connect inspector for PDF frame verification");
        Inspector inspector(m_session); auto context = Ready(inspector, first);
        Require(inspector.Evaluate("PDFViewerApplicationOptions.get('enableScripting')===false && PDFViewerApplicationOptions.get('annotationEditorMode')===-1 && PDFViewerApplicationOptions.get('annotationMode')===1", context) == true,
            "PDF scripting and editing are disabled while annotations remain visible");
        Require(inspector.Evaluate("getComputedStyle(document.getElementById('editorModeButtons')).display==='none'", context) == true,
            "editing controls are absent from the reader toolbar");
        Require(inspector.Evaluate("(()=>{const c=PDFViewerApplication.pdfViewer.getPageView(0).div.querySelector('canvas');const p=c.getContext('2d').getImageData(20,20,1,1).data;return p[0]<70 && p[1]>50 && p[1]<110 && p[2]<100 && p[3]===255})()", context) == true,
            "PDF canvas contains the fixture's rendered green artwork");
        Require(inspector.Evaluate("PDFViewerApplication.pdfViewer.getPageView(0).div.innerText.includes('Ελληνικά')", context) == true,
            "embedded Unicode font produces selectable Greek text");
        inspector.Evaluate("(()=>{window.__pdfResourceChecks=null;Promise.all([fetch('../extras/haiku/style.css').then(r=>r.ok),fetch('../extras/haiku/style.css',{method:'POST'}).then(r=>!r.ok,()=>true),fetch('../not-a-compiled-resource').then(r=>!r.ok,()=>true)]).then(r=>window.__pdfResourceChecks=r);return true})()", context);
        Wait([&] { return inspector.Evaluate("Array.isArray(window.__pdfResourceChecks) && window.__pdfResourceChecks.length===3 && window.__pdfResourceChecks.every(Boolean)", context) == true; },
            "viewer resources support reads and reject writes and unlisted paths");
        snooze(700000); Capture(m_capture);
        Click(inspector, context, "next");
        Wait([&] { return Rendered(inspector, context, 2); }, "native Next button renders page two");
        Click(inspector, context, "next");
        Wait([&] { return Rendered(inspector, context, 3); }, "native Next button renders the landscape page");
        auto scale = inspector.Evaluate("PDFViewerApplication.pdfViewer.currentScale", context).get<double>();
        Click(inspector, context, "zoomInButton");
        Wait([&] { auto value = inspector.Evaluate("PDFViewerApplication.pdfViewer.currentScale", context); return value.is_number() && value.get<double>() > scale; }, "native Zoom In increases the rendered scale");
        Click(inspector, context, "previous");
        Wait([&] { return Rendered(inspector, context, 2); }, "native Previous returns to page two");
        Click(inspector, context, "viewFindButton");
        inspector.Evaluate("(()=>{const e=document.getElementById('findInput');e.value='PDF_PAGE_3_MARKER';e.dispatchEvent(new Event('input',{bubbles:true}));return true})()", context);
        Wait([&] { return inspector.Evaluate("PDFViewerApplication.findController.selected.pageIdx===2 && PDFViewerApplication.findController.selected.matchIdx===0", context) == true; },
            "PDF search finds the marker on the third page");
        Click(inspector, context, "viewFindButton");
        Download(inspector, context);
        Require(inspector.Evaluate("location.href") == first
            && inspector.Evaluate("PDFViewerApplication.pdfDocument.numPages", context) == 3,
            "saving leaves the inline PDF open at its original address");

        const auto password = m_base + "/password.pdf";
        Navigate(password);
        Wait([&] { if (inspector.Evaluate("location.href") != password) return false; context = inspector.Viewer(); return context && inspector.Evaluate("document.getElementById('passwordDialog').open", context) == true; }, "encrypted PDF requests its password");
        inspector.Evaluate("document.getElementById('password').value='wrong-password'", context);
        Click(inspector, context, "passwordSubmit");
        Wait([&] { return inspector.Evaluate("document.getElementById('passwordDialog').open && document.getElementById('passwordText').getAttribute('data-l10n-id')==='pdfjs-password-invalid'", context) == true; }, "wrong PDF password gives a retry prompt");
        inspector.Evaluate("document.getElementById('password').value='summit-test'", context);
        Click(inspector, context, "passwordSubmit"); Ready(inspector, password);

        const auto scripted = m_base + "/scripted.pdf";
        Navigate(scripted); context = Ready(inspector, scripted);
        Require(inspector.Evaluate("PDFViewerApplication.pdfScriptingManager?.ready !== true && PDFViewerApplicationOptions.get('enableScripting') === false", context) == true,
            "PDF with embedded JavaScript renders without running its script");
        Require(CountWindows() == 1, "scripted PDF creates no native script alert");

        Navigate(m_base + "/start");
        Wait([&] { return inspector.Evaluate("document.title") == "PDF session ready"; }, "establish the private context's authenticated HTTP session");
        Navigate(m_base + "/private.pdf"); context = Ready(inspector, m_base + "/private.pdf");
        Download(inspector, context);
        for (const char* name : {"invalid.pdf", "empty.pdf"}) {
            auto url = m_base + "/" + name; Navigate(url);
            Wait([&] { if (inspector.Evaluate("location.href") != url) return false; context = inspector.Viewer(); return context && inspector.Evaluate("!!document.getElementById('summit-pdf-error')?.textContent", context) == true; }, "unreadable PDF displays a useful visible error");
        }
        size_t previous; { std::lock_guard lock(m_mutex); previous = m_downloads.size(); }
        Navigate(m_base + "/attachment.pdf"); VerifyDownload(previous);
        Navigate("file://" + m_golden); Ready(inspector, "file://" + m_golden);
        Require(inspector.Evaluate("document.title") == "fixture.pdf", "local PDF title falls back to its filename");
        for (const char* tag : {"object", "embed", "iframe"}) {
            const auto embedded = m_base + "/embedded?tag=" + tag;
            Navigate(embedded); context = Ready(inspector, embedded);
            Require(inspector.Evaluate("PDFViewerApplicationOptions.get('enableScripting')===false", context) == true,
                "blob PDF under inherited frame-ancestors renders with PDF scripting disabled");
        }
        // Proton Mail's attachment preview (issue #78): an object with a File's
        // blob URL whose fallback content is an embed and a notice.
        for (const char* variant : {"", "?order=late", "?frame=1", "?source=http"}) {
            const auto preview = m_base + "/webmail-preview" + variant;
            Navigate(preview); context = Ready(inspector, preview);
            Wait([&] { return inspector.Evaluate("window.preview?.state") == "shown"; },
                "webmail preview object shows the PDF instead of its fallback notice");
        }
        Require(inspector.Evaluate("navigator.pdfViewerEnabled===true && navigator.plugins.length===5"
            " && [...navigator.mimeTypes].map(m=>m.type).join()==='application/pdf,text/pdf'") == true,
            "navigator reports the built-in PDF viewer and its two MIME types");
        Navigate(m_base + "/embedded?source=http-blocked&tag=iframe");
        Wait([&] { return inspector.Evaluate("window.fixtureLoaded===true") == true; },
            "HTTP PDF with frame-ancestors completes its blocked frame load");
        Require(!inspector.Viewer(), "HTTP frame-ancestors restriction still blocks PDF embedding");
        for (const char* tag : {"object", "iframe"}) {
            Navigate(m_base + "/embedded?policy=block&tag=" + tag);
            Wait([&] { return inspector.Evaluate("window.fixtureViolations?.length>0") == true; },
                "embedding page still enforces object-src and frame-src for blob PDFs");
            Require(!inspector.Viewer(), "disallowed blob PDF does not create a viewer");
        }
        Navigate(m_base + "/embedded?source=html-blob&tag=iframe");
        Wait([&] { return inspector.Evaluate("!!document.querySelector('#pdf')?.contentDocument?.getElementById('local-blob')") == true; },
            "local HTML blob is not blocked by inherited frame-ancestors");
        Require(inspector.Evaluate("document.querySelector('#pdf').contentWindow.forbiddenInlineScript!==true") == true,
            "local HTML blob retains its inherited script-src restriction");
        Navigate("about:blank");
        Wait([&] { return inspector.Evaluate("location.href") == "about:blank"; }, "leave the PDF before checking resource isolation");
        inspector.Evaluate("(()=>{const frame=document.createElement('iframe');frame.src='webkit-pdfjs-viewer://pdfjs/web/viewer.html';document.body.append(frame);return true})()");
        snooze(1500000);
        Require(!inspector.Viewer(), "ordinary HTML iframe cannot access the privileged viewer resources");
        Navigate("webkit-pdfjs-viewer://pdfjs/web/viewer.html"); snooze(1500000);
        Require(!inspector.Viewer(), "ordinary navigation cannot open the privileged viewer resources");
    }
    std::string m_base, m_directory, m_bundle, m_golden, m_capture;
    std::shared_ptr<BWebKitContext> m_context;
    std::shared_ptr<BWebKitInspectorSession> m_session;
    BWindow* m_window {nullptr}; BWebKitView* m_view {nullptr};
    std::thread m_worker; std::mutex m_mutex; std::vector<BMessage> m_downloads; BMessage m_state;
    std::atomic<bool> m_done {false}, m_failed {false};
    bigtime_t m_closing {0}; bool m_finished {false};
};

int main(int argc, char** argv)
{
    if (argc != 6) { std::fprintf(stderr, "Usage: %s BASE_URL DOWNLOAD_DIRECTORY BUNDLE GOLDEN_PDF SCREENSHOT_PPM\n", argv[0]); return 2; }
    status_t status; PDFTests app(argv, status); if (status != B_OK) return 1;
    app.Run(); return app.Result();
}

#endif // SUMMIT_PDF_HELPERS_ONLY
