#pragma once

#if SUMMIT_MODERN_WEBKIT

#include <Messenger.h>
#include <Window.h>
#include <string>
#include <utility>
#include <vector>

class BMenuField;

namespace summit {
// The passkey sheet's answer, to its browser window: "identifier" and
// "account" (int32: the passkey chosen, 0 to create one or to acknowledge,
// negative to decline), as BWebKitView::AnswerPasskeyRequest takes it.
constexpr uint32 kPasskeyAnswer = 'pkan';

// The sheet a page that asks for a passkey gets when Summit's own
// authenticator answers it (#111, B_WEBKIT_PASSKEY_REQUESTED): create a
// passkey for the site, or sign in with one of its passkeys, chosen from a
// menu when there are several. Its confirmation is the passkey's user
// verification. Over its browser window, which takes no input until it is
// answered; Escape and Close decline.
//
// Scripting, as a BAlert's buttons are (hey): 'ALTB' with which=int32(0)
// declines and which=int32(1) confirms; account=int32(N) chooses the passkey.
class PasskeyPrompt final : public BWindow {
public:
    enum class Kind { Create, AlreadySaved, SignIn, NoPasskey };
    struct Fields {
        uint64 identifier = 0;
        Kind kind = Kind::SignIn;
        // The relying party ID (the site the passkey is for) and the page's
        // host, when it is another (a subdomain).
        std::string site, pageHost;
        // Create: the account the site names (its own words).
        std::string userName, displayName;
        bool replaces = false;
        bool privateWindow = false;
        // Sign in: the passkeys offered, as (name, display name).
        std::vector<std::pair<std::string, std::string>> accounts;
    };
    PasskeyPrompt(const Fields& fields, BMessenger target);
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;

private:
    void Answer(int32 account);
    Fields fFields;
    BMessenger fTarget;
    BMenuField* fAccounts = nullptr;
    int32 fChosen = 0;
    bool fAnswered = false;
};

// An account as the sheet shows it: "Display Name (name)", or one of them.
std::string PasskeyAccountLabel(const std::string& name, const std::string& displayName);
}

#endif
