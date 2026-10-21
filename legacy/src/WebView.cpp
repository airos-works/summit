/*
 * Copyright 2026 Summit contributors. Distributed under the terms of the MIT License.
 *
 * BWebView on Summit's engine: a container for the engine's BWebKitView,
 * which draws the page and takes its input itself. The class layout is the
 * old one's, so programs built for it run unchanged; fOffscreenView holds
 * the engine view, the other drawing members stay unused.
 */
#include "Private.h"

#include "WebViewConstants.h"

#include <Clipboard.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <Window.h>

#include <cstdio>
#include <cstdlib>

namespace {
enum {
	kOpenLinkInNewTab = 'solt',
	kCopyAddress = 'scad',
	kDownloadAddress = 'sdla',
	kOpenImageInNewTab = 'soit',
	kEditCommand = 'sedc',
	kNavigate = 'snav'
};
}

BWebView::UserData::~UserData()
{
}


BWebView::BWebView(const char* name, BPrivate::Network::BUrlContext* context)
	:
	BView(name, B_WILL_DRAW | B_FRAME_EVENTS | B_FULL_UPDATE_ON_RESIZE),
	fLastMouseButtons(0),
	fLastMouseMovedTime(0),
	fLastMousePos(-1, -1),
	fAutoHidePointer(false),
	fOffscreenBitmap(nullptr),
	fOffscreenView(nullptr),
	fWebPage(new BWebPage(this, context)),
	fUserData(nullptr),
	fInspectorView(nullptr)
{
	SetViewColor(255, 255, 255);
	fWebPage->Init();
}


BWebView::~BWebView()
{
	// Deleted without Shutdown(): the page goes with it.
	if (fWebPage != nullptr) {
		fWebPage->fWebView = nullptr;
		fWebPage->Shutdown();
		fWebPage = nullptr;
	}
	delete fUserData;
}


void
BWebView::Shutdown()
{
	if (fWebPage != nullptr)
		fWebPage->Shutdown();
}


void
BWebView::AttachedToWindow()
{
	BView::AttachedToWindow();
	// As before: the window hears of its view's page (WebPositive never sets
	// a listener itself).
	if (fWebPage != nullptr)
		fWebPage->SetListener(BMessenger(Window()));
	// A page the engine opened is shown now that the view has a window.
	if (fOffscreenView == nullptr && fWebPage != nullptr)
		WebCore::ChromeClientHaiku::CreateEngineView(fWebPage);
	if (fOffscreenView != nullptr) {
		fOffscreenView->MoveTo(0, 0);
		fOffscreenView->ResizeTo(Bounds().Width(), Bounds().Height());
	}
}


void
BWebView::DetachedFromWindow()
{
	BView::DetachedFromWindow();
}


void
BWebView::Show()
{
	BView::Show();
	// The engine view learns that it can be seen again only from its own
	// hooks; showing its parent (a tab becoming the current one) calls none.
	if (fOffscreenView != nullptr && Window() != nullptr) {
		fOffscreenView->ResizeBy(0, -1);
		fOffscreenView->ResizeBy(0, 1);
	}
}


void
BWebView::Hide()
{
	BView::Hide();
}


void
BWebView::Draw(BRect)
{
	// The engine's view covers this one.
}


void
BWebView::FrameResized(float width, float height)
{
	if (getenv("SUMMIT_LEGACY_TRACE") != nullptr)
		fprintf(stderr, "Summit legacy view %p: resized to %.0fx%.0f, engine view %p\n", this, width + 1, height + 1,
			fOffscreenView);
	if (fOffscreenView != nullptr) {
		fOffscreenView->MoveTo(0, 0);
		fOffscreenView->ResizeTo(width, height);
	}
}


void
BWebView::GetPreferredSize(float* width, float* height)
{
	if (width != nullptr)
		*width = 100;
	if (height != nullptr)
		*height = 100;
}


void
BWebView::MessageReceived(BMessage* message)
{
	BWebKitView* view = static_cast<BWebKitView*>(fOffscreenView);
	switch (message->what) {
		case B_WEBKIT_CONTEXT_MENU:
		{
			// The engine leaves the menu to its host; the old engine showed
			// its own, so this is it.
			BPoint where;
			if (message->FindPoint("where", &where) != B_OK || Window() == nullptr)
				break;
			BPopUpMenu* menu = new BPopUpMenu("context", false, false);
			menu->SetAsyncAutoDestruct(true);
			auto add = [&](const char* label, uint32 what, const char* url = nullptr, uint32 command = 0) {
				BMessage* item = new BMessage(what);
				if (url != nullptr)
					item->AddString("url", url);
				if (command != 0)
					item->AddUInt32("command", command);
				BMenuItem* menuItem = new BMenuItem(label, item);
				menuItem->SetTarget(this);
				menu->AddItem(menuItem);
			};
			const char* link = nullptr;
			const char* image = nullptr;
			const char* selected = nullptr;
			bool editable = message->GetBool("editable", false);
			if (message->FindString("link_url", &link) == B_OK && link[0] != '\0') {
				add("Open link in new tab", kOpenLinkInNewTab, link);
				add("Copy link address", kCopyAddress, link);
				add("Download linked file", kDownloadAddress, link);
			}
			if (message->FindString("image_url", &image) == B_OK && image[0] != '\0') {
				if (menu->CountItems() > 0)
					menu->AddSeparatorItem();
				add("Open image in new tab", kOpenImageInNewTab, image);
				add("Copy image address", kCopyAddress, image);
				add("Save image", kDownloadAddress, image);
			}
			bool hasSelection = message->FindString("selected_text", &selected) == B_OK
				&& selected[0] != '\0';
			if (editable || hasSelection) {
				if (menu->CountItems() > 0)
					menu->AddSeparatorItem();
				if (editable)
					add("Cut", kEditCommand, nullptr, B_CUT);
				add("Copy", kEditCommand, nullptr, B_COPY);
				if (editable)
					add("Paste", kEditCommand, nullptr, B_PASTE);
			}
			if (menu->CountItems() == 0) {
				add("Back", kNavigate, nullptr, 'back');
				add("Forward", kNavigate, nullptr, 'frwd');
				add("Reload", kNavigate, nullptr, 'reld');
				menu->AddSeparatorItem();
				add("Select all", kEditCommand, nullptr, B_SELECT_ALL);
			}
			menu->Go(ConvertToScreen(where), true, false, true);
			break;
		}
		case kOpenLinkInNewTab:
		case kOpenImageInNewTab:
		{
			BMessage request(NEW_WINDOW_REQUESTED);
			request.AddString("url", message->GetString("url", ""));
			request.AddBool("primary", false);
			fWebPage->dispatchMessage(request);
			break;
		}
		case kCopyAddress:
			SummitLegacy::CopyToClipboard(message->GetString("url", ""));
			break;
		case kDownloadAddress:
			fWebPage->RequestDownload(message->GetString("url", ""));
			break;
		case kEditCommand:
			if (view != nullptr)
				view->ExecuteEditCommand(message->GetUInt32("command", B_COPY));
			break;
		case kNavigate:
		{
			uint32 command = message->GetUInt32("command", 0);
			if (command == 'back')
				GoBack();
			else if (command == 'frwd')
				GoForward();
			else if (command == 'reld')
				Reload();
			break;
		}
		case B_CUT:
		case B_COPY:
		case B_PASTE:
		case B_SELECT_ALL:
		case B_UNDO:
		case B_REDO:
			if (view != nullptr)
				view->ExecuteEditCommand(message->what);
			break;
		case B_MOUSE_WHEEL_CHANGED:
			// BWebWindow passes on the wheel of a window without a focus.
			if (view != nullptr)
				view->MessageReceived(message);
			break;
		default:
			BView::MessageReceived(message);
			break;
	}
}


void
BWebView::MakeFocus(bool focused)
{
	// The keyboard goes to the engine's view.
	if (fOffscreenView != nullptr)
		fOffscreenView->MakeFocus(focused);
	else
		BView::MakeFocus(focused);
}


void
BWebView::WindowActivated(bool activated)
{
	BView::WindowActivated(activated);
}


void
BWebView::MouseMoved(BPoint where, uint32 transit, const BMessage* dragMessage)
{
	BView::MouseMoved(where, transit, dragMessage);
}


void
BWebView::MouseDown(BPoint where)
{
	BView::MouseDown(where);
}


void
BWebView::MouseUp(BPoint where)
{
	BView::MouseUp(where);
}


void
BWebView::KeyDown(const char* bytes, int32 numBytes)
{
	BView::KeyDown(bytes, numBytes);
}


void
BWebView::KeyUp(const char* bytes, int32 numBytes)
{
	BView::KeyUp(bytes, numBytes);
}


void
BWebView::Pulse()
{
}


BString
BWebView::MainFrameTitle() const
{
	return fWebPage != nullptr ? fWebPage->MainFrameTitle() : BString();
}


BString
BWebView::MainFrameRequestedURL() const
{
	return fWebPage != nullptr ? fWebPage->MainFrameRequestedURL() : BString();
}


BString
BWebView::MainFrameURL() const
{
	return fWebPage != nullptr ? fWebPage->MainFrameURL() : BString();
}


void
BWebView::LoadURL(const char* urlString, bool aquireFocus)
{
	if (fWebPage == nullptr)
		return;
	fWebPage->LoadURL(urlString);
	if (aquireFocus && fOffscreenView != nullptr && LockLooper()) {
		MakeFocus(true);
		UnlockLooper();
	}
}


void
BWebView::Reload()
{
	if (fWebPage != nullptr)
		fWebPage->Reload();
}


void
BWebView::GoBack()
{
	if (fWebPage != nullptr)
		fWebPage->GoBack();
}


void
BWebView::GoForward()
{
	if (fWebPage != nullptr)
		fWebPage->GoForward();
}


void
BWebView::StopLoading()
{
	if (fWebPage != nullptr)
		fWebPage->StopLoading();
}


void
BWebView::IncreaseZoomFactor(bool textOnly)
{
	if (fOffscreenView != nullptr)
		static_cast<BWebKitView*>(fOffscreenView)->IncreaseZoomFactor(textOnly);
}


void
BWebView::DecreaseZoomFactor(bool textOnly)
{
	if (fOffscreenView != nullptr)
		static_cast<BWebKitView*>(fOffscreenView)->DecreaseZoomFactor(textOnly);
}


void
BWebView::ResetZoomFactor()
{
	if (fOffscreenView != nullptr)
		static_cast<BWebKitView*>(fOffscreenView)->ResetZoomFactor();
}


void
BWebView::FindString(const char* string, bool forward, bool caseSensitive,
	bool wrapSelection, bool /*startInSelection*/)
{
	if (fOffscreenView != nullptr)
		static_cast<BWebKitView*>(fOffscreenView)->FindString(string, forward, caseSensitive, wrapSelection);
}


void
BWebView::SetDarkMode(bool /*dark*/)
{
	// The engine follows the system's colours itself.
}


void
BWebView::SetAutoHidePointer(bool doIt)
{
	fAutoHidePointer = doIt;
}


void
BWebView::SetUserData(UserData* userData)
{
	if (userData == fUserData)
		return;
	delete fUserData;
	fUserData = userData;
}


BWebView::UserData*
BWebView::GetUserData() const
{
	return fUserData;
}


void
BWebView::SetInspectorView(BWebView* inspector)
{
	fInspectorView = inspector;
}


BWebView*
BWebView::GetInspectorView()
{
	return fInspectorView;
}


void
BWebView::SetRootLayer(WebCore::GraphicsLayer*)
{
	// The engine composites its own layers.
}
