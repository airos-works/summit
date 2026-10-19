#pragma once
#include "core/Profile.h"
#include "core/WebAppManifest.h"
#include <Entry.h>
#include <filesystem>
#include <string>

namespace summit {
// Websites installed as apps (#75). Each has a launcher file in the
// profile's Apps folder: a file of the type kWebAppType, which Summit opens,
// with the app's icon and its identifier in the attribute "SUMMIT:app",
// linked into the Deskbar's Applications menu. Opening it (Deskbar, Tracker,
// or `Summit --app ID`) shows the app in a window of its own.
inline constexpr const char* kWebAppType = "application/x-vnd.Kunanyi-Summit-webapp";
inline constexpr const char* kWebAppAttribute = "SUMMIT:app";
// The script that tells Install as App… about the page (an async function
// body for BWebKitView::EvaluateJavaScript): its manifest, icons and title.
extern const char* const kWebAppPageScript;
// Downloads the icon and makes the launcher and its Deskbar link; app is
// filled in. Blocks (network): call it on a thread of its own. An error
// message, or empty.
std::string InstallWebApp(const WebAppPlan& plan, const std::filesystem::path& profileDirectory, WebApp& app);
// Removes the launcher and its Deskbar link (not the profile's record).
void RemoveWebAppFiles(const WebApp& app);
// The app a launcher file opens, or empty when ref is not one.
std::string WebAppOfLauncher(const entry_ref& ref);
}
