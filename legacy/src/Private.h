/*
 * Copyright 2026 Summit contributors. Distributed under the terms of the MIT License.
 *
 * The legacy BWebView API (libWebKitLegacy) on Summit's engine: what the
 * classes share. docs/legacy-webview.md describes the whole library.
 */
#pragma once

#include "WebDownload.h"
#include "WebFrame.h"
#include "WebPage.h"
#include "WebSettings.h"
#include "WebView.h"

#include <WebKit/WebKitContext.h>
#include <WebKit/WebKitEmbedding.h>
#include <WebKit/WebKitView.h>

#include <Path.h>
#include <String.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>

class BBitmap;
class BWindow;

namespace BPrivate {
class WebPagePrivate;
namespace Network {
class BUrlContext;
}
}

namespace SummitLegacy {

enum : uint32 { kApplySettings = 'swas' };

// The engine context every BWebView shares: one website data store (cookies,
// cache, storage) in <persistent storage path>/WebKit. Made on the
// application thread; until then, and on other threads before it exists,
// views use the engine's default context.
std::shared_ptr<BWebKitContext> Context();
// BWebSettings::SetPersistentStoragePath(). Application thread.
void SetStoragePath(const BString& path);
// BWebPage::InitializeOnce()/ShutdownOnce(). Application thread.
status_t Initialize();
void Shutdown();

// Site icons the engine reported, decoded, by page address.
void StoreIcon(const BString& pageURL, const void* data, size_t size);
// Archives the icon of the page into archive as "icon"; false if none.
bool ArchiveIcon(const BString& pageURL, BMessage& archive);
void ClearIcons();

// A page asked for a new one (window.open, a target): the next BWebView made
// on this thread shows it (BWebPage creates it on the application thread).
extern thread_local uint64 gNewPageRequest;

// An address as the old engine took it: with a scheme, a file path, or a
// host name that gets http://.
BString NormalizedURL(const BString& url);

// Writes text to the clipboard as text/plain.
void CopyToClipboard(const BString& text);

// Runs source in the view's main frame as an async function body and waits
// for its string result, up to timeout. Not on the application thread,
// which runs the engine.
status_t EvaluateAndWait(BWebKitView* view, const char* source, BString& result, bigtime_t timeout);

// WebPositive's cookie window reads the BNetworkCookieJar of the BUrlContext
// it gives its views; the engine keeps the cookies (Cookies.cpp). Pages
// register their context and engine view; the jars are kept in step with
// the engine on a looper of the library's. Any thread; RemoveCookieView()
// before the view is deleted (~BWebView, whose child it is).
void StartCookieMirror();
// BWebPage::ShutdownOnce(): the jars are not used any more.
void StopCookieMirror();
void AddCookieJar(BPrivate::Network::BUrlContext* context);
void RemoveCookieJar(BPrivate::Network::BUrlContext* context);
void AddCookieView(BWebKitView* view);
void RemoveCookieView(BWebKitView* view);
// A page finished loading: its cookies are fetched soon.
void SyncCookiesSoon();

// The page's web inspector: Summit's developer tools window (Inspector.cpp).
// Opens it, or shows it again; where (view coordinates, or negative) selects
// the element there. The window's notifications go to page. On the thread
// of near, the page's window, which is locked.
void ShowInspector(BPrivate::WebPagePrivate& data, BHandler* page, BWindow* near, BPoint where);
void CloseInspector(BPrivate::WebPagePrivate& data);
void SetInspectorTitle(BPrivate::WebPagePrivate& data, const BString& title);
// What the inspector window sends its page: it closed ("window"), and the
// dock buttons' request, which is ignored (summit::kDeveloperToolsClosed and
// kDockDeveloperTools of src/ui/Messages.h).
enum : uint32 { kInspectorClosed = 'dvtc', kInspectorDock = 'dvdk' };

}

namespace BPrivate {

// What a BWebPage knows of its page, from the engine's notifications
// (B_WEBKIT_STATE_CHANGED). Written on the application thread, read by the
// window threads that ask BWebView for its title and address.
class WebPagePrivate {
public:
	BWebKitView* view = nullptr;
	mutable std::mutex lock;
	BString url, requestedURL, title;
	bool loading = false, canGoBack = false, canGoForward = false;
	float progress = 0;
	uint64 successSequence = 0;
	std::string outcome;
	bool shutDown = false;
	// A page the engine opened for this view (window.open), shown once the
	// view is in a window.
	uint64 pendingNewPage = 0;
	uint64 nextScript = 1;
	// Scripts whose answers the page waits for, by identifier: what they are for.
	std::map<uint64, uint32> scripts;
	// Legacy listeners reply without an identifier. A separate reply handler
	// binds each response to its engine challenge, on the application looper.
	std::map<uint64, std::unique_ptr<BHandler>> authentications;
	// One asynchronous certificate question per failed navigation. Generation
	// is checked again by the engine when applying an explicit approval.
	std::unique_ptr<BHandler> certificateReply;
	uint64 certificateGeneration = 0;
	// The engine's identifier of the page (B_WEBKIT_STATE_CHANGED), which
	// names it to an inspector that connects after another.
	uint64 pageIdentifier = 0;
	// The page's inspector window, while it is open.
	BMessenger devTools;
	// BWebPage::SetDeveloperExtrasEnabled(): "Inspect element" in the
	// context menu.
	bool developerExtras = true;
};

// A download the engine reported, until it is finished and handed over.
class WebDownloadPrivate {
public:
	uint64 identifier = 0;
	BString url, filename;
	BPath path;
	off_t currentSize = 0, expectedSize = -1;
	BMessenger progressListener;
	// Where BWebDownload::Start() asked for the file; empty until then.
	BPath directory;
	bool started = false, finished = false, succeeded = false, handedOver = false;
	BWebDownload* download = nullptr;

	// BWebDownload's and BWebPage's private parts, which this class may use.
	static BWebDownload* Create(WebDownloadPrivate* data) { return new BWebDownload(data); }
	static void Destroy(BWebDownload* download) { delete download; }
	static BMessenger DownloadListener() { return BWebPage::sDownloadListener; }
};

// Pending setters and committed values are separate: Apply() publishes a
// snapshot. A page's explicit overrides survive later default changes.
class WebSettingsPrivate {
public:
	static void Bind(BWebSettings*, BMessenger page);
	static BMessage Snapshot(BWebSettings*);
	static void ApplyTo(BWebSettings*, BWebKitView*);
	~WebSettingsPrivate();
	std::mutex lock;
	BMessage pending, applied;
	BMessenger page;
	BString localStoragePath;
};

}

// The old headers name WebCore::ChromeClientHaiku a friend of BWebView and
// BWebPage; this library uses the name for what the two classes do to each
// other's private parts.
namespace WebCore {
class ChromeClientHaiku {
public:
	// Makes the page's engine view, as the view's child: at once, or for a
	// page the engine opened, when the view is in a window (the engine then
	// shows the page in a view that has its size and window).
	static void CreateEngineView(BWebPage* page);
};
}

// The main frame of a page (the only one this API exposes).
class WebFramePrivate {
public:
	BWebPage* page = nullptr;
	// The page's engine view; null once the page is shut down.
	BWebKitView* view = nullptr;
};
