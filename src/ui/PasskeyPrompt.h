#pragma once

#if SUMMIT_MODERN_WEBKIT

#include <Messenger.h>
#include <Window.h>
#include <string>
#include <utility>
#include <vector>

class BGroupView;
class BListView;
class BMenuField;
class BTextControl;

namespace summit {
// The passkey sheet's answer, to its browser window: "identifier" and
// "account" (int32: the passkey chosen, 0 to create one or to acknowledge,
// negative to decline), as BWebKitView::AnswerPasskeyRequest takes it. A
// security key's question is answered with "pin" (string) or "key_account"
// (int32) instead, as BWebKitView::AnswerSecurityKey takes them.
constexpr uint32 kPasskeyAnswer = 'pkan';
// To a sheet: "note" (string), what a security key is doing, shown in place.
constexpr uint32 kPasskeyKeyNote = 'pkkn';
// To a sheet: close without answering (another sheet for the same request
// replaces it).
constexpr uint32 kPasskeyReplaced = 'pkrp';
// A passkey chosen in the autofill menu under a sign-in field, to its browser
// window: "identifier" (uint64, or int64 from hey) and "account" (int32).
constexpr uint32 kPasskeyConditionalChosen = 'pkcc';

// The sheet a page that asks for a passkey gets when Summit's own
// authenticator answers it (#111, B_WEBKIT_PASSKEY_REQUESTED): create a
// passkey for the site, or sign in with one of its passkeys, chosen from a
// menu when there are several. Its confirmation is the passkey's user
// verification. Over its browser window, which takes no input until it is
// answered; Escape and Close decline.
//
// A security key may answer the same request (#111): the sheet says so, and
// a key's questions (its PIN, a new PIN, which of its passkeys) get sheets
// of their own (the Kind::SecurityKey* kinds).
//
// Scripting, as a BAlert's buttons are (hey): 'ALTB' with which=int32(0)
// declines and which=int32(1) confirms; account=int32(N) chooses the passkey;
// pin=string(...) fills the PIN fields first.
class PasskeyPrompt final : public BWindow {
public:
    enum class Kind {
        Create, AlreadySaved, SignIn, NoPasskey,
        // Only a security key may answer: insert it and touch it.
        SecurityKey,
        // The key's PIN, a new PIN for it, or which of its passkeys.
        SecurityKeyPin, SecurityKeyNewPin, SecurityKeyAccount,
    };
    struct Fields {
        uint64 identifier = 0;
        Kind kind = Kind::SignIn;
        // The relying party ID (the site the passkey is for) and the page's
        // host, when it is another (a subdomain, or a related origin the
        // site lists); topHost: the host of the page when a frame of another
        // origin asks.
        std::string site, pageHost, topHost;
        // Create: the account the site names (its own words).
        std::string userName, displayName;
        bool replaces = false;
        bool privateWindow = false;
        // Sign in: the passkeys offered, as (name, display name); for
        // SecurityKeyAccount, the key's.
        std::vector<std::pair<std::string, std::string>> accounts;
        // Create or sign in: a security key may answer too. create: the
        // request is a create() (for the SecurityKey kinds).
        bool securityKey = false;
        bool create = false;
        // What the security key is doing ("Touch it", an error).
        std::string keyNote;
        // SecurityKeyPin: tries left (negative: not known) and whether the
        // last PIN was wrong; SecurityKeyNewPin: the shortest PIN allowed
        // and why the last one was refused.
        int32 retries = -1;
        bool wrongPin = false;
        int32 minimumPinLength = 4;
        std::string pinProblem;
    };
    PasskeyPrompt(const Fields& fields, BMessenger target);
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;

private:
    void Answer(int32 account);
    void Confirm();
    void SetKeyNote(const std::string& note);
    Fields fFields;
    BMessenger fTarget;
    BMenuField* fAccounts = nullptr;
    BGroupView* fKeyNote = nullptr;
    BView* fProblem = nullptr;
    BTextControl* fPin = nullptr;
    BTextControl* fPinAgain = nullptr;
    int32 fChosen = 0;
    bool fAnswered = false;
};

// Passkey autofill (conditional mediation): the site's passkeys in a small
// list under the sign-in field, as other browsers show autofill. It takes no
// focus (the field keeps it, and typing goes on), and a click on a passkey
// sends kPasskeyConditionalChosen with "identifier" and "account". The
// browser window closes it when the field is left, the request ends, or the
// tab or window changes. Scripting, for tests: 'ALTB' with account=int32(N).
class PasskeyAutofill final : public BWindow {
public:
    PasskeyAutofill(BPoint where, uint64 identifier, const std::vector<std::pair<std::string, std::string>>& accounts,
        BMessenger target);
    void MessageReceived(BMessage* message) override;
    uint64 Identifier() const { return fIdentifier; }

private:
    void Choose(int32 account);
    uint64 fIdentifier;
    BMessenger fTarget;
    BListView* fList = nullptr;
    bool fChosen = false;
};

// An account as the sheet shows it: "Display Name (name)", or one of them.
std::string PasskeyAccountLabel(const std::string& name, const std::string& displayName);
}

#endif
