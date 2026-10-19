#include "PreferencesWindow.h"
#if SUMMIT_MODERN_WEBKIT
#include "BrowsingPreferences.h"
#endif
#include "Messages.h"
#include "McpService.h"
#include "core/Address.h"
#include <Alert.h>
#include <interface/ColumnListView.h>
#include <interface/ColumnTypes.h>
#include <GroupView.h>
#include <TabView.h>
#include <Button.h>
#include <CheckBox.h>
#include <Invoker.h>
#include <LayoutBuilder.h>
#include <MenuField.h>
#include <MenuItem.h>
#include <PopUpMenu.h>
#include <RadioButton.h>
#include <StringView.h>
#include <TextControl.h>
#include <Clipboard.h>
#include <MessageRunner.h>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <thread>

namespace summit {
namespace {
constexpr uint32 homeEdited = 'phed', useStartPage = 'phsp', bookmarksBarToggled = 'pbar', styleChosen = 'psty',
    searchChosen = 'psrc', clearHistory = 'pchi', clearHistoryConfirmed = 'pchc', clearCache = 'pcca',
    clearSiteData = 'pcsd', clearSiteDataConfirmed = 'pcsc', cacheMeasured = 'pcms',
    forgetCertificates = 'pfce', forgetCertificatesConfirmed = 'pfcc', siteSelected = 'psse', siteAllow = 'pssa',
    restoreSessionToggled = 'prss', mcpToggled = 'pmcp', mcpLocalToggled = 'pmcl', mcpPortEdited = 'pmpe',
    mcpCopy = 'pmcc', mcpNewToken = 'pmnt', mcpNewTokenConfirmed = 'pmnc',
    siteBlock = 'pssb', siteRemove = 'pssr', protocolSelected = 'ppse', protocolRemove = 'pprm';
enum SiteColumn { kSiteColumn, kPermissionColumn, kSettingColumn };

std::string SizeLabel(uintmax_t bytes)
{
    char text[32];
    if (bytes < 1024) std::snprintf(text, sizeof(text), "%ju bytes", bytes);
    else if (bytes < 1024 * 1024) std::snprintf(text, sizeof(text), "%.0f KiB", bytes / 1024.0);
    else if (bytes < 1024ull * 1024 * 1024) std::snprintf(text, sizeof(text), "%.1f MiB", bytes / 1048576.0);
    else std::snprintf(text, sizeof(text), "%.2f GiB", bytes / 1073741824.0);
    return text;
}

BStringView* SectionTitle(const char* name, const char* text)
{
    auto* title = new BStringView(name, text);
    title->SetFont(be_bold_font);
    return title;
}
}

PreferencesWindow::PreferencesWindow(BMessenger owner, const PreferencesState& state)
    : BWindow(BRect(180, 150, 700, 400), "Preferences — Summit", B_TITLED_WINDOW,
        B_NOT_ZOOMABLE | B_NOT_RESIZABLE | B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , fOwner(owner)
    , fWebKitDirectory(state.webKitDirectory)
{
    fDefaultInfo = new BStringView("default-browser-info", "Default browser: checking…");
    fMakeDefault = new BButton("make-default-browser", "Make Summit the Default Browser", new BMessage(kMakeDefaultBrowser));
    fMakeDefault->SetEnabled(false);
    fHome = new BTextControl("home-url", "Home page:", state.homeURL.c_str(), nullptr);
    fHome->SetModificationMessage(new BMessage(homeEdited));
    fHome->SetExplicitMinSize(BSize(380, B_SIZE_UNSET));
    fHint = new BStringView("home-hint", "");
    fHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    auto* engines = new BPopUpMenu("search-engines");
    for (const auto& engine : SearchEngines()) {
        auto* message = new BMessage(searchChosen);
        message->AddString("search_engine", engine.id);
        auto* item = new BMenuItem(engine.name, message);
        item->SetMarked(state.searchEngine == engine.id);
        engines->AddItem(item);
    }
    fSearchEngine = new BMenuField("search-engine", "Search engine:", engines);
    auto* searchHint = new BStringView("search-hint", "Used for text typed in the address field that is not an address.");
    searchHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fRestoreSession = new BCheckBox("restore-session", "Reopen the windows and tabs from last time when Summit starts",
        new BMessage(restoreSessionToggled));
    fRestoreSession->SetValue(state.restoreSession ? B_CONTROL_ON : B_CONTROL_OFF);
    auto* restoreHint = new BStringView("restore-hint", "Also after the computer was shut down or restarted.");
    restoreHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fBookmarksBar = new BCheckBox("show-bookmarks-bar", "Show the bookmarks bar under the toolbar",
        new BMessage(bookmarksBarToggled));
    fBookmarksBar->SetValue(state.showBookmarksBar ? B_CONTROL_ON : B_CONTROL_OFF);
    auto styleMessage = [](const char* style) {
        auto* message = new BMessage(styleChosen);
        message->AddString("interface_style", style);
        return message;
    };
    fHaikuStyle = new BRadioButton("style-haiku", "Haiku: buttons and tabs like other Haiku applications", styleMessage("haiku"));
    fSafariStyle = new BRadioButton("style-safari", "Safari-like: a flat toolbar with the tabs underneath", styleMessage("safari"));
    (state.interfaceStyle == "safari" ? fSafariStyle : fHaikuStyle)->SetValue(B_CONTROL_ON);
    fHistoryInfo = new BStringView("history-info", "");
    fCacheInfo = new BStringView("cache-info", "Cache: measuring…");
    auto* dataInfo = new BStringView("site-data-info", "Cookies and site data");
    auto* dataHint = new BStringView("site-data-hint", "Clearing them signs you out of websites. Extensions keep their own data.");
    dataHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fDataStatus = new BStringView("data-status", "");
    fDataStatus->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    ShowHistoryCount(state.historyCount);
    fCertificatesInfo = new BStringView("certificates-info", "");
    // The three buttons share one width, so they line up in their column.
    auto clearButton = [](const char* name, const char* label, uint32 what) {
        auto* button = new BButton(name, label, new BMessage(what));
        button->SetExplicitMaxSize(BSize(B_SIZE_UNLIMITED, B_SIZE_UNSET));
        return button;
    };
    // Site Permissions: what sites asked for and were answered.
    fSites = new BColumnListView("site-permissions", B_NAVIGABLE, B_FANCY_BORDER, true);
    fSites->AddColumn(new BStringColumn("Site", 240, 80, 600, B_TRUNCATE_MIDDLE), kSiteColumn);
    fSites->AddColumn(new BStringColumn("Permission", 120, 60, 300, B_TRUNCATE_END), kPermissionColumn);
    fSites->AddColumn(new BStringColumn("Setting", 90, 50, 200, B_TRUNCATE_END), kSettingColumn);
    fSites->SetSelectionMessage(new BMessage(siteSelected));
    fSites->SetSelectionMode(B_SINGLE_SELECTION_LIST);
    // Rows stay in fSiteEntries' order, which the buttons rely on.
    fSites->SetSortingEnabled(false);
    fSites->SetExplicitMinSize(BSize(470, 170));
    fSiteAllow = new BButton("site-allow", "Allow", new BMessage(siteAllow));
    fSiteBlock = new BButton("site-block", "Block", new BMessage(siteBlock));
    fSiteRemove = new BButton("site-remove", "Remove", new BMessage(siteRemove));
    auto* sitesIntro = new BStringView("sites-intro", "Sites you have allowed or blocked. Other sites ask first.");
    auto* sitesRemoveHint = new BStringView("sites-remove-hint", "A removed site asks again the next time it wants to.");
    sitesRemoveHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    auto* locationHint = new BStringView("location-hint", "Your location comes from BeaconDB (beacondb.net), from the "
        "Wi-Fi networks nearby and your network address.");
    locationHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    ShowSitePermissions({ });

    auto* general = new BGroupView("General", B_VERTICAL, 10);
    BLayoutBuilder::Group<>(general)
        .SetInsets(18)
        .Add(SectionTitle("general-title", "General"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .AddGroup(B_HORIZONTAL, 8)
                .Add(fDefaultInfo)
                .AddGlue()
                .Add(fMakeDefault)
            .End()
            .AddStrut(4)
            .Add(fHome)
            .AddGroup(B_HORIZONTAL, 8)
                .AddGlue()
                .Add(new BButton("use-current", "Use Current Page", new BMessage(kPreferencesUseCurrentPage)))
                .Add(new BButton("use-start", "Use Start Page", new BMessage(useStartPage)))
            .End()
            .Add(fHint)
            .AddStrut(4)
            .AddGroup(B_HORIZONTAL, 0)
                .Add(fSearchEngine)
                .AddGlue()
            .End()
            .Add(searchHint)
            .AddStrut(4)
            .Add(fRestoreSession)
            .Add(restoreHint)
        .End()
        .AddStrut(6)
        .Add(SectionTitle("bookmarks-title", "Bookmarks"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fBookmarksBar)
        .End()
        .AddStrut(6)
        .Add(SectionTitle("appearance-title", "Appearance"))
        .AddGroup(B_VERTICAL, 4).SetInsets(12, 0, 0, 0)
            .Add(fHaikuStyle)
            .Add(fSafariStyle)
        .End()
        .AddGlue();
    auto* data = new BGroupView("History and Data", B_VERTICAL, 10);
    BLayoutBuilder::Group<>(data)
        .SetInsets(18)
        .Add(SectionTitle("data-title", "History and Data"))
        .AddGrid(10, 6).SetInsets(12, 0, 0, 0)
            .Add(fHistoryInfo, 0, 0)
            .Add(clearButton("clear-history", "Clear History…", clearHistory), 1, 0)
            .Add(fCacheInfo, 0, 1)
            .Add(clearButton("clear-cache", "Clear Cache", clearCache), 1, 1)
            .Add(dataInfo, 0, 2)
            .Add(clearButton("clear-site-data", "Clear Cookies and Site Data…", clearSiteData), 1, 2)
            .Add(dataHint, 0, 3, 2, 1)
            .Add(fCertificatesInfo, 0, 4)
            .Add(fForgetCertificates = clearButton("forget-certificates", "Forget Trusted Certificates…",
                forgetCertificates), 1, 4)
            .Add(fDataStatus, 0, 5, 2, 1)
        .End()
        .AddGlue();
    auto* sites = new BGroupView("Site Permissions", B_VERTICAL, 8);
    BLayoutBuilder::Group<>(sites)
        .SetInsets(18)
        .Add(SectionTitle("sites-title", "Site Permissions"))
        .Add(sitesIntro)
        .Add(fSites)
        .AddGroup(B_HORIZONTAL, 8)
            .Add(sitesRemoveHint)
            .AddGlue()
            .Add(fSiteAllow)
            .Add(fSiteBlock)
            .Add(fSiteRemove)
        .End()
        .Add(locationHint);
    fProtocols = new BColumnListView("link-handlers", B_NAVIGABLE, B_FANCY_BORDER, true);
    fProtocols->AddColumn(new BStringColumn("Link scheme", 120, 80, 200, B_TRUNCATE_END), 0);
    fProtocols->AddColumn(new BStringColumn("Opens with", 350, 100, 800, B_TRUNCATE_MIDDLE), 1);
    fProtocols->SetSelectionMessage(new BMessage(protocolSelected));
    fProtocols->SetSelectionMode(B_SINGLE_SELECTION_LIST);
    fProtocols->SetSortingEnabled(false);
    fProtocols->SetExplicitMinSize(BSize(470, 170));
    fProtocolRemove = new BButton("link-handler-remove", "Remove", new BMessage(protocolRemove));
    ShowProtocolHandlers({});
    auto* protocols = new BGroupView("Link Handlers", B_VERTICAL, 8);
    BLayoutBuilder::Group<>(protocols).SetInsets(18)
        .Add(SectionTitle("link-handlers-title", "Link Handlers"))
        .Add(new BStringView("link-handlers-intro", "Applications and websites chosen to open links."))
        .Add(fProtocols)
        .AddGroup(B_HORIZONTAL, 8)
            .Add(new BStringView("link-handlers-hint", "Remove a choice to be asked again."))
            .AddGlue().Add(fProtocolRemove)
        .End();
    // Developer: Summit's own debugging server.
    fMcpEnabled = new BCheckBox("mcp-enabled", "Let debugging tools connect to Summit (MCP server)", new BMessage(mcpToggled));
    fMcpEnabled->SetValue(state.mcpEnabled ? B_CONTROL_ON : B_CONTROL_OFF);
    auto* mcpHint = new BStringView("mcp-hint", "An MCP client such as Claude Code can then look inside Summit: pages, "
        "processes, memory, rendering, media and extensions.");
    mcpHint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    auto* mcpWarning = new BStringView("mcp-warning", "Whoever has the access token can read your pages and act in them.");
    mcpWarning->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fMcpPort = new BTextControl("mcp-port", "Port:", std::to_string(state.mcpPort).c_str(), new BMessage(mcpPortEdited));
    fMcpPort->SetExplicitMaxSize(BSize(140, B_SIZE_UNSET));
    for (int32 c = 0; c < 256; ++c) if (c < '0' || c > '9') fMcpPort->TextView()->DisallowChar(c);
    fMcpPort->TextView()->SetMaxBytes(5);
    fMcpLocalOnly = new BCheckBox("mcp-local", "Only from this computer", new BMessage(mcpLocalToggled));
    fMcpLocalOnly->SetValue(state.mcpLocalOnly ? B_CONTROL_ON : B_CONTROL_OFF);
    fMcpStatus = new BStringView("mcp-status", "Status: off");
    fMcpEndpoint = new BStringView("mcp-endpoint", "");
    fMcpEndpoint->SetHighUIColor(B_PANEL_TEXT_COLOR, B_DARKEN_2_TINT);
    fMcpCopy = new BButton("mcp-copy", "Copy Client Configuration", new BMessage(mcpCopy));
    fMcpNewToken = new BButton("mcp-new-token", "New Access Token…", new BMessage(mcpNewToken));
    auto* developer = new BGroupView("Developer", B_VERTICAL, 8);
    BLayoutBuilder::Group<>(developer).SetInsets(18)
        .Add(SectionTitle("mcp-title", "Debugging Server"))
        .AddGroup(B_VERTICAL, 6).SetInsets(12, 0, 0, 0)
            .Add(fMcpEnabled)
            .Add(mcpHint)
            .Add(mcpWarning)
            .AddStrut(4)
            .AddGroup(B_HORIZONTAL, 12)
                .Add(fMcpPort)
                .Add(fMcpLocalOnly)
                .AddGlue()
            .End()
            .AddStrut(4)
            .Add(fMcpStatus)
            .Add(fMcpEndpoint)
            .AddGroup(B_HORIZONTAL, 8)
                .AddGlue()
                .Add(fMcpNewToken)
                .Add(fMcpCopy)
            .End()
        .End()
        .AddGlue();
    fTabs = new BTabView("preferences-tabs", B_WIDTH_FROM_LABEL);
    fTabs->AddTab(general);
#if SUMMIT_MODERN_WEBKIT
    fTabs->AddTab(new BrowsingPreferences(fOwner, state.autoplay, state.spellChecking, state.savePasswords));
#endif
    fTabs->AddTab(data);
    fTabs->AddTab(sites);
    fTabs->AddTab(protocols);
    fTabs->AddTab(developer);
    fTabs->SetBorder(B_NO_BORDER);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 0)
        .SetInsets(0, B_USE_SMALL_SPACING, 0, 0)
        .Add(fTabs);
    ShowTrustedCertificates(state.trustedCertificates);
    ShowHomeHint();
    MeasureCache();
    fMcpCopy->SetEnabled(false);
    fMcpNewToken->SetEnabled(state.mcpEnabled);
    // The server's status (requests answered, the last client) changes by
    // itself; ask the application for it now and then.
    BMessage refresh(kMcpStatus);
    fMcpRefresh = std::make_unique<BMessageRunner>(fOwner, &refresh, 3000000);
}

void PreferencesWindow::SendMcpPort()
{
    const long port = std::strtol(fMcpPort->Text(), nullptr, 10);
    if (port <= 0 || port > 65535) {
        fMcpStatus->SetText("Status: the port must be between 1 and 65535");
        return;
    }
    BMessage changed(kPreferencesChanged);
    changed.AddInt32("mcp_port", static_cast<int32>(port));
    fOwner.SendMessage(&changed);
}

void PreferencesWindow::ShowHomeHint()
{
    const auto text = Trim(fHome->Text());
    if (text.empty()) {
        fHint->SetText("The Home button opens the Summit start page.");
        return;
    }
    const auto address = ResolveAddress(text);
    if (!address.error.empty()) fHint->SetText(address.error.c_str());
    else if (address.search) fHint->SetText("This is not an address, so Home will search for it.");
    else fHint->SetText(("The Home button opens " + address.url).c_str());
}

void PreferencesWindow::ShowTrustedCertificates(size_t count)
{
    // Sites whose certificate failed verification but which the user chose to
    // visit anyway (a router's self-signed certificate, for example).
    fCertificatesInfo->SetText(count == 0 ? "No untrusted certificates accepted"
        : count == 1 ? "1 untrusted certificate accepted"
        : (std::to_string(count) + " untrusted certificates accepted").c_str());
    fForgetCertificates->SetEnabled(count > 0);
}

void PreferencesWindow::ShowSitePermissions(const std::vector<SiteEntry>& entries)
{
    // Keep the selection on the same site and permission across updates.
    std::string selectedOrigin, selectedPermission;
    if (auto* row = fSites->CurrentSelection(); row) {
        const auto index = fSites->IndexOf(row);
        if (index >= 0 && static_cast<size_t>(index) < fSiteEntries.size()) {
            selectedOrigin = fSiteEntries[index].origin;
            selectedPermission = fSiteEntries[index].permission;
        }
    }
    fSites->Clear();
    fSiteEntries = entries;
    std::sort(fSiteEntries.begin(), fSiteEntries.end(), [](const SiteEntry& a, const SiteEntry& b) {
        return std::tie(a.origin, a.permission) < std::tie(b.origin, b.permission);
    });
    BRow* selected = nullptr;
    for (const auto& entry : fSiteEntries) {
        auto* row = new BRow();
        // https is the rule and goes without saying; http is shown.
        const std::string site = entry.origin.rfind("https://", 0) == 0 ? entry.origin.substr(8) : entry.origin;
        row->SetField(new BStringField(site.c_str()), kSiteColumn);
        row->SetField(new BStringField(entry.permission == "notifications" ? "Notifications"
            : entry.permission == "geolocation" ? "Location"
            : entry.permission == "camera" ? "Camera"
            : entry.permission == "microphone" ? "Microphone"
            : entry.permission == "popups" ? "Pop-ups"
            : entry.permission == "autoplay" ? "Autoplay" : entry.permission.c_str()), kPermissionColumn);
        row->SetField(new BStringField(entry.allowed ? "Allowed" : "Blocked"), kSettingColumn);
        fSites->AddRow(row);
        if (entry.origin == selectedOrigin && entry.permission == selectedPermission) selected = row;
    }
    if (selected) fSites->SetFocusRow(selected, true);
    const bool hasSelection = fSites->CurrentSelection() != nullptr;
    fSiteAllow->SetEnabled(hasSelection);
    fSiteBlock->SetEnabled(hasSelection);
    fSiteRemove->SetEnabled(hasSelection);
}

void PreferencesWindow::ShowProtocolHandlers(const std::vector<std::pair<std::string, std::string>>& entries)
{
    std::string selection;
    if (auto* row = fProtocols->CurrentSelection()) {
        const auto index = fProtocols->IndexOf(row);
        if (index >= 0 && static_cast<size_t>(index) < fProtocolEntries.size()) selection = fProtocolEntries[index].first;
    }
    fProtocols->Clear();
    fProtocolEntries = entries;
    std::sort(fProtocolEntries.begin(), fProtocolEntries.end());
    for (const auto& [scheme, target] : fProtocolEntries) {
        auto* row = new BRow();
        row->SetField(new BStringField((scheme + ":").c_str()), 0);
        row->SetField(new BStringField(target.c_str()), 1);
        fProtocols->AddRow(row);
        if (selection == scheme) fProtocols->SetFocusRow(row, true);
    }
    fProtocolRemove->SetEnabled(fProtocols->CurrentSelection() != nullptr);
}

void PreferencesWindow::ShowHistoryCount(size_t count)
{
    const std::string text = count == 0 ? "History is empty"
        : "History: " + std::to_string(count) + (count == 1 ? " page" : " pages");
    fHistoryInfo->SetText(text.c_str());
}

void PreferencesWindow::MeasureCache()
{
    // Walking a few thousand cache files takes a moment; the window stays usable.
    std::thread([directory = fWebKitDirectory / "Cache", window = BMessenger(this)] {
        uintmax_t total = 0;
        std::error_code error;
        for (auto entry = std::filesystem::recursive_directory_iterator(directory,
                 std::filesystem::directory_options::skip_permission_denied, error);
            !error && entry != std::filesystem::recursive_directory_iterator(); entry.increment(error)) {
            std::error_code ignored;
            if (entry->is_regular_file(ignored)) total += entry->file_size(ignored);
        }
        BMessage measured(cacheMeasured);
        measured.AddUInt64("bytes", total);
        window.SendMessage(&measured);
    }).detach();
}

void PreferencesWindow::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case homeEdited: {
            ShowHomeHint();
            const auto text = Trim(fHome->Text());
            if (!text.empty() && !ResolveAddress(text).error.empty()) break;
            BMessage changed(kPreferencesChanged);
            changed.AddString("home_url", text.c_str());
            fOwner.SendMessage(&changed);
            break;
        }
        case useStartPage:
            fHome->SetText("");
            PostMessage(homeEdited);
            break;
        case restoreSessionToggled: {
            BMessage changed(kPreferencesChanged);
            changed.AddBool("restore_session", fRestoreSession->Value() == B_CONTROL_ON);
            fOwner.SendMessage(&changed);
            break;
        }
        case mcpToggled: {
            BMessage changed(kPreferencesChanged);
            changed.AddBool("mcp_enabled", fMcpEnabled->Value() == B_CONTROL_ON);
            // A port typed but not entered counts.
            const long port = std::strtol(fMcpPort->Text(), nullptr, 10);
            if (port > 0 && port <= 65535) changed.AddInt32("mcp_port", static_cast<int32>(port));
            fOwner.SendMessage(&changed);
            break;
        }
        case mcpLocalToggled: {
            BMessage changed(kPreferencesChanged);
            changed.AddBool("mcp_local_only", fMcpLocalOnly->Value() == B_CONTROL_ON);
            fOwner.SendMessage(&changed);
            break;
        }
        case mcpPortEdited:
            SendMcpPort();
            break;
        case mcpCopy:
            if (!fMcpConfiguration.empty() && be_clipboard->Lock()) {
                be_clipboard->Clear();
                be_clipboard->Data()->AddData("text/plain", B_MIME_TYPE, fMcpConfiguration.data(), fMcpConfiguration.size());
                be_clipboard->Commit();
                be_clipboard->Unlock();
                fMcpEndpoint->SetText("Copied: the server's address and access token, for an MCP client.");
            }
            break;
        case mcpNewToken: {
            auto* alert = new BAlert("New Access Token", "Make a new access token?\n\nClients that use the old one can "
                "no longer connect until they are given the new one.", "Cancel", "New Token", nullptr, B_WIDTH_AS_USUAL,
                B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            alert->Go(new BInvoker(new BMessage(mcpNewTokenConfirmed), this));
            break;
        }
        case mcpNewTokenConfirmed:
            if (message->GetInt32("which", 0) != 1) break;
            {
                BMessage changed(kPreferencesChanged);
                changed.AddBool("mcp_new_token", true);
                fOwner.SendMessage(&changed);
            }
            break;
        case bookmarksBarToggled: {
            BMessage changed(kPreferencesChanged);
            changed.AddBool("show_bookmarks_bar", fBookmarksBar->Value() == B_CONTROL_ON);
            fOwner.SendMessage(&changed);
            break;
        }
        case styleChosen: {
            BMessage changed(kPreferencesChanged);
            changed.AddString("interface_style", message->GetString("interface_style", "haiku"));
            fOwner.SendMessage(&changed);
            break;
        }
        case searchChosen: {
            BMessage changed(kPreferencesChanged);
            changed.AddString("search_engine", message->GetString("search_engine", "duckduckgo"));
            fOwner.SendMessage(&changed);
            // The home page hint may now search elsewhere.
            ShowHomeHint();
            break;
        }
        case clearHistory: {
            auto* alert = new BAlert("Clear History", "Clear your history?\n\nEvery page you have visited is forgotten, "
                "with the icons of sites you have not bookmarked. Bookmarks and open tabs are kept.",
                "Cancel", "Clear History", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            alert->Go(new BInvoker(new BMessage(clearHistoryConfirmed), this));
            break;
        }
        case clearHistoryConfirmed:
            if (message->GetInt32("which", 0) != 1) break;
            fDataStatus->SetText("Clearing history…");
            fOwner.SendMessage(kClearHistoryRequest);
            break;
        case clearCache:
            fDataStatus->SetText("Clearing the cache…");
            fOwner.SendMessage(kClearCacheRequest);
            break;
        case clearSiteData: {
            auto* alert = new BAlert("Clear Cookies and Site Data", "Clear cookies and site data?\n\nYou will be signed "
                "out of websites, and the data they keep on this computer is deleted. Extensions keep their own data.",
                "Cancel", "Clear", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            alert->Go(new BInvoker(new BMessage(clearSiteDataConfirmed), this));
            break;
        }
        case clearSiteDataConfirmed:
            if (message->GetInt32("which", 0) != 1) break;
            fDataStatus->SetText("Clearing cookies and site data…");
            fOwner.SendMessage(kClearSiteDataRequest);
            break;
        case forgetCertificates: {
            auto* alert = new BAlert("Forget Trusted Certificates", "Forget the certificates you chose to trust?\n\n"
                "Sites whose certificate Summit cannot verify, such as a router's own, will show the warning again.",
                "Cancel", "Forget", nullptr, B_WIDTH_AS_USUAL, B_WARNING_ALERT);
            alert->SetShortcut(0, B_ESCAPE);
            alert->Go(new BInvoker(new BMessage(forgetCertificatesConfirmed), this));
            break;
        }
        case forgetCertificatesConfirmed:
            if (message->GetInt32("which", 0) != 1) break;
            fOwner.SendMessage(kForgetCertificatesRequest);
            break;
        case kDataCleared: {
            const char* error = nullptr;
            const uint32 kind = message->GetInt32("kind", 0);
            if (message->FindString("error", &error) == B_OK && error)
                fDataStatus->SetText((std::string("Could not clear: ") + error).c_str());
            else fDataStatus->SetText(kind == kClearHistoryRequest ? "History cleared."
                : kind == kClearCacheRequest ? "Cache cleared."
                : kind == kForgetCertificatesRequest ? "Trusted certificates forgotten." : "Cookies and site data cleared.");
            if (kind == kClearCacheRequest) MeasureCache();
            break;
        }
        case cacheMeasured:
            fCacheInfo->SetText(("Cache: " + SizeLabel(message->GetUInt64("bytes", 0))).c_str());
            break;
        case kPreferencesUseCurrentPage:
            fOwner.SendMessage(kPreferencesUseCurrentPage);
            break;
        case kPreferencesState: {
            if (message->GetBool("has_protocol_handlers", false)) {
                std::vector<std::pair<std::string, std::string>> entries;
                const char* scheme = nullptr;
                const char* target = nullptr;
                for (int32 i = 0; message->FindString("protocol_scheme", i, &scheme) == B_OK
                    && message->FindString("protocol_target", i, &target) == B_OK; ++i) entries.emplace_back(scheme, target);
                ShowProtocolHandlers(entries);
            }
            // The browser changed a setting (the current page, or View › Bookmarks Bar).
            const char* home = nullptr;
            if (message->FindString("home_url", &home) == B_OK && home && Trim(fHome->Text()) != home) {
                fHome->SetText(home);
                ShowHomeHint();
            }
            bool mcp = false;
            if (message->FindBool("mcp_enabled", &mcp) == B_OK) {
                fMcpEnabled->SetValue(mcp ? B_CONTROL_ON : B_CONTROL_OFF);
                fMcpNewToken->SetEnabled(mcp);
            }
            if (message->FindBool("mcp_local_only", &mcp) == B_OK) fMcpLocalOnly->SetValue(mcp ? B_CONTROL_ON : B_CONTROL_OFF);
            if (int32 port = 0; message->FindInt32("mcp_port", &port) == B_OK && !fMcpPort->TextView()->IsFocus())
                fMcpPort->SetText(std::to_string(port).c_str());
            if (const char* status = nullptr; message->FindString("mcp_status", &status) == B_OK) {
                std::string text = std::string("Status: ") + status;
                if (message->GetBool("mcp_environment", false)) text += " (set by SUMMIT_MCP in the environment)";
                fMcpStatus->SetText(text.c_str());
                const bool running = std::string(status).rfind("Listening", 0) == 0;
                const char* endpoint = message->GetString("mcp_endpoint", "");
                if (std::string(fMcpEndpoint->Text()).rfind("Copied", 0) != 0 || !running)
                    fMcpEndpoint->SetText(running ? (std::string("Clients connect to ") + endpoint).c_str() : "");
                fMcpConfiguration = running ? message->GetString("mcp_configuration", "") : "";
                fMcpCopy->SetEnabled(running);
            }
            bool bar = false;
            if (message->FindBool("show_bookmarks_bar", &bar) == B_OK)
                fBookmarksBar->SetValue(bar ? B_CONTROL_ON : B_CONTROL_OFF);
            bool restore = true;
            if (message->FindBool("restore_session", &restore) == B_OK)
                fRestoreSession->SetValue(restore ? B_CONTROL_ON : B_CONTROL_OFF);
            const char* style = nullptr;
            if (message->FindString("interface_style", &style) == B_OK && style)
                (std::string(style) == "safari" ? fSafariStyle : fHaikuStyle)->SetValue(B_CONTROL_ON);
            const char* engine = nullptr;
            if (message->FindString("search_engine", &engine) == B_OK && engine)
                for (int32 i = 0; auto* item = fSearchEngine->Menu()->ItemAt(i); ++i)
                    item->SetMarked(item->Message() && std::string(item->Message()->GetString("search_engine", "")) == engine);
            int32 count = 0;
            if (message->FindInt32("history_count", &count) == B_OK) ShowHistoryCount(count);
            if (message->FindInt32("trusted_certificates", &count) == B_OK) ShowTrustedCertificates(count);
            if (message->GetBool("has_site_permissions", false)) {
                std::vector<SiteEntry> entries;
                const char* permission = nullptr;
                const char* origin = nullptr;
                bool allowed = false;
                for (int32 i = 0; message->FindString("site_permission", i, &permission) == B_OK
                    && message->FindString("site_origin", i, &origin) == B_OK
                    && message->FindBool("site_allowed", i, &allowed) == B_OK; ++i)
                    entries.push_back({permission, origin, allowed});
                ShowSitePermissions(entries);
            }
            break;
        }
        case kMakeDefaultBrowser:
            fMakeDefault->SetEnabled(false);
            fDefaultInfo->SetText("Making Summit the default browser…");
            fOwner.SendMessage(kMakeDefaultBrowser);
            break;
        case kDefaultBrowserState: {
            const bool isDefault = message->GetBool("is_default", false);
            const std::string current = message->GetString("current", "");
            const char* error = nullptr;
            if (message->FindString("error", &error) == B_OK && error && *error) fDefaultInfo->SetText(error);
            else if (isDefault) fDefaultInfo->SetText("Summit is your default browser.");
            else if (current.empty()) fDefaultInfo->SetText("No default browser is set.");
            else fDefaultInfo->SetText(("Your default browser is " + current + ".").c_str());
            fDefaultInfo->SetToolTip("Web links and pages you open from other applications, Tracker and the "
                "command line open in the default browser.");
            fMakeDefault->SetEnabled(!isDefault);
            break;
        }
        case kShowPreferences:
            if (const char* tab = nullptr; message->FindString("tab", &tab) == B_OK && tab) {
                const std::string name = tab;
                fTabs->Select(name == "data" ? 1 : name == "sites" ? 2 : name == "links" ? 3 : name == "developer" ? 4 : 0);
            }
            Activate();
            break;
        case protocolSelected:
            fProtocolRemove->SetEnabled(fProtocols->CurrentSelection() != nullptr);
            break;
        case protocolRemove: {
            auto* row = fProtocols->CurrentSelection();
            if (!row) break;
            const auto index = fProtocols->IndexOf(row);
            if (index < 0 || static_cast<size_t>(index) >= fProtocolEntries.size()) break;
            BMessage remove(kProtocolHandlerRemove);
            remove.AddString("scheme", fProtocolEntries[index].first.c_str());
            fOwner.SendMessage(&remove);
            break;
        }
        case siteSelected: {
            auto* row = fSites->CurrentSelection();
            fSiteAllow->SetEnabled(row != nullptr);
            fSiteBlock->SetEnabled(row != nullptr);
            fSiteRemove->SetEnabled(row != nullptr);
            break;
        }
        case siteAllow:
        case siteBlock:
        case siteRemove: {
            auto* row = fSites->CurrentSelection();
            if (!row) break;
            const auto index = fSites->IndexOf(row);
            if (index < 0 || static_cast<size_t>(index) >= fSiteEntries.size()) break;
            const auto& entry = fSiteEntries[index];
            BMessage change(kSitePermissionChange);
            change.AddString("permission", entry.permission.c_str());
            change.AddString("origin", entry.origin.c_str());
            change.AddInt32("state", message->what == siteAllow ? 1 : message->what == siteBlock ? 0 : -1);
            fOwner.SendMessage(&change);
            break;
        }
        default: BWindow::MessageReceived(message); break;
    }
}

bool PreferencesWindow::QuitRequested()
{
    BMessage closed(kPreferencesClosed);
    closed.AddMessenger("window", BMessenger(this));
    fOwner.SendMessage(&closed);
    return true;
}
}
