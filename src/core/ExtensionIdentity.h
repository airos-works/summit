#pragma once
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

namespace summit {
// An engine-verified CRX signer identity takes precedence for CRX packages.
// The optional value must come from package verification, not manifest data.
// Otherwise a declared Gecko identity takes precedence for cross-browser manifests.
// Otherwise a Chrome manifest key determines the ID; unkeyed local packages
// retain the caller's installation identity. Malformed declarations throw.
std::string ExtensionIdentity(const nlohmann::json& manifest, std::string_view localIdentity, std::string_view verifiedCRXIdentity = { });
// Whether a CRX3 package carries a proof with the Chrome Web Store's
// publisher key, as Chrome requires of packages from the store. The engine
// checks every proof's signature (a package with one that fails is not
// prepared); this only finds whose keys they are.
bool CRXHasWebStorePublisherProof(std::string_view crx);
}
