/*
 * Copyright 2026 Summit contributors. Distributed under the terms of the MIT License.
 *
 * BWebPage on Summit's engine. As before, the page is a handler of the
 * application looper: the engine's notifications for its view arrive here
 * and go on to the page's listener as the old engine's messages (LOAD_*,
 * TITLE_CHANGED, UPDATE_NAVIGATION_INTERFACE... in WebViewConstants.h).
 */
#include "Private.h"

#include "WebViewConstants.h"

#include <Application.h>
#include <DataIO.h>
#include <Window.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

BMessenger BWebPage::sDownloadListener;

namespace {
enum {
	kShutdown = 'swsd',
	kAuthenticationAnswered = 'saau'
};
enum : uint32 {
	kScriptPageSource = 1
};

BString
StringField(const BMessage& message, const char* name)
{
	const char* value = nullptr;
	return message.FindString(name, &value) == B_OK && value != nullptr ? BString(value) : BString();
}

// Waiting synchronously for the legacy listener here would block the engine's
// application thread while WebPositive's authentication panel is open.
class AuthenticationReply final : public BHandler {
public:
	AuthenticationReply(BMessenger page, uint64 identifier)
		: BHandler("legacy authentication reply"), fPage(page), fIdentifier(identifier) { }

	void MessageReceived(BMessage* message) override
	{
		if (!message->IsReply()) {
			BHandler::MessageReceived(message);
			return;
		}
		const char* user = nullptr;
		const char* password = nullptr;
		if (message->FindString("user", &user) != B_OK
			|| message->FindString("password", &password) != B_OK)
			BWebKitView::CancelAuthentication(fIdentifier);
		else if (!user[0] && !password[0])
			BWebKitView::AnswerAuthentication(fIdentifier, nullptr, nullptr);
		else {
			// WebPositive manages Remember in its own credential storage.
			// The engine keeps an accepted credential only for this session.
			BWebKitView::AnswerAuthentication(fIdentifier, user, password,
				B_WEBKIT_CREDENTIAL_FOR_SESSION);
		}
		BMessage finished(kAuthenticationAnswered);
		finished.AddUInt64("identifier", fIdentifier);
		fPage.SendMessage(&finished);
	}

private:
	BMessenger fPage;
	uint64 fIdentifier;
};

void
ForgetAuthentication(BPrivate::WebPagePrivate& data, uint64 identifier)
{
	auto found = data.authentications.find(identifier);
	if (found == data.authentications.end())
		return;
	BHandler* handler = found->second.get();
	if (handler->Looper() != nullptr)
		handler->Looper()->RemoveHandler(handler);
	data.authentications.erase(found);
}
}


/*static*/ void
BWebPage::InitializeOnce()
{
	SummitLegacy::Initialize();
}


/*static*/ void
BWebPage::ShutdownOnce()
{
	SummitLegacy::Shutdown();
}


/*static*/ void
BWebPage::SetCacheModel(BWebKitCacheModel)
{
	// The engine sizes its caches itself.
}


BWebPage::BWebPage(BWebView* webView, BPrivate::Network::BUrlContext* context)
	:
	BHandler("BWebPage"),
	fWebView(webView),
	fMainFrame(nullptr),
	fSettings(nullptr),
	fContext(context),
	fPagePrivate(new BPrivate::WebPagePrivate()),
	fDumpRenderTree(nullptr),
	fLoadingProgress(100),
	fPageVisible(true),
	fPageDirty(false),
	fLayoutingView(false),
	fToolbarsVisible(true),
	fStatusbarVisible(true),
	fMenubarVisible(true)
{
	WebFramePrivate* frame = new WebFramePrivate();
	frame->page = this;
	fMainFrame = new BWebFrame(this, nullptr, frame);
	fSettings = new BWebSettings();
}


BWebPage::~BWebPage()
{
	delete fMainFrame;
	delete fSettings;
}


void
BWebPage::Init()
{
	if (be_app != nullptr && be_app->Lock()) {
		be_app->AddHandler(this);
		be_app->Unlock();
	}
	fPagePrivate->pendingNewPage = std::exchange(SummitLegacy::gNewPageRequest, 0);
	if (fPagePrivate->pendingNewPage == 0)
		WebCore::ChromeClientHaiku::CreateEngineView(this);
}


/*static*/ void
WebCore::ChromeClientHaiku::CreateEngineView(BWebPage* page)
{
	BPrivate::WebPagePrivate& data = *page->fPagePrivate;
	if (data.view != nullptr || page->fWebView == nullptr)
		return;
	const uint64 request = std::exchange(data.pendingNewPage, 0);
	std::shared_ptr<BWebKitContext> context = SummitLegacy::Context();
	BRect frame = page->fWebView->Bounds();
	if (!frame.IsValid())
		frame.Set(0, 0, 99, 99);
	BWebKitView* view = request != 0
		? new BWebKitView(frame, "engine view", BMessenger(page), request, B_FOLLOW_ALL, context)
		: new BWebKitView(frame, "engine view", BMessenger(page), B_FOLLOW_ALL, context);
	view->SetAuthenticationRequests(true);
	page->fWebView->AddChild(view);
	if (getenv("SUMMIT_LEGACY_TRACE") != nullptr)
		fprintf(stderr, "Summit legacy page %p: engine view %s, frame %.0fx%.0f, in window %d, hidden %d\n", page,
			request != 0 ? "for an opened page" : "", frame.Width() + 1, frame.Height() + 1,
			page->fWebView->Window() != nullptr, page->fWebView->Window() != nullptr && view->IsHidden());
	page->fWebView->fOffscreenView = view;
	data.view = view;
	page->fMainFrame->fData->view = view;
}


void
BWebPage::Shutdown()
{
	{
		std::lock_guard lock(fPagePrivate->lock);
		if (fPagePrivate->shutDown)
			return;
		fPagePrivate->shutDown = true;
	}
	BMessenger(this).SendMessage(kShutdown);
}


void
BWebPage::SetListener(const BMessenger& listener)
{
	fListener = listener;
}


/*static*/ void
BWebPage::SetDownloadListener(const BMessenger& listener)
{
	sDownloadListener = listener;
}


BWebFrame*
BWebPage::MainFrame() const
{
	return fMainFrame;
}


BWebSettings*
BWebPage::Settings() const
{
	return fSettings;
}


BWebView*
BWebPage::WebView() const
{
	return fWebView;
}


void
BWebPage::LoadURL(const char* urlString)
{
	if (fPagePrivate->view == nullptr || urlString == nullptr)
		return;
	BString url = SummitLegacy::NormalizedURL(urlString);
	{
		std::lock_guard lock(fPagePrivate->lock);
		fPagePrivate->requestedURL = url;
	}
	fPagePrivate->view->LoadURL(url.String());
}


void
BWebPage::Reload()
{
	if (fPagePrivate->view != nullptr)
		fPagePrivate->view->Reload();
}


void
BWebPage::GoBack()
{
	if (fPagePrivate->view != nullptr)
		fPagePrivate->view->GoBack();
}


void
BWebPage::GoForward()
{
	if (fPagePrivate->view != nullptr)
		fPagePrivate->view->GoForward();
}


void
BWebPage::StopLoading()
{
	if (fPagePrivate->view != nullptr)
		fPagePrivate->view->Stop();
}


BString
BWebPage::MainFrameTitle() const
{
	std::lock_guard lock(fPagePrivate->lock);
	return fPagePrivate->title;
}


BString
BWebPage::MainFrameRequestedURL() const
{
	std::lock_guard lock(fPagePrivate->lock);
	return fPagePrivate->requestedURL;
}


BString
BWebPage::MainFrameURL() const
{
	std::lock_guard lock(fPagePrivate->lock);
	return fPagePrivate->url;
}


status_t
BWebPage::GetContentsAsMHTML(BDataIO& output)
{
	// The page as a MIME archive of its document: its markup as it stands
	// now, with a base address so that its links and pictures still lead
	// to the site.
	if (fPagePrivate->view == nullptr)
		return B_NO_INIT;
	BString markup;
	status_t status = SummitLegacy::EvaluateAndWait(fPagePrivate->view,
		"const doctype = document.doctype ? '<!DOCTYPE ' + document.doctype.name + '>\\n' : '';"
		"return doctype + document.documentElement.outerHTML;", markup, 15000000);
	if (status != B_OK)
		return status;
	BString url = MainFrameURL();
	BString title = MainFrameTitle();
	title.ReplaceAll("\n", " ");
	BString boundary;
	boundary << "----=_SummitPart_" << system_time();
	BString archive;
	archive << "From: <Saved by Summit>\r\n"
		<< "Snapshot-Content-Location: " << url << "\r\n"
		<< "Subject: " << title << "\r\n"
		<< "MIME-Version: 1.0\r\n"
		<< "Content-Type: multipart/related;\r\n\ttype=\"text/html\";\r\n\tboundary=\"" << boundary << "\"\r\n\r\n"
		<< "--" << boundary << "\r\n"
		<< "Content-Type: text/html; charset=utf-8\r\n"
		<< "Content-Transfer-Encoding: binary\r\n"
		<< "Content-Location: " << url << "\r\n\r\n";
	int32 head = markup.IFindFirst("<head");
	int32 headEnd = head >= 0 ? markup.FindFirst(">", head) : -1;
	if (headEnd >= 0 && markup.IFindFirst("<base") < 0) {
		BString base;
		base << "<base href=\"" << url << "\">";
		markup.Insert(base, headEnd + 1);
	}
	archive << markup << "\r\n--" << boundary << "--\r\n";
	ssize_t written = output.Write(archive.String(), archive.Length());
	return written == archive.Length() ? B_OK : (written < 0 ? written : B_IO_ERROR);
}


void
BWebPage::ChangeZoomFactor(float increment, bool textOnly)
{
	if (fPagePrivate->view == nullptr)
		return;
	if (increment > 0)
		fPagePrivate->view->IncreaseZoomFactor(textOnly);
	else if (increment < 0)
		fPagePrivate->view->DecreaseZoomFactor(textOnly);
	else
		fPagePrivate->view->ResetZoomFactor();
}


void
BWebPage::FindString(const char* string, bool forward, bool caseSensitive,
	bool wrapSelection, bool /*startInSelection*/)
{
	if (fPagePrivate->view != nullptr)
		fPagePrivate->view->FindString(string, forward, caseSensitive, wrapSelection);
}


void
BWebPage::SetDeveloperExtrasEnabled(bool)
{
	// Summit's developer tools come with Summit.
}


void
BWebPage::SetStatusMessage(const BString& status)
{
	fStatusMessage = status;
	setDisplayedStatusMessage(status);
}


void
BWebPage::ResendNotifications()
{
	BMessage navigation(UPDATE_NAVIGATION_INTERFACE);
	BString title, url;
	{
		std::lock_guard lock(fPagePrivate->lock);
		navigation.AddBool("can go backward", fPagePrivate->canGoBack);
		navigation.AddBool("can go forward", fPagePrivate->canGoForward);
		navigation.AddBool("can stop", fPagePrivate->loading);
		title = fPagePrivate->title;
		url = fPagePrivate->url;
	}
	dispatchMessage(navigation);
	if (url.Length() > 0) {
		BMessage committed(LOAD_COMMITTED);
		committed.AddString("url", url);
		dispatchMessage(committed);
	}
	if (title.Length() > 0) {
		BMessage titleChanged(TITLE_CHANGED);
		titleChanged.AddString("title", title);
		dispatchMessage(titleChanged);
	}
	setLoadingProgress(fLoadingProgress);
	setDisplayedStatusMessage(fStatusMessage, true);
}


void
BWebPage::SendEditingCapabilities()
{
	// The engine does not say what the focused element allows; the commands
	// do nothing where they cannot apply.
	BMessage message(B_EDITING_CAPABILITIES_RESULT);
	message.AddBool("can cut", true);
	message.AddBool("can copy", true);
	message.AddBool("can paste", true);
	dispatchMessage(message);
}


void
BWebPage::SendPageSource()
{
	BString url = MainFrameURL();
	if (url.FindFirst("file://") == 0 || fPagePrivate->view == nullptr) {
		BMessage message(B_PAGE_SOURCE_RESULT);
		message.AddString("url", url);
		dispatchMessage(message);
		return;
	}
	// The page's source as the server sent it (from the cache), or its
	// markup as it stands when that cannot be had.
	uint64 identifier;
	{
		std::lock_guard lock(fPagePrivate->lock);
		identifier = fPagePrivate->nextScript++;
		fPagePrivate->scripts[identifier] = kScriptPageSource;
	}
	fPagePrivate->view->EvaluateJavaScript(
		"try { const response = await fetch(location.href, { cache: 'force-cache' });"
		" if (response.ok) return await response.text(); } catch (error) { }"
		"return (document.doctype ? '<!DOCTYPE ' + document.doctype.name + '>\\n' : '')"
		" + document.documentElement.outerHTML;",
		BMessenger(this), identifier, true);
}


void
BWebPage::RequestDownload(const BString& url)
{
	if (fPagePrivate->view != nullptr && url.Length() > 0)
		fPagePrivate->view->DownloadURL(url.String());
}


void
BWebPage::setLoadingProgress(float progress)
{
	fLoadingProgress = progress;
	BMessage message(LOAD_PROGRESS);
	message.AddFloat("progress", progress);
	dispatchMessage(message);
}


void
BWebPage::setStatusMessage(const BString& message)
{
	SetStatusMessage(message);
}


void
BWebPage::setDisplayedStatusMessage(const BString& statusMessage, bool force)
{
	if (fDisplayedStatusMessage == statusMessage && !force)
		return;
	fDisplayedStatusMessage = statusMessage;
	BMessage message(SET_STATUS_TEXT);
	message.AddString("text", statusMessage);
	dispatchMessage(message);
}


status_t
BWebPage::dispatchMessage(BMessage& message, BMessage* reply) const
{
	static const bool trace = getenv("SUMMIT_LEGACY_TRACE") != nullptr;
	if (trace) {
		char what[5] = "";
		if (message.what > 1000)
			memcpy(what, &message.what, 4);
		fprintf(stderr, "Summit legacy page %p: %" B_PRIu32 " %s %s%s\n", this, message.what, what,
			message.GetString("url", message.GetString("title", "")), fListener.IsValid() ? "" : " (no listener)");
	}
	message.AddPointer("view", fWebView);
	if (reply != nullptr)
		return fListener.SendMessage(&message, reply);
	return fListener.SendMessage(&message);
}


void
BWebPage::MessageReceived(BMessage* message)
{
	switch (message->what) {
		case B_WEBKIT_AUTHENTICATION_REQUESTED:
		{
			const uint64 identifier = message->GetUInt64("identifier", 0);
			if (!identifier)
				break;
			if (message->GetBool("withdrawn", false)) {
				// A late reply to an abandoned navigation must not answer a
				// different challenge or retain a handler for a closed page.
				ForgetAuthentication(*fPagePrivate, identifier);
				break;
			}
			if (fPagePrivate->authentications.contains(identifier))
				break;
			bool shuttingDown;
			{
				std::lock_guard lock(fPagePrivate->lock);
				shuttingDown = fPagePrivate->shutDown;
			}
			if (shuttingDown || fWebView == nullptr || !fListener.IsValid() || Looper() == nullptr) {
				BWebKitView::AnswerAuthentication(identifier, nullptr, nullptr);
				break;
			}
			BString text(message->GetBool("proxy", false) ? "Proxy \"" : "Host \"");
			// WebPositive uses this text as its credential key. Include the
			// scheme and port so different origins cannot share that key.
			text << (message->GetBool("secure", false) ? "https://" : "http://")
				<< message->GetString("host", "") << ":" << message->GetInt32("port", 0)
				<< "\" requests authentication for realm \"" << message->GetString("realm", "")
				<< "\"\nAuthentication scheme: " << message->GetString("scheme", "");
			if (!message->GetBool("secure", false)
				&& StringField(*message, "scheme") == "basic")
				text << " (credentials are sent without encryption)";
			BMessage challenge(AUTHENTICATION_CHALLENGE);
			challenge.AddString("text", text);
			challenge.AddString("user", message->GetString("user", ""));
			challenge.AddString("password", "");
			challenge.AddUInt32("failureCount", std::max<int32>(0, message->GetInt32("previous_failures", 0)));
			challenge.AddPointer("view", fWebView);
			auto reply = std::make_unique<AuthenticationReply>(BMessenger(this), identifier);
			Looper()->AddHandler(reply.get());
			BMessenger target(reply.get());
			fPagePrivate->authentications.emplace(identifier, std::move(reply));
			if (fListener.SendMessage(&challenge, target, 0) != B_OK) {
				ForgetAuthentication(*fPagePrivate, identifier);
				BWebKitView::AnswerAuthentication(identifier, nullptr, nullptr);
			}
			break;
		}
		case kAuthenticationAnswered:
			ForgetAuthentication(*fPagePrivate, message->GetUInt64("identifier", 0));
			break;
		case B_WEBKIT_STATE_CHANGED:
		{
			const BString url = StringField(*message, "url");
			const BString title = StringField(*message, "title");
			const bool loading = message->GetBool("loading", false);
			const float progress = static_cast<float>(message->GetDouble("progress", 0)) * 100;
			const bool canGoBack = message->GetBool("canGoBack", false);
			const bool canGoForward = message->GetBool("canGoForward", false);
			const std::string outcome = message->GetString("loadOutcome", "");
			const uint64 successSequence = message->GetUInt64("loadSuccessSequence", 0);
			const BString successURL = StringField(*message, "loadSuccessURL");
			BWebKitView* view = fPagePrivate->view;
			(void)view;
			bool wasLoading, navigationChanged, urlChanged, titleChanged, succeeded;
			{
				std::lock_guard lock(fPagePrivate->lock);
				wasLoading = fPagePrivate->loading;
				navigationChanged = loading != fPagePrivate->loading || canGoBack != fPagePrivate->canGoBack
					|| canGoForward != fPagePrivate->canGoForward;
				urlChanged = url != fPagePrivate->url;
				titleChanged = title != fPagePrivate->title;
				succeeded = successSequence != fPagePrivate->successSequence && successSequence != 0;
				fPagePrivate->url = url;
				fPagePrivate->title = title;
				fPagePrivate->loading = loading;
				fPagePrivate->canGoBack = canGoBack;
				fPagePrivate->canGoForward = canGoForward;
				fPagePrivate->successSequence = successSequence;
				if (loading && !wasLoading)
					fPagePrivate->requestedURL = url;
			}
			if (loading && !wasLoading) {
				BMessage started(LOAD_NEGOTIATING);
				started.AddString("url", url);
				dispatchMessage(started);
				BMessage startedAlso(LOAD_STARTED);
				startedAlso.AddString("url", url);
				dispatchMessage(startedAlso);
			}
			if (urlChanged) {
				BMessage committed(LOAD_COMMITTED);
				committed.AddString("url", url);
				dispatchMessage(committed);
			}
			if (titleChanged) {
				BMessage changed(TITLE_CHANGED);
				changed.AddString("title", title);
				changed.AddString("url", url);
				dispatchMessage(changed);
			}
			if (loading && std::fabs(progress - fLoadingProgress) >= 1)
				setLoadingProgress(progress);
			if (wasLoading && !loading) {
				if (outcome == "failed" || outcome == "process-exited") {
					BMessage failed(LOAD_FAILED);
					failed.AddString("url", url);
					dispatchMessage(failed);
					BString description = StringField(*message, "loadErrorDescription");
					BString failingURL = StringField(*message, "loadErrorURL");
					if (outcome == "process-exited")
						description = "The page's process ended.";
					if (description.Length() > 0) {
						BMessage error(MAIN_DOCUMENT_ERROR);
						error.AddString("url", failingURL.Length() > 0 ? failingURL : url);
						error.AddString("error", description);
						dispatchMessage(error);
					}
				} else {
					setLoadingProgress(100);
					BMessage completed(LOAD_DOC_COMPLETED);
					completed.AddString("url", url);
					dispatchMessage(completed);
					BMessage finished(LOAD_FINISHED);
					finished.AddString("url", url);
					dispatchMessage(finished);
				}
				fLoadingProgress = 100;
			}
			if (succeeded) {
				BMessage history(UPDATE_HISTORY);
				history.AddString("url", successURL.Length() > 0 ? successURL : url);
				dispatchMessage(history);
			}
			if (navigationChanged) {
				BMessage navigation(UPDATE_NAVIGATION_INTERFACE);
				navigation.AddBool("can go backward", canGoBack);
				navigation.AddBool("can go forward", canGoForward);
				navigation.AddBool("can stop", loading);
				dispatchMessage(navigation);
			}
			break;
		}
		case B_WEBKIT_ICON_LOADED:
		{
			const void* data = nullptr;
			ssize_t size = 0;
			const BString url = StringField(*message, "url");
			if (message->FindData("data", B_RAW_TYPE, &data, &size) == B_OK && size > 0) {
				SummitLegacy::StoreIcon(url, data, size);
				BMessage received(ICON_RECEIVED);
				received.AddString("url", url);
				dispatchMessage(received);
			}
			break;
		}
		case B_WEBKIT_LINK_HOVERED:
			setDisplayedStatusMessage(StringField(*message, "url"));
			break;
		case B_WEBKIT_LINK_OPEN_REQUESTED:
		{
			// A middle click (or Command-click) on a link: a new tab, as before.
			BMessage request(NEW_WINDOW_REQUESTED);
			request.AddString("url", StringField(*message, "url"));
			request.AddBool("primary", false);
			dispatchMessage(request);
			break;
		}
		case B_WEBKIT_NEW_PAGE_REQUESTED:
		{
			// window.open() or a link with a target. The new view shows the
			// page the engine opened; the application places it (in a tab, or
			// in a window of the size the page asked for).
			uint64 identifier = 0;
			if (message->FindUInt64("identifier", &identifier) != B_OK)
				break;
			if (!fListener.IsValid()) {
				BWebKitView::DeclineNewPage(identifier);
				break;
			}
			// Preserve the engine's page and opener relationship. The view
			// accepts the request when its host attaches it to a window.
			SummitLegacy::gNewPageRequest = identifier;
			BWebView* view = new BWebView("web view", fContext);
			SummitLegacy::gNewPageRequest = 0;
			BRect frame;
			float width, height, x = 50, y = 50;
			if (message->GetBool("popup", false) && message->FindFloat("width", &width) == B_OK
				&& message->FindFloat("height", &height) == B_OK) {
				message->FindFloat("x", &x);
				message->FindFloat("y", &y);
				frame.Set(x, y, x + width - 1, y + height - 1);
			}
			BMessage created(NEW_PAGE_CREATED);
			created.AddPointer("view", view);
			created.AddRect("frame", frame);
			created.AddBool("modal", false);
			created.AddBool("resizable", true);
			created.AddBool("activate", message->GetBool("user_gesture", true));
			// Unlike dispatchMessage(), "view" is the new page.
			fListener.SendMessage(&created);
			break;
		}
		case B_WEBKIT_CLOSE_REQUESTED:
		{
			BMessage close(CLOSE_WINDOW_REQUESTED);
			dispatchMessage(close);
			break;
		}
		case B_WEBKIT_CONTEXT_MENU:
			if (fWebView != nullptr)
				BMessenger(fWebView).SendMessage(message);
			break;
		case B_WEBKIT_JAVASCRIPT_RESULT:
		{
			uint64 identifier = message->GetUInt64("identifier", 0);
			if (getenv("SUMMIT_LEGACY_TRACE") != nullptr)
				fprintf(stderr, "Summit legacy page %p: script %" B_PRIu64 " answered: %s %.80s\n", this, identifier,
					message->GetString("result_type", ""), message->GetString("error", message->GetString("result", "")));
			uint32 purpose = 0;
			{
				std::lock_guard lock(fPagePrivate->lock);
				auto found = fPagePrivate->scripts.find(identifier);
				if (found == fPagePrivate->scripts.end())
					break;
				purpose = found->second;
				fPagePrivate->scripts.erase(found);
			}
			if (purpose == kScriptPageSource) {
				BMessage source(B_PAGE_SOURCE_RESULT);
				source.AddString("url", MainFrameURL());
				source.AddString("source", StringField(*message, "result"));
				dispatchMessage(source);
			}
			break;
		}
		case kShutdown:
		{
			while (!fPagePrivate->authentications.empty()) {
				const uint64 identifier = fPagePrivate->authentications.begin()->first;
				BWebKitView::CancelAuthentication(identifier);
				ForgetAuthentication(*fPagePrivate, identifier);
			}
			// On the application thread, as before: the view (and with it the
			// engine's page) goes, then the page.
			if (fWebView != nullptr) {
				BWebView* view = std::exchange(fWebView, nullptr);
				view->fWebPage = nullptr;
				if (view->LockLooper()) {
					view->RemoveSelf();
					view->UnlockLooper();
				}
				delete view;
			}
			fPagePrivate->view = nullptr;
			fMainFrame->fData->view = nullptr;
			// A page opened for a view that never reached a window.
			if (fPagePrivate->pendingNewPage != 0)
				BWebKitView::DeclineNewPage(std::exchange(fPagePrivate->pendingNewPage, 0));
			if (Looper() != nullptr)
				Looper()->RemoveHandler(this);
			delete this;
			break;
		}
		default:
			BHandler::MessageReceived(message);
			break;
	}
}
