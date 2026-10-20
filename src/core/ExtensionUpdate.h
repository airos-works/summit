#pragma once
#include <nlohmann/json.hpp>
#include <optional>
#include <string>

namespace summit {
// Where an installed extension's updates come from: "chrome" for a package
// signed with its Chrome Web Store identity whose manifest names Google's
// update service (the store adds that update_url), "firefox" for one whose
// identity is its Firefox add-on ID, else empty. Packages from elsewhere
// (a folder, a ZIP, a CRX for another update server) are not updated.
std::string ExtensionUpdateSource(const nlohmann::json& manifest, const std::string& identifier, const std::string& verifiedCRXIdentity);
// Below zero when version a is older than b, zero when the same, above zero
// when newer. Dotted parts compare as numbers, then by what follows them
// ("1.0a1" is older than "1.0").
int CompareExtensionVersions(const std::string& a, const std::string& b);
// Google's update service, asked about one extension at a version.
std::string ChromeUpdateCheckURL(const std::string& identifier, const std::string& version);
// Its answer (XML): the version it offers, or nothing when there is no newer one.
std::optional<std::string> ChromeUpdateCheckVersion(const std::string& response, const std::string& identifier);
// Mozilla's add-on API for an add-on ID.
std::string FirefoxAddonURL(const std::string& identifier);
}
