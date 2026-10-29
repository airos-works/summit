/*
 * Copyright 2026 Summit contributors. Distributed under the terms of the MIT License.
 *
 * The web inspector of a BWebView: Summit's own developer tools window
 * (src/ui/DevToolsWindow.cpp, built into this library; Elements, Network,
 * Console, Storage, Sources, Performance), connected to the page through the
 * engine's inspector protocol (BWebKitInspectorSession). HaikuWebKit never
 * had a working inspector (its InspectorClient was a stub), and WebPositive
 * has no menu item for one, so the view's context menu offers
 * "Inspect element". docs/legacy-webview.md says more.
 */
#include "Private.h"

#include "ui/DevToolsWindow.h"
#include "ui/Messages.h"

#include <WebKit/WebKitInspector.h>

#include <GroupLayout.h>
#include <Window.h>

#include <string>

namespace SummitLegacy {

void
ShowInspector(BPrivate::WebPagePrivate& data, BHandler* page, BWindow* near, BPoint where)
{
	BMessage show(summit::kShowDeveloperTools);
	show.AddString("panel", "elements");
	if (where.x >= 0 && where.y >= 0) {
		show.AddPoint("inspect", where);
		// The engine does not say its zoom; the old API's zoom steps are
		// not followed here (see the docs).
		show.AddDouble("zoom", 1.0);
	}
	BMessenger tools;
	BWebKitView* view;
	uint64 pageIdentifier;
	BString title;
	{
		std::lock_guard lock(data.lock);
		if (data.shutDown || data.view == nullptr)
			return;
		tools = data.devTools;
		view = data.view;
		pageIdentifier = data.pageIdentifier;
		title = data.title;
	}
	if (tools.IsValid() && tools.SendMessage(&show) == B_OK)
		return;
	BRect frame(100, 100, 900, 700);
	if (near != nullptr)
		frame = near->Frame();
	// The page's handler hears when the window closes (kDeveloperToolsClosed).
	auto* window = new summit::DevToolsWindow(BMessenger(page), 0, title.String(), frame);
	// The window cannot dock into a BWebView program's window: the row of
	// dock buttons (the first item of its layout) goes.
	if (auto* layout = dynamic_cast<BGroupLayout*>(window->GetLayout())) {
		if (BLayoutItem* row = layout->ItemAt(0))
			row->SetVisible(false);
	}
	// The engine names the page's target to its first connection only; the
	// page's identifier names it to later ones.
	window->SetInspector(BWebKitInspectorSession::Create(*view, BMessenger(window)),
		pageIdentifier != 0 ? "page-" + std::to_string(pageIdentifier) : std::string());
	{
		std::lock_guard lock(data.lock);
		data.devTools = BMessenger(window);
	}
	window->PostMessage(&show);
	window->Show();
}


void
CloseInspector(BPrivate::WebPagePrivate& data)
{
	BMessenger tools;
	{
		std::lock_guard lock(data.lock);
		tools = data.devTools;
		data.devTools = BMessenger();
	}
	if (tools.IsValid())
		tools.SendMessage(B_QUIT_REQUESTED);
}


void
SetInspectorTitle(BPrivate::WebPagePrivate& data, const BString& title)
{
	BMessenger tools;
	{
		std::lock_guard lock(data.lock);
		tools = data.devTools;
	}
	if (!tools.IsValid())
		return;
	BMessage page(summit::kDeveloperToolsPage);
	page.AddString("title", title);
	tools.SendMessage(&page);
}


static_assert(kInspectorClosed == summit::kDeveloperToolsClosed && kInspectorDock == summit::kDockDeveloperTools);

}
