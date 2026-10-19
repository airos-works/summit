#pragma once
#include <Looper.h>
#include <Messenger.h>
#include <Rect.h>
#include <memory>
#include <string>

class BPrintJob;
class BWindow;

namespace summit {
class PrintPageView;
// Printing a tab (#65) with the Haiku print kit, so that every installed
// printer works, the PDF Writer driver included. The job runs on its own
// looper: the printer's dialogs (page setup, then copies and pages) wait for
// the user there, and the pages come from the engine one at a time as
// pictures (through the tab's window, which owns the view), each with the
// page's title, address, date and number in its margins.
class PrintJob : public BLooper {
public:
    static void Start(int64 tab, const std::string& title, const std::string& url, BMessenger window);
    // Paper for Save as PDF: the size the last print job used, else A4.
    static BSize DefaultPaper();
    // Page Setup… from the File menu (on a thread of its own).
    static void ConfigurePage();
    void MessageReceived(BMessage* message) override;
private:
    PrintJob(int64 tab, const std::string& title, const std::string& url, BMessenger window);
    ~PrintJob() override;
    void Begin();
    void RequestPage();
    void PageReceived(BMessage* message);
    void Finish(bool commit);
    int64 fTab;
    std::string fTitle, fURL, fDate;
    BMessenger fWindow;
    std::unique_ptr<BPrintJob> fJob;
    BWindow* fPageWindow = nullptr;
    PrintPageView* fPageView = nullptr;
    BRect fPrintable;
    float fScale = 2;
    int32 fFirst = 0, fLast = 0, fNext = 0, fPageCount = -1;
};
}
