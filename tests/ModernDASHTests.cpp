// Real native DASH playback, transport failures and decoder regression checks.
#define SUMMIT_PDF_HELPERS_ONLY
#include "ModernPDFTests.cpp"
#include <WebKit/WebKitDiagnostics.h>
#include <cmath>

class DASHTests final : public BApplication {
public:
    DASHTests(char** argv, status_t& status)
        : BApplication("application/x-vnd.Summit-DASHTests", &status), m_base(argv[1]), m_bundle(argv[2]), m_capture(argv[3]) { }
    int Result() const { return m_failed ? 1 : 0; }
    void ReadyToRun() override
    {
        try {
            Require(BWebKitInitialize() == B_OK, "initialize native DASH WebKit");
            m_context = std::make_shared<BWebKitContext>(nullptr, true);
            Require(m_context->InitCheck() == B_OK, "create isolated private DASH context");
            m_window = new BWindow(BRect(60, 60, 1139, 909), "Summit DASH tests", B_TITLED_WINDOW, B_ASYNCHRONOUS_CONTROLS);
            m_view = new BWebKitView(m_window->Bounds(), "dash-page", BMessenger(this), B_FOLLOW_ALL, m_context);
            m_window->AddChild(m_view); Require(m_view->InitCheck() == B_OK, "create native DASH view");
            m_window->Show(); SetPulseRate(100000);
            m_worker = std::thread([this] {
                try { RunTests(); }
                catch (const std::exception& error) { std::fprintf(stderr, "DASH_FAILURE %s\n", error.what()); m_failed = true; }
                m_done = true;
            });
        } catch (const std::exception& error) { std::fprintf(stderr, "DASH_FAILURE %s\n", error.what()); m_failed = true; m_done = true; SetPulseRate(100000); }
    }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_INSPECTOR_MESSAGES) return;
        if (message->what == B_WEBKIT_STATE_CHANGED) { std::lock_guard lock(m_mutex); m_state = *message; return; }
        BApplication::MessageReceived(message);
    }
    void Pulse() override
    {
        if (!m_done) return;
        if (!m_closing) {
            m_closing = system_time(); if (m_worker.joinable()) m_worker.join(); m_session.reset();
            if (m_window && m_window->Lock()) { m_window->Quit(); m_window = nullptr; m_view = nullptr; }
            m_context.reset();
        }
        if (!HasHelpers(m_bundle) && !BWebKitHasPendingNativeUI()) {
            std::printf("DASH_RESULT %s checks=%u\n", m_failed ? "FAIL" : "PASS", checks);
            m_finished = true; PostMessage(B_QUIT_REQUESTED);
        } else if (system_time() - m_closing > 30000000) { std::fputs("DASH_FAILURE helper teardown deadline\n", stderr); _Exit(1); }
    }
    bool QuitRequested() override { return m_finished; }
private:
    void Wait(const std::function<bool()>& predicate, const char* description, bigtime_t duration = 45000000)
    {
        auto deadline = system_time() + duration;
        while (system_time() < deadline) { if (predicate()) { Require(true, description); return; } snooze(100000); }
        Require(false, description);
    }
    void Navigate(const std::string& url)
    {
        Require(m_window->Lock(), "lock DASH view for navigation"); m_view->LoadURL(url.c_str()); m_window->Unlock();
        Wait([&] { std::lock_guard lock(m_mutex); return std::string(m_state.GetString("url", "")) == url && !m_state.GetBool("loading", true); }, "DASH fixture page finishes navigation");
    }
    void WaitScript(Inspector& inspector, const std::string& expression, const char* description, bigtime_t duration = 45000000)
    {
        try { Wait([&] { return inspector.Evaluate(expression) == true; }, description, duration); }
        catch (...) { std::fprintf(stderr, "DASH_STATE %s\n", inspector.Evaluate("dashTest.state()").dump().c_str()); throw; }
    }
    void Load(Inspector& inspector, const char* mode)
    {
        Require(inspector.Evaluate("dashTest.load(" + JSON(mode).dump() + ")") == true, "load DASH presentation in the media element");
    }
    void Ready(Inspector& inspector)
    {
        WaitScript(inspector, "video.readyState>=2 && video.videoWidth>0 && !video.error", "DASH decodes a first frame while paused");
        Require(inspector.Evaluate("dashTest.pixels().colored>500") == true, "DASH canvas contains decoded colorful video pixels");
    }
    void Play(Inspector& inspector)
    {
        inspector.Evaluate("(()=>{window.playResult=null;dashTest.play().then(r=>window.playResult=r);return true})()");
        WaitScript(inspector, "window.playResult===true", "DASH play promise resolves");
    }
    void CheckPlaybackRate(Inspector& inspector, double rate)
    {
        inspector.Evaluate("video.playbackRate=" + std::to_string(rate));
        auto sample = [&] { return inspector.Evaluate("[video.currentTime,performance.now()/1000]"); };
        auto start = sample();
        snooze(1000000);
        auto end = sample();
        double elapsed = end[1].get<double>() - start[1].get<double>();
        double advanced = end[0].get<double>() - start[0].get<double>();
        Require(std::abs(advanced / elapsed - rate) < 0.15, "media time advances at the requested playback rate");
    }
    void CheckFrameTimestamps(Inspector& inspector)
    {
        inspector.Evaluate(R"JS(
            window.frameTimes = [];
            video.requestVideoFrameCallback(function collect(now, metadata) {
                frameTimes.push(metadata.mediaTime);
                if (frameTimes.length < 70)
                    video.requestVideoFrameCallback(collect);
            });
        )JS");
        WaitScript(inspector, "frameTimes.length === 70", "receive 70 MSE video frame callbacks");
        // The fixture has one frame every 1/30 s. Allow for the decoder's
        // microsecond precision and for callbacks that skip frames.
        Require(inspector.Evaluate(R"JS(frameTimes.every((time, i) =>
            Math.abs(time - Math.round(time * 30) / 30) <= 0.000001
            && (!i || time > frameTimes[i - 1])))JS") == true,
            "MSE callbacks report successive presentation timestamps from the 30 fps fixture");
    }
    void WaitForIdleTimer()
    {
        auto team = JSON::parse(m_view->Diagnostics()).at("page").at("webProcess").get<team_id>();
        Wait([&] {
            auto report = JSON::parse(BWebKitProcessDiagnostics(team, R"({"topics":"media"})"));
            for (const auto& element : report.at("media").at("elements")) {
                if (element.at("connected") == true && element.at("paused") == true
                    && element.at("player").at("playbackTimerActive") == false)
                    return true;
            }
            return false;
        }, "paused on-demand DASH stops its playback timer");
    }
    void Click(Inspector& inspector, const char* id)
    {
        auto point = inspector.Evaluate("(()=>{const r=document.getElementById(" + JSON(id).dump() + ").getBoundingClientRect();return [r.x+r.width/2,r.y+r.height/2]})()");
        Require(point.is_array(), "locate the native audio playback control");
        BPoint where(point[0].get<float>(), point[1].get<float>()); BMessenger view(m_view);
        for (uint32 what : {B_MOUSE_DOWN, B_MOUSE_UP}) {
            BMessage message(what); message.AddPoint("where", where); message.AddPoint("be:view_where", where);
            message.AddInt32("buttons", what == B_MOUSE_DOWN ? B_PRIMARY_MOUSE_BUTTON : 0);
            message.AddInt32("clicks", 1); message.AddInt32("modifiers", 0); message.AddInt64("when", system_time());
            Require(view.SendMessage(&message) == B_OK, "send native DASH play pointer event");
        }
    }
    void Seek(Inspector& inspector, double position)
    {
        auto expression = "(()=>{window.seekDone=false;video.addEventListener('seeked',()=>window.seekDone=true,{once:true});video.currentTime=" + std::to_string(position) + ";return true})()";
        inspector.Evaluate(expression);
        WaitScript(inspector, "window.seekDone && !video.seeking && Math.abs(video.currentTime-" + std::to_string(position) + ")<0.2 && video.readyState>=2", "DASH seek resolves with a decoded frame at the requested position");
    }
    void RunTests()
    {
        auto page = m_base + "/test.html?run=native-" + std::to_string(getpid()); Navigate(page);
        Require(m_window->Lock(), "lock DASH view for Inspector attachment");
        m_session = BWebKitInspectorSession::Create(*m_view, BMessenger(this)); m_window->Unlock(); Inspector inspector(m_session);
        WaitScript(inspector, "typeof dashTest==='object'", "connect Inspector to the loaded fixture");
        Require(inspector.Evaluate("video.canPlayType('application/dash+xml')!=='' && video.canPlayType('application/dash+xml; codecs=\"vp9\"')==='' && MediaSource.isTypeSupported('video/mp4; codecs=\"avc1.64001e\"')") == true,
            "DASH capability reports implemented codecs and preserves MSE support");
        Load(inspector, "fast"); Ready(inspector);
        Require(inspector.Evaluate("video.duration===18 && video.seekable.length===1 && video.seekable.start(0)===0 && video.seekable.end(0)===18") == true,
            "finite DASH exposes duration and seekable range");
        Click(inspector, "play");
        WaitScript(inspector, "video.currentTime>2 && !video.muted && video.getVideoPlaybackQuality().totalVideoFrames>30", "native user gesture plays DASH video with audio enabled");
        snooze(700000); Capture(m_capture);
        CheckPlaybackRate(inspector, 2);
        CheckPlaybackRate(inspector, 0.5);
        CheckPlaybackRate(inspector, 1);
        inspector.Evaluate("video.pause()"); auto time = inspector.Evaluate("video.currentTime").get<double>(); snooze(600000);
        Require(std::abs(inspector.Evaluate("video.currentTime").get<double>() - time) < 0.05, "paused DASH clock remains stable");
        WaitForIdleTimer();
        Seek(inspector, 12.4); Require(inspector.Evaluate("dashTest.pixels().colored>500") == true, "seeking retains a decoded video picture");
        Seek(inspector, 18); Seek(inspector, 16); Play(inspector);
        WaitScript(inspector, "video.ended && video.currentTime===18", "finite DASH reaches a clean ended state");

        Load(inspector, "adaptive"); Ready(inspector); Play(inspector);
        WaitScript(inspector, "dashTest.state().widths.join(',').includes('320,640,320')", "DASH switches up and down as measured network throughput changes", 65000000);
        Require(inspector.Evaluate("video.currentTime>5 && !video.error") == true, "adaptive playback continues after decoder quality changes");
        inspector.Evaluate("video.pause()");
        Load(inspector, "range"); Ready(inspector); Seek(inspector, 10);
        Require(inspector.Evaluate("video.duration===18 && !video.error") == true, "SegmentList byte-range presentation renders and seeks");
        for (const char* mode : {"bad", "bad-range", "missing", "auth"}) {
            Load(inspector, mode); WaitScript(inspector, "video.error!==null", "invalid or unauthorized DASH resources report a media error");
        }
        Navigate(m_base + "/session"); Navigate(page); WaitScript(inspector, "typeof dashTest==='object'", "authenticated fixture page is ready");
        Load(inspector, "auth"); Ready(inspector); Play(inspector);
        WaitScript(inspector, "video.currentTime>1", "DASH requests preserve the private context's HTTP session cookie");
        Load(inspector, "live"); Ready(inspector);
        Require(inspector.Evaluate("video.duration===Infinity && video.seekable.length===1 && video.seekable.end(0)<=10") == true, "live DASH exposes an infinite duration and a finite live window");
        auto end = inspector.Evaluate("video.seekable.end(0)").get<double>();
        WaitScript(inspector, "video.paused && video.seekable.end(0)>" + std::to_string(end + 1), "paused live DASH still refreshes its seekable window");
        Play(inspector);
        WaitScript(inspector, "video.seekable.end(0)>" + std::to_string(end + 2) + " && video.currentTime>3", "live MPD refresh advances playback and the seekable window");
        inspector.Evaluate("video.pause()");
        Require(inspector.Evaluate("dashTest.clear()") == true, "cancel the active live media load");
        Load(inspector, "fast"); inspector.Evaluate("dashTest.clear()"); Load(inspector, "fast"); Ready(inspector);
        Require(inspector.Evaluate("dashTest.loadMSE()") == true, "switch from direct DASH to JavaScript MediaSource");
        Ready(inspector); Play(inspector);
        WaitScript(inspector, "video.currentTime>1 && !window.mseError", "existing MSE audio/video playback remains functional");
        CheckFrameTimestamps(inspector);
        CheckPlaybackRate(inspector, 2);
        CheckPlaybackRate(inspector, 0);
        CheckPlaybackRate(inspector, 1);
        Navigate(m_base + "/policy.html?run=policy-" + std::to_string(getpid()));
        WaitScript(inspector, "typeof dashTest==='object'", "media policy fixture is ready"); Load(inspector, "fast");
        WaitScript(inspector, "video.error!==null", "DASH respects the document's media-src policy");
        Navigate(page); WaitScript(inspector, "typeof dashTest==='object'", "close-during-playback fixture is ready");
        Load(inspector, "fast"); Ready(inspector); Play(inspector);
    }
    std::string m_base, m_bundle, m_capture;
    std::shared_ptr<BWebKitContext> m_context; std::shared_ptr<BWebKitInspectorSession> m_session;
    BWindow* m_window {nullptr}; BWebKitView* m_view {nullptr};
    std::thread m_worker; std::mutex m_mutex; BMessage m_state;
    std::atomic<bool> m_done {false}, m_failed {false}; bigtime_t m_closing {0}; bool m_finished {false};
};

int main(int argc, char** argv)
{
    if (argc != 4) { std::fprintf(stderr, "Usage: %s BASE_URL BUNDLE SCREENSHOT_PPM\n", argv[0]); return 2; }
    status_t status; DASHTests app(argv, status); if (status != B_OK) return 1;
    app.Run(); return app.Result();
}
