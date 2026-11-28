// The Android app's bridge to Summit's shared core (src/core), the same code
// air/OS's Summit is built from: the profile (session, bookmarks, history,
// reading list, settings, saved in the same profile.json format), address
// resolution, search engines, type-ahead and user agents. Kotlin calls the
// static natives of works.airos.summit.core.SummitCore; structured results are
// JSON strings.
#include "core/Address.h"
#include "core/LoadError.h"
#include "core/Profile.h"
#include "core/ReadingList.h"
#include "core/Suggest.h"
#include "core/UserAgent.h"

#include <android/log.h>
#include <jni.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <string>

using nlohmann::json;

namespace {

std::mutex gLock;
summit::Profile gProfile;
std::filesystem::path gProfilePath;
bool gOpen = false;
bool gWritable = true;

std::string ToString(JNIEnv* env, jstring value)
{
    if (!value)
        return {};
    const char* chars = env->GetStringUTFChars(value, nullptr);
    std::string result(chars ? chars : "");
    if (chars)
        env->ReleaseStringUTFChars(value, chars);
    return result;
}

jstring ToJava(JNIEnv* env, const std::string& value)
{
    // NewStringUTF wants modified UTF-8; json::dump(-1, ' ', true) keeps
    // non-ASCII text escaped, and other strings here come from the profile
    // (valid UTF-8). Characters outside the BMP would need surrogate pairs,
    // so go through a Java byte array to be safe.
    jbyteArray bytes = env->NewByteArray(static_cast<jsize>(value.size()));
    env->SetByteArrayRegion(bytes, 0, static_cast<jsize>(value.size()), reinterpret_cast<const jbyte*>(value.data()));
    jclass stringClass = env->FindClass("java/lang/String");
    jmethodID constructor = env->GetMethodID(stringClass, "<init>", "([BLjava/lang/String;)V");
    jstring charset = env->NewStringUTF("UTF-8");
    auto result = static_cast<jstring>(env->NewObject(stringClass, constructor, bytes, charset));
    env->DeleteLocalRef(bytes);
    env->DeleteLocalRef(charset);
    env->DeleteLocalRef(stringClass);
    return result;
}

json PageJson(const summit::PageRecord& page)
{
    return {{"url", page.url}, {"title", page.title}, {"visited", page.visited}};
}

int64_t Now()
{
    return static_cast<int64_t>(std::time(nullptr));
}

std::vector<summit::PageRecord> BookmarkPages()
{
    std::vector<summit::PageRecord> pages = gProfile.bookmarks.Pages();
    return pages;
}

} // namespace

#define SUMMIT_JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_works_airos_summit_core_SummitCore_##name

// Opens (or restores from its backups) <dir>/profile.json. Returns JSON
// {"error": "...", "restoredFrom": "...", "writable": bool}.
SUMMIT_JNI(jstring, open)(JNIEnv* env, jclass, jstring directory)
{
    std::lock_guard lock(gLock);
    std::filesystem::path dir = ToString(env, directory);
    std::error_code ignored;
    std::filesystem::create_directories(dir, ignored);
    gProfilePath = dir / "profile.json";
    summit::ProfileLoad load = summit::Profile::LoadOrRestore(gProfilePath);
    gProfile = std::move(load.profile);
    gWritable = load.writable;
    gOpen = true;
    summit::SetSearchEngine(gProfile.searchEngine);
    json result = {{"error", load.error}, {"restoredFrom", load.restoredFrom.string()}, {"writable", load.writable}};
    return ToJava(env, result.dump());
}

// Saves the profile. Returns an error message, or "" when saved.
SUMMIT_JNI(jstring, save)(JNIEnv* env, jclass)
{
    std::lock_guard lock(gLock);
    if (!gOpen)
        return ToJava(env, "profile not open");
    if (!gWritable)
        return ToJava(env, "profile is read-only");
    std::string error;
    gProfile.Save(gProfilePath, error);
    return ToJava(env, error);
}

// {"url": ..., "search": bool, "error": ...} for what was typed in the address field.
SUMMIT_JNI(jstring, resolveAddress)(JNIEnv* env, jclass, jstring input)
{
    std::lock_guard lock(gLock);
    std::string text = ToString(env, input);
    if (auto keyword = gProfile.bookmarks.ResolveKeyword(text)) {
        json result = {{"url", keyword->url}, {"search", false}, {"error", ""}};
        return ToJava(env, result.dump());
    }
    summit::Address address = summit::ResolveAddress(text);
    json result = {{"url", address.url}, {"search", address.search}, {"error", address.error}};
    return ToJava(env, result.dump());
}

// Type-ahead: {"fill": {"text", "url"}, "pages": [{"url", "title", "bookmark"}]}.
SUMMIT_JNI(jstring, matchAddress)(JNIEnv* env, jclass, jstring typed, jint limit, jboolean complete)
{
    std::lock_guard lock(gLock);
    std::string text = ToString(env, typed);
    auto matches = summit::MatchAddress(gProfile.history, BookmarkPages(), text, Now(),
        static_cast<size_t>(limit), complete);
    json pages = json::array();
    for (const auto& page : matches.pages)
        pages.push_back({{"url", page.url}, {"title", page.title},
            {"bookmark", page.kind == summit::Suggestion::Kind::Bookmark},
            {"short", summit::ShortAddress(page.url)}});
    json result = {{"fill", {{"text", matches.fill.text}, {"url", matches.fill.url}}}, {"pages", pages}};
    return ToJava(env, result.dump());
}

SUMMIT_JNI(void, visit)(JNIEnv* env, jclass, jstring url, jstring title)
{
    std::lock_guard lock(gLock);
    summit::PageRecord page;
    page.url = ToString(env, url);
    page.title = ToString(env, title);
    gProfile.Visit(page);
}

// History, newest first: [{"url", "title", "visited"}].
SUMMIT_JNI(jstring, history)(JNIEnv* env, jclass, jint limit)
{
    std::lock_guard lock(gLock);
    std::vector<const summit::PageRecord*> pages;
    for (const auto& page : gProfile.history)
        pages.push_back(&page);
    std::stable_sort(pages.begin(), pages.end(), [](auto* a, auto* b) { return a->visited > b->visited; });
    json result = json::array();
    for (auto* page : pages) {
        if (limit > 0 && static_cast<jint>(result.size()) >= limit)
            break;
        result.push_back(PageJson(*page));
    }
    return ToJava(env, result.dump());
}

SUMMIT_JNI(void, removeHistory)(JNIEnv* env, jclass, jstring url)
{
    std::lock_guard lock(gLock);
    std::string target = ToString(env, url);
    std::erase_if(gProfile.history, [&](const summit::PageRecord& page) { return page.url == target; });
}

// Forgets visits since `since` (seconds since the epoch; 0 clears everything).
SUMMIT_JNI(jint, clearHistory)(JNIEnv*, jclass, jlong since)
{
    std::lock_guard lock(gLock);
    if (since <= 0) {
        jint count = static_cast<jint>(gProfile.history.size());
        gProfile.history.clear();
        return count;
    }
    return static_cast<jint>(gProfile.ForgetVisitsSince(since));
}

SUMMIT_JNI(jboolean, isBookmarked)(JNIEnv* env, jclass, jstring url)
{
    std::lock_guard lock(gLock);
    return gProfile.bookmarks.Contains(ToString(env, url));
}

// Adds a bookmark for the page, or removes the existing one. Returns whether
// the page is bookmarked afterwards.
SUMMIT_JNI(jboolean, toggleBookmark)(JNIEnv* env, jclass, jstring url, jstring title)
{
    std::lock_guard lock(gLock);
    std::string address = ToString(env, url);
    if (gProfile.bookmarks.Contains(address)) {
        gProfile.RemoveBookmark(address);
        return false;
    }
    summit::PageRecord page;
    page.url = address;
    page.title = ToString(env, title);
    gProfile.AddBookmark(page, false);
    return true;
}

namespace {
json NodeJson(const summit::BookmarkNode& node)
{
    json result = {{"id", node.id}, {"title", node.title}};
    if (node.folder) {
        json children = json::array();
        for (const auto& child : node.children)
            children.push_back(NodeJson(child));
        result["children"] = children;
    } else if (node.separator) {
        result["separator"] = true;
    } else {
        result["url"] = node.url;
    }
    return result;
}
}

// The bookmark tree: {"bar": node, "other": node}; folders have "children".
SUMMIT_JNI(jstring, bookmarks)(JNIEnv* env, jclass)
{
    std::lock_guard lock(gLock);
    json result = {{"bar", NodeJson(gProfile.bookmarks.Bar())}, {"other", NodeJson(gProfile.bookmarks.Other())}};
    return ToJava(env, result.dump());
}

SUMMIT_JNI(jstring, readingList)(JNIEnv* env, jclass)
{
    std::lock_guard lock(gLock);
    json result = json::array();
    for (const auto& item : gProfile.readingList)
        result.push_back({{"url", item.url}, {"title", item.title}, {"read", item.read != 0}});
    return ToJava(env, result.dump());
}

SUMMIT_JNI(void, addToReadingList)(JNIEnv* env, jclass, jstring url, jstring title)
{
    std::lock_guard lock(gLock);
    summit::AddToReadingList(gProfile.readingList, ToString(env, url), ToString(env, title), Now());
}

// The first window's tabs: {"tabs": [{"url", "title"}], "selected": n}.
SUMMIT_JNI(jstring, session)(JNIEnv* env, jclass)
{
    std::lock_guard lock(gLock);
    json tabs = json::array();
    size_t selected = 0;
    if (!gProfile.windows.empty()) {
        for (const auto& tab : gProfile.windows.front().tabs)
            tabs.push_back(PageJson(tab));
        selected = gProfile.windows.front().selected;
    }
    json result = {{"tabs", tabs}, {"selected", selected}, {"restore", gProfile.restoreSession}};
    return ToJava(env, result.dump());
}

SUMMIT_JNI(void, setSession)(JNIEnv* env, jclass, jstring sessionJson)
{
    std::lock_guard lock(gLock);
    json session = json::parse(ToString(env, sessionJson), nullptr, false);
    if (session.is_discarded() || !session.is_object())
        return;
    summit::WindowSession window;
    for (const auto& tab : session.value("tabs", json::array())) {
        summit::PageRecord page;
        page.url = tab.value("url", "");
        page.title = tab.value("title", "");
        if (!page.url.empty())
            window.tabs.push_back(page);
    }
    window.selected = session.value("selected", 0u);
    if (window.selected >= window.tabs.size())
        window.selected = window.tabs.empty() ? 0 : window.tabs.size() - 1;
    gProfile.windows.clear();
    if (!window.tabs.empty())
        gProfile.windows.push_back(std::move(window));
}

// Settings shared with air/OS's Preferences, as strings ("true"/"false" for
// switches). Unknown keys read as "".
SUMMIT_JNI(jstring, setting)(JNIEnv* env, jclass, jstring keyString)
{
    std::lock_guard lock(gLock);
    std::string key = ToString(env, keyString);
    auto flag = [](bool value) { return std::string(value ? "true" : "false"); };
    std::string value;
    if (key == "searchEngine") value = gProfile.searchEngine;
    else if (key == "homeURL") value = gProfile.homeURL;
    else if (key == "userAgent") value = gProfile.userAgent;
    else if (key == "autoplay") value = gProfile.autoplay;
    else if (key == "restoreSession") value = flag(gProfile.restoreSession);
    else if (key == "httpsFirst") value = flag(gProfile.httpsFirst);
    else if (key == "preventCrossSiteTracking") value = flag(gProfile.preventCrossSiteTracking);
    else if (key == "savePasswords") value = flag(gProfile.savePasswords);
    else if (key == "reduceMotion") value = flag(gProfile.reduceMotion);
    else if (key == "unloadWhenMemoryIsLow") value = flag(gProfile.unloadWhenMemoryIsLow);
    else if (key == "spellChecking") value = flag(gProfile.spellChecking);
    else if (key == "minimumFontSize") value = std::to_string(gProfile.minimumFontSize);
    return ToJava(env, value);
}

SUMMIT_JNI(jboolean, setSetting)(JNIEnv* env, jclass, jstring keyString, jstring valueString)
{
    std::lock_guard lock(gLock);
    std::string key = ToString(env, keyString);
    std::string value = ToString(env, valueString);
    bool on = value == "true";
    if (key == "searchEngine") {
        summit::SetSearchEngine(value);
        gProfile.searchEngine = summit::CurrentSearchEngine().id;
    } else if (key == "homeURL") gProfile.homeURL = value;
    else if (key == "userAgent") gProfile.userAgent = summit::FindUserAgentPreset(value).id;
    else if (key == "autoplay") gProfile.autoplay = value;
    else if (key == "restoreSession") gProfile.restoreSession = on;
    else if (key == "httpsFirst") gProfile.httpsFirst = on;
    else if (key == "preventCrossSiteTracking") gProfile.preventCrossSiteTracking = on;
    else if (key == "savePasswords") gProfile.savePasswords = on;
    else if (key == "reduceMotion") gProfile.reduceMotion = on;
    else if (key == "unloadWhenMemoryIsLow") gProfile.unloadWhenMemoryIsLow = on;
    else if (key == "spellChecking") gProfile.spellChecking = on;
    else if (key == "minimumFontSize") gProfile.minimumFontSize = std::atoi(value.c_str());
    else return false;
    return true;
}

// [{"id", "name"}] in menu order.
SUMMIT_JNI(jstring, searchEngines)(JNIEnv* env, jclass)
{
    json result = json::array();
    for (const auto& engine : summit::SearchEngines())
        result.push_back({{"id", engine.id}, {"name", engine.name}});
    return ToJava(env, result.dump());
}

// [{"id", "name", "agent"}]; an empty agent means the engine's own.
SUMMIT_JNI(jstring, userAgentPresets)(JNIEnv* env, jclass)
{
    json result = json::array();
    for (const auto& preset : summit::UserAgentPresets())
        result.push_back({{"id", preset.id}, {"name", preset.name}, {"agent", preset.agent}});
    return ToJava(env, result.dump());
}

SUMMIT_JNI(jstring, mobileUserAgent)(JNIEnv* env, jclass)
{
    return ToJava(env, summit::MobileUserAgent());
}

SUMMIT_JNI(jstring, shortAddress)(JNIEnv* env, jclass, jstring url)
{
    return ToJava(env, summit::ShortAddress(ToString(env, url)));
}

// A site's saved answer for a permission (Profile::sitePermissions, the same
// list air/OS's Preferences › Site Permissions edits): "allow", "deny" or "".
SUMMIT_JNI(jstring, sitePermission)(JNIEnv* env, jclass, jstring permissionString, jstring originString)
{
    std::lock_guard lock(gLock);
    auto permission = gProfile.sitePermissions.find(ToString(env, permissionString));
    if (permission == gProfile.sitePermissions.end())
        return ToJava(env, "");
    auto entry = permission->second.find(ToString(env, originString));
    if (entry == permission->second.end())
        return ToJava(env, "");
    return ToJava(env, entry->second ? "allow" : "deny");
}

// Saves (or, with "", forgets) an answer. Returns false for permissions and
// origins the profile does not keep.
SUMMIT_JNI(jboolean, setSitePermission)(JNIEnv* env, jclass, jstring permission, jstring origin, jstring answer)
{
    std::lock_guard lock(gLock);
    std::string value = ToString(env, answer);
    std::optional<bool> allowed;
    if (value == "allow") allowed = true;
    else if (value == "deny") allowed = false;
    return gProfile.SetSitePermission(ToString(env, permission), ToString(env, origin), allowed);
}

// The error page's text for a failed load, the same words air/OS's Summit
// uses (core/LoadError.h). kind: "host", "refused", "reset", "timeout",
// "offline", "secure" or anything else. Returns {"title", "explanation"}.
SUMMIT_JNI(jstring, describeLoadFailure)(JNIEnv* env, jclass, jstring kindString, jstring url, jstring description)
{
    std::string kind = ToString(env, kindString);
    summit::LoadFailure failure = summit::LoadFailure::Other;
    if (kind == "host") failure = summit::LoadFailure::HostNotFound;
    else if (kind == "refused") failure = summit::LoadFailure::Refused;
    else if (kind == "reset") failure = summit::LoadFailure::Reset;
    else if (kind == "timeout") failure = summit::LoadFailure::TimedOut;
    else if (kind == "offline") failure = summit::LoadFailure::Offline;
    else if (kind == "secure") failure = summit::LoadFailure::Secure;
    auto text = summit::DescribeLoadFailure(failure, ToString(env, url), ToString(env, description));
    json result = {{"title", text.title}, {"explanation", text.explanation}};
    return ToJava(env, result.dump());
}
