#include "PrintJob.h"
#include "FeatureMessages.h"
#include <WebKit/WebKitView.h>
#include <Alert.h>
#include <Bitmap.h>
#include <PrintJob.h>
#include <View.h>
#include <Window.h>
#include <algorithm>
#include <climits>
#include <cmath>
#include <cstring>
#include <ctime>
#include <mutex>
#include <thread>

namespace summit {
namespace {
constexpr uint32 kBegin = 'pjbg';
std::mutex sSettingsLock;
// The page settings of the last job, so Page Setup is asked only once.
BMessage* sPageSettings = nullptr;
uint64 sNextIdentifier = 1;

void KeepSettings(BPrintJob& job)
{
    std::lock_guard lock(sSettingsLock);
    if (auto* settings = job.Settings()) {
        delete sPageSettings;
        sPageSettings = settings;
    }
}
}

// One page as the printer gets it: the engine's picture of the page, and
// the title, date, address and page number in its margins.
class PrintPageView : public BView {
public:
    explicit PrintPageView(BRect frame) : BView(frame, "print-page", B_FOLLOW_NONE, B_WILL_DRAW) { }
    void SetPage(std::unique_ptr<BBitmap> bitmap, std::string header, std::string date, std::string footer, std::string number)
    {
        fBitmap = std::move(bitmap);
        fHeader = std::move(header); fDate = std::move(date); fFooter = std::move(footer); fNumber = std::move(number);
    }
    void Draw(BRect) override
    {
        if (fBitmap) DrawBitmap(fBitmap.get(), fBitmap->Bounds(), Bounds(), B_FILTER_BITMAP_BILINEAR);
        BFont font(be_plain_font);
        font.SetSize(8);
        SetFont(&font);
        SetHighColor(70, 70, 70);
        SetDrawingMode(B_OP_OVER);
        const BRect bounds = Bounds();
        auto fit = [&](std::string text, float width) {
            BString string(text.c_str());
            TruncateString(&string, B_TRUNCATE_MIDDLE, width);
            return string;
        };
        const float half = (bounds.Width() - 36) / 2;
        BString header = fit(fHeader, half), date = fit(fDate, half);
        DrawString(header, BPoint(18, 16));
        DrawString(date, BPoint(bounds.right - 18 - StringWidth(date), 16));
        BString footer = fit(fFooter, half), number = fit(fNumber, half);
        DrawString(footer, BPoint(18, bounds.bottom - 10));
        DrawString(number, BPoint(bounds.right - 18 - StringWidth(number), bounds.bottom - 10));
    }
private:
    std::unique_ptr<BBitmap> fBitmap;
    std::string fHeader, fDate, fFooter, fNumber;
};

void PrintJob::Start(int64 tab, const std::string& title, const std::string& url, BMessenger window)
{
    auto* job = new PrintJob(tab, title, url, window);
    job->Run();
    job->PostMessage(kBegin);
}

BSize PrintJob::DefaultPaper()
{
    std::lock_guard lock(sSettingsLock);
    BRect paper;
    if (sPageSettings && sPageSettings->FindRect("paper_rect", &paper) == B_OK && paper.IsValid())
        return BSize(paper.Width(), paper.Height());
    return BSize(595, 842);
}

void PrintJob::ConfigurePage()
{
    std::thread([] {
        BPrintJob job("Summit");
        {
            std::lock_guard lock(sSettingsLock);
            if (sPageSettings) job.SetSettings(new BMessage(*sPageSettings));
        }
        if (job.ConfigPage() == B_OK) KeepSettings(job);
    }).detach();
}

PrintJob::PrintJob(int64 tab, const std::string& title, const std::string& url, BMessenger window)
    : BLooper("print job"), fTab(tab), fTitle(title), fURL(url), fWindow(window)
{
    char date[64] = "";
    const std::time_t now = std::time(nullptr);
    std::tm local;
    if (localtime_r(&now, &local)) std::strftime(date, sizeof(date), "%x %H:%M", &local);
    fDate = date;
}

PrintJob::~PrintJob()
{
    if (fPageWindow) {
        fPageWindow->Lock();
        fPageWindow->Quit();
    }
}

void PrintJob::Begin()
{
    fJob = std::make_unique<BPrintJob>(fTitle.empty() ? "Summit page" : fTitle.c_str());
    {
        std::lock_guard lock(sSettingsLock);
        if (sPageSettings) fJob->SetSettings(new BMessage(*sPageSettings));
    }
    bool needsPage;
    {
        std::lock_guard lock(sSettingsLock);
        needsPage = !sPageSettings;
    }
    // These wait for the user: this looper's thread is the job's own.
    if ((needsPage && fJob->ConfigPage() != B_OK) || fJob->ConfigJob() != B_OK) { Finish(false); return; }
    KeepSettings(*fJob);
    fPrintable = fJob->PrintableRect();
    if (!fPrintable.IsValid() || fPrintable.Width() < 72 || fPrintable.Height() < 72) { Finish(false); return; }
    int32 xDPI = 300, yDPI = 300;
    fJob->GetResolution(&xDPI, &yDPI);
    // Pictures at the printer's resolution, at most 200 dots an inch: text
    // stays sharp and a page stays below 16 MB.
    fScale = std::clamp(std::min(xDPI, yDPI) / 72.0f, 1.0f, 200.0f / 72.0f);
    fFirst = std::max(1, fJob->FirstPage()) - 1;
    fLast = fJob->LastPage() <= 0 || fJob->LastPage() >= INT_MAX / 2 ? INT_MAX : fJob->LastPage() - 1;
    fNext = fFirst;
    fJob->BeginJob();
    if (!fJob->CanContinue()) { Finish(false); return; }
    fPageWindow = new BWindow(BRect(-10000, -10000, -10000 + fPrintable.Width(), -10000 + fPrintable.Height()), "print page",
        B_NO_BORDER_WINDOW_LOOK, B_NORMAL_WINDOW_FEEL, B_AVOID_FOCUS);
    fPageView = new PrintPageView(BRect(0, 0, fPrintable.Width(), fPrintable.Height()));
    fPageWindow->AddChild(fPageView);
    fPageWindow->Hide();
    fPageWindow->Show();
    RequestPage();
}

void PrintJob::RequestPage()
{
    BMessage request(kPrintPagesRequest);
    request.AddInt64("tab", fTab);
    request.AddMessenger("reply", BMessenger(this));
    request.AddUInt64("identifier", sNextIdentifier++);
    request.AddSize("paper", BSize(fPrintable.Width(), fPrintable.Height()));
    // Room for the header and footer lines.
    request.AddRect("margins", BRect(18, 28, 18, 28));
    request.AddInt32("first", fNext);
    request.AddInt32("count", 1);
    request.AddFloat("scale", fScale);
    if (fWindow.SendMessage(&request) != B_OK) Finish(false);
}

void PrintJob::PageReceived(BMessage* message)
{
    if (message->HasString("error")) {
        (new BAlert("Print — Summit", (std::string("Could not print the page: ") + message->GetString("error", "")).c_str(), "OK"))->Go(nullptr);
        Finish(false);
        return;
    }
    fPageCount = message->GetInt32("pageCount", 0);
    const int32 width = message->GetInt32("width", 0), height = message->GetInt32("height", 0);
    const void* pixels = nullptr;
    ssize_t size = 0;
    if (fNext >= fPageCount || message->FindData("page", B_RAW_TYPE, &pixels, &size) != B_OK
        || width <= 0 || height <= 0 || size < ssize_t(width) * height * 4) {
        Finish(fNext > fFirst);
        return;
    }
    auto bitmap = std::make_unique<BBitmap>(BRect(0, 0, width - 1, height - 1), B_RGB32);
    if (bitmap->InitCheck() == B_OK) {
        for (int32 y = 0; y < height; ++y)
            std::memcpy(static_cast<uint8*>(bitmap->Bits()) + y * bitmap->BytesPerRow(),
                static_cast<const uint8*>(pixels) + size_t(y) * width * 4, size_t(width) * 4);
    }
    if (fPageWindow->Lock()) {
        fPageView->SetPage(std::move(bitmap), fTitle, fDate, fURL,
            std::to_string(fNext + 1) + " of " + std::to_string(fPageCount));
        fJob->DrawView(fPageView, fPageView->Bounds(), BPoint(0, 0));
        fPageWindow->Unlock();
    }
    fJob->SpoolPage();
    ++fNext;
    if (!fJob->CanContinue() || fNext > fLast || fNext >= fPageCount) { Finish(true); return; }
    RequestPage();
}

void PrintJob::Finish(bool commit)
{
    if (fJob) {
        if (commit) fJob->CommitJob();
        else fJob->CancelJob();
    }
    PostMessage(B_QUIT_REQUESTED);
}

void PrintJob::MessageReceived(BMessage* message)
{
    switch (message->what) {
    case kBegin: Begin(); return;
    case B_WEBKIT_PRINT_PAGES: PageReceived(message); return;
    }
    BLooper::MessageReceived(message);
}
}
