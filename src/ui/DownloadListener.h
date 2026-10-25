#pragma once
#if SUMMIT_MODERN_WEBKIT
#include <WebKit/WebKitContext.h>
#include <Entry.h>
#include <FilePanel.h>
#include <Handler.h>
#include <Message.h>
#include <Messenger.h>
#include <Path.h>
#include <Window.h>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>

namespace summit {
// Each engine context has its own application-loop listener. Privacy and
// session lifetime survive the originating window, including queued starts.
// It also asks where to save a download that wants it
// (B_WEBKIT_DOWNLOAD_DESTINATION: an extension's saveAs), one save panel at
// a time.
class DownloadListener final : public BHandler {
public:
    using Receive = std::function<void(const BMessage&, bool privateBrowsing, bool active)>;
    DownloadListener(bool privateBrowsing, std::weak_ptr<BWebKitContext> context, Receive receive)
        : BHandler("download context"), fPrivate(privateBrowsing), fContext(std::move(context)), fReceive(std::move(receive)) { }
    void Deactivate() { fActive = false; }
    void MessageReceived(BMessage* message) override
    {
        switch (message->what) {
        case B_WEBKIT_DOWNLOAD_STARTED:
        case B_WEBKIT_DOWNLOAD_PROGRESS:
        case B_WEBKIT_DOWNLOAD_FINISHED:
            fReceive(*message, fPrivate, fActive);
            return;
        case B_WEBKIT_DOWNLOAD_DESTINATION:
            if (const uint64 identifier = message->GetUInt64("identifier", 0)) {
                fRequests.push_back({identifier, message->GetString("directory", ""), message->GetString("suggested_filename", "")});
                if (!fAsking) AskNext();
            }
            return;
        case kDestinationChosen: {
            entry_ref directory;
            const char* name = nullptr;
            if (!fAsking || fAnswered || message->FindRef("directory", &directory) != B_OK
                || message->FindString("name", &name) != B_OK || !*name) return;
            BPath path(&directory);
            if (path.InitCheck() != B_OK || path.Append(name) != B_OK) return;
            fAnswered = true;
            Answer(fAsking, path.Path());
            return;
        }
        case B_CANCEL:
            // The panel closed, after a choice or without one.
            if (!fAsking) return;
            if (!fAnswered) Answer(fAsking, nullptr);
            fAsking = 0;
            AskNext();
            return;
        default:
            BHandler::MessageReceived(message);
        }
    }
private:
    static constexpr uint32 kDestinationChosen = 'dldc';
    struct Request {
        uint64 identifier;
        std::string directory, filename;
    };
    void Answer(uint64 identifier, const char* path)
    {
        if (auto context = fContext.lock()) context->SetDownloadDestination(identifier, path);
    }
    void AskNext()
    {
        if (fRequests.empty()) return;
        const auto request = std::move(fRequests.front());
        fRequests.pop_front();
        if (!fPanel) {
            BMessenger target(this);
            fPanel = std::make_unique<BFilePanel>(B_SAVE_PANEL, &target, nullptr, 0, false, new BMessage(kDestinationChosen));
        }
        fAsking = request.identifier;
        fAnswered = false;
        if (!request.directory.empty()) fPanel->SetPanelDirectory(request.directory.c_str());
        fPanel->SetSaveText(request.filename.c_str());
        fPanel->Window()->SetTitle("Summit: Save Download As");
        fPanel->Show();
    }
    bool fPrivate, fActive = true;
    std::weak_ptr<BWebKitContext> fContext;
    Receive fReceive;
    std::unique_ptr<BFilePanel> fPanel;
    std::deque<Request> fRequests;
    uint64 fAsking = 0;
    bool fAnswered = false;
};
}
#endif
