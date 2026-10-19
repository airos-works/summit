#include "WebApps.h"
#include "DefaultBrowser.h"
#include "FaviconCache.h"
#include <Bitmap.h>
#include <File.h>
#include <FindDirectory.h>
#include <IconUtils.h>
#include <Mime.h>
#include <Node.h>
#include <NodeInfo.h>
#include <Path.h>
#include <SymLink.h>
#include <Directory.h>
#include <curl/curl.h>
#include <OS.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

namespace summit {
const char* const kWebAppPageScript = R"js(
const link = document.querySelector('link[rel~="manifest"]');
let manifest = null, manifestURL = '';
if (link && link.href) {
  manifestURL = link.href;
  try {
    const response = await fetch(link.href, { credentials: link.crossOrigin === 'use-credentials' ? 'include' : 'same-origin' });
    if (response.ok) manifest = await response.json();
  } catch (error) { }
}
const icons = [...document.querySelectorAll('link[rel~="icon"], link[rel~="apple-touch-icon"]')]
  .map((icon) => ({ src: icon.href, sizes: icon.sizes ? icon.sizes.value : '', type: icon.type || '' }));
const named = document.querySelector('meta[name="application-name"], meta[name="apple-mobile-web-app-title"]');
return JSON.stringify({ manifest, manifestURL, icons, title: document.title, appName: named ? named.content : '', url: location.href });
)js";

namespace {
size_t Collect(char* data, size_t size, size_t count, void* target)
{
    auto* body = static_cast<std::string*>(target);
    if (body->size() + size * count > (4u << 20)) return 0;
    body->append(data, size * count);
    return size * count;
}

std::string Download(const std::string& url)
{
    std::string body;
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> curl(curl_easy_init(), curl_easy_cleanup);
    if (!curl) return body;
    auto* handle = curl.get();
    curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
    curl_easy_setopt(handle, CURLOPT_USERAGENT, "Summit web app installer");
    curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(handle, CURLOPT_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, Collect);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &body);
    long status = 0;
    if (curl_easy_perform(handle) != CURLE_OK || curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status) != CURLE_OK
        || status < 200 || status >= 300)
        body.clear();
    return body;
}

// Haiku's file icons: 32x32 and 16x16 in the classic attributes, made from
// the app's picture.
void SetIcons(BNode& node, const std::string& data)
{
    BNodeInfo info(&node);
    for (const auto [pixels, size] : { std::pair { 32, B_LARGE_ICON }, std::pair { 16, B_MINI_ICON } }) {
        auto rgba = FaviconCache::DecodeIcon(data.data(), data.size(), pixels);
        if (!rgba) continue;
        BBitmap cmap(BRect(0, 0, pixels - 1, pixels - 1), B_CMAP8);
        if (cmap.InitCheck() == B_OK && BIconUtils::ConvertToCMAP8(rgba.get(), &cmap) == B_OK)
            info.SetIcon(&cmap, size);
    }
}

std::filesystem::path DeskbarApplications()
{
    BPath path;
    if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) != B_OK) return {};
    return std::filesystem::path(path.Path()) / "deskbar" / "menu" / "Applications";
}

void RegisterType()
{
    BMimeType type(kWebAppType);
    if (!type.IsInstalled()) type.Install();
    type.SetShortDescription("Summit web app");
    type.SetLongDescription("A website installed as an app with Summit");
    type.SetPreferredApp(kSummitSignature);
}
}

std::string InstallWebApp(const WebAppPlan& plan, const std::filesystem::path& profileDirectory, WebApp& app)
{
    if (plan.name.empty() || plan.startURL.empty()) return "This page cannot be installed as an app.";
    app.name = plan.name;
    app.startURL = plan.startURL;
    app.scope = plan.scope;
    // An identifier no other app has: the time and a random part.
    static constexpr char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    app.id.clear();
    uint64 seed = system_time() ^ (uint64(find_thread(nullptr)) << 32);
    for (int i = 0; i < 12; ++i) { app.id += digits[seed % 36]; seed = seed / 36 * 6364136223846793005ull + 1442695040888963407ull; }
    std::error_code error;
    const auto directory = profileDirectory / "Apps";
    std::filesystem::create_directories(directory, error);
    auto launcher = directory / LauncherName(plan.name);
    for (int i = 2; std::filesystem::exists(launcher, error) && i < 100; ++i)
        launcher = directory / (LauncherName(plan.name) + " " + std::to_string(i));
    RegisterType();
    BFile file(launcher.c_str(), B_WRITE_ONLY | B_CREATE_FILE | B_ERASE_FILE);
    if (file.InitCheck() != B_OK) return "Could not write " + launcher.string() + ".";
    // The file's text says what it is to anyone who opens it elsewhere.
    const std::string text = "Summit web app\n" + app.name + "\n" + app.startURL + "\n";
    file.Write(text.data(), text.size());
    BNodeInfo(&file).SetType(kWebAppType);
    BNodeInfo(&file).SetPreferredApp(kSummitSignature);
    const BString id(app.id.c_str());
    file.WriteAttrString(kWebAppAttribute, &id);
    const BString url(app.startURL.c_str());
    file.WriteAttrString("META:url", &url);
    if (!plan.iconURL.empty()) {
        const auto icon = Download(plan.iconURL);
        if (!icon.empty()) SetIcons(file, icon);
    }
    app.launcher = launcher.string();
    // Into the Deskbar's Applications menu.
    const auto menu = DeskbarApplications();
    if (!menu.empty()) {
        std::filesystem::create_directories(menu, error);
        auto link = menu / launcher.filename();
        if (!std::filesystem::exists(link, error)) {
            BDirectory folder(menu.c_str());
            folder.CreateSymLink(link.filename().c_str(), launcher.c_str(), nullptr);
        }
    }
    return {};
}

void RemoveWebAppFiles(const WebApp& app)
{
    std::error_code error;
    if (app.launcher.empty()) return;
    const std::filesystem::path launcher(app.launcher);
    const auto menu = DeskbarApplications();
    if (!menu.empty()) {
        const auto link = menu / launcher.filename();
        if (std::filesystem::is_symlink(link, error) && std::filesystem::read_symlink(link, error) == launcher)
            std::filesystem::remove(link, error);
    }
    std::filesystem::remove(launcher, error);
}

std::string WebAppOfLauncher(const entry_ref& ref)
{
    BNode node(&ref);
    BString id;
    if (node.InitCheck() != B_OK || node.ReadAttrString(kWebAppAttribute, &id) != B_OK) return {};
    std::string text = id.String();
    if (text.empty() || text.size() > 64
        || !std::all_of(text.begin(), text.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)); }))
        return {};
    return text;
}
}
