#include "PasskeyPrompt.h"

#if SUMMIT_MODERN_WEBKIT

#include "AuthenticationPrompt.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <StringView.h>
#include <TextControl.h>
#include <string>
#include <GroupView.h>
#include <ListView.h>
#include <algorithm>
#include <cmath>
#include <Screen.h>

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
    // The create() sheets name the account; so does a security key's, for
    // create(), since it makes the passkey for that account.
    bool creating = fields.kind == Kind::Create || (fields.kind == Kind::SecurityKey && fields.create);
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
        case Kind::SecurityKey:
            heading = "Use your security key";
            explanation = creating ? "Insert your security key and touch it to create a passkey for " + site + " on it."
                : "Insert your security key and touch it to sign in to " + site + ".";
            break;
        case Kind::SecurityKeyPin:
            heading = "Enter your security key's PIN";
            explanation = site + " asks your security key to check that it is you.";
            confirm = "Continue";
            break;
        case Kind::SecurityKeyNewPin:
            heading = "Set a PIN for your security key";
            explanation = site + " asks your security key to check that it is you, and the key has no PIN yet. Choose one of at least "
                + std::to_string(fields.minimumPinLength) + " characters; the key keeps it, and asks for it from now on.";
            confirm = "Set PIN";
            break;
        case Kind::SecurityKeyAccount:
            heading = "Sign in to " + site + " with your security key";
            explanation = "The security key keeps more than one passkey for this site. Choose the account.";
            confirm = "Sign In";
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
    // A frame of another site asks: name the page it is in, so that a
    // passkey is not used for a page the user did not mean.
    if (!fields.topHost.empty() && fields.topHost != fields.pageHost)
        layout.Add(Line("page", "Asked by " + PromptDisplayText(fields.pageHost.empty() ? fields.site : fields.pageHost, 120)
            + ", in a frame of " + PromptDisplayText(fields.topHost, 120)));
    else if (!fields.pageHost.empty() && fields.pageHost != fields.site)
        layout.Add(Line("page", "Asked by " + PromptDisplayText(fields.pageHost, 120)));
    if (creating)
        layout.Add(Line("account", "Account: " + PasskeyAccountLabel(fields.userName, fields.displayName)));
    const bool choosesAccount = fields.kind == Kind::SignIn || fields.kind == Kind::SecurityKeyAccount;
    if (fields.kind == Kind::SignIn && fields.accounts.size() == 1)
        layout.Add(Line("account", "Account: " + PasskeyAccountLabel(fields.accounts[0].first, fields.accounts[0].second)));
    if (choosesAccount && (fields.accounts.size() > 1 || fields.kind == Kind::SecurityKeyAccount) && !fields.accounts.empty()) {
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
    if (fields.kind == Kind::SecurityKeyPin || fields.kind == Kind::SecurityKeyNewPin) {
        fPin = new BTextControl("pin", fields.kind == Kind::SecurityKeyPin ? "PIN:" : "New PIN:", "", nullptr);
        fPin->TextView()->HideTyping(true);
        layout.Add(fPin);
        if (fields.kind == Kind::SecurityKeyNewPin) {
            fPinAgain = new BTextControl("pin again", "Again:", "", nullptr);
            fPinAgain->TextView()->HideTyping(true);
            layout.Add(fPinAgain);
        }
        std::string problem = fields.pinProblem;
        if (fields.kind == Kind::SecurityKeyPin) {
            if (fields.wrongPin) problem = "Wrong PIN.";
            // The key locks itself after its last try: say how many are left
            // once it matters.
            if (fields.retries >= 0 && (fields.wrongPin || fields.retries <= 3))
                problem += std::string(problem.empty() ? "" : " ") + std::to_string(fields.retries)
                    + (fields.retries == 1 ? " try left before the key locks." : " tries left before the key locks.");
        }
        if (!problem.empty()) {
            fProblem = Paragraph("problem", problem);
            layout.Add(fProblem);
        }
    }
    // Create and sign-in sheets: a security key may answer instead.
    if (fields.securityKey && (fields.kind == Kind::Create || fields.kind == Kind::SignIn || fields.kind == Kind::AlreadySaved))
        layout.Add(Paragraph("security key", fields.kind == Kind::SignIn
            ? "Or sign in with a security key: insert it and touch it."
            : "Or create the passkey on a security key: insert it and touch it."));
    fKeyNote = new BGroupView("key note", B_VERTICAL, 0);
    layout.Add(fKeyNote);
    auto buttons = layout.AddGroup(B_HORIZONTAL).AddGlue();
    if (cancel) buttons.Add(new BButton("cancel", cancel, new BMessage(kCancel)));
    BButton* confirmButton = confirm ? new BButton("confirm", confirm, new BMessage(kConfirm)) : nullptr;
    if (confirmButton) buttons.Add(confirmButton);
    buttons.End();
    if (confirmButton) SetDefaultButton(confirmButton);
    SetKeyNote(fields.keyNote);
    if (fPin) fPin->MakeFocus(true);
    ResizeToPreferred();
}

// A line of what the security key is doing, replaced in place: the sheet
// stays, and with it what the user chose or typed.
void PasskeyPrompt::SetKeyNote(const std::string& note)
{
    if (!fKeyNote) return;
    while (BView* child = fKeyNote->ChildAt(0)) {
        child->RemoveSelf();
        delete child;
    }
    if (!note.empty()) fKeyNote->AddChild(Paragraph(nullptr, note));
    ResizeToPreferred();
}

bool PasskeyPrompt::QuitRequested()
{
    // Closed without an answer (Escape, Close, or the request went away).
    Answer(-1);
    return true;
}

void PasskeyPrompt::Confirm()
{
    switch (fFields.kind) {
        case Kind::SignIn: Answer(fChosen); break;
        case Kind::NoPasskey: Answer(-1); break;
        case Kind::SecurityKey: return;
        case Kind::SecurityKeyPin:
        case Kind::SecurityKeyNewPin: {
            std::string pin = fPin->Text();
            std::string problem;
            if (pin.empty())
                problem = "Enter the PIN.";
            else if (fPinAgain && pin != fPinAgain->Text())
                problem = "The two PINs differ.";
            else if (fFields.kind == Kind::SecurityKeyNewPin && static_cast<int32>(pin.size()) < fFields.minimumPinLength)
                problem = "The PIN is too short: at least " + std::to_string(fFields.minimumPinLength) + " characters.";
            if (!problem.empty()) {
                SetKeyNote(problem);
                return;
            }
            if (fAnswered) return;
            fAnswered = true;
            BMessage answer(kPasskeyAnswer);
            answer.AddUInt64("identifier", fFields.identifier);
            answer.AddString("pin", pin.c_str());
            fTarget.SendMessage(&answer, static_cast<BHandler*>(nullptr), 0);
            // Not kept in the window's fields after it closes.
            std::fill(pin.begin(), pin.end(), '\0');
            fPin->SetText("");
            if (fPinAgain) fPinAgain->SetText("");
            break;
        }
        case Kind::SecurityKeyAccount: {
            if (fAnswered) return;
            fAnswered = true;
            BMessage answer(kPasskeyAnswer);
            answer.AddUInt64("identifier", fFields.identifier);
            answer.AddInt32("key_account", fChosen);
            fTarget.SendMessage(&answer, static_cast<BHandler*>(nullptr), 0);
            break;
        }
        default: Answer(0); break;
    }
    Quit();
}

void PasskeyPrompt::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kChoose:
            fChosen = message->GetInt32("account", 0);
            return;
        case kPasskeyKeyNote:
            SetKeyNote(message->GetString("note", ""));
            return;
        case kPasskeyReplaced:
            fAnswered = true;
            Quit();
            return;
        case kScriptedButton:
            if (message->HasInt32("account") && fAccounts) {
                const int32 account = message->GetInt32("account", 0);
                if (account >= 0 && account < static_cast<int32>(fFields.accounts.size())) {
                    fChosen = account;
                    if (auto* item = fAccounts->Menu()->ItemAt(account)) item->SetMarked(true);
                }
            }
            if (const char* pin = nullptr; message->FindString("pin", &pin) == B_OK && fPin) {
                fPin->SetText(pin);
                if (fPinAgain) fPinAgain->SetText(pin);
            }
            if (message->GetInt32("which", 0) == 0 && fFields.kind != Kind::AlreadySaved && fFields.kind != Kind::NoPasskey) {
                Answer(-1);
                Quit();
            } else
                Confirm();
            return;
        case kConfirm:
            Confirm();
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

namespace {
constexpr uint32 kAutofillClicked = 'pkac';

// One passkey in the autofill list: the account, and "Passkey" below it.
class PasskeyAutofillItem final : public BListItem {
public:
    explicit PasskeyAutofillItem(const std::string& label) : fLabel(label) { }
    void DrawItem(BView* owner, BRect frame, bool) override
    {
        const rgb_color background = ui_color(IsSelected() ? B_LIST_SELECTED_BACKGROUND_COLOR : B_LIST_BACKGROUND_COLOR);
        owner->SetLowColor(background);
        owner->FillRect(frame, B_SOLID_LOW);
        font_height height;
        owner->GetFontHeight(&height);
        const float line = ceilf(height.ascent + height.descent + height.leading);
        owner->SetHighColor(ui_color(IsSelected() ? B_LIST_SELECTED_ITEM_TEXT_COLOR : B_LIST_ITEM_TEXT_COLOR));
        owner->DrawString(fLabel.c_str(), BPoint(frame.left + 8, frame.top + 4 + height.ascent));
        owner->SetHighColor(tint_color(ui_color(B_LIST_ITEM_TEXT_COLOR), B_LIGHTEN_1_TINT));
        owner->DrawString("Passkey saved in Summit", BPoint(frame.left + 8, frame.top + 4 + line + height.ascent));
    }
    void Update(BView* owner, const BFont* font) override
    {
        BListItem::Update(owner, font);
        font_height height;
        font->GetHeight(&height);
        SetHeight(ceilf(height.ascent + height.descent + height.leading) * 2 + 8);
        SetWidth(std::max(font->StringWidth(fLabel.c_str()), font->StringWidth("Passkey saved in Summit")) + 16);
    }

private:
    std::string fLabel;
};
}

PasskeyAutofill::PasskeyAutofill(BPoint where, uint64 identifier, const std::vector<std::pair<std::string, std::string>>& accounts,
    BMessenger target)
    : BWindow(BRect(where, where + BPoint(200, 40)), "Passkeys", B_BORDERED_WINDOW_LOOK, B_FLOATING_SUBSET_WINDOW_FEEL,
        B_AVOID_FOCUS | B_NOT_MOVABLE | B_NOT_RESIZABLE | B_NOT_ZOOMABLE | B_NOT_MINIMIZABLE | B_NOT_CLOSABLE
        | B_ASYNCHRONOUS_CONTROLS),
      fIdentifier(identifier), fTarget(target)
{
    fList = new BListView(Bounds(), "passkeys", B_SINGLE_SELECTION_LIST, B_FOLLOW_ALL);
    float width = 220, height = 0;
    for (const auto& [name, displayName] : accounts) {
        auto* item = new PasskeyAutofillItem(PasskeyAccountLabel(name, displayName));
        fList->AddItem(item);
    }
    AddChild(fList);
    for (int32 i = 0; i < fList->CountItems(); ++i) {
        width = std::max(width, fList->ItemAt(i)->Width());
        height += fList->ItemAt(i)->Height() + 1;
    }
    fList->SetSelectionMessage(new BMessage(kAutofillClicked));
    fList->SetTarget(this);
    // Below the field, on the screen.
    const BRect screen = BScreen(this).Frame();
    width = std::min(width, screen.Width() / 2);
    height = std::min(height, screen.Height() / 3);
    ResizeTo(width, height);
    MoveTo(std::clamp(where.x, screen.left, screen.right - width), std::clamp(where.y, screen.top, screen.bottom - height));
}

void PasskeyAutofill::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kAutofillClicked:
            if (fList->CurrentSelection() >= 0) Choose(fList->CurrentSelection());
            return;
        case kScriptedButton:
            Choose(message->GetInt32("account", 0));
            return;
        default:
            BWindow::MessageReceived(message);
    }
}

void PasskeyAutofill::Choose(int32 account)
{
    if (fChosen || account < 0 || account >= fList->CountItems()) return;
    fChosen = true;
    BMessage chosen(kPasskeyConditionalChosen);
    chosen.AddUInt64("identifier", fIdentifier);
    chosen.AddInt32("account", account);
    fTarget.SendMessage(&chosen, static_cast<BHandler*>(nullptr), 0);
    Quit();
}
}

#endif
