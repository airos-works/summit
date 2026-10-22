#include "ExtensionPermissionPrompt.h"

#if SUMMIT_MODERN_WEBKIT

#include <WebKit/WebKitExtensionPermission.h>
#include <Button.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <MessageRunner.h>
#include <ScrollView.h>
#include <StringView.h>
#include <TextView.h>
#include <Window.h>
#include <algorithm>
#include <cstring>
#include <utility>

namespace summit {
namespace {

constexpr uint32 kDecision = 'epde';
constexpr uint32 kAllow = 'epal';
constexpr uint32 kDeny = 'epdn';
constexpr uint32 kDismiss = 'epdi';

// Extension names and permission strings must not inject layout or bidi
// controls into the browser's consent text. Preserve ordinary Unicode text.
std::string DisplayText(const char* text)
{
    std::string result;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text);
    for (size_t index = 0, length = std::strlen(text); index < length;) {
        auto c = bytes[index];
        if (c < 0x20 || c == 0x7f) {
            result += ' ';
            ++index;
        } else if ((index + 1 < length && c == 0xc2 && bytes[index + 1] >= 0x80 && bytes[index + 1] <= 0x9f)
            || (index + 1 < length && c == 0xd8 && bytes[index + 1] == 0x9c)) {
            result += ' ';
            index += 2;
        } else if (index + 2 < length && c == 0xe2
            && ((bytes[index + 1] == 0x80 && ((bytes[index + 2] >= 0x8b && bytes[index + 2] <= 0x8f)
                    || (bytes[index + 2] >= 0xa8 && bytes[index + 2] <= 0xae)))
                || (bytes[index + 1] == 0x81 && bytes[index + 2] >= 0xa6 && bytes[index + 2] <= 0xa9))) {
            result += ' ';
            index += 3;
        } else {
            result += static_cast<char>(c);
            ++index;
        }
    }
    return result;
}

const char* PermissionDescription(const char* permission)
{
    struct Description { const char* name; const char* text; };
    static constexpr Description descriptions[] {
        { "tabs", "See open tabs and their addresses" },
        { "history", "Read and change browsing history" },
        { "bookmarks", "Read and change bookmarks" },
        { "downloads", "Manage downloads" },
        { "cookies", "Access cookies on permitted sites" },
        { "clipboardRead", "Read the clipboard" },
        { "clipboardWrite", "Write to the clipboard" },
        { "notifications", "Show notifications" },
        { "nativeMessaging", "Communicate with installed applications" },
        { "webRequest", "Observe requests to permitted sites" },
        { "scripting", "Run scripts on permitted sites" },
        { "userScripts", "Run user scripts you add on permitted sites" },
        { "storage", "Store extension data" }
    };
    for (auto& description : descriptions) {
        if (!std::strcmp(permission, description.name))
            return description.text;
    }
    return nullptr;
}

class PermissionWindow final : public BWindow {
public:
    PermissionWindow(const std::string& identifier, const std::string& body, bigtime_t deadline,
        BMessenger target, std::shared_ptr<std::atomic<unsigned>> count)
        : BWindow(BRect(0, 0, 580, 420), "Extension permission request", B_TITLED_WINDOW_LOOK,
            B_MODAL_APP_WINDOW_FEEL, B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_ASYNCHRONOUS_CONTROLS | B_CLOSE_ON_ESCAPE)
        , fIdentifier(identifier), fTarget(target), fCount(std::move(count))
    {
        ++*fCount;
        auto* heading = new BStringView("heading", "Allow additional extension access?");
        heading->SetFont(be_bold_font);
        auto* text = new BTextView("permission-details");
        text->SetText(body.c_str());
        text->MakeEditable(false);
        text->MakeSelectable(true);
        text->SetWordWrap(true);
        text->SetViewColor(ui_color(B_DOCUMENT_BACKGROUND_COLOR));
        text->SetLowColor(text->ViewColor());
        auto textColor = ui_color(B_DOCUMENT_TEXT_COLOR);
        text->SetFontAndColor(be_plain_font, B_FONT_ALL, &textColor);
        text->SetInsets(10, 10, 10, 10);
        auto* scroll = new BScrollView("permission-list", text, 0, false, true);
        scroll->SetExplicitMinSize(BSize(320, 160));
        auto* deny = new BButton("deny", "Deny", new BMessage(kDeny));
        auto* allow = new BButton("allow", "Allow", new BMessage(kAllow));
        BLayoutBuilder::Group<>(this, B_VERTICAL, 12)
            .SetInsets(16)
            .Add(heading)
            .Add(scroll)
            .AddGroup(B_HORIZONTAL, 12)
                .AddGlue()
                .Add(deny)
                .Add(allow)
            .End();
        SetDefaultButton(deny);
        deny->MakeFocus(true);
        CenterOnScreen();
        MoveOnScreen();
        BMessage expired(kDismiss);
        fExpiry = std::make_unique<BMessageRunner>(BMessenger(this), &expired,
            std::max<bigtime_t>(1, deadline - system_time()), 1);
        if (fExpiry->InitCheck() != B_OK)
            PostMessage(kDeny);
    }

    ~PermissionWindow() override { --*fCount; }

    bool QuitRequested() override
    {
        Decide(false);
        return true;
    }

    void MessageReceived(BMessage* message) override
    {
        if (message->what == kDismiss || message->what == kDeny || message->what == kAllow) {
            Decide(message->what == kAllow);
            Quit();
            return;
        }
        BWindow::MessageReceived(message);
    }

private:
    void Decide(bool allowed)
    {
        if (std::exchange(fDecided, true))
            return;
        BMessage response(kDecision);
        response.AddString("identifier", fIdentifier.c_str());
        response.AddBool("allowed", allowed);
        // Never block the window while the application is closing it.
        auto error = fTarget.SendMessage(&response, static_cast<BHandler*>(nullptr), 0);
        if (error == B_WOULD_BLOCK || error == B_TIMED_OUT)
            BMessageRunner::StartSending(fTarget, &response, 10000, 1);
    }

    std::string fIdentifier;
    BMessenger fTarget;
    std::shared_ptr<std::atomic<unsigned>> fCount;
    std::unique_ptr<BMessageRunner> fExpiry;
    bool fDecided = false;
};

} // namespace

ExtensionPermissionPrompt::ExtensionPermissionPrompt(Respond respond)
    : BHandler("Extension permission consent"), fRespond(std::move(respond))
    , fOpenWindows(std::make_shared<std::atomic<unsigned>>(0))
{
}

ExtensionPermissionPrompt::~ExtensionPermissionPrompt() { Shutdown(); }

void ExtensionPermissionPrompt::Dismiss()
{
    fIdentifier.clear();
    fDeadline = 0;
    if (fWindow.IsValid()) {
        // All window-to-application messages are asynchronous, so taking its
        // lock cannot deadlock against a synchronous decision reply.
        if (fWindow.LockTarget()) {
            BLooper* looper = nullptr;
            fWindow.Target(&looper);
            if (looper)
                looper->Quit();
        }
    }
    fWindow = { };
}

void ExtensionPermissionPrompt::Shutdown()
{
    fShutdown = true;
    Dismiss();
    fRespond = { };
}

bool ExtensionPermissionPrompt::HasOpenWindows() const { return fOpenWindows->load() != 0; }

void ExtensionPermissionPrompt::MessageReceived(BMessage* message)
{
    const char* identifier = nullptr;
    if (message->what != B_WEBKIT_EXTENSION_PERMISSION_REQUESTED
        && message->what != B_WEBKIT_EXTENSION_PERMISSION_CANCELLED && message->what != kDecision) {
        BHandler::MessageReceived(message);
        return;
    }
    if (message->FindString("identifier", &identifier) != B_OK || !*identifier || std::strlen(identifier) > 128)
        return;
    if (message->what == B_WEBKIT_EXTENSION_PERMISSION_CANCELLED) {
        if (fIdentifier == identifier)
            Dismiss();
        return;
    }
    if (message->what == kDecision) {
        bool allowed = false;
        if (fIdentifier != identifier || message->FindBool("allowed", &allowed) != B_OK)
            return;
        std::string copiedIdentifier = fIdentifier;
        allowed = allowed && system_time() < fDeadline;
        Dismiss();
        if (fRespond)
            fRespond(copiedIdentifier, allowed);
        return;
    }
    if (fShutdown)
        return;
    const char* name = nullptr;
    const char* extensionIdentifier = nullptr;
    bool privateBrowsing = false;
    int64 deadline = 0;
    if (message->FindString("extension_name", &name) != B_OK || !*name
        || message->FindString("extension_identifier", &extensionIdentifier) != B_OK || !*extensionIdentifier
        || message->FindInt64("deadline", &deadline) != B_OK || deadline <= system_time()
        || deadline - system_time() > 120000000
        || message->FindBool("private_browsing", &privateBrowsing) != B_OK
        || message->FlattenedSize() > 131072) {
        if (fRespond)
            fRespond(identifier, false);
        return;
    }
    std::string body = "Extension: " + DisplayText(name) + "\nID: " + DisplayText(extensionIdentifier)
        + (privateBrowsing ? "\nPrivate browsing profile\n" : "\n")
        + "\nThis extension is requesting the following access:\n";
    size_t count = 0;
    for (const char* field : { "permission", "origin" }) {
        const char* value = nullptr;
        bool headingAdded = false;
        for (int32 index = 0; message->FindString(field, index, &value) == B_OK; ++index) {
            if (!headingAdded) {
                body += field[0] == 'p' ? "\nPermissions\n" : "\nSites (read and change site data)\n";
                headingAdded = true;
            }
            if (++count > 256 || !*value) {
                if (fRespond)
                    fRespond(identifier, false);
                return;
            }
            body += "  ";
            if (field[0] == 'p') {
                if (auto* description = PermissionDescription(value))
                    body += std::string(description) + " — ";
            }
            body += DisplayText(value) + "\n";
        }
    }
    if (!count) {
        if (fRespond)
            fRespond(identifier, false);
        return;
    }
    // A newer request replaces a cancelled prompt even if its cancellation
    // notification was lost to a full native message queue.
    Dismiss();
    fIdentifier = identifier;
    fDeadline = deadline;
    auto* window = new PermissionWindow(fIdentifier, body, deadline, BMessenger(this), fOpenWindows);
    fWindow = BMessenger(window);
    window->Show();
}

} // namespace summit

#endif
