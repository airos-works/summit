#pragma once
#include <optional>
#include <string>
#include <string_view>

namespace summit {
// The card Summit puts on an extension's page in the Chrome Web Store or on
// Firefox Add-ons: the Summit icon, the extension's name and Install
// Extension, or Manage Extension once it is installed.
struct StoreOffer {
    std::string name;
    bool installed = false;
    // Installed, but turned off in the Extensions window.
    bool disabled = false;
    // A secret of the browser window that the card's links carry, so that
    // only they count as the card's buttons.
    std::string token;
};
// The card's links are summit-extension:ACTION?token=TOKEN, ACTION being
// install, manage or dismiss. The engine loads no such address: a click on
// one reaches the browser as B_WEBKIT_SCHEME_OPEN_REQUESTED.
inline constexpr const char* kStoreOfferScheme = "summit-extension";
std::string StoreOfferLink(const std::string& action, const std::string& token);
struct StoreOfferChoice {
    std::string action, token;
};
std::optional<StoreOfferChoice> ParseStoreOfferLink(std::string_view url);
// A script for BWebKitView::EvaluateJavaScriptIsolated() that shows the card
// (in place of one shown before) or, given none, takes it away. The card is
// built in a closed shadow root from elements and a constructed style sheet:
// store pages enforce Trusted Types and may refuse inline styles.
std::string StoreOfferScript(const StoreOffer* offer);
// The extension's name from a store page's title: "Dark Reader - Chrome Web
// Store", "Dark Reader – Get this Extension for 🦊 Firefox (en-US)".
std::string StoreItemNameFromTitle(std::string_view title);
}
