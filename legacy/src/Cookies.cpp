/*
 * Copyright 2026 Summit contributors. Distributed under the terms of the MIT License.
 *
 * The old engine kept its cookies in the BNetworkCookieJar of the
 * application's BUrlContext, and WebPositive's cookie window still reads that
 * jar (BrowserApp passes fContext->GetCookieJar() to CookieWindow). Summit's
 * engine keeps cookies in its own store, so this keeps every jar given to a
 * BWebView in step with it, on a looper of its own:
 *
 * - every two seconds, and soon after a page finished loading, the engine's
 *   cookies are fetched (BWebKitView::GetCookies) and the jar is made to hold
 *   exactly those (cookies the engine no longer has are removed from it);
 * - a cookie that was put in the jar and has gone from it since was removed
 *   by the application (WebPositive's Delete button adds an expired copy),
 *   and is removed from the engine as well, by setting an expired cookie of
 *   the same name, domain and path (the engine deletes exactly that one).
 *
 * The jar is read and written through its own locks; the cookie window reads
 * it on its thread. SUMMIT_LEGACY_COOKIES=0 turns the mirroring off.
 */
#include "Private.h"

#include <Application.h>
#include <DateTime.h>
#include <MessageRunner.h>
#include <NetworkCookie.h>
#include <NetworkCookieJar.h>
#include <UrlContext.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <tuple>
#include <vector>

// WebPage.h declares a global BNetworkCookieJar of its own.
using Cookie = BPrivate::Network::BNetworkCookie;
using CookieJar = BPrivate::Network::BNetworkCookieJar;
using BPrivate::Network::BUrlContext;

namespace {

enum : uint32 {
	kTick = 'sckt',
	kSyncSoon = 'scks'
};
constexpr bigtime_t kInterval = 2000000;

// A cookie as the jar files it: domain (without a leading dot, lower case),
// path, name. The engine's "example.com" (host only) and ".example.com"
// (domain) cookies of one name and path are one jar cookie. Not BStrings:
// C++20 would order those as the pointers they convert to.
using Key = std::tuple<std::string, std::string, std::string>;

struct EngineCookie {
	BString name, domain, path;
};

struct Entry {
	BString value;
	// Seconds since the epoch; -1 for a session cookie.
	int64 expires = -1;
	bool secure = false, httpOnly = false;
	// The engine's cookies behind it, as the engine names them.
	std::vector<EngineCookie> engine;

	bool SameAs(const Entry& other) const
	{
		return value == other.value && expires == other.expires && secure == other.secure
			&& httpOnly == other.httpOnly;
	}
};

using Cookies = std::map<Key, Entry>;

bool
JarDomain(const BString& engineDomain, BString& domain)
{
	domain = engineDomain;
	if (domain.ByteAt(0) == '.')
		domain.Remove(0, 1);
	domain.ToLower();
	// BNetworkCookie refuses single-label domains (localhost) and TLDs.
	return domain.FindLast('.') > 0;
}

std::set<Key>
JarKeys(CookieJar& jar)
{
	std::set<Key> keys;
	// The iterator holds read locks on the lists it walks: no changes to the
	// jar until it is gone.
	CookieJar::Iterator iterator = jar.GetIterator();
	while (const Cookie* cookie = iterator.Next())
		keys.emplace(cookie->Domain().String(), cookie->Path().String(), cookie->Name().String());
	return keys;
}

void
RemoveFromJar(CookieJar& jar, const Key& key)
{
	// As WebPositive's cookie window deletes: the jar drops a cookie of the
	// same name and path for an expired one.
	Cookie cookie;
	cookie.SetName(std::get<2>(key).c_str());
	if (cookie.SetDomain(std::get<0>(key).c_str()) != B_OK || cookie.SetPath(std::get<1>(key).c_str()) != B_OK)
		return;
	cookie.SetExpirationDate(static_cast<time_t>(1));
	jar.AddCookie(cookie);
}

void
AddToJar(CookieJar& jar, const Key& key, const Entry& entry)
{
	Cookie cookie;
	cookie.SetName(std::get<2>(key).c_str());
	cookie.SetValue(entry.value);
	if (cookie.SetDomain(std::get<0>(key).c_str()) != B_OK || cookie.SetPath(std::get<1>(key).c_str()) != B_OK)
		return;
	if (entry.expires >= 0)
		cookie.SetExpirationDate(static_cast<time_t>(entry.expires));
	cookie.SetSecure(entry.secure);
	cookie.SetHttpOnly(entry.httpOnly);
	jar.AddCookie(cookie);
}


// A looper of its own, not the application's: Haiku's cookie jar takes its
// map lock before a list's lock when adding, and an iterator (the cookie
// window's) a list's before the map's, so the two can deadlock; that must
// not stop the application. Never deleted: pages and views reach it from
// any thread until the end.
class CookieMirror final : public BLooper {
public:
	CookieMirror() : BLooper("summit legacy cookies", B_LOW_PRIORITY) { }

	void MessageReceived(BMessage* message) override;
	// The application is quitting (BWebPage::ShutdownOnce()): it archives
	// its jar next, and may delete its context; the jars are not touched
	// any more.
	void Stop();

	std::mutex lock;
	// Jars by their context, with the number of pages using each.
	std::map<BUrlContext*, int32> contexts;
	// Live engine views (any will do: they share the engine context).
	std::vector<BWebKitView*> views;

private:
	// Calls use(view) with a live engine view, under the lock: a window
	// that closes deletes its views on its own thread (~BWebView), which
	// takes them out of the list first. False if there is none.
	template<typename Use> bool WithView(Use use);
	// Finds the cookies the application removed from its jars since they
	// were put there and removes them from the engine. True if any.
	bool PushDeletions();
	void Fetch();
	void Apply(const BMessage& reply);

	// What was put in each jar last.
	std::map<BUrlContext*, Cookies> fMirrored;
	uint64 fNextRequest = 1;
	uint64 fFetching = 0;
	int32 fDeleting = 0;
	// When the last request went out: an answer lost with a network
	// process that ended must not stop the mirroring.
	bigtime_t fAsked = 0;
	bool fTrace = getenv("SUMMIT_LEGACY_TRACE") != nullptr;
	// Held while a message is handled; Stop() waits for it, a while.
	std::timed_mutex fWork;
	std::atomic<bool> fStopped = false;
};

std::mutex sStartLock;
std::atomic<CookieMirror*> sMirror = nullptr;
BMessageRunner* sRunner = nullptr;


template<typename Use>
bool
CookieMirror::WithView(Use use)
{
	std::lock_guard guard(lock);
	if (views.empty())
		return false;
	use(views.front());
	return true;
}


bool
CookieMirror::PushDeletions()
{
	std::vector<BUrlContext*> jars;
	{
		std::lock_guard guard(lock);
		for (auto& entry : contexts)
			jars.push_back(entry.first);
	}
	// A context no page uses any more may be deleted, and another made at
	// its address.
	std::erase_if(fMirrored, [&](const auto& entry) {
		return std::find(jars.begin(), jars.end(), entry.first) == jars.end();
	});
	bool deleted = false;
	for (BUrlContext* context : jars) {
		auto mirrored = fMirrored.find(context);
		if (mirrored == fMirrored.end())
			continue;
		const std::set<Key> present = JarKeys(context->GetCookieJar());
		for (auto entry = mirrored->second.begin(); entry != mirrored->second.end();) {
			if (present.contains(entry->first)) {
				++entry;
				continue;
			}
			for (const EngineCookie& cookie : entry->second.engine) {
				if (fTrace)
					fprintf(stderr, "Summit legacy cookies: the application deleted %s (%s%s)\n",
						cookie.name.String(), cookie.domain.String(), cookie.path.String());
				// Expired, the engine's store deletes the cookie of exactly
				// this name, domain and path.
				BMessage fields;
				fields.AddString("name", cookie.name);
				fields.AddString("value", "");
				fields.AddString("domain", cookie.domain);
				fields.AddString("path", cookie.path);
				fields.AddDouble("expires", 1000.0);
				fields.AddBool("session", false);
				if (!WithView([&](BWebKitView* view) { view->SetCookie(fields, BMessenger(this), 0); }))
					continue;
				fDeleting++;
				fAsked = system_time();
				deleted = true;
			}
			entry = mirrored->second.erase(entry);
		}
	}
	return deleted;
}


void
CookieMirror::Fetch()
{
	if ((fFetching != 0 || fDeleting > 0) && system_time() - fAsked < 10 * kInterval)
		return;
	fDeleting = 0;
	const uint64 request = fNextRequest++;
	if (WithView([&](BWebKitView* view) { view->GetCookies(nullptr, BMessenger(this), request); })) {
		fFetching = request;
		fAsked = system_time();
	}
}


void
CookieMirror::Apply(const BMessage& reply)
{
	Cookies cookies;
	BMessage cookie;
	for (int32 i = 0; reply.FindMessage("cookie", i, &cookie) == B_OK; i++) {
		BString domain;
		const BString name = cookie.GetString("name", "");
		const BString path = cookie.GetString("path", "/");
		// The jar has no place for unnamed cookies or single-label hosts.
		if (name.Length() == 0 || path.ByteAt(0) != '/'
			|| !JarDomain(cookie.GetString("domain", ""), domain))
			continue;
		Entry& entry = cookies[Key(domain.String(), path.String(), name.String())];
		if (entry.engine.empty()) {
			entry.value = cookie.GetString("value", "");
			double expires;
			if (!cookie.GetBool("session", true) && cookie.FindDouble("expires", &expires) == B_OK
				&& std::isfinite(expires))
				entry.expires = static_cast<int64>(std::floor(expires / 1000));
			entry.secure = cookie.GetBool("secure", false);
			entry.httpOnly = cookie.GetBool("httpOnly", false);
		}
		entry.engine.push_back({ name, cookie.GetString("domain", ""), path });
	}
	std::vector<BUrlContext*> jars;
	{
		std::lock_guard guard(lock);
		for (auto& entry : contexts)
			jars.push_back(entry.first);
	}
	for (BUrlContext* context : jars) {
		CookieJar& jar = context->GetCookieJar();
		Cookies& mirrored = fMirrored[context];
		const std::set<Key> present = JarKeys(jar);
		int32 removed = 0, added = 0;
		// Cookies the engine does not have, also those the application kept
		// from before (WebPositive archives its jar in its settings).
		for (const Key& key : present) {
			if (!cookies.contains(key)) {
				RemoveFromJar(jar, key);
				removed++;
			}
		}
		for (auto& [key, entry] : cookies) {
			auto known = mirrored.find(key);
			if (present.contains(key) && known != mirrored.end() && known->second.SameAs(entry))
				continue;
			AddToJar(jar, key, entry);
			added++;
		}
		mirrored = cookies;
		if (fTrace && (removed || added))
			fprintf(stderr, "Summit legacy cookies: jar %p now has the engine's %zu cookies (%" B_PRId32
				" added or changed, %" B_PRId32 " removed)\n", context, cookies.size(), added, removed);
	}
}


void
CookieMirror::Stop()
{
	std::unique_lock work(fWork, std::defer_lock);
	// A mirror stuck in the jar's locks is left as it is.
	if (!work.try_lock_for(std::chrono::seconds(1)))
		fprintf(stderr, "Summit legacy cookies: the cookie jar did not answer\n");
	fStopped = true;
}


void
CookieMirror::MessageReceived(BMessage* message)
{
	std::lock_guard work(fWork);
	if (fStopped)
		return;
	switch (message->what) {
		case kTick:
		case kSyncSoon:
			if (!PushDeletions())
				Fetch();
			break;
		case B_WEBKIT_COOKIES_CHANGED:
			// An application's deletion reached the engine.
			if (fDeleting > 0 && --fDeleting == 0)
				Fetch();
			break;
		case B_WEBKIT_COOKIES:
		{
			const uint64 identifier = message->GetUInt64("identifier", 0);
			if (identifier != fFetching)
				break;
			fFetching = 0;
			if (message->HasString("error"))
				break;
			// What the application deleted since the request is not put back:
			// it goes to the engine first, and the cookies are fetched again.
			if (PushDeletions())
				break;
			Apply(*message);
			break;
		}
		default:
			BHandler::MessageReceived(message);
			break;
	}
}

bool
Enabled()
{
	const char* setting = getenv("SUMMIT_LEGACY_COOKIES");
	return setting == nullptr || strcmp(setting, "0") != 0;
}

}


namespace SummitLegacy {

void
StartCookieMirror()
{
	std::lock_guard start(sStartLock);
	if (sMirror != nullptr || be_app == nullptr || !Enabled())
		return;
	auto* mirror = new CookieMirror();
	mirror->Run();
	BMessage tick(kTick);
	sRunner = new BMessageRunner(BMessenger(mirror), &tick, kInterval);
	sMirror = mirror;
}


void
StopCookieMirror()
{
	std::lock_guard start(sStartLock);
	delete sRunner;
	sRunner = nullptr;
	if (CookieMirror* mirror = sMirror)
		mirror->Stop();
}


void
AddCookieJar(BUrlContext* context)
{
	if (context == nullptr)
		return;
	// Also for a program that did not call BWebPage::InitializeOnce().
	StartCookieMirror();
	if (sMirror == nullptr)
		return;
	std::lock_guard guard(sMirror.load()->lock);
	sMirror.load()->contexts[context]++;
}


void
RemoveCookieJar(BUrlContext* context)
{
	if (sMirror == nullptr || context == nullptr)
		return;
	CookieMirror* mirror = sMirror;
	std::lock_guard guard(mirror->lock);
	auto found = mirror->contexts.find(context);
	if (found != mirror->contexts.end() && --found->second <= 0)
		mirror->contexts.erase(found);
}


void
AddCookieView(BWebKitView* view)
{
	if (sMirror == nullptr || view == nullptr)
		return;
	CookieMirror* mirror = sMirror;
	std::lock_guard guard(mirror->lock);
	mirror->views.push_back(view);
}


void
RemoveCookieView(BWebKitView* view)
{
	if (sMirror == nullptr || view == nullptr)
		return;
	CookieMirror* mirror = sMirror;
	std::lock_guard guard(mirror->lock);
	std::erase(mirror->views, view);
}


void
SyncCookiesSoon()
{
	if (CookieMirror* mirror = sMirror)
		BMessenger(mirror).SendMessage(kSyncSoon);
}

}
