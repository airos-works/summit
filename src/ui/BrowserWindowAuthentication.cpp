#if SUMMIT_MODERN_WEBKIT
// Sites that ask for a name and password (HTTP Basic, Digest and NTLM
// authentication, #87): the engine asks (B_WEBKIT_AUTHENTICATION_REQUESTED),
// a saved account answers by itself, and otherwise a sheet over the window
// asks the user (AuthenticationPrompt). Passkeys of Summit's own WebAuthn
// authenticator (#111) are asked about the same way (PasskeyPrompt).
#include "AuthenticationPrompt.h"
#include "PasskeyPrompt.h"
#include "BrowserWindow.h"
#include "FeatureMessages.h"
#include "PasswordStore.h"
#include "SharedProfile.h"
#include "core/Profile.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <thread>

namespace summit {
namespace {
// scheme://host[:port], lower case: where Remember saves the account.
std::string RequestOrigin(const std::string& url)
{
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    std::string origin = url.substr(0, url.find_first_of("/?#", scheme + 3));
    for (auto& character : origin) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    if (const auto at = origin.find('@', scheme + 3); at != std::string::npos) origin.erase(scheme + 3, at + 1 - (scheme + 3));
    return origin.starts_with("https://") || origin.starts_with("http://") ? origin : std::string();
}

// One server and realm: a name and password given for one request also
// answers the others that wait for the same.
template<typename Request> std::string AuthenticationSpace(const Request& request)
{
    return std::string(request.proxy ? "proxy " : "") + request.host + ":" + std::to_string(request.port) + " "
        + request.scheme + " " + request.realm;
}

// The port is shown when it is not the scheme's own.
template<typename Request> std::string DisplayHost(const Request& request)
{
    const int32 standard = request.secure ? 443 : 80;
    return request.port && request.port != standard ? request.host + ":" + std::to_string(request.port) : request.host;
}
}

void BrowserWindow::AuthenticationRequested(const BMessage& message)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    if (!identifier) return;
    if (message.GetBool("withdrawn", false)) {
        // The page went on to another address, closed or crashed.
        std::erase_if(fAuthentications, [&](const AuthenticationRequest& request) { return request.identifier == identifier; });
        if (fAuthenticationShown == identifier && fAuthenticationPrompt.IsValid()) {
            BMessage close(B_QUIT_REQUESTED);
            fAuthenticationPrompt.SendMessage(&close);
        }
        return;
    }
    BMessenger view;
    auto* tab = message.FindMessenger("view", &view) == B_OK ? FindTab(view) : nullptr;
    if (!tab) {
        BWebKitView::AnswerAuthentication(identifier, nullptr, nullptr);
        return;
    }
    AuthenticationRequest request;
    request.identifier = identifier;
    request.tab = tab->id;
    request.host = message.GetString("host", "");
    request.port = message.GetInt32("port", 0);
    request.realm = message.GetString("realm", "");
    request.scheme = message.GetString("scheme", "basic");
    request.secure = message.GetBool("secure", false);
    request.proxy = message.GetBool("proxy", false);
    request.failures = message.GetInt32("previous_failures", 0);
    request.origin = request.proxy ? std::string() : RequestOrigin(message.GetString("url", ""));
    request.user = message.GetString("user", "");
    if (request.user.empty())
        if (auto known = fAuthenticationUsers.find(AuthenticationSpace(request)); known != fAuthenticationUsers.end())
            request.user = known->second;
    std::fprintf(stderr, "Summit: %s asks for a name and password (%s, realm \"%s\", %d refused)\n",
        DisplayHost(request).c_str(), request.scheme.c_str(), request.realm.c_str(), static_cast<int>(request.failures));
    // Private windows neither fill in nor save passwords. The key store may
    // wait for the user (to allow Summit, or unlock the keyring): it is asked
    // on a thread of its own, never the window's.
    if (!fPrivate && !request.origin.empty()) {
        request.lookingUp = true;
        std::thread([window = BMessenger(this), identifier, origin = request.origin] {
            BMessage accounts(kAuthenticationAccounts);
            accounts.AddUInt64("identifier", identifier);
            for (const auto& account : PasswordStore::Accounts(origin)) {
                accounts.AddString("user", account.username.c_str());
                accounts.AddString("password", account.password.c_str());
            }
            window.SendMessage(&accounts);
        }).detach();
    }
    fAuthentications.push_back(std::move(request));
    ShowAuthenticationPrompt();
}

void BrowserWindow::AuthenticationAccounts(const BMessage& message)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    auto found = std::find_if(fAuthentications.begin(), fAuthentications.end(),
        [&](const AuthenticationRequest& request) { return request.identifier == identifier; });
    if (found == fAuthentications.end()) return;
    found->lookingUp = false;
    const char* user = nullptr;
    const char* password = nullptr;
    for (int32 i = 0; message.FindString("user", i, &user) == B_OK && message.FindString("password", i, &password) == B_OK; ++i)
        found->accounts.emplace_back(user, password);
    // One saved account signs in by itself, the first time only: one the
    // server refused is asked about.
    if (found->accounts.size() == 1 && !found->failures) {
        const auto [savedUser, savedPassword] = found->accounts.front();
        const auto space = AuthenticationSpace(*found);
        fAuthenticationUsers[space] = savedUser;
        std::fprintf(stderr, "Summit: signing in to %s with the saved account\n", DisplayHost(*found).c_str());
        std::erase_if(fAuthentications, [&](const AuthenticationRequest& request) {
            if (AuthenticationSpace(request) != space || (request.lookingUp && request.identifier != identifier)) return false;
            BWebKitView::AnswerAuthentication(request.identifier, savedUser.c_str(), savedPassword.c_str(), B_WEBKIT_CREDENTIAL_FOR_SESSION);
            return true;
        });
        return;
    }
    if (found->user.empty() && !found->accounts.empty()) found->user = found->accounts.front().first;
    ShowAuthenticationPrompt();
}

void BrowserWindow::ShowAuthenticationPrompt()
{
    if (fAuthenticationPrompt.IsValid()) return;
    fAuthenticationShown = 0;
    auto found = std::find_if(fAuthentications.begin(), fAuthentications.end(),
        [&](const AuthenticationRequest& request) { return request.tab == fSelected && !request.lookingUp; });
    if (found == fAuthentications.end()) return;
    AuthenticationPrompt::Fields fields;
    fields.identifier = found->identifier;
    fields.host = DisplayHost(*found);
    fields.realm = found->realm;
    fields.proxy = found->proxy;
    fields.secure = found->secure;
    fields.basic = found->scheme == "basic";
    fields.failed = found->failures > 0;
    fields.canRemember = !fPrivate && !found->origin.empty();
    fields.remember = fields.canRemember && fShared->Read([&](const Profile& profile) {
        return profile.savePasswords && !profile.passwordNever.contains(found->origin);
    });
    fields.user = found->user;
    for (const auto& [user, password] : found->accounts)
        if (user == found->user && !fields.failed) fields.password = password;
    fields.accounts = found->accounts;
    auto* prompt = new AuthenticationPrompt(fields, BMessenger(this));
    if (prompt->AddToSubset(this) != B_OK) {
        prompt->Quit();
        return;
    }
    // Over the page, a little above its middle. Not CenterIn(): that moves a
    // window that is not shown yet to the screen the pointer is on.
    const BRect frame = Frame(), size = prompt->Frame();
    prompt->MoveTo(frame.left + (frame.Width() - size.Width()) / 2, frame.top + (frame.Height() - size.Height()) / 3);
    fAuthenticationShown = found->identifier;
    fAuthenticationPrompt = BMessenger(prompt);
    prompt->Show();
}

void BrowserWindow::AuthenticationAnswered(const BMessage& message)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    if (identifier == fAuthenticationShown) {
        fAuthenticationShown = 0;
        fAuthenticationPrompt = BMessenger();
    }
    auto found = std::find_if(fAuthentications.begin(), fAuthentications.end(),
        [&](const AuthenticationRequest& request) { return request.identifier == identifier; });
    if (found == fAuthentications.end()) {
        ShowAuthenticationPrompt();
        return;
    }
    const AuthenticationRequest answered = *found;
    const auto space = AuthenticationSpace(answered);
    const bool cancel = message.GetBool("cancel", false);
    const std::string user = message.GetString("user", "");
    const std::string password = message.GetString("password", "");
    // The others waiting for the same server and realm get the same answer;
    // Cancel shows the server's own page, as other browsers do.
    std::erase_if(fAuthentications, [&](const AuthenticationRequest& request) {
        if (AuthenticationSpace(request) != space || request.tab != answered.tab) return false;
        if (cancel) BWebKitView::AnswerAuthentication(request.identifier, nullptr, nullptr);
        else BWebKitView::AnswerAuthentication(request.identifier, user.c_str(), password.c_str(), B_WEBKIT_CREDENTIAL_FOR_SESSION);
        return true;
    });
    if (!cancel) {
        fAuthenticationUsers[space] = user;
        if (message.GetBool("remember", false) && !fPrivate && !answered.origin.empty() && !password.empty()) {
            std::thread([window = BMessenger(this), origin = answered.origin, user, password] {
                BMessage saved(kPasswordSaved);
                saved.AddString("error", PasswordStore::Save(origin, user, password).c_str());
                window.SendMessage(&saved);
            }).detach();
        }
    }
    ShowAuthenticationPrompt();
}

void BrowserWindow::DropTabAuthentications(int64 tab)
{
    std::erase_if(fAuthentications, [&](const AuthenticationRequest& request) {
        if (request.tab != tab) return false;
        if (request.identifier == fAuthenticationShown && fAuthenticationPrompt.IsValid()) {
            // Its answer, Cancel, finds nothing left to answer.
            BMessage close(B_QUIT_REQUESTED);
            fAuthenticationPrompt.SendMessage(&close);
        }
        BWebKitView::AnswerAuthentication(request.identifier, nullptr, nullptr);
        return true;
    });
    std::erase_if(fPasskeyRequests, [&](const PasskeyRequest& request) {
        if (request.tab != tab) return false;
        if (request.identifier == fPasskeyShown && fPasskeyPrompt.IsValid()) {
            BMessage close(B_QUIT_REQUESTED);
            fPasskeyPrompt.SendMessage(&close);
        }
        BWebKitView::AnswerPasskeyRequest(request.identifier, -1);
        return true;
    });
}

// A page's WebAuthn request reached Summit's own authenticator: create a
// passkey, or sign in with one (#111). The engine keeps the passkeys (the
// key store's "Summit passkeys" keyring) and makes the keys and signatures;
// the user's confirmation here is all it needs.
void BrowserWindow::PasskeyRequested(const BMessage& message)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    if (!identifier) return;
    if (message.GetBool("withdrawn", false)) {
        // The page aborted the request, closed, or it timed out.
        std::erase_if(fPasskeyRequests, [&](const PasskeyRequest& request) { return request.identifier == identifier; });
        if (fPasskeyShown == identifier && fPasskeyPrompt.IsValid()) {
            BMessage close(B_QUIT_REQUESTED);
            fPasskeyPrompt.SendMessage(&close);
        }
        return;
    }
    BMessenger view;
    auto* tab = message.FindMessenger("view", &view) == B_OK ? FindTab(view) : nullptr;
    if (!tab) {
        BWebKitView::AnswerPasskeyRequest(identifier, -1);
        return;
    }
    const bool create = message.GetBool("create", false);
    std::fprintf(stderr, "Summit: %s asks %s a passkey for %s (request %llu)\n", message.GetString("origin", ""),
        create ? "to create" : "for", message.GetString("rp_id", ""), static_cast<unsigned long long>(identifier));
    fPasskeyRequests.push_back({identifier, tab->id, message});
    ShowPasskeyPrompt();
}

void BrowserWindow::ShowPasskeyPrompt()
{
    if (fPasskeyPrompt.IsValid()) return;
    fPasskeyShown = 0;
    auto found = std::find_if(fPasskeyRequests.begin(), fPasskeyRequests.end(),
        [&](const PasskeyRequest& request) { return request.tab == fSelected; });
    if (found == fPasskeyRequests.end()) return;
    const BMessage& request = found->request;
    PasskeyPrompt::Fields fields;
    fields.identifier = found->identifier;
    fields.site = request.GetString("rp_id", "");
    const std::string origin = request.GetString("origin", "");
    if (const auto scheme = origin.find("://"); scheme != std::string::npos) fields.pageHost = origin.substr(scheme + 3);
    fields.privateWindow = fPrivate;
    if (request.GetBool("create", false)) {
        fields.kind = request.GetBool("exists", false) ? PasskeyPrompt::Kind::AlreadySaved : PasskeyPrompt::Kind::Create;
        fields.userName = request.GetString("user_name", "");
        fields.displayName = request.GetString("user_display_name", "");
        fields.replaces = request.GetBool("replaces", false);
    } else {
        const char* name = nullptr;
        for (int32 i = 0; request.FindString("account_name", i, &name) == B_OK; ++i)
            fields.accounts.emplace_back(name, request.GetString("account_display_name", i, ""));
        fields.kind = fields.accounts.empty() ? PasskeyPrompt::Kind::NoPasskey : PasskeyPrompt::Kind::SignIn;
    }
    auto* prompt = new PasskeyPrompt(fields, BMessenger(this));
    if (prompt->AddToSubset(this) != B_OK) {
        prompt->Quit();
        return;
    }
    // Over the page, a little above its middle, as the sign-in sheet.
    const BRect frame = Frame(), size = prompt->Frame();
    prompt->MoveTo(frame.left + (frame.Width() - size.Width()) / 2, frame.top + (frame.Height() - size.Height()) / 3);
    fPasskeyShown = found->identifier;
    fPasskeyPrompt = BMessenger(prompt);
    prompt->Show();
}

void BrowserWindow::PasskeyAnswered(const BMessage& message)
{
    const uint64 identifier = message.GetUInt64("identifier", 0);
    if (identifier == fPasskeyShown) {
        fPasskeyShown = 0;
        fPasskeyPrompt = BMessenger();
    }
    const auto found = std::find_if(fPasskeyRequests.begin(), fPasskeyRequests.end(),
        [&](const PasskeyRequest& request) { return request.identifier == identifier; });
    if (found != fPasskeyRequests.end()) {
        const int32 account = message.GetInt32("account", -1);
        std::fprintf(stderr, "Summit: passkey request %llu %s\n", static_cast<unsigned long long>(identifier),
            account < 0 ? "declined" : "confirmed");
        BWebKitView::AnswerPasskeyRequest(identifier, account);
        fPasskeyRequests.erase(found);
    }
    ShowPasskeyPrompt();
}
}
#endif
