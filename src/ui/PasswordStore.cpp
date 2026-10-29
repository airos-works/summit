#include "PasswordStore.h"
#include <Key.h>
#include <KeyStore.h>
#include <DataIO.h>
#include <Message.h>
#include <algorithm>
#include <cstring>
#include <sstream>

namespace summit {
namespace {
constexpr const char* kKeyring = "Summit";

std::string OriginOfURL(const std::string& url)
{
    const auto scheme = url.find("://");
    if (scheme == std::string::npos) return {};
    std::string origin = url.substr(0, url.find_first_of("/?#", scheme + 3));
    for (auto& character : origin) character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return origin.starts_with("https://") || origin.starts_with("http://") ? origin : std::string();
}

// One CSV field, quoted when it needs to be.
std::string Field(const std::string& value)
{
    if (value.find_first_of(",\"\n\r") == std::string::npos) return value;
    std::string quoted = "\"";
    for (char character : value) {
        if (character == '"') quoted += '"';
        quoted += character;
    }
    return quoted + "\"";
}

std::vector<std::vector<std::string>> ParseCSV(const std::string& text)
{
    std::vector<std::vector<std::string>> rows(1);
    std::string field;
    bool quoted = false, any = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char character = text[i];
        if (quoted) {
            if (character == '"' && i + 1 < text.size() && text[i + 1] == '"') { field += '"'; ++i; }
            else if (character == '"') quoted = false;
            else field += character;
        } else if (character == '"') { quoted = true; any = true; }
        else if (character == ',') { rows.back().push_back(field); field.clear(); any = true; }
        else if (character == '\n' || character == '\r') {
            if (character == '\r' && i + 1 < text.size() && text[i + 1] == '\n') ++i;
            if (any || !field.empty()) rows.back().push_back(field);
            field.clear();
            any = false;
            rows.emplace_back();
        } else { field += character; any = true; }
    }
    if (any || !field.empty()) rows.back().push_back(field);
    while (!rows.empty() && rows.back().empty()) rows.pop_back();
    return rows;
}
}

namespace PasswordStore {
std::vector<SavedAccount> Accounts(const std::string& origin)
{
    std::vector<SavedAccount> accounts;
    BKeyStore store;
    uint32 cookie = 0;
    BPasswordKey key;
    while (store.GetNextKey(kKeyring, B_KEY_TYPE_PASSWORD, B_KEY_PURPOSE_WEB, cookie, key) == B_OK) {
        const char* identifier = key.Identifier();
        if (!identifier || (!origin.empty() && origin != identifier)) continue;
        accounts.push_back({identifier, key.SecondaryIdentifier() ? key.SecondaryIdentifier() : "",
            key.Password() ? key.Password() : ""});
        if (accounts.size() >= 10000) break;
    }
    return accounts;
}

std::string Save(const std::string& origin, const std::string& username, const std::string& password)
{
    if (OriginOfURL(origin) != origin || password.empty()) return "That is not a web site's address.";
    BKeyStore store;
    // The keyring is made on first use; it exists from then on.
    store.AddKeyring(kKeyring);
    Remove(origin, username);
    BPasswordKey key(password.c_str(), B_KEY_PURPOSE_WEB, origin.c_str(), username.c_str());
    const status_t status = store.AddKey(kKeyring, key);
    return status == B_OK ? std::string() : std::string(strerror(status));
}

bool Remove(const std::string& origin, const std::string& username)
{
    BKeyStore store;
    BPasswordKey key;
    if (store.GetKey(kKeyring, B_KEY_TYPE_PASSWORD, origin.c_str(), username.c_str(), false, key) != B_OK) return false;
    return store.RemoveKey(kKeyring, key) == B_OK;
}

std::string ExportCSV()
{
    std::ostringstream csv;
    csv << "url,username,password\n";
    for (const auto& account : Accounts())
        csv << Field(account.origin) << ',' << Field(account.username) << ',' << Field(account.password) << '\n';
    return csv.str();
}

size_t ImportCSV(const std::string& text, std::string& error)
{
    const auto rows = ParseCSV(text);
    if (rows.empty()) { error = "The file is empty."; return 0; }
    // Firefox: "url","username","password",...; Chrome: name,url,username,password.
    size_t urlColumn = 0, userColumn = 1, passwordColumn = 2;
    const auto& header = rows.front();
    for (size_t i = 0; i < header.size(); ++i) {
        if (header[i] == "url" || header[i] == "origin") urlColumn = i;
        else if (header[i] == "username" || header[i] == "login") userColumn = i;
        else if (header[i] == "password") passwordColumn = i;
    }
    size_t imported = 0;
    for (size_t row = 1; row < rows.size(); ++row) {
        const auto& fields = rows[row];
        if (fields.size() <= std::max({urlColumn, userColumn, passwordColumn})) continue;
        const auto origin = OriginOfURL(fields[urlColumn]);
        if (origin.empty() || fields[passwordColumn].empty()) continue;
        if (Save(origin, fields[userColumn], fields[passwordColumn]).empty()) ++imported;
    }
    if (!imported) error = "No accounts were found in the file.";
    return imported;
}
}

namespace PasskeyStore {
namespace {
constexpr const char* kPasskeyKeyring = "Summit passkeys";
}

std::vector<SavedPasskey> Passkeys()
{
    std::vector<SavedPasskey> passkeys;
    BKeyStore store;
    uint32 cookie = 0;
    BKey key;
    // A keyring that does not exist yet (no passkey was ever made) answers
    // at once, without asking the user to allow Summit.
    while (store.GetNextKey(kPasskeyKeyring, B_KEY_TYPE_GENERIC, B_KEY_PURPOSE_WEB, cookie, key) == B_OK) {
        if (!key.Identifier() || !key.SecondaryIdentifier() || !key.Data()) continue;
        BMemoryIO input(key.Data(), key.DataLength());
        BMessage record;
        if (record.Unflatten(&input) != B_OK) continue;
        passkeys.push_back({key.Identifier(), key.SecondaryIdentifier(), record.GetString("user_name", ""),
            record.GetString("display_name", "")});
        if (passkeys.size() >= 10000) break;
    }
    return passkeys;
}

bool Remove(const std::string& site, const std::string& credential)
{
    // Found by enumeration, as Passkeys() does: keystore_server asks the
    // user once per kind of access, and listing already asked.
    BKeyStore store;
    uint32 cookie = 0;
    BKey key;
    while (store.GetNextKey(kPasskeyKeyring, B_KEY_TYPE_GENERIC, B_KEY_PURPOSE_WEB, cookie, key) == B_OK) {
        if (key.Identifier() && key.SecondaryIdentifier() && site == key.Identifier() && credential == key.SecondaryIdentifier())
            return store.RemoveKey(kPasskeyKeyring, key) == B_OK;
    }
    return false;
}
}
}
