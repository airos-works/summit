/*
 * Copyright 2026 Summit contributors. Distributed under the terms of the MIT License.
 *
 * BWebSettings on Summit's engine. The persistent storage path chooses where
 * the engine keeps the application's website data; site icons come from the
 * engine. Apply publishes application defaults or a page's local overrides.
 */
#include "Private.h"

#include <Font.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace {
BWebSettings* sDefault = nullptr;
std::mutex sDefaultLock;
std::mutex sPagesLock;
std::vector<BMessenger> sPages;

BString
FontName(const BFont& font)
{
	font_family family {};
	font_style style {};
	font.GetFamilyAndStyle(&family, &style);
	return BString(family);
}

void
PutString(BMessage& settings, const char* name, const BString& value)
{
	settings.RemoveName(name);
	settings.AddString(name, value);
}

void
PutFont(BMessage& settings, const char* name, const BFont& font)
{
	BString family = FontName(font);
	if (!family.IsEmpty())
		PutString(settings, name, family);
}

void
PutSize(BMessage& settings, const char* name, float value)
{
	if (!std::isfinite(value) || value < 1 || value > 512)
		return;
	settings.RemoveName(name);
	settings.AddInt32(name, static_cast<int32>(value));
}

BMessage
Defaults()
{
	BMessage settings;
	settings.AddString("standardFont", "Noto Serif Display");
	settings.AddString("serifFont", "Noto Serif Display");
	settings.AddString("sansSerifFont", FontName(*be_plain_font));
	settings.AddString("fixedFont", FontName(*be_fixed_font));
	settings.AddInt32("standardFontSize", 14);
	settings.AddInt32("fixedFontSize", 14);
	settings.AddBool("javascriptEnabled", true);
	settings.AddString("userAgent", "");
	return settings;
}

void
Overlay(BMessage& settings, const BMessage& local)
{
	for (const char* name : {"standardFont", "serifFont", "sansSerifFont", "fixedFont", "userAgent"}) {
		const char* value;
		if (local.FindString(name, &value) == B_OK)
			PutString(settings, name, value);
	}
	for (const char* name : {"standardFontSize", "fixedFontSize"}) {
		int32 value;
		if (local.FindInt32(name, &value) == B_OK)
			PutSize(settings, name, value);
	}
	bool value;
	if (local.FindBool("javascriptEnabled", &value) == B_OK) {
		settings.RemoveName("javascriptEnabled");
		settings.AddBool("javascriptEnabled", value);
	}
}
}

BPrivate::WebSettingsPrivate::~WebSettingsPrivate()
{
	std::lock_guard lock(sPagesLock);
	std::erase(sPages, page);
}

/*static*/ void
BPrivate::WebSettingsPrivate::Bind(BWebSettings* settings, BMessenger page)
{
	std::lock_guard lock(settings->fData->lock);
	settings->fData->page = page;
	std::lock_guard pages(sPagesLock);
	sPages.push_back(page);
}

/*static*/ BMessage
BPrivate::WebSettingsPrivate::Snapshot(BWebSettings* settings)
{
	BWebSettings* defaults = BWebSettings::Default();
	BMessage applied;
	{
		std::lock_guard lock(defaults->fData->lock);
		applied = defaults->fData->applied;
	}
	if (settings != defaults) {
		std::lock_guard lock(settings->fData->lock);
		Overlay(applied, settings->fData->applied);
	}
	return applied;
}

/*static*/ void
BPrivate::WebSettingsPrivate::ApplyTo(BWebSettings* settings, BWebKitView* view)
{
	view->SetPageSettings(Snapshot(settings));
}


BWebSettings::BWebSettings()
	:
	BHandler("BWebSettings"),
	fData(new BPrivate::WebSettingsPrivate())
{
}


BWebSettings::BWebSettings(WebCore::Settings*)
	:
	BHandler("BWebSettings"),
	fData(new BPrivate::WebSettingsPrivate())
{
}


BWebSettings::~BWebSettings()
{
	delete fData;
}


/*static*/ BWebSettings*
BWebSettings::Default()
{
	std::lock_guard lock(sDefaultLock);
	if (sDefault == nullptr) {
		sDefault = new BWebSettings();
		sDefault->fData->pending = Defaults();
		sDefault->fData->applied = sDefault->fData->pending;
	}
	return sDefault;
}


/*static*/ void
BWebSettings::SetPersistentStoragePath(const BString& path)
{
	SummitLegacy::SetStoragePath(path);
}


/*static*/ void
BWebSettings::SetIconDatabasePath(const BString&)
{
	// Icons are kept in memory, as the engine reports them.
}


/*static*/ void
BWebSettings::ClearIconDatabase()
{
	SummitLegacy::ClearIcons();
}


/*static*/ void
BWebSettings::SendIconForURL(const BString& url, const BMessage& reply, const BMessenger& target)
{
	BMessage message(reply);
	BMessage icon;
	if (SummitLegacy::ArchiveIcon(url, icon))
		message.AddMessage("icon", &icon);
	target.SendMessage(&message);
}


/*static*/ void BWebSettings::SetOfflineStoragePath(const BString&) { }
/*static*/ void BWebSettings::SetOfflineStorageDefaultQuota(int64) { }
/*static*/ void BWebSettings::SetOfflineWebApplicationCachePath(const BString&) { }
/*static*/ void BWebSettings::SetOfflineWebApplicationCacheQuota(int64) { }


void
BWebSettings::SetLocalStoragePath(const BString& path)
{
	std::lock_guard lock(fData->lock);
	fData->localStoragePath = path;
}


void
BWebSettings::SetSerifFont(const BFont& font)
{
	std::lock_guard lock(fData->lock);
	PutFont(fData->pending, "serifFont", font);
}


void
BWebSettings::SetSansSerifFont(const BFont& font)
{
	std::lock_guard lock(fData->lock);
	PutFont(fData->pending, "sansSerifFont", font);
}


void
BWebSettings::SetFixedFont(const BFont& font)
{
	std::lock_guard lock(fData->lock);
	PutFont(fData->pending, "fixedFont", font);
}


void
BWebSettings::SetStandardFont(const BFont& font)
{
	std::lock_guard lock(fData->lock);
	PutFont(fData->pending, "standardFont", font);
}


void
BWebSettings::SetDefaultStandardFontSize(float size)
{
	std::lock_guard lock(fData->lock);
	PutSize(fData->pending, "standardFontSize", size);
}


void
BWebSettings::SetDefaultFixedFontSize(float size)
{
	std::lock_guard lock(fData->lock);
	PutSize(fData->pending, "fixedFontSize", size);
}


void
BWebSettings::SetJavascriptEnabled(bool enable)
{
	std::lock_guard lock(fData->lock);
	fData->pending.RemoveName("javascriptEnabled");
	fData->pending.AddBool("javascriptEnabled", enable);
}

void
BWebSettings::SetUserAgent(const BString& userAgent)
{
	std::lock_guard lock(fData->lock);
	PutString(fData->pending, "userAgent", userAgent);
}


/*static*/ void
BWebSettings::SetProxyInfo(const BString&, uint32, BProxyType, const BString&, const BString&)
{
	// The engine's network process uses the system's network settings.
}


void
BWebSettings::Apply()
{
	BMessenger page;
	{
		std::lock_guard lock(fData->lock);
		fData->applied = fData->pending;
		page = fData->page;
	}
	if (page.IsValid()) {
		page.SendMessage(SummitLegacy::kApplySettings);
		return;
	}
	if (this != Default())
		return;
	std::vector<BMessenger> pages;
	{
		std::lock_guard lock(sPagesLock);
		pages = sPages;
	}
	for (const auto& target : pages)
		target.SendMessage(SummitLegacy::kApplySettings);
}


void
BWebSettings::MessageReceived(BMessage* message)
{
	BHandler::MessageReceived(message);
}
