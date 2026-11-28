// The Android app's bridge to Summit's WebExtensions: the engine's
// webkit_summit_extensions_* API (Source/WebKit/UIProcess/Extensions/wpe/
// SummitExtensionsWPE.cpp, the WPE counterpart of BWebKitContext's extension
// methods) and the shared catalog of installed extensions (core/ExtensionCatalog,
// as air/OS's Summit keeps it). Kotlin: works.airos.summit.extensions.ExtensionsNative.
#include "core/ExtensionCatalog.h"
#include "core/ExtensionIdentity.h"
#include "core/ExtensionStore.h"
#include "core/ExtensionStoreDownload.h"
#include "core/StoreOffer.h"

#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>
#include <nlohmann/json.hpp>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

using nlohmann::json;

namespace {

using Sink = void (*)(const char*, void*);
using InitializeFunction = void (*)(void* session, const char* storage, Sink, void*);
using RequestFunction = char* (*)(const char*);
using PageIdFunction = uint64_t (*)(void* webView);
using FreeFunction = void (*)(void*);

struct Engine {
    InitializeFunction initialize = nullptr;
    RequestFunction request = nullptr;
    PageIdFunction pageId = nullptr;
    FreeFunction free = nullptr;
};

JavaVM* gVM = nullptr;
jclass gNativeClass = nullptr;
jmethodID gOnEvent = nullptr;
std::unique_ptr<summit::ExtensionCatalog> gCatalog;
std::mutex gCatalogLock;

const Engine& engine()
{
    static Engine engine = [] {
        Engine e;
        // The engine and GLib are loaded (by wpeview) before this is used.
        void* webkit = dlopen("libWPEWebKit-2.0.so", RTLD_NOW | RTLD_NOLOAD);
        void* glib = dlopen("libglib-2.0.so", RTLD_NOW | RTLD_NOLOAD);
        if (webkit) {
            e.initialize = reinterpret_cast<InitializeFunction>(dlsym(webkit, "webkit_summit_extensions_initialize"));
            e.request = reinterpret_cast<RequestFunction>(dlsym(webkit, "webkit_summit_extensions_request"));
            e.pageId = reinterpret_cast<PageIdFunction>(dlsym(webkit, "webkit_summit_extensions_page_identifier"));
        }
        if (glib)
            e.free = reinterpret_cast<FreeFunction>(dlsym(glib, "g_free"));
        if (!e.initialize || !e.request)
            __android_log_print(ANDROID_LOG_WARN, "Summit", "the engine has no Summit WebExtensions");
        return e;
    }();
    return engine;
}

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

// Engine events, on the main thread (where the engine runs its sink).
void onEngineEvent(const char* text, void*)
{
    if (!gVM || !gNativeClass || !gOnEvent || !text)
        return;
    JNIEnv* env = nullptr;
    bool attached = false;
    if (gVM->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        if (gVM->AttachCurrentThread(&env, nullptr) != JNI_OK)
            return;
        attached = true;
    }
    jstring event = ToJava(env, text);
    env->CallStaticVoidMethod(gNativeClass, gOnEvent, event);
    if (env->ExceptionCheck())
        env->ExceptionClear();
    env->DeleteLocalRef(event);
    if (attached)
        gVM->DetachCurrentThread();
}

json EntryJson(const summit::InstalledExtension& e)
{
    return { { "identifier", e.identifier }, { "name", e.name }, { "version", e.version }, { "fingerprint", e.fingerprint },
        { "package", e.package }, { "enabled", e.enabled }, { "allowFileURLs", e.allowFileURLs },
        { "allowPrivateBrowsing", e.allowPrivateBrowsing }, { "installationOrder", e.installationOrder }, { "source", e.source },
        { "permissions", e.permissions }, { "origins", e.origins } };
}

} // namespace

#define SUMMIT_EXT_JNI(ret, name) extern "C" JNIEXPORT ret JNICALL Java_works_airos_summit_extensions_ExtensionsNative_##name

// Starts the engine's extensions for a network session (WebContext's handle).
// catalogDirectory holds the installed packages and their record.
SUMMIT_EXT_JNI(jboolean, initialize)(JNIEnv* env, jclass type, jlong session, jstring storage, jstring catalogDirectory)
{
    env->GetJavaVM(&gVM);
    if (!gNativeClass) {
        gNativeClass = static_cast<jclass>(env->NewGlobalRef(type));
        gOnEvent = env->GetStaticMethodID(gNativeClass, "onEvent", "(Ljava/lang/String;)V");
    }
    {
        std::lock_guard lock(gCatalogLock);
        gCatalog = std::make_unique<summit::ExtensionCatalog>(ToString(env, catalogDirectory));
    }
    auto& e = engine();
    if (!e.initialize || !session)
        return false;
    auto storageDirectory = ToString(env, storage);
    e.initialize(reinterpret_cast<void*>(session), storageDirectory.c_str(), onEngineEvent, nullptr);
    return true;
}

SUMMIT_EXT_JNI(jstring, request)(JNIEnv* env, jclass, jstring requestJson)
{
    auto& e = engine();
    if (!e.request)
        return ToJava(env, "{\"error\":\"unavailable\"}");
    auto text = ToString(env, requestJson);
    char* answer = e.request(text.c_str());
    std::string result = answer ? answer : "{}";
    if (answer && e.free)
        e.free(answer);
    return ToJava(env, result);
}

// The engine's identifier of a page (WebPage's handle): its WebPageProxyIdentifier,
// which browser commands and tab lists name tabs by.
SUMMIT_EXT_JNI(jlong, pageId)(JNIEnv*, jclass, jlong webView)
{
    auto& e = engine();
    return e.pageId && webView ? static_cast<jlong>(e.pageId(reinterpret_cast<void*>(webView))) : 0;
}

// The installed extensions: [{identifier, name, version, package, path, ...}].
SUMMIT_EXT_JNI(jstring, catalog)(JNIEnv* env, jclass)
{
    std::lock_guard lock(gCatalogLock);
    json list = json::array();
    std::vector<summit::InstalledExtension> entries;
    std::string error;
    if (gCatalog && gCatalog->Load(entries, error)) {
        for (auto& entry : entries) {
            auto item = EntryJson(entry);
            item["path"] = gCatalog->PackagePath(entry).string();
            list.push_back(item);
        }
    }
    return ToJava(env, list.dump());
}

// Copies a package into the catalog's staging area; returns {"staged": path} or {"error"}.
// The staged copy is kept until installStaged or discardStaged.
namespace {
std::unique_ptr<summit::StagedExtensionPackage> gStaged;
}

SUMMIT_EXT_JNI(jstring, stage)(JNIEnv* env, jclass, jstring source)
{
    std::lock_guard lock(gCatalogLock);
    std::string error;
    gStaged = gCatalog ? gCatalog->Stage(ToString(env, source), error) : nullptr;
    json result = { { "error", error.empty() ? "no catalog" : error } };
    if (gStaged) {
        // As air/OS's installer (src/ui/ExtensionInstaller.cpp): the package is
        // named by its staging directory, and installs after the others.
        std::vector<summit::InstalledExtension> entries;
        std::string ignored;
        gCatalog->Load(entries, ignored);
        result = { { "staged", gStaged->Path().string() }, { "package", gStaged->Path().parent_path().filename().string() },
            { "order", summit::ExtensionCatalog::NextInstallationOrder(entries) } };
    }
    return ToJava(env, result.dump());
}

// Records the staged package as installed (entry: the fields of EntryJson). Returns an error or "".
SUMMIT_EXT_JNI(jstring, installStaged)(JNIEnv* env, jclass, jstring entryJson)
{
    std::lock_guard lock(gCatalogLock);
    if (!gCatalog || !gStaged)
        return ToJava(env, "nothing staged");
    json in = json::parse(ToString(env, entryJson), nullptr, false);
    if (in.is_discarded())
        return ToJava(env, "bad entry");
    summit::InstalledExtension entry;
    entry.identifier = in.value("identifier", "");
    entry.name = in.value("name", "");
    entry.version = in.value("version", "");
    entry.fingerprint = in.value("fingerprint", "");
    entry.source = in.value("source", "");
    entry.allowPrivateBrowsing = in.value("allowPrivateBrowsing", false);
    entry.permissions = in.value("permissions", std::vector<std::string> {});
    entry.origins = in.value("origins", std::vector<std::string> {});
    std::vector<summit::InstalledExtension> existing;
    std::string error;
    gCatalog->Load(existing, error);
    entry.package = in.value("package", "");
    entry.installationOrder = in.value("installationOrder", summit::ExtensionCatalog::NextInstallationOrder(existing));
    error.clear();
    bool installed = gCatalog->Install(*gStaged, entry, error);
    gStaged.reset();
    return ToJava(env, installed ? "" : error);
}

SUMMIT_EXT_JNI(void, discardStaged)(JNIEnv*, jclass)
{
    std::lock_guard lock(gCatalogLock);
    gStaged.reset();
}

SUMMIT_EXT_JNI(jstring, forget)(JNIEnv* env, jclass, jstring identifier)
{
    std::lock_guard lock(gCatalogLock);
    std::string error;
    if (!gCatalog || !gCatalog->Forget(ToString(env, identifier), error))
        return ToJava(env, error.empty() ? "no catalog" : error);
    return ToJava(env, "");
}

// The identity the engine loads an extension under (core/ExtensionIdentity):
// Firefox's add-on ID, the verified Chrome ID, else the local one.
SUMMIT_EXT_JNI(jstring, identity)(JNIEnv* env, jclass, jstring manifestJson, jstring localIdentity, jstring verifiedCRX)
{
    json manifest = json::parse(ToString(env, manifestJson), nullptr, false);
    if (manifest.is_discarded())
        manifest = json::object();
    return ToJava(env, summit::ExtensionIdentity(manifest, ToString(env, localIdentity), ToString(env, verifiedCRX)));
}

// ---------------------------------------------------------------- the stores
// The Chrome Web Store's and Firefox Add-ons' extension pages, with the card
// air/OS's Summit puts on them (core/StoreOffer, core/ExtensionStoreDownload).

namespace {

std::optional<summit::ExtensionStoreItem> StoreItem(JNIEnv* env, jstring url)
{
    return summit::ParseExtensionStoreURL(ToString(env, url));
}

std::atomic<bool> gStoreCancelled { false };

} // namespace

// {"store": "chrome"|"firefox", "identifier"} for an extension's store page, or "".
SUMMIT_EXT_JNI(jstring, storeItem)(JNIEnv* env, jclass, jstring url)
{
    auto item = StoreItem(env, url);
    if (!item)
        return ToJava(env, "");
    json result = { { "store", item->store == summit::ExtensionStoreItem::Store::Chrome ? "chrome" : "firefox" },
        { "identifier", item->identifier } };
    return ToJava(env, result.dump());
}

// The extension's name from the store page's title.
SUMMIT_EXT_JNI(jstring, storeItemName)(JNIEnv* env, jclass, jstring title)
{
    return ToJava(env, summit::StoreItemNameFromTitle(ToString(env, title)));
}

// The script that shows the card ({"name", "installed", "disabled", "token"}) or, given "", removes it.
SUMMIT_EXT_JNI(jstring, storeOfferScript)(JNIEnv* env, jclass, jstring offerJson)
{
    json in = json::parse(ToString(env, offerJson), nullptr, false);
    if (!in.is_object())
        return ToJava(env, summit::StoreOfferScript(nullptr));
    summit::StoreOffer offer { in.value("name", ""), in.value("installed", false), in.value("disabled", false), in.value("token", "") };
    return ToJava(env, summit::StoreOfferScript(&offer));
}

// {"action", "token"} for one of the card's links, or "".
SUMMIT_EXT_JNI(jstring, parseStoreOfferLink)(JNIEnv* env, jclass, jstring url)
{
    auto choice = summit::ParseStoreOfferLink(ToString(env, url));
    return ToJava(env, choice ? json({ { "action", choice->action }, { "token", choice->token } }).dump() : std::string());
}

// A Firefox Add-ons page's add-on: {"identifier", "name", "extension"} or {"error"}. Blocks for the network.
SUMMIT_EXT_JNI(jstring, resolveFirefoxItem)(JNIEnv* env, jclass, jstring url)
{
    auto item = StoreItem(env, url);
    if (!item || item->store != summit::ExtensionStoreItem::Store::Firefox)
        return ToJava(env, json({ { "error", "not a Firefox Add-ons extension page" } }).dump());
    std::string identifier, name, error;
    bool extension = true;
    if (!summit::ResolveFirefoxStoreItem(*item, identifier, name, extension, error))
        return ToJava(env, json({ { "error", error } }).dump());
    return ToJava(env, json({ { "identifier", identifier }, { "name", name }, { "extension", extension } }).dump());
}

// Downloads the store page's extension and stages it, as stage() a file:
// {"staged", "package", "order", "expectedIdentity", "store"} or {"error"}.
// Blocks for the network; cancelStoreDownload() stops it.
SUMMIT_EXT_JNI(jstring, downloadStoreItem)(JNIEnv* env, jclass, jstring url, jstring temporaryDirectory)
{
    auto item = StoreItem(env, url);
    if (!item)
        return ToJava(env, json({ { "error", "not an extension's store page" } }).dump());
    summit::ExtensionCatalog* catalog = nullptr;
    {
        std::lock_guard lock(gCatalogLock);
        catalog = gCatalog.get();
    }
    if (!catalog)
        return ToJava(env, json({ { "error", "no catalog" } }).dump());
    gStoreCancelled = false;
    std::string expectedIdentity, error;
    std::filesystem::path temporary = ToString(env, temporaryDirectory);
    std::error_code ignored;
    std::filesystem::create_directories(temporary, ignored);
    auto staged = summit::DownloadStoreExtension(*item, temporary, *catalog, gStoreCancelled, expectedIdentity, error);
    if (!staged)
        return ToJava(env, json({ { "error", error.empty() ? "The extension could not be downloaded." : error } }).dump());
    std::lock_guard lock(gCatalogLock);
    std::vector<summit::InstalledExtension> entries;
    std::string ignoredError;
    gCatalog->Load(entries, ignoredError);
    gStaged = std::move(staged);
    json result = { { "staged", gStaged->Path().string() }, { "package", gStaged->Path().parent_path().filename().string() },
        { "order", summit::ExtensionCatalog::NextInstallationOrder(entries) }, { "expectedIdentity", expectedIdentity },
        { "store", item->store == summit::ExtensionStoreItem::Store::Chrome ? "Chrome Web Store" : "Firefox Add-ons" } };
    return ToJava(env, result.dump());
}

SUMMIT_EXT_JNI(void, cancelStoreDownload)(JNIEnv*, jclass)
{
    gStoreCancelled = true;
}
