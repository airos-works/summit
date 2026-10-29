#include "PasskeyPrompt.h"

#if SUMMIT_MODERN_WEBKIT

#include "AuthenticationPrompt.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <StringView.h>
#include <GroupView.h>

namespace summit {
namespace {
constexpr uint32 kConfirm = 'pkok';
constexpr uint32 kCancel = 'pkca';
constexpr uint32 kChoose = 'pkch';
// What a BAlert's buttons send; hey drives the sheet with it.
constexpr uint32 kScriptedButton = 'ALTB';

BStringView* Line(const char* name, const std::string& text)
{
    auto* view = new BStringView(name, text.c_str());
    view->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
    return view;
}

// An explanation, broken into lines of a fixed width: a string view does
// not wrap, and a text view's wrapping width does not follow the layout.
BView* Paragraph(const char* name, const std::string& text)
{
    const float width = be_plain_font->StringWidth("m") * 34;
    auto* group = new BGroupView(name, B_VERTICAL, 0);
    std::string line;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find(' ', start);
        if (end == std::string::npos) end = text.size();
        const std::string word = text.substr(start, end - start);
        const std::string longer = line.empty() ? word : line + " " + word;
        if (!line.empty() && be_plain_font->StringWidth(longer.c_str()) > width) {
            group->AddChild(new BStringView(nullptr, line.c_str()));
            line = word;
        } else
            line = longer;
        start = end + 1;
    }
    if (!line.empty()) group->AddChild(new BStringView(nullptr, line.c_str()));
    return group;
}
}

std::string PasskeyAccountLabel(const std::string& name, const std::string& displayName)
{
    const std::string shownName = PromptDisplayText(name, 80), shownDisplay = PromptDisplayText(displayName, 80);
    if (shownDisplay.empty() || shownDisplay == shownName) return shownName.empty() ? "(no name)" : shownName;
    if (shownName.empty()) return shownDisplay;
    return shownDisplay + " (" + shownName + ")";
}

PasskeyPrompt::PasskeyPrompt(const Fields& fields, BMessenger target)
    : BWindow(BRect(0, 0, 420, 160), "Passkey", B_TITLED_WINDOW_LOOK, B_MODAL_SUBSET_WINDOW_FEEL,
        B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS
        | B_CLOSE_ON_ESCAPE),
      fFields(fields), fTarget(target)
{
    const std::string site = PromptDisplayText(fields.site, 120);
    std::string heading, explanation;
    const char* confirm = nullptr;
    const char* cancel = "Cancel";
    switch (fields.kind) {
        case Kind::Create:
            heading = "Create a passkey for " + site + "?";
            explanation = "Summit keeps the passkey in this computer's key store and signs in to " + site
                + " with it, after asking you each time.";
            if (fields.replaces) explanation += " It replaces the passkey saved for this account.";
            if (fields.privateWindow) explanation += " The passkey stays after this private window closes.";
            confirm = "Create";
            break;
        case Kind::AlreadySaved:
            heading = "A passkey for " + site + " is already saved";
            explanation = "Summit already keeps a passkey for this account. Sign in with it instead.";
            confirm = "OK";
            cancel = nullptr;
            break;
        case Kind::SignIn:
            heading = "Sign in to " + site + " with a passkey?";
            break;
        case Kind::NoPasskey:
            heading = "No passkey for " + site;
            explanation = "Summit keeps no passkey this site accepts. A password manager extension may have one, "
                "or the site may offer another way to sign in.";
            confirm = "OK";
            cancel = nullptr;
            break;
    }
    if (fields.kind == Kind::SignIn) confirm = "Sign In";
    auto* title = Line("heading", heading);
    BFont bold(be_bold_font);
    bold.SetSize(bold.Size() * 1.2f);
    title->SetFont(&bold);
    BLayoutBuilder::Group<> layout(this, B_VERTICAL, B_USE_SMALL_SPACING);
    layout.SetInsets(B_USE_WINDOW_SPACING).Add(title);
    // A page on a subdomain may ask for its parent domain's passkey; say
    // which page asks.
    if (!fields.pageHost.empty() && fields.pageHost != fields.site)
        layout.Add(Line("page", "Asked by " + PromptDisplayText(fields.pageHost, 120)));
    if (fields.kind == Kind::Create)
        layout.Add(Line("account", "Account: " + PasskeyAccountLabel(fields.userName, fields.displayName)));
    if (fields.kind == Kind::SignIn && fields.accounts.size() == 1)
        layout.Add(Line("account", "Account: " + PasskeyAccountLabel(fields.accounts[0].first, fields.accounts[0].second)));
    if (fields.kind == Kind::SignIn && fields.accounts.size() > 1) {
        auto* menu = new BPopUpMenu("accounts");
        for (size_t index = 0; index < fields.accounts.size(); ++index) {
            auto* choose = new BMessage(kChoose);
            choose->AddInt32("account", static_cast<int32>(index));
            menu->AddItem(new BMenuItem(PasskeyAccountLabel(fields.accounts[index].first, fields.accounts[index].second).c_str(), choose));
        }
        menu->ItemAt(0)->SetMarked(true);
        fAccounts = new BMenuField("accounts", "Account:", menu);
        layout.Add(fAccounts);
    }
    if (!explanation.empty()) layout.Add(Paragraph("explanation", explanation));
    auto buttons = layout.AddGroup(B_HORIZONTAL).AddGlue();
    if (cancel) buttons.Add(new BButton("cancel", cancel, new BMessage(kCancel)));
    auto* confirmButton = new BButton("confirm", confirm, new BMessage(kConfirm));
    buttons.Add(confirmButton).End();
    SetDefaultButton(confirmButton);
    ResizeToPreferred();
}

bool PasskeyPrompt::QuitRequested()
{
    // Closed without an answer (Escape, Close, or the request went away).
    Answer(-1);
    return true;
}

void PasskeyPrompt::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kChoose:
            fChosen = message->GetInt32("account", 0);
            return;
        case kScriptedButton:
            if (message->HasInt32("account") && fAccounts) {
                const int32 account = message->GetInt32("account", 0);
                if (account >= 0 && account < static_cast<int32>(fFields.accounts.size())) {
                    fChosen = account;
                    if (auto* item = fAccounts->Menu()->ItemAt(account)) item->SetMarked(true);
                }
            }
            if (message->GetInt32("which", 0) == 0 && (fFields.kind == Kind::Create || fFields.kind == Kind::SignIn))
                Answer(-1);
            else
                Answer(fFields.kind == Kind::SignIn ? fChosen : fFields.kind == Kind::NoPasskey ? -1 : 0);
            Quit();
            return;
        case kConfirm:
            Answer(fFields.kind == Kind::SignIn ? fChosen : fFields.kind == Kind::NoPasskey ? -1 : 0);
            Quit();
            return;
        case kCancel:
            Answer(-1);
            Quit();
            return;
        default:
            BWindow::MessageReceived(message);
    }
}

void PasskeyPrompt::Answer(int32 account)
{
    if (fAnswered) return;
    fAnswered = true;
    BMessage answer(kPasskeyAnswer);
    answer.AddUInt64("identifier", fFields.identifier);
    answer.AddInt32("account", account);
    fTarget.SendMessage(&answer, static_cast<BHandler*>(nullptr), 0);
}
}

#endif
