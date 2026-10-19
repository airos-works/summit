#pragma once
#include <string>
#include <vector>

namespace summit {
// Saved website passwords (#67), kept in the Haiku key store
// (keystore_server) in the keyring "Summit": a password key per account,
// with the site's origin as its identifier and the account name as the
// secondary one. Nothing is written to the profile. The key store may ask
// the user to allow Summit, or to unlock the keyring.
struct SavedAccount {
    std::string origin, username, password;
};
namespace PasswordStore {
// Every saved account, or those of one origin ("https://host[:port]").
std::vector<SavedAccount> Accounts(const std::string& origin = {});
// Adds the account, or replaces its password. An error message, or empty.
std::string Save(const std::string& origin, const std::string& username, const std::string& password);
bool Remove(const std::string& origin, const std::string& username);
// CSV as Firefox and Chrome export it: url,username,password with a header
// line. Import returns how many accounts were added or changed.
std::string ExportCSV();
size_t ImportCSV(const std::string& text, std::string& error);
}
}
