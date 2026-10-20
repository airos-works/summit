#pragma once

#if SUMMIT_MODERN_WEBKIT

#include <Messenger.h>
#include <Window.h>
#include <string>
#include <utility>
#include <vector>

class BCheckBox;
class BTextControl;

namespace summit {
// The sign-in sheet's answer, to its browser window: "identifier", and
// either "cancel" (true) or "user", "password" and "remember" (bool).
constexpr uint32 kAuthenticationAnswer = 'auan';
// The key store's accounts for a request's site, found off the window's
// thread: "identifier", repeated "user" and "password".
constexpr uint32 kAuthenticationAccounts = 'auac';

// The sheet a site that asks for a name and password gets (HTTP Basic,
// Digest or NTLM authentication, #87): over its browser window, which takes
// no input until it is answered. Escape and Close are Cancel.
class AuthenticationPrompt final : public BWindow {
public:
    struct Fields {
        uint64 identifier = 0;
        // As shown: host, with the port when it is not the scheme's own.
        std::string host;
        // The server's own words for the account (untrusted).
        std::string realm;
        bool proxy = false;
        // False over plain http: the sheet warns.
        bool secure = true;
        // Basic sends the password itself, readable on plain http.
        bool basic = true;
        // A name and password were refused already.
        bool failed = false;
        // Remember (the key store) is offered: not in private windows.
        bool canRemember = false, remember = false;
        std::string user, password;
        // Saved accounts of the site: choosing one of their names fills in
        // its password.
        std::vector<std::pair<std::string, std::string>> accounts;
    };
    AuthenticationPrompt(const Fields& fields, BMessenger target);
    void MessageReceived(BMessage* message) override;
    bool QuitRequested() override;

private:
    void Answer(bool signIn);
    Fields fFields;
    BMessenger fTarget;
    BTextControl* fName = nullptr;
    BTextControl* fPassword = nullptr;
    BCheckBox* fRemember = nullptr;
    bool fAnswered = false;
};
}

#endif
