#pragma once
#include <string>

namespace summit {
// What Install as App… makes of a page (#75): the page's web app manifest
// when it has one, else its title and icons.
struct WebAppPlan {
    std::string name, startURL, scope, iconURL;
    // "standalone", "minimal-ui", "fullscreen" or "browser" (the manifest's
    // display; Summit gives every app its own window).
    std::string display;
};
// pageInfo: the JSON Summit's page script returns: {manifest (object or
// null), manifestURL, icons: [{src, sizes, type}], title, appName, url}.
// Empty name when the page cannot be installed.
WebAppPlan PlanWebApp(const std::string& pageInfo);
// Resolves a manifest or page address against base.
std::string ResolveURL(const std::string& base, const std::string& reference);
// Whether url belongs to an app with this scope.
bool InWebAppScope(const std::string& scope, const std::string& url);
// A file name for the app's launcher: no slashes, at most 64 bytes.
std::string LauncherName(const std::string& name);
}
