#include "AuthenticationPrompt.h"

#if SUMMIT_MODERN_WEBKIT

#include <Button.h>
#include <CheckBox.h>
#include <LayoutBuilder.h>
#include <StringView.h>
#include <TextControl.h>
#include <TextView.h>
#include <cstring>

namespace summit {
namespace {
constexpr uint32 kSignIn = 'ausi';
constexpr uint32 kCancel = 'auca';
constexpr uint32 kNameChanged = 'aunm';

// The realm is the server's text: no control or direction characters, and
// not so long that it pushes the rest off the sheet.
std::string DisplayText(const std::string& text, size_t limit)
{
    std::string result;
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.data());
    const size_t length = text.size();
    for (size_t index = 0; index < length && result.size() < limit;) {
        const auto c = bytes[index];
        if (c < 0x20 || c == 0x7f) {
            result += ' ';
            ++index;
        } else if (index + 1 < length && ((c == 0xc2 && bytes[index + 1] >= 0x80 && bytes[index + 1] <= 0x9f)
            || (c == 0xd8 && bytes[index + 1] == 0x9c))) {
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
    if (result.size() >= limit) {
        // Do not cut a UTF-8 sequence in two.
        while (!result.empty() && (static_cast<unsigned char>(result.back()) & 0xc0) == 0x80) result.pop_back();
        if (!result.empty() && static_cast<unsigned char>(result.back()) >= 0xc0) result.pop_back();
        result += "…";
    }
    return result;
}

BStringView* Line(const char* name, const std::string& text)
{
    auto* view = new BStringView(name, text.c_str());
    view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    return view;
}
}

std::string PromptDisplayText(const std::string& text, size_t limit)
{
    return DisplayText(text, limit);
}

AuthenticationPrompt::AuthenticationPrompt(const Fields& fields, BMessenger target)
    : BWindow(BRect(0, 0, 460, 200), "Sign In", B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
        B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS
        | B_CLOSE_ON_ESCAPE),
      fFields(fields), fTarget(target)
{
    const std::string host = DisplayText(fields.host, 120);
    auto* heading = Line("heading", fields.proxy ? "The proxy " + host + " asks you to sign in" : host + " asks you to sign in");
    BFont bold(be_bold_font);
    bold.SetSize(bold.Size() * 1.2f);
    heading->SetFont(&bold);
    BLayoutBuilder::Group<> layout(this, B_VERTICAL, B_USE_SMALL_SPACING);
    layout.SetInsets(B_USE_WINDOW_SPACING).Add(heading);
    if (!fields.realm.empty())
        layout.Add(Line("realm", "It says: “" + DisplayText(fields.realm, 100) + "”"));
    if (!fields.secure) {
        auto* warning = Line("warning", fields.basic
            ? "This connection is not encrypted: your password is sent as you type it, and others on the network can read it."
            : "This connection is not encrypted: others on the network can see the page and your name.");
        warning->SetHighUIColor(B_FAILURE_COLOR);
        layout.Add(warning);
    }
    if (fields.failed) {
        auto* failed = Line("failed", "That name or password was not accepted. Try again.");
        failed->SetFont(be_bold_font);
        layout.Add(failed);
    }
    fName = new BTextControl("name", "Name:", fields.user.c_str(), nullptr);
    fName->SetModificationMessage(new BMessage(kNameChanged));
    fPassword = new BTextControl("password", "Password:", fields.password.c_str(), nullptr);
    fPassword->TextView()->HideTyping(true);
    // HideTyping drops the text it had.
    fPassword->SetText(fields.password.c_str());
    BGridLayout* grid;
    layout.AddGrid(B_USE_DEFAULT_SPACING, B_USE_SMALL_SPACING).GetLayout(&grid).End();
    const float fieldWidth = be_plain_font->StringWidth("m") * 26;
    auto* nameInput = fName->CreateTextViewLayoutItem();
    nameInput->SetExplicitMinSize(BSize(fieldWidth, B_SIZE_UNSET));
    grid->AddItem(fName->CreateLabelLayoutItem(), 0, 0);
    grid->AddItem(nameInput, 1, 0);
    grid->AddItem(fPassword->CreateLabelLayoutItem(), 0, 1);
    grid->AddItem(fPassword->CreateTextViewLayoutItem(), 1, 1);
    if (fields.canRemember) {
        fRemember = new BCheckBox("remember", "Remember this password", nullptr);
        fRemember->SetValue(fields.remember ? B_CONTROL_ON : B_CONTROL_OFF);
        layout.Add(fRemember);
    }
    auto* cancel = new BButton("cancel", "Cancel", new BMessage(kCancel));
    auto* signIn = new BButton("signin", "Sign In", new BMessage(kSignIn));
    layout.AddGroup(B_HORIZONTAL).AddGlue().Add(cancel).Add(signIn).End();
    SetDefaultButton(signIn);
    if (fields.user.empty() || fields.failed) fName->MakeFocus(true);
    else fPassword->MakeFocus(true);
    if (fields.failed && !fields.user.empty()) {
        fPassword->SetText("");
        fPassword->MakeFocus(true);
    }
    ResizeToPreferred();
}

bool AuthenticationPrompt::QuitRequested()
{
    Answer(false);
    return true;
}

void AuthenticationPrompt::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kSignIn:
        case kCancel:
            Answer(message->what == kSignIn);
            Quit();
            return;
        case kNameChanged:
            for (const auto& [user, password] : fFields.accounts) {
                if (user == fName->Text()) {
                    fPassword->SetText(password.c_str());
                    break;
                }
            }
            return;
        default:
            BWindow::MessageReceived(message);
    }
}

void AuthenticationPrompt::Answer(bool signIn)
{
    if (fAnswered) return;
    fAnswered = true;
    BMessage answer(kAuthenticationAnswer);
    answer.AddUInt64("identifier", fFields.identifier);
    if (signIn) {
        answer.AddString("user", fName->Text());
        answer.AddString("password", fPassword->Text());
        answer.AddBool("remember", fRemember && fRemember->Value() == B_CONTROL_ON);
    } else
        answer.AddBool("cancel", true);
    fTarget.SendMessage(&answer, static_cast<BHandler*>(nullptr), 0);
}
}

#endif
