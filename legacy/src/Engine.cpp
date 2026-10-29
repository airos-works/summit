/*
 * Copyright 2026 Summit contributors. Distributed under the terms of the MIT License.
 *
 * What the legacy classes share: the engine context, downloads (reported by
 * the engine to a handler of the application looper and handed to the
 * application as BWebDownload objects, as before), site icons, and helpers.
 */
#include "Private.h"

#include <Application.h>
#include <Bitmap.h>
#include <Clipboard.h>
#include <DataIO.h>
#include <Directory.h>
#include <Entry.h>
#include <FindDirectory.h>
#include <Looper.h>
#include <Mime.h>
#include <Node.h>
#include <Roster.h>
#include <TranslationUtils.h>

#include <cstdio>
#include <cstring>

thread_local uint64 SummitLegacy::gNewPageRequest = 0;

namespace {

constexpr uint32 kStart = 'sdst';
std::mutex sLock;
std::shared_ptr<BWebKitContext> sContext;
BString sStoragePath;
bool sInitialized = false;
std::map<BString, BBitmap*> sIcons;

// Receives the engine's download notifications on the application looper.
class DownloadDispatcher : public BHandler {
public:
	DownloadDispatcher() : BHandler("summit legacy downloads") { }
	~DownloadDispatcher() override;
	void MessageReceived(BMessage* message) override;
	void Finish(BPrivate::WebDownloadPrivate& data);
	std::map<uint64, BPrivate::WebDownloadPrivate*> downloads;
};
DownloadDispatcher* sDispatcher = nullptr;


BString
DefaultDownloadDirectory()
{
	BPath path;
	if (find_directory(B_DESKTOP_DIRECTORY, &path) != B_OK)
		find_directory(B_USER_DIRECTORY, &path);
	return path.Path();
}


// The directory the engine saves to, and the storage path's WebKit folder.
std::shared_ptr<BWebKitContext>
MakeContext(const BString& storagePath)
{
	BString profile;
	if (storagePath.Length() > 0)
		profile << storagePath << "/WebKit";
	else {
		// The application's own settings folder, as the old engine used.
		BPath path;
		app_info info;
		if (find_directory(B_USER_SETTINGS_DIRECTORY, &path) == B_OK && be_app != nullptr
			&& be_app->GetAppInfo(&info) == B_OK)
			path.Append(info.ref.name);
		path.Append("WebKit");
		profile = path.Path();
	}
	create_directory(profile.String(), 0700);
	auto context = std::make_shared<BWebKitContext>(profile.String());
	if (context->InitCheck() != B_OK) {
		fprintf(stderr, "Summit legacy WebKit: could not open %s: %s\n", profile.String(),
			strerror(context->InitCheck()));
		return nullptr;
	}
	if (sDispatcher != nullptr)
		context->SetDownloadListener(BMessenger(sDispatcher));
	context->SetDownloadDirectory(DefaultDownloadDirectory().String());
	return context;
}


// A name in directory that no file has yet, like the old engine's.
BPath
AvailablePath(const BPath& directory, const BString& name)
{
	BPath path(directory.Path(), name.String());
	BString base(name), extension;
	int32 dot = base.FindLast('.');
	if (dot > 0)
		base.MoveInto(extension, dot, base.Length() - dot);
	for (int32 i = 1; BEntry(path.Path()).Exists(); i++) {
		BString candidate(base);
		candidate << "-" << i << extension;
		path.SetTo(directory.Path(), candidate.String());
	}
	return path;
}

}


DownloadDispatcher::~DownloadDispatcher()
{
	for (auto& entry : downloads)
		delete entry.second;
}


void
DownloadDispatcher::MessageReceived(BMessage* message)
{
	if (message->what == kStart) {
		// BWebDownload::Start(), from the application's thread.
		auto found = downloads.find(message->GetUInt64("identifier", 0));
		if (found == downloads.end())
			return;
		BPrivate::WebDownloadPrivate& data = *found->second;
		const char* directory = nullptr;
		if (message->FindString("directory", &directory) == B_OK && directory[0] != '\0')
			data.directory.SetTo(directory);
		data.started = true;
		if (data.finished)
			Finish(data);
		else if (data.progressListener.IsValid() && data.path.InitCheck() == B_OK) {
			BMessage started(B_DOWNLOAD_STARTED);
			started.AddString("path", data.path.Path());
			data.progressListener.SendMessage(&started);
		}
		return;
	}
	if (message->what != B_WEBKIT_DOWNLOAD_STARTED && message->what != B_WEBKIT_DOWNLOAD_PROGRESS
		&& message->what != B_WEBKIT_DOWNLOAD_FINISHED) {
		BHandler::MessageReceived(message);
		return;
	}
	uint64 identifier = 0;
	if (message->FindUInt64("identifier", &identifier) != B_OK)
		return;
	BPrivate::WebDownloadPrivate* data;
	auto found = downloads.find(identifier);
	if (found == downloads.end()) {
		if (message->what == B_WEBKIT_DOWNLOAD_FINISHED)
			return;
		data = new BPrivate::WebDownloadPrivate();
		data->identifier = identifier;
		downloads[identifier] = data;
		data->download = BPrivate::WebDownloadPrivate::Create(data);
		if (Looper() != nullptr)
			Looper()->AddHandler(data->download);
		data->url = message->GetString("url", "");
		data->filename = message->GetString("filename", "");
		// As the old engine did: the application answers with Start().
		BMessage added(B_DOWNLOAD_ADDED);
		added.AddPointer("download", data->download);
		BPrivate::WebDownloadPrivate::DownloadListener().SendMessage(&added);
	} else
		data = found->second;

	const char* path = nullptr;
	if (message->FindString("path", &path) == B_OK && path[0] != '\0' && BString(path) != data->path.Path()) {
		data->path.SetTo(path);
		if (data->filename.Length() == 0 && data->path.Leaf() != nullptr)
			data->filename = data->path.Leaf();
		if (data->started && data->progressListener.IsValid()) {
			BMessage started(B_DOWNLOAD_STARTED);
			started.AddString("path", data->path.Path());
			data->progressListener.SendMessage(&started);
		}
	}
	data->currentSize = static_cast<off_t>(message->GetUInt64("current_size", data->currentSize));
	data->expectedSize = message->GetInt64("expected_size", data->expectedSize);
	if (data->started && data->progressListener.IsValid()) {
		BMessage progress(B_DOWNLOAD_PROGRESS);
		progress.AddFloat("progress", data->expectedSize > 0 ? data->currentSize * 100.0f / data->expectedSize : 0);
		progress.AddInt64("current size", data->currentSize);
		progress.AddInt64("expected size", data->expectedSize > 0 ? data->expectedSize : data->currentSize);
		data->progressListener.SendMessage(&progress);
	}
	if (message->what == B_WEBKIT_DOWNLOAD_FINISHED) {
		data->finished = true;
		data->succeeded = message->GetUInt32("result", B_WEBKIT_DOWNLOAD_FAILED) == B_WEBKIT_DOWNLOAD_SUCCEEDED;
		if (data->started)
			Finish(*data);
	}
}


void
DownloadDispatcher::Finish(BPrivate::WebDownloadPrivate& data)
{
	if (data.handedOver)
		return;
	data.handedOver = true;
	if (data.succeeded && data.path.InitCheck() == B_OK) {
		// Into the folder the application chose, if that is not where the
		// engine saved it.
		BPath parent;
		if (data.directory.InitCheck() == B_OK && data.path.GetParent(&parent) == B_OK
			&& strcmp(parent.Path(), data.directory.Path()) != 0) {
			BPath target = AvailablePath(data.directory, data.path.Leaf());
			BEntry entry(data.path.Path());
			BDirectory directory(data.directory.Path());
			if (entry.MoveTo(&directory, target.Leaf()) == B_OK)
				data.path = target;
		}
		BNode node(data.path.Path());
		node.WriteAttrString("META:url", &data.url);
		update_mime_info(data.path.Path(), false, true, false);
	}
	if (data.progressListener.IsValid()) {
		BMessage started(B_DOWNLOAD_STARTED);
		started.AddString("path", data.path.Path());
		data.progressListener.SendMessage(&started);
		BMessage removed(B_DOWNLOAD_REMOVED);
		removed.AddPointer("download", data.download);
		// As before: until the listener has let go of the object.
		BMessage reply;
		data.progressListener.SendMessage(&removed, &reply);
	}
	BWebDownload* download = data.download;
	downloads.erase(data.identifier);
	if (download->Looper() != nullptr)
		download->Looper()->RemoveHandler(download);
	// ~BWebDownload deletes data.
	BPrivate::WebDownloadPrivate::Destroy(download);
}


// #pragma mark - BWebDownload


BWebDownload::BWebDownload(BPrivate::WebDownloadPrivate* data)
	:
	BHandler("BWebDownload"),
	fData(data)
{
}


BWebDownload::~BWebDownload()
{
	delete fData;
}


void
BWebDownload::Start(const BPath& path)
{
	// path is the folder to save in (DownloadWindow gives its download
	// folder); the engine has already begun, in its own folder.
	// The download's state is the application looper's.
	BMessage start(kStart);
	start.AddUInt64("identifier", fData->identifier);
	if (path.InitCheck() == B_OK)
		start.AddString("directory", path.Path());
	if (sDispatcher != nullptr)
		BMessenger(sDispatcher).SendMessage(&start);
}


void
BWebDownload::Cancel()
{
	if (auto context = SummitLegacy::Context())
		context->CancelDownload(fData->identifier);
}


void
BWebDownload::HasMovedTo(const BPath& path)
{
	fData->path = path;
}


void
BWebDownload::SetProgressListener(const BMessenger& listener)
{
	fData->progressListener = listener;
}


const BString&
BWebDownload::URL() const
{
	return fData->url;
}


const BPath&
BWebDownload::Path() const
{
	return fData->path;
}


const BString&
BWebDownload::Filename() const
{
	return fData->filename;
}


off_t
BWebDownload::CurrentSize() const
{
	return fData->currentSize;
}


off_t
BWebDownload::ExpectedSize() const
{
	return fData->expectedSize;
}


void
BWebDownload::MessageReceived(BMessage* message)
{
	BHandler::MessageReceived(message);
}


void
BWebDownload::_HandleCancel()
{
	Cancel();
}


// #pragma mark - SummitLegacy


namespace SummitLegacy {

status_t
Initialize()
{
	std::lock_guard lock(sLock);
	if (sInitialized)
		return B_OK;
	status_t status = BWebKitInitialize();
	if (status != B_OK) {
		fprintf(stderr, "Summit legacy WebKit: the engine did not start: %s\n", strerror(status));
		return status;
	}
	if (be_app != nullptr && be_app->Lock()) {
		sDispatcher = new DownloadDispatcher();
		be_app->AddHandler(sDispatcher);
		be_app->Unlock();
	}
	StartCookieMirror();
	sInitialized = true;
	return B_OK;
}


void
Shutdown()
{
	StopCookieMirror();
	std::lock_guard lock(sLock);
	sContext.reset();
}


void
SetStoragePath(const BString& path)
{
	std::lock_guard lock(sLock);
	sStoragePath = path;
	// Views made after this use the new place; the application sets it
	// before its first window.
	if (sInitialized && be_app != nullptr && be_app->Thread() == find_thread(nullptr))
		sContext = MakeContext(sStoragePath);
}


std::shared_ptr<BWebKitContext>
Context()
{
	{
		std::lock_guard lock(sLock);
		if (sContext)
			return sContext;
	}
	if (be_app == nullptr || be_app->Thread() != find_thread(nullptr))
		return nullptr;
	// Also for a program that did not call BWebPage::InitializeOnce().
	Initialize();
	std::lock_guard lock(sLock);
	if (!sContext && sInitialized)
		sContext = MakeContext(sStoragePath);
	return sContext;
}


void
StoreIcon(const BString& pageURL, const void* data, size_t size)
{
	BMemoryIO stream(data, size);
	BBitmap* bitmap = BTranslationUtils::GetBitmap(&stream);
	if (bitmap == nullptr || !bitmap->IsValid()) {
		delete bitmap;
		return;
	}
	std::lock_guard lock(sLock);
	auto found = sIcons.find(pageURL);
	if (found != sIcons.end()) {
		delete found->second;
		found->second = bitmap;
	} else
		sIcons[pageURL] = bitmap;
}


bool
ArchiveIcon(const BString& pageURL, BMessage& archive)
{
	std::lock_guard lock(sLock);
	auto found = sIcons.find(pageURL);
	if (found == sIcons.end())
		return false;
	// The old engine gave 16 pixel icons.
	BBitmap* icon = found->second;
	if (icon->Bounds().Width() > 15) {
		BBitmap small(BRect(0, 0, 15, 15), B_RGBA32, true);
		if (small.Lock()) {
			BView view(small.Bounds(), "scale", 0, 0);
			small.AddChild(&view);
			view.SetDrawingMode(B_OP_ALPHA);
			view.SetBlendingMode(B_PIXEL_ALPHA, B_ALPHA_OVERLAY);
			view.SetHighColor(0, 0, 0, 0);
			view.FillRect(small.Bounds());
			view.DrawBitmap(icon, icon->Bounds(), small.Bounds(), B_FILTER_BITMAP_BILINEAR);
			view.Sync();
			small.RemoveChild(&view);
			small.Unlock();
		}
		BBitmap result(BRect(0, 0, 15, 15), B_RGBA32);
		result.ImportBits(&small);
		return result.Archive(&archive) == B_OK;
	}
	return icon->Archive(&archive) == B_OK;
}


void
ClearIcons()
{
	std::lock_guard lock(sLock);
	for (auto& entry : sIcons)
		delete entry.second;
	sIcons.clear();
}


BString
NormalizedURL(const BString& url)
{
	BString text(url);
	text.Trim();
	if (text.Length() == 0)
		return text;
	if (text.FindFirst("://") > 0 || text.IFindFirst("about:") == 0 || text.IFindFirst("data:") == 0
		|| text.IFindFirst("javascript:") == 0 || text.IFindFirst("mailto:") == 0)
		return text;
	if (text[0] == '/') {
		BString file("file://");
		return file << text;
	}
	BString http("http://");
	return http << text;
}


void
CopyToClipboard(const BString& text)
{
	if (!be_clipboard->Lock())
		return;
	be_clipboard->Clear();
	if (BMessage* data = be_clipboard->Data()) {
		data->AddData("text/plain", B_MIME_TYPE, text.String(), text.Length());
		be_clipboard->Commit();
	}
	be_clipboard->Unlock();
}


namespace {
class ScriptWaiter : public BLooper {
public:
	ScriptWaiter() : BLooper("summit legacy script"), fDone(create_sem(0, "script answered")) { }
	~ScriptWaiter() override { delete_sem(fDone); }
	void MessageReceived(BMessage* message) override
	{
		if (message->what != B_WEBKIT_JAVASCRIPT_RESULT) {
			BLooper::MessageReceived(message);
			return;
		}
		const char* text = nullptr;
		if (message->FindString("result", &text) == B_OK) {
			fResult = text;
			fStatus = B_OK;
		} else
			fStatus = B_ERROR;
		release_sem(fDone);
	}
	sem_id fDone;
	BString fResult;
	status_t fStatus = B_ERROR;
};
}


status_t
EvaluateAndWait(BWebKitView* view, const char* source, BString& result, bigtime_t timeout)
{
	if (view == nullptr)
		return B_NO_INIT;
	if (be_app != nullptr && be_app->Thread() == find_thread(nullptr))
		return B_WOULD_BLOCK;
	ScriptWaiter* waiter = new ScriptWaiter();
	waiter->Run();
	view->EvaluateJavaScript(source, BMessenger(waiter), 1, true);
	status_t status = acquire_sem_etc(waiter->fDone, 1, B_RELATIVE_TIMEOUT, timeout);
	if (status == B_OK && waiter->Lock()) {
		status = waiter->fStatus;
		result = waiter->fResult;
		waiter->Unlock();
	}
	// An answer that comes later finds no one.
	if (waiter->Lock())
		waiter->Quit();
	return status;
}

}
