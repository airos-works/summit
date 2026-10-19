#include "core/ExtensionStore.h"
#include "core/StoreOffer.h"
#include <iostream>

int main()
{
    using namespace summit;
    unsigned checks = 0;
    auto check = [&](bool value, const std::string& label) {
        ++checks;
        if (!value) { std::cerr << "FAIL: " << label << '\n'; std::exit(1); }
    };
    const std::string id = "eimadpbcbfnmbkopoojfekhnkhdbieeh";
    for (auto url : {"https://chromewebstore.google.com/detail/dark-reader/" + id,
            "https://chromewebstore.google.com/detail/" + id + "?hl=en#reviews",
            "https://chrome.google.com/webstore/detail/dark-reader/" + id + "/",
            "  https://CHROMEWEBSTORE.GOOGLE.COM:443/detail/" + id + "  "}) {
        auto item = ParseExtensionStoreURL(url);
        check(item && item->store == ExtensionStoreItem::Store::Chrome && item->identifier == id
            && item->requestURL.find("acceptformat=crx3") != std::string::npos, url);
    }
    for (auto url : {"https://addons.mozilla.org/en-US/firefox/addon/darkreader/",
            "https://addons.mozilla.org/firefox/addon/darkreader/?utm_source=summit",
            "https://addons.mozilla.org/de/firefox/addon/darkreader"}) {
        auto item = ParseExtensionStoreURL(url);
        check(item && item->store == ExtensionStoreItem::Store::Firefox && item->identifier == "darkreader"
            && item->requestURL == "https://addons.mozilla.org/api/v5/addons/addon/darkreader/", url);
    }
    for (auto url : {"http://addons.mozilla.org/firefox/addon/darkreader/",
            "https://addons.mozilla.org.evil.test/firefox/addon/darkreader/",
            "https://addons.mozilla.org@evil.test/firefox/addon/darkreader/",
            "https://evil.test@addons.mozilla.org/firefox/addon/darkreader/",
            "https://addons.mozilla.org:444/firefox/addon/darkreader/",
            "https://addons.mozilla.org/firefox/addon/../",
            "https://addons.mozilla.org/firefox/addon/%2e%2e/",
            "https://addons.mozilla.org/firefox/addon/darkreader/extra",
            "https://addons.mozilla.org/firefox/addon/dark\\reader/",
            "https://addons.mozilla.org/firefox/addon/darkreader\n/",
            "https://chromewebstore.google.com/detail/dark-reader/short",
            "https://chromewebstore.google.com/detail/qqqqqqqqqqqqqqqqqqqqqqqqqqqqqqqq",
            kChromeExtensionStore, kFirefoxExtensionStore, "file:///extension.xpi"})
        check(!ParseExtensionStoreURL(url), url);

    // The card on store pages.
    const std::string token = "0123456789abcdef0123456789abcdef";
    check(StoreOfferLink("install", token) == "summit-extension:install?token=" + token, "offer link");
    for (const char* action : {"install", "manage", "dismiss"}) {
        auto choice = ParseStoreOfferLink(StoreOfferLink(action, token));
        check(choice && choice->action == action && choice->token == token, std::string("offer choice ") + action);
    }
    for (const char* url : {"summit-extension:remove?token=abc", "summit-extension:install?token=", "summit-extension:install",
            "summit-extension:install?token=ABC", "summit-extension:install?token=ab cd", "summit-extensions:install?token=ab",
            "https://example.com/?token=ab", "summit-extension:"})
        check(!ParseStoreOfferLink(url), std::string("bad offer link ") + url);
    StoreOffer offer { "Dark \"Reader\" </script> %OFFER% %STYLE%", false, false, token };
    auto script = StoreOfferScript(&offer);
    check(script.find("\"Install Extension\"") != std::string::npos && script.find("Manage Extension") == std::string::npos, "install card");
    check(script.find("summit-extension:install?token=" + token) != std::string::npos, "install link");
    check(script.find("\"Dark \\\"Reader\\\" </script> %OFFER% %STYLE%\"") != std::string::npos, "name kept as a JSON string");
    check(script.find("%LOGO%") == std::string::npos && script.find("summit-offer-logo-rock-front") != std::string::npos, "logo");
    check(script.find("attachShadow({ mode: 'closed' })") != std::string::npos && script.find("innerHTML") == std::string::npos,
        "built from elements in a closed shadow root");
    offer.installed = true;
    offer.disabled = true;
    script = StoreOfferScript(&offer);
    check(script.find("\"Manage Extension\"") != std::string::npos && script.find("turned off") != std::string::npos
        && script.find("summit-extension:manage?token=" + token) != std::string::npos, "manage card");
    offer.name = std::string("bad \xff utf-8");
    check(StoreOfferScript(&offer).find("const offer = {") != std::string::npos, "invalid UTF-8 replaced");
    check(StoreOfferScript(nullptr).find("const offer = null;") != std::string::npos, "removal script");
    check(StoreItemNameFromTitle("Dark Reader - Chrome Web Store") == "Dark Reader", "Chrome title");
    check(StoreItemNameFromTitle("uBlock Origin \xe2\x80\x93 Get this Extension for \xf0\x9f\xa6\x8a Firefox (en-US)") == "uBlock Origin", "Firefox title");
    check(StoreItemNameFromTitle("  Plain  ") == "Plain", "plain title");
    std::cout << checks << " extension store checks passed\n";
}
