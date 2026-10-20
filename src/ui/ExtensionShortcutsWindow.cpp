#include "ExtensionShortcutsWindow.h"
#if SUMMIT_MODERN_WEBKIT
#include "ExtensionInstaller.h"
#include "ExtensionShortcuts.h"
#include "Messages.h"
#include <Button.h>
#include <LayoutBuilder.h>
#include <Message.h>
#include <StringView.h>
#include <interface/ColumnListView.h>
#include <interface/ColumnTypes.h>
#include <algorithm>
#include <cstring>

namespace summit {
namespace {
constexpr uint32 selected = 'eksl', change = 'ekch', clear = 'ekcl', reset = 'ekrs';
enum { kExtensionColumn, kCommandColumn, kKeysColumn, kNoteColumn };
class CommandRow final : public BRow {
public:
    explicit CommandRow(size_t index) : index(index) { }
    size_t index;
};
std::string CommandLabel(const ExtensionCommandBinding& binding)
{
    const bool action = binding.command == "_execute_action" || binding.command == "_execute_browser_action"
        || binding.command == "_execute_page_action";
    if (action) return "Open " + ExtensionDisplayText(binding.extensionName) + " (its toolbar button)";
    return ExtensionDisplayText(binding.description.empty() ? binding.command : binding.description);
}
}

ExtensionShortcutsWindow::ExtensionShortcutsWindow(BMessenger owner, std::shared_ptr<std::atomic<unsigned>> windows)
    : BWindow(BRect(180, 160, 940, 600), "Keyboard Shortcuts — Summit", B_TITLED_WINDOW,
        B_ASYNCHRONOUS_CONTROLS | B_AUTO_UPDATE_SIZE_LIMITS | B_CLOSE_ON_ESCAPE)
    , fOwner(owner), fWindows(std::move(windows))
{
    ++*fWindows;
    auto* title = new BStringView("shortcuts-title", "Extension keyboard shortcuts");
    title->SetFont(be_bold_font);
    auto* hint = new BStringView("shortcuts-hint", "Summit's own shortcuts come first; keys they use do nothing for an extension.");
    fList = new BColumnListView("extension-shortcuts", B_NAVIGABLE, B_FANCY_BORDER, true);
    fList->AddColumn(new BStringColumn("Extension", 160, 80, 400, B_TRUNCATE_END), kExtensionColumn);
    fList->AddColumn(new BStringColumn("Command", 250, 80, 600, B_TRUNCATE_END), kCommandColumn);
    fList->AddColumn(new BStringColumn("Keys", 130, 60, 300, B_TRUNCATE_END), kKeysColumn);
    fList->AddColumn(new BStringColumn("Note", 200, 60, 600, B_TRUNCATE_END), kNoteColumn);
    fList->SetSelectionMessage(new BMessage(selected));
    fList->SetInvocationMessage(new BMessage(change));
    fList->SetExplicitMinSize(BSize(620, 220));
    fChange = new BButton("shortcut-change", "Change…", new BMessage(change));
    fClear = new BButton("shortcut-clear", "Clear", new BMessage(clear));
    fReset = new BButton("shortcut-reset", "Reset to Default", new BMessage(reset));
    fStatus = new BStringView("shortcut-status", "");
    fStatus->SetTruncation(B_TRUNCATE_END);
    BLayoutBuilder::Group<>(this, B_VERTICAL, 10)
        .SetInsets(16)
        .Add(title)
        .Add(hint)
        .Add(fList)
        .AddGroup(B_HORIZONTAL, 8).Add(fChange).Add(fClear).Add(fReset).AddGlue().End()
        .Add(fStatus);
    Refresh();
}
ExtensionShortcutsWindow::~ExtensionShortcutsWindow()
{
    --*fWindows;
}
bool ExtensionShortcutsWindow::QuitRequested()
{
    BMessage closed(kExtensionShortcutsClosed);
    Send(closed);
    return true;
}
void ExtensionShortcutsWindow::Send(BMessage& message)
{
    message.AddMessenger("window", BMessenger(this));
    if (fOwner.SendMessage(&message, static_cast<BHandler*>(nullptr), 0) != B_OK)
        fStatus->SetText("Could not reach Summit. Please try again.");
}
const ExtensionCommandBinding* ExtensionShortcutsWindow::Selected() const
{
    auto* row = dynamic_cast<CommandRow*>(fList->CurrentSelection());
    return row && row->index < fBindings.size() ? &fBindings[row->index] : nullptr;
}
void ExtensionShortcutsWindow::Refresh()
{
    std::string extension, command;
    if (const auto* binding = Selected()) {
        extension = binding->extension;
        command = binding->command;
    }
    fBindings = ExtensionShortcuts::Default().Bindings();
    fList->Clear();
    CommandRow* select = nullptr;
    for (size_t i = 0; i < fBindings.size(); ++i) {
        const auto& binding = fBindings[i];
        auto* row = new CommandRow(i);
        row->SetField(new BStringField(ExtensionDisplayText(binding.extensionName).c_str()), kExtensionColumn);
        row->SetField(new BStringField(CommandLabel(binding).c_str()), kCommandColumn);
        row->SetField(new BStringField(binding.keys ? ExtensionShortcuts::Label(*binding.keys).c_str() : "None"), kKeysColumn);
        std::string note = binding.conflict;
        if (note.empty() && binding.shortcut != binding.suggested)
            note = binding.suggested.empty() ? "Set in Summit" : "Its own: " + (ParseExtensionShortcut(binding.suggested)
                ? ExtensionShortcuts::Label(*ParseExtensionShortcut(binding.suggested)) : binding.suggested);
        row->SetField(new BStringField(note.c_str()), kNoteColumn);
        fList->AddRow(row);
        if (binding.extension == extension && binding.command == command) select = row;
    }
    if (!select && fList->CountRows()) select = dynamic_cast<CommandRow*>(fList->RowAt(0));
    if (select) fList->AddToSelection(select);
    if (fBindings.empty()) fStatus->SetText("No running extension has commands.");
    Render();
}
void ExtensionShortcutsWindow::Render()
{
    const auto* binding = Selected();
    fChange->SetEnabled(binding && !fCapturing);
    fClear->SetEnabled(binding && !fCapturing && !binding->shortcut.empty());
    fReset->SetEnabled(binding && !fCapturing && binding->shortcut != binding->suggested);
}
void ExtensionShortcutsWindow::Capture(const BMessage& keyDown)
{
    const auto* binding = Selected();
    const char* bytes = keyDown.GetString("bytes", "");
    const uint32 modifiers = keyDown.GetInt32("modifiers", 0);
    const bool escape = bytes[0] == B_ESCAPE || keyDown.GetInt32("raw_char", 0) == B_ESCAPE;
    if (!binding || (escape && !(modifiers & (B_COMMAND_KEY | B_CONTROL_KEY | B_SHIFT_KEY)))) {
        fCapturing = false;
        fStatus->SetText("The keys were not changed.");
        Render();
        return;
    }
    const auto keys = ExtensionShortcutKeysFor(keyDown);
    if (!keys) {
        fStatus->SetText(("Use " + ExtensionShortcuts::CommandKeyName() + " or " + ExtensionShortcuts::ControlKeyName()
            + ", and Shift if you like, with a letter, a digit, an arrow, Comma, Period or Space, or a function key.").c_str());
        return;
    }
    if (const auto browser = ExtensionShortcuts::Default().BrowserUses(*keys); !browser.empty()) {
        fStatus->SetText(("Summit uses " + ExtensionShortcuts::Label(*keys) + " for " + browser + ". Press other keys.").c_str());
        return;
    }
    for (const auto& other : fBindings) {
        if (&other != binding && other.keys && other.conflict.empty() && *other.keys == *keys) {
            fStatus->SetText((ExtensionDisplayText(other.extensionName) + " uses " + ExtensionShortcuts::Label(*keys)
                + " for " + CommandLabel(other) + ". Press other keys.").c_str());
            return;
        }
    }
    fCapturing = false;
    BMessage request(kExtensionSetShortcut);
    request.AddString("extension_identifier", binding->extension.c_str());
    request.AddString("command", binding->command.c_str());
    request.AddString("shortcut", ExtensionShortcutText(*keys).c_str());
    Send(request);
    fStatus->SetText(("Saving " + ExtensionShortcuts::Label(*keys) + "…").c_str());
    Render();
}
void ExtensionShortcutsWindow::DispatchMessage(BMessage* message, BHandler* handler)
{
    // While waiting for keys every key press is a candidate, Alt-Q too.
    if (fCapturing && message && (message->what == B_KEY_DOWN || message->what == B_UNMAPPED_KEY_DOWN)) {
        if (message->what == B_KEY_DOWN) Capture(*message);
        return;
    }
    BWindow::DispatchMessage(message, handler);
}
void ExtensionShortcutsWindow::MessageReceived(BMessage* message)
{
    switch (message->what) {
        case kExtensionShortcutsState: {
            Refresh();
            if (const char* status = message->GetString("status", nullptr)) fStatus->SetText(status);
            else if (!std::strncmp(fStatus->Text(), "Saving", 6)) fStatus->SetText("Saved. The keys stay after a restart.");
            if (message->GetBool("activate", false)) Activate();
            return;
        }
        case selected:
            fCapturing = false;
            Render();
            return;
        case change:
            if (const auto* binding = Selected()) {
                fCapturing = true;
                fStatus->SetText(("Press the new keys for " + CommandLabel(*binding) + ", or Escape to keep them.").c_str());
                Render();
            }
            return;
        case clear:
        case reset:
            if (const auto* binding = Selected()) {
                BMessage request(kExtensionSetShortcut);
                request.AddString("extension_identifier", binding->extension.c_str());
                request.AddString("command", binding->command.c_str());
                if (message->what == clear) request.AddString("shortcut", "");
                Send(request);
                fStatus->SetText("Saving…");
            }
            return;
    }
    BWindow::MessageReceived(message);
}
}
#endif
