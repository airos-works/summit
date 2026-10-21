#if SUMMIT_MODERN_WEBKIT
#include "BrowserWindow.h"
#include "PageTranslation.h"
#include "NoticeBar.h"
#include "SharedProfile.h"
#include "core/PageLanguage.h"
#include <MessageRunner.h>
#include <WebKit/WebKitEmbedding.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <utility>

namespace summit {
using json = nlohmann::json;

void BrowserWindow::TranslationScript(Tab& tab, const char* action, const std::string& expression, bool library)
{
    auto& state = tab.translation;
    if (tab.loading || tab.processExited || fClosingWindow || tab.closeQueued || tab.closeRequested || tab.closeApproved) return;
    state.request = ++fReaderRequest; state.generation = tab.loadGeneration; state.action = action;
    const auto source = (library ? TranslationDocumentScript() + "\n" : std::string()) + "JSON.stringify(" + expression + ")";
    tab.view->EvaluateJavaScriptIsolated(source.c_str(), BMessenger(this), state.request, state.generation, true);
}
void BrowserWindow::ProbeTranslation(Tab& tab, bool manual)
{
    auto& state = tab.translation;
    if (fClosingWindow || tab.closeQueued || tab.closeRequested || tab.closeApproved || tab.loading || tab.processExited
        || !tab.loadError.empty() || tab.readerActive || tab.loadOutcome != "succeeded" || !tab.loadGeneration
        || state.request || (!manual && state.probed == tab.loadGeneration)
        || !(tab.url.starts_with("https://") || tab.url.starts_with("http://")) || TranslationDocumentScript().empty()) return;
    state.probed = tab.loadGeneration;
    state.probeAfter = 0; ++state.probeAttempts;
    TranslationScript(tab, manual ? "manual-probe" : "probe", "__summitTranslation.sample()", true);
}
void BrowserWindow::CancelTranslation(Tab& tab, bool invalidated)
{
    auto& state = tab.translation;
    if (fTranslationTab == tab.id) {
        if (fTranslator) fTranslator->Cancel();
        fTranslationTab = 0;
    }
    state.request = 0; state.job = 0;
    if (!invalidated && !state.pendingIDs.empty() && state.generation == tab.loadGeneration) {
        const auto script = "globalThis.__summitTranslation?.fail(" + std::to_string(state.epoch) + "," + state.pendingIDs + ")";
        tab.view->EvaluateJavaScriptIsolated(script.c_str(), BMessenger(this), ++fReaderRequest, tab.loadGeneration, true);
    }
    state.pendingIDs.clear();
    if (!invalidated && state.active && (state.phase == "working" || state.phase == "downloading")) state.phase = "translated";
}
void BrowserWindow::StartTranslation(Tab& tab)
{
    auto& state = tab.translation;
    if (tab.loading || state.request || state.from.empty() || state.to.empty()
        || TranslationRoute(AvailableTranslationModels(), state.from, state.to).empty()) return;
    CancelTranslation(tab, false);
    state.phase = "working"; state.message = "Preparing page…"; state.dismissed = false;
    TranslationScript(tab, "begin", "__summitTranslation.begin(" + json(state.from).dump() + "," + json(state.to).dump() + ")");
    RefreshNoticeBar();
}
void BrowserWindow::RestoreTranslation(Tab& tab)
{
    CancelTranslation(tab, false);
    tab.translation.dismissed = false;
    TranslationScript(tab, "restore", "globalThis.__summitTranslation?.restore() || {}");
}
void BrowserWindow::PrepareTranslation(Tab& tab)
{
    if (fTranslationTab && fTranslationTab != tab.id) {
        if (auto* other = FindTabByID(fTranslationTab)) CancelTranslation(*other, false);
    }
    if (!fTranslator) fTranslator = std::make_unique<PageTranslator>(*this);
    ScheduleTranslationTick();
    fTranslationTab = tab.id;
    auto& state = tab.translation;
    state.phase = "downloading"; state.message = "Preparing language files…";
    state.job = fTranslator->Prepare(TranslationRoute(AvailableTranslationModels(), state.from, state.to),
        fShared->Path().parent_path() / "TranslationModels");
    RefreshNoticeBar();
}
void BrowserWindow::ScheduleTranslationTick()
{
    if (fTranslationTimer) return;
    BMessage tick(kTranslationTick);
    fTranslationTimer = std::make_unique<BMessageRunner>(BMessenger(this), &tick, 1000000);
}
void BrowserWindow::TranslationTick()
{
    if (std::none_of(fTabs.begin(), fTabs.end(), [](const Tab& tab) {
        return tab.translation.active || tab.translation.probeAfter;
    })) { fTranslationTimer.reset(); return; }
    auto* tab = ActiveTab();
    if (!tab || tab->loading || tab->processExited || fClosingWindow || tab->closeQueued
        || tab->closeRequested || tab->closeApproved) return;
    auto& state = tab->translation;
    if (!state.active && state.probeAfter && system_time() >= state.probeAfter && !state.request) {
        state.probed = 0;
        ProbeTranslation(*tab);
        return;
    }
    if (!state.active || state.request || (state.phase != "working" && state.phase != "translated")) return;
    if (fTranslationTab != tab->id || !fTranslator || !fTranslator->Ready()) {
        PrepareTranslation(*tab); return;
    }
    if (fTranslator->Busy()) return;
    TranslationScript(*tab, "collect", "__summitTranslation.collect()");
}

bool BrowserWindow::TranslationResult(const BMessage& message)
{
    BMessenger view;
    if (message.FindMessenger("view", &view) != B_OK) return false;
    auto* tab = FindTab(view);
    if (!tab || !tab->translation.request || tab->translation.request != message.GetUInt64("identifier", 0)) return false;
    auto& state = tab->translation;
    const auto action = std::exchange(state.action, {});
    state.request = 0;
    if (fClosingWindow || tab->closeQueued || tab->closeRequested || tab->closeApproved || tab->loading || tab->processExited
        || state.generation != tab->loadGeneration || state.generation != message.GetUInt64("generation", 0)) return true;
    auto fail = [&](const std::string& text) {
        CancelTranslation(*tab, false);
        state.phase = "error"; state.message = text; state.dismissed = false;
        RefreshNoticeBar();
    };
    const std::string text = message.GetString("result", "");
    const auto result = text.size() <= 1000000 ? json::parse(text, nullptr, false) : json();
    if (message.HasString("error") || !result.is_object()) {
        if (action != "probe") fail("Could not read this page for translation.");
        return true;
    }
    try {
        if (action == "probe" || action == "manual-probe") {
            if (result.value("url", "") != tab->url) return true;
            if (result.value("active", false)) {
                state.active = true; state.phase = "translated"; state.epoch = result.value("epoch", uint64(0));
                state.from = result.value("source", ""); state.to = result.value("target", "");
                state.truncated = result.value("truncated", false);
                ScheduleTranslationTick();
            } else {
                const auto detected = DetectPageLanguage(result.value("text", ""));
                auto target = PreferredTranslationLanguage();
                if (target.empty() && result.contains("languages") && result["languages"].is_array())
                    target = PreferredPageLanguage(result["languages"].get<std::vector<std::string>>());
                const auto route = TranslationRoute(AvailableTranslationModels(), detected.code, target);
                if (PageNeedsTranslation(detected, target) && !route.empty()) {
                    state.from = detected.code; state.to = target; state.phase = "offered"; state.dismissed = false;
                } else if (action == "manual-probe") {
                    state.from.clear(); state.to.clear();
                    state.phase = "error";
                    state.message = !detected.reliable ? "There is not enough text to determine this page's language."
                        : target.empty() ? "Choose a language in the system Locale preferences first."
                        : detected.code == target ? "This page is already in " + TranslationLanguageName(target) + "."
                        : "Local translation from " + TranslationLanguageName(detected.code) + " to " + TranslationLanguageName(target) + " is not available.";
                } else if (!detected.reliable && state.probeAttempts < 4) {
                    // Some applications render their article after the load
                    // event. Retry a few times, with no ongoing idle polling.
                    state.probeAfter = system_time() + state.probeAttempts * 2000000;
                    ScheduleTranslationTick();
                }
            }
        } else if (action == "begin") {
            state.epoch = result.at("epoch").get<uint64>(); state.active = result.value("active", false);
            if (!state.active) { fail("This page could not start translation."); return true; }
            PrepareTranslation(*tab); return true;
        } else if (action == "restore") {
            state.active = false; state.epoch = result.value("epoch", uint64(0));
            state.phase = "offered"; state.message.clear(); state.dismissed = true;
        } else if (action == "collect") {
            if (!state.active || result.value("epoch", uint64(0)) != state.epoch) return true;
            const auto& groups = result.at("groups");
            state.truncated = result.value("truncated", false);
            if (!groups.is_array() || groups.size() > 256) { fail("This page has too much text in one translation batch."); return true; }
            if (groups.empty()) {
                if (result.value("translated", uint64(0))) { state.phase = "translated"; state.message.clear(); }
                else { fail("No translatable text was found on this page."); return true; }
            } else {
                json ids = json::array();
                for (const auto& group : groups) for (const auto& id : group.at("ids")) ids.push_back(id);
                state.pendingIDs = ids.dump(); state.phase = "working"; state.message = "Translating to " + TranslationLanguageName(state.to) + "…";
                if (fTranslationTab != tab->id || !fTranslator || !fTranslator->Ready()) {
                    CancelTranslation(*tab, false); PrepareTranslation(*tab); return true;
                }
                fTranslator->Translate(state.epoch, groups.dump());
            }
        } else if (action == "apply") {
            state.pendingIDs.clear(); state.truncated = result.value("truncated", false);
            if (state.active && tab->id == fSelected) TranslationTick();
        }
    } catch (...) { fail("Could not process this page for translation."); return true; }
    if (tab->id == fSelected) RefreshNoticeBar();
    return true;
}

void BrowserWindow::TranslationStatus(const BMessage& message)
{
    auto* tab = FindTabByID(fTranslationTab);
    if (!tab || tab->translation.job != message.GetUInt64("job", 0) || !tab->translation.job
        || tab->translation.generation != tab->loadGeneration || tab->loading || tab->processExited) return;
    auto& state = tab->translation;
    const std::string status = message.GetString("state", "");
    if (status == "downloading") {
        state.phase = "downloading"; state.message = message.GetString("text", "");
    } else if (status == "ready") {
        state.phase = "working"; state.message = "Translating to " + TranslationLanguageName(state.to) + "…";
        TranslationScript(*tab, "collect", "__summitTranslation.collect()");
    } else if (status == "result") {
        const std::string text = message.GetString("text", "");
        const auto result = text.size() <= 1000000 ? json::parse(text, nullptr, false) : json();
        if (!result.is_object() || !result.contains("values") || !result["values"].is_array()
            || !result.contains("epoch") || !result["epoch"].is_number_unsigned() || result["epoch"].get<uint64>() != state.epoch) {
            CancelTranslation(*tab, false); state.phase = "error"; state.message = "The local translator returned incomplete text.";
        } else TranslationScript(*tab, "apply", "__summitTranslation.apply(" + std::to_string(state.epoch) + "," + result["values"].dump() + ")");
    } else if (status == "error") {
        CancelTranslation(*tab, false); state.phase = "error"; state.message = message.GetString("text", "Could not translate this page.");
    }
    if (tab->id == fSelected) RefreshNoticeBar();
}

bool BrowserWindow::TranslationNotice(Tab& tab)
{
    const auto& state = tab.translation;
    if (state.phase.empty() || state.dismissed || tab.loading || tab.processExited) return false;
    auto button = [&](uint32 what) { auto* message = new BMessage(what); message->AddInt64("tab", tab.id); return message; };
    if (state.phase == "offered") {
        fNoticeBar->SetNotice("Translate " + TranslationLanguageName(state.from) + " to "
            + TranslationLanguageName(state.to) + "? Runs locally; language files download once.",
            {{"Translate", button(kTranslatePage)}, {"Not Now", button(kDismissTranslation)}});
    } else if (state.phase == "working" || state.phase == "downloading") {
        fNoticeBar->SetNotice(state.message, {{"Cancel", button(kRestoreTranslation)}});
    } else if (state.phase == "translated") {
        fNoticeBar->SetNotice(std::string(state.truncated ? "Part of this page was translated" : "Translated") + " from "
            + TranslationLanguageName(state.from) + " to " + TranslationLanguageName(state.to) + ".",
            {{"Show Original", button(kRestoreTranslation)}, {"Close", button(kDismissTranslation)}});
    } else if (state.phase == "error") {
        std::vector<std::pair<std::string, BMessage*>> buttons;
        if (!state.from.empty() && !state.to.empty()) buttons.emplace_back("Try Again", button(kTranslatePage));
        if (state.active) buttons.emplace_back("Show Original", button(kRestoreTranslation));
        buttons.emplace_back("Close", button(kDismissTranslation));
        fNoticeBar->SetNotice(state.message, std::move(buttons));
    } else return false;
    return true;
}
}
#endif
