/*
 * Copyright 2026 Summit contributors. Distributed under the terms of the MIT License.
 *
 * BWebFrame on Summit's engine: the page's main frame. The engine runs pages
 * in other processes: what needs their document (its text, its markup, its
 * source) is asked of the page with a script and waited for, so it is empty
 * when called from the application thread (which the answer needs) or when
 * the page does not answer in time. The editing commands go to the page.
 */
#include "Private.h"

#include <cstdio>
#include <cstring>

BWebFrame::BWebFrame(BWebPage* webPage, BWebFrame* /*parentFrame*/, WebFramePrivate* data)
	:
	fZoomFactor(1),
	fIsEditable(false),
	fData(data)
{
	if (fData != nullptr)
		fData->page = webPage;
}


BWebFrame::~BWebFrame()
{
	delete fData;
}


static BWebKitView*
EngineView(const WebFramePrivate* data)
{
	return data != nullptr ? data->view : nullptr;
}


// The result of an asynchronous script, or an empty string.
static BString
AskPage(const WebFramePrivate* data, const char* source)
{
	BString result;
	if (SummitLegacy::EvaluateAndWait(EngineView(data), source, result, 10000000) != B_OK)
		result.Truncate(0);
	return result;
}


static const char* const kMarkupScript =
	"return (document.doctype ? '<!DOCTYPE ' + document.doctype.name + '>\\n' : '')"
	" + document.documentElement.outerHTML;";


void
BWebFrame::SetListener(const BMessenger& listener)
{
	if (fData != nullptr && fData->page != nullptr)
		fData->page->SetListener(listener);
}


void
BWebFrame::LoadURL(BString url)
{
	if (fData != nullptr && fData->page != nullptr)
		fData->page->LoadURL(url.String());
}


void
BWebFrame::StopLoading()
{
	if (fData != nullptr && fData->page != nullptr)
		fData->page->StopLoading();
}


void
BWebFrame::Reload()
{
	if (fData != nullptr && fData->page != nullptr)
		fData->page->Reload();
}


BString
BWebFrame::RequestedURL() const
{
	return fData != nullptr && fData->page != nullptr ? fData->page->MainFrameRequestedURL() : BString();
}


BString
BWebFrame::URL() const
{
	return fData != nullptr && fData->page != nullptr ? fData->page->MainFrameURL() : BString();
}


BString
BWebFrame::MIMEType() const
{
	return "text/html";
}


bool BWebFrame::CanCopy() const { return true; }
bool BWebFrame::CanCut() const { return true; }
bool BWebFrame::CanPaste() const { return true; }
bool BWebFrame::CanUndo() const { return true; }
bool BWebFrame::CanRedo() const { return true; }


static void
Edit(const WebFramePrivate* data, uint32 command)
{
	if (BWebKitView* view = EngineView(data))
		view->ExecuteEditCommand(command);
}


void BWebFrame::Copy() { Edit(fData, B_COPY); }
void BWebFrame::Cut() { Edit(fData, B_CUT); }
void BWebFrame::Paste() { Edit(fData, B_PASTE); }
void BWebFrame::Undo() { Edit(fData, B_UNDO); }
void BWebFrame::Redo() { Edit(fData, B_REDO); }


bool
BWebFrame::AllowsScrolling() const
{
	return true;
}


void
BWebFrame::SetAllowsScrolling(bool)
{
}


BPoint
BWebFrame::ScrollPosition()
{
	return BPoint(0, 0);
}


BString
BWebFrame::FrameSource() const
{
	// As the server sent it (from the cache, as BWebPage::SendPageSource
	// does), else the markup as it stands.
	BString script("try { const response = await fetch(location.href, { cache: 'force-cache' });"
		" if (response.ok) return await response.text(); } catch (error) { }");
	script << kMarkupScript;
	return AskPage(fData, script.String());
}


void
BWebFrame::SetFrameSource(const BString& source)
{
	// Shown as a document of its own.
	if (fData == nullptr || fData->page == nullptr)
		return;
	BString url("data:text/html;charset=utf-8,");
	for (int32 i = 0; i < source.Length(); i++) {
		const unsigned char c = source[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || strchr("-_.~", c))
			url << static_cast<char>(c);
		else {
			char escaped[4];
			snprintf(escaped, sizeof(escaped), "%%%02X", c);
			url << escaped;
		}
	}
	fData->page->LoadURL(url.String());
}


void
BWebFrame::SetTransparent(bool)
{
}


bool
BWebFrame::IsTransparent() const
{
	return false;
}


BString
BWebFrame::InnerText() const
{
	return AskPage(fData, "return document.body ? document.body.innerText : '';");
}


BString
BWebFrame::AsMarkup() const
{
	return AskPage(fData, kMarkupScript);
}


BString
BWebFrame::ExternalRepresentation() const
{
	return BString();
}


bool
BWebFrame::FindString(const BString& string, uint32 options)
{
	BWebKitView* view = EngineView(fData);
	if (view == nullptr)
		return false;
	view->FindString(string.String(), !(options & B_FIND_BACKWARDS), (options & B_FIND_CASE_SENSITIVE) != 0,
		(options & B_FIND_WRAP_AROUND) != 0);
	return true;
}


bool
BWebFrame::CanIncreaseZoomFactor() const
{
	return true;
}


bool
BWebFrame::CanDecreaseZoomFactor() const
{
	return true;
}


void
BWebFrame::IncreaseZoomFactor(bool textOnly)
{
	if (BWebKitView* view = EngineView(fData))
		view->IncreaseZoomFactor(textOnly);
}


void
BWebFrame::DecreaseZoomFactor(bool textOnly)
{
	if (BWebKitView* view = EngineView(fData))
		view->DecreaseZoomFactor(textOnly);
}


void
BWebFrame::ResetZoomFactor()
{
	if (BWebKitView* view = EngineView(fData))
		view->ResetZoomFactor();
}


void
BWebFrame::SetEditable(bool editable)
{
	fIsEditable = editable;
	// The whole document, as HaikuWebKit's editable frames were.
	if (BWebKitView* view = EngineView(fData))
		view->EvaluateJavaScript(editable ? "document.designMode = 'on';" : "document.designMode = 'off';",
			BMessenger(), 0);
}


bool
BWebFrame::IsEditable() const
{
	return fIsEditable;
}


void
BWebFrame::SetTitle(const BString& title)
{
	fTitle = title;
}


const BString&
BWebFrame::Title() const
{
	BString& title = const_cast<BString&>(fTitle);
	title = fData != nullptr && fData->page != nullptr ? fData->page->MainFrameTitle() : BString();
	return title;
}


const char*
BWebFrame::Name() const
{
	fName = "main";
	return fName.String();
}


JSGlobalContextRef
BWebFrame::GlobalContext() const
{
	// The page's script runs in another process.
	return nullptr;
}
