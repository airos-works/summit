// Native MIME-only drag source for browser interoperability tests.
// Replies after DELAY_MS so navigation and expired drops can be checked.
// Build on Haiku: g++ -std=c++17 negotiated-drag.cpp -o negotiated-drag -lbe
// Usage: negotiated-drag X Y FILE MIME_TYPE [DELAY_MS]
#include <Application.h>
#include <File.h>
#include <Message.h>
#include <MessageRunner.h>
#include <Messenger.h>
#include <View.h>
#include <Window.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

class SourceView final : public BView {
public:
    SourceView(std::vector<char> data, const char* mime, bigtime_t delay)
        : BView(BRect(0, 0, 300, 60), "source", B_FOLLOW_ALL, B_WILL_DRAW)
        , fData(std::move(data)), fMime(mime), fDelay(delay)
    {
        SetViewColor(70, 130, 200);
    }

    void Draw(BRect) override
    {
        SetHighColor(255, 255, 255);
        DrawString("Drag negotiated MIME data", BPoint(10, 25));
    }

    void MouseDown(BPoint point) override
    {
        BMessage drag(B_SIMPLE_DATA);
        drag.AddString("be:types", fMime.c_str());
        drag.AddInt32("be:actions", B_COPY_TARGET);
        DragMessage(&drag, BRect(point.x - 10, point.y - 10, point.x + 10, point.y + 10));
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == B_COPY_TARGET) {
            if (fMime != message->GetString("be:types", ""))
                return;
            fRequest = std::make_unique<BMessage>(*message);
            BMessage reply('mime');
            BMessageRunner::StartSending(BMessenger(this), &reply, fDelay, 1);
            std::puts("MIME requested");
            std::fflush(stdout);
        } else if (message->what == 'mime' && fRequest) {
            BMessage data(B_MIME_DATA);
            data.AddData(fMime.c_str(), B_MIME_TYPE, fData.data(), fData.size());
            const status_t status = fRequest->SendReply(&data);
            fRequest.reset();
            std::printf("MIME reply status: %ld\n", long(status));
            std::fflush(stdout);
        } else
            BView::MessageReceived(message);
    }

private:
    std::vector<char> fData;
    std::string fMime;
    bigtime_t fDelay;
    std::unique_ptr<BMessage> fRequest;
};

int main(int argc, char** argv)
{
    if (argc < 5 || argc > 6) {
        std::fprintf(stderr, "usage: negotiated-drag X Y FILE MIME_TYPE [DELAY_MS]\n");
        return 2;
    }
    BApplication app("application/x-vnd.Summit-NegotiatedDragSource");
    BFile file(argv[3], B_READ_ONLY);
    off_t size = 0;
    if (file.InitCheck() != B_OK || file.GetSize(&size) != B_OK || size < 1 || size > 16 * 1024 * 1024)
        return 3;
    std::vector<char> data(size);
    if (file.Read(data.data(), size) != size)
        return 4;
    const long delayMs = argc == 6 ? std::strtol(argv[5], nullptr, 10) : 1;
    if (delayMs < 1 || delayMs > 30000)
        return 2;
    const float x = std::atof(argv[1]), y = std::atof(argv[2]);
    auto* window = new BWindow(BRect(x, y, x + 300, y + 60), "issue90-negotiated-source",
        B_FLOATING_WINDOW_LOOK, B_FLOATING_ALL_WINDOW_FEEL,
        B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_AVOID_FOCUS | B_WILL_ACCEPT_FIRST_CLICK | B_QUIT_ON_WINDOW_CLOSE);
    window->AddChild(new SourceView(std::move(data), argv[4], bigtime_t(delayMs) * 1000));
    window->Show();
    app.Run();
}
