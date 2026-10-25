#pragma once
#if SUMMIT_MODERN_WEBKIT
#include <WebKit/WebKitContext.h>
#include <Handler.h>
#include <Message.h>
#include <functional>
#include <utility>

namespace summit {
// Each engine context has its own application-loop listener. Privacy and
// session lifetime survive the originating window, including queued starts.
class DownloadListener final : public BHandler {
public:
    using Receive = std::function<void(const BMessage&, bool privateBrowsing, bool active)>;
    DownloadListener(bool privateBrowsing, Receive receive)
        : BHandler("download context"), fPrivate(privateBrowsing), fReceive(std::move(receive)) { }
    void Deactivate() { fActive = false; }
    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_WEBKIT_DOWNLOAD_STARTED || message->what == B_WEBKIT_DOWNLOAD_PROGRESS
            || message->what == B_WEBKIT_DOWNLOAD_FINISHED) fReceive(*message, fPrivate, fActive);
        else BHandler::MessageReceived(message);
    }
private:
    bool fPrivate, fActive = true;
    Receive fReceive;
};
}
#endif
