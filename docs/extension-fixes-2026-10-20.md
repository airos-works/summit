# Extension notifications, shortcuts, removal and updates (20 October 2026)

Issues #102 (clicking an extension's notification), #101 (extension keyboard
shortcuts) and #104 (removal deletes the data; extensions update themselves).
Everything was tried on the X399 with a test copy of the browser on the test
engine `ext2` (CI's tree plus the stage files listed at the end), in a
profile of its own (`/boot/home/summit/claude-s36/pext1`), with a test
extension that reports what it hears to a small server on the build host.

## #102 Clicking an extension's notification

**What the user sees.** Clicking a notification that an extension showed
(`notifications.create`) now does what the extension promises: the extension
hears `notifications.onClicked` and opens its page, a mail, a login. Haiku's
notifications have no buttons of their own, so a click on one that has
buttons opens a small window named for the extension, with the notification's
title, its buttons (two at most, as in Chrome) and **Open**. A button gives
the extension `onButtonClicked` with that button's index; Open is a plain
click. An extension whose background page had gone to sleep (an event page)
is woken up for the click.

**How it works.** A `BNotification` can only start an application with
arguments when it is clicked. The engine's notifications
(`WebExtensionAPINotificationsHaiku.cpp`) therefore set the on-click file to
the browser that started the web process and the arguments
`--summit-extension-notification-click EXTENSION NOTIFICATION TITLE [BUTTON1 [BUTTON2]]`.
The notification server starts that executable; Summit is single-launch, so
the running Summit gets the arguments as `B_ARGV_RECEIVED` (`src/main.cpp`)
and calls `BWebKitContext::ExtensionNotificationClicked(extension,
notification, button)`. The registry (`ExtensionPackageRegistryHaiku::
notificationClicked`) checks the extension is loaded and has the
`notifications` permission, wakes its background content and sends
`DispatchNotificationsEventHaiku` to its processes, which fire the event.
Web notifications (#10) have taken the same route since October.

**On air/OS since 29 October** the notification server answers the sender
(its `_reply:target`, `_reply:activated` and `_reply:closed` fields,
docs/airos/NOTIFICATIONS.md in the OS tree). The engine asks for those
answers: the buttons are the notification's own (`_buttons`, two at most), a
click on the notification or a button comes back to the web process
(`NotificationRepliesHaiku`, on its application's looper) as `onClicked` or
`onButtonClicked` without starting Summit or its window, and
`onClosed(id, byUser)` fires when the notification goes away, `byUser` true
when the user clicked or closed it, false when it timed out. The launch
arguments above stay in the notification for servers without the answers.
`SUMMIT_NOTIFICATION_REPLIES=0` leaves the answers out.

Two-argument events (`onButtonClicked`, `onClosed`) used to hand the
notification's identifier over in quotes (`"\"buttons-installed\""`):
WebKit's two-argument `invokeListenersWithJSONArgument()` takes its first
argument as a plain string, not JSON. They now go through
`invokeListenersWithJSONArguments()`.

Not done: `onClosed` with `byUser` (Haiku does not say when the user closes a
notification). As for web notifications, any program that starts Summit with
these arguments can "click" a notification of an extension.

**Checked.** The test extension shows a plain notification and one with two
buttons at install and at every start.

- A real click on the plain notification on the X399's screen (VNC pointer,
  1.2 s after it appeared): the extension heard `onClicked("plain-startup")`
  and opened `https://example.com/?notification-clicked=plain-startup`.
- Starting the test copy's executable with the arguments, as the notification
  server does: `onClicked` and a new tab, as above. With the two button
  titles the chooser window appeared ("Summit test: buttons", First button,
  Second button, Open); a real click on **Second button** woke the sleeping
  event page and gave `onButtonClicked` with index 1, and the extension
  opened its tab. That run showed the quoting bug above (engine `ext1`).
  With `ext2`, a real click on the buttons notification on the screen opened
  the chooser ("Summit Test Extension", "Summit test: buttons"), and **First
  button** gave `onButtonClicked("buttons-startup", 0)` and its tab.

## #101 Extension keyboard shortcuts

**What the user sees.** The keyboard shortcuts extensions declare in their
manifest (`commands`) work in every browser window: Dark Reader's on/off
toggle, 1Password's "open", a tab manager's keys. The keys that open an
extension's toolbar popup (`_execute_action`, `_execute_browser_action`) do
what a click on its toolbar button does. Extensions name keys as Chrome does
on a PC; Summit takes their **Ctrl** as its own shortcut key, Command (New Tab
is Command-T, Chrome's Ctrl-T), and their **Alt** as the other one, Control.
In Haiku's keyboard mode that is Alt for "Ctrl" and Ctrl for "Alt" (Dark
Reader's Alt+Shift+D is Shift+Ctrl+D); in the Windows/Linux mode the keys are
the ones the extension names. Where the manifest gives a `linux` key, Summit
takes it over the `default` one.

**Window › Extensions… › Keyboard Shortcuts…** lists every command of the
running extensions with its keys, as this keyboard names them. **Change…**
waits for the new keys, **Clear** leaves the command without keys, **Reset to
Default** gives it the extension's own again. Summit's own shortcuts come
first: keys that Summit uses (its menus and Command-1…9) do nothing for an
extension, the window says which command of Summit's has them, and Change…
refuses them, as it refuses keys another extension's command has. When two
extensions ask for the same keys, the one installed first keeps them; the
list notes the other. The keys are kept with the extension's state, the same
place `commands.update()` keeps them, so changes from either survive a
restart, and the extension hears `commands.onChanged`.

**How it works.**

- Engine (`WebKitContext.h`): `GetExtensionCommands()` answers
  `B_WEBKIT_EXTENSION_COMMANDS` with each loaded extension's commands, their
  keys now and the manifest's. `PerformExtensionCommand(extension, command,
  window)` runs one for the focused window's active tab with a user gesture,
  through `WebExtensionContext::performCommand` (`commands.onCommand` with
  the tab, activeTab granted; action commands run the action as the toolbar
  button does). `SetExtensionCommandShortcut()` goes through the same code as
  `commands.update()`/`commands.reset()` (`WebExtensionContext::
  setCommandShortcutHaiku`), so the state's `CommandOverrides` keep it. A
  changed shortcut sends `B_WEBKIT_EXTENSION_ACTIONS_CHANGED` to the action
  listener (`fireCommandChangedEventIfNeeded`). `WebExtension.cpp` reads the
  `linux` suggested key on Haiku.
- Summit: the application asks for the commands whenever the extensions
  change (`B_WEBKIT_EXTENSION_ACTIONS_CHANGED`), orders them by installation
  and gives them to `ExtensionShortcuts` (`src/ui/ExtensionShortcuts.cpp`),
  which every window reads. The keys are parsed and conflicts resolved in
  `src/core/ExtensionShortcut.cpp` (host tests in
  `tests/ExtensionShortcutTests.cpp`). `BrowserWindow::DispatchMessage` looks
  up every key press before the page or the menus get it; a match whose keys
  are one of the window's own shortcuts (`HasShortcut`) goes on as before,
  any other runs the command (held keys run it once). The first full browser
  window records Summit's menu shortcuts for the conflict notes
  (`CollectMenuShortcuts`). The editor is
  `src/ui/ExtensionShortcutsWindow.cpp`.

Not done: global shortcuts (outside Summit's windows) and media keys, which
Haiku's keyboards do not deliver as key presses; there is no Preferences tab
for extensions, so the editor and Check for Updates are in the Extensions
window. A key pressed in the very moment a window becomes active can reach
the engine before it knows the window has the focus, and is then ignored.

**Checked** (key presses through the window's own message dispatch, from the
MCP server's `press_key`).

- The test extension's command (`Alt+Shift+L`, its `linux` key, so
  Shift+Ctrl+L): `commands.onCommand("test-command", tab)` with the tab's
  id, address and title, and `tabs.executeScript` into that tab worked with
  only `activeTab`, so the press counted as a user gesture.
- Its `_execute_browser_action` (Ctrl+Shift+Y, so Shift+Alt+Y) opened its
  popup.
- Changed in the editor to Shift+Alt+K: the extension heard
  `commands.onChanged` (Ctrl+Shift+Y to Ctrl+Shift+K), the new keys opened
  the popup, the old ones did nothing, and after quitting and starting again
  the new keys still opened it (`"CommandOverrides":{"_execute_browser_action":
  {"shortcut":"Ctrl+Shift+K"}}` in its `State.json`). Alt+T in the editor was
  refused: "Summit uses Alt+T for New Tab."
- Dark Reader 4.9.133: Shift+Ctrl+D turned it off on the page (light) and on
  again (dark), twice. (A page whose content script came from the version
  before an update does not change until it is loaded again, as in Chrome.)
- 1Password 8.12.41.1 from Firefox Add-ons, not set up: Alt+Period did what
  its toolbar button does in that state (it shows its welcome page; there is
  no popup until it is set up). Its signed-in popup was not tried: the test
  profile has no 1Password account.

## #104 Removal deletes the data; extensions update themselves

**What the user sees.**

- **Remove** in the Extensions window asks first, naming the extension:
  "Remove "Dark Reader"? Its settings and the data it saved in Summit are
  deleted. To keep them, disable it instead." Removing deletes its package,
  its unpacked copy, its storage (`storage.local` and `storage.sync`, saved
  permissions and settings, registered scripts, rules), the website data of
  its own origin (local storage, IndexedDB, service workers, caches, cookies)
  and its toolbar setting. Reinstalling it starts from scratch. **Disable**
  keeps everything, as before.
- Extensions from the **Chrome Web Store** or **Firefox Add-ons** update
  themselves: Summit looks for new versions a minute after it starts, then
  once a day, and when asked (**Check for Updates** in the Extensions
  window). A new version is downloaded and checked as an installation from
  the store is, installed in place of the running one and keeps the
  extension's settings and data; the extension hears `runtime.onInstalled`
  with `reason: "update"` and `previousVersion`. A version that asks for
  more access waits: the extension's page in the window lists the new
  access, and **Update** approves and installs it. The page shows the
  version and "updated on …", where the extension comes from, and an
  **Update automatically** switch.

**How it works.**

- Removal (`ExtensionController::FinishRemovalOrDisable`): the runtime is
  unloaded and its catalog record forgotten first, then
  `BWebKitContext::RemoveExtensionData(extension, package)` erases what the
  engine keeps: the website data store's records for the extension's last
  origin (read from its `State.json`; `webExtensionWebsiteDataTypes()`,
  cookies by host), then the extension's storage directory
  (`WebKit/WebExtensions/<identity>`) and the package cache entries made from
  its archive. The catalog then deletes the package (`RemovePackage`). Since
  the state with the origin is gone, a reinstallation gets a new origin.
- Where updates come from (`src/core/ExtensionUpdate.cpp`): a package signed
  with its Chrome Web Store identity whose manifest names Google's update
  service (the store adds that `update_url`), or one whose identity is its
  Firefox add-on ID. The catalog keeps that (`source`, `update_url`), the
  automatic-update switch, when it was updated and checked, and the required
  permissions and sites the user approved; older catalogs get them at the
  next start. Packages from a folder or ZIP, or for another update server,
  are not updated.
- `ExtensionUpdater` (`src/ui/ExtensionUpdater.cpp`): a worker asks Google's
  update service (`response=updatecheck` for the installed version) or
  Mozilla's add-on API (`current_version`), downloads a newer version with
  `DownloadStoreExtension` (CRX3 signer, or Mozilla's size, SHA-256 and
  add-on ID) and stages it. Back on the application thread it prepares the
  package, checks its identity and version, compares its required access with
  the approved access, and hands it to `ExtensionController::ApplyUpdate`:
  unload the running version, load the new one with the new load purpose
  `BWebKitExtensionLoadOptions::Purpose::ExtensionUpdate`, replace the
  catalog record (`ExtensionCatalog::Replace`) and delete the old package.
  If the new version does not start, the old one is started again.
- `ExtensionUpdate` loads (`ExtensionPackageRegistryHaiku::load`) restore the
  replaced version's state although its fingerprint differs: the saved grants
  stay (`restoreNativePermissionState()` keeps those the new version still
  declares), the base URL stays (so its website data does), and the approved
  permissions and sites are granted on top. The state's version differs, so
  the engine's install decision is "update" with `previousVersion`.
- For tests, `SUMMIT_EXTENSION_TEST_UPDATE=IDENTITY=/path/to/package` makes
  the updater take that local package as the store's new version of that
  extension.

**Checked.**

- Removal: after Remove (the confirmation clicked over VNC) nothing of the
  test extension was left in the profile: package, package cache entry,
  `WebKit/WebExtensions/summit-test@example.test`,
  `WebKit/Data/LocalStorage/webkit-extension_a69beb26…` and
  `WebKit/Data/IndexedDB/v1/webkit-extension_a69beb26…` all gone, the catalog
  empty (a file list of the profile before and after). Installed again, it
  heard `onInstalled` with `reason: "install"` and found `storage.local`,
  local storage and IndexedDB empty; it got a new origin. Disable kept all
  of its files.
- Firefox Add-ons: Dark Reader 4.9.131 installed from its saved XPI
  (`addons.mozilla.org/…/darkreader-4.9.131.xpi`) was updated to 4.9.133 by
  the first daily check, a minute after it was installed: new package, old
  package deleted, same origin, its grants (`alarms`, `contextMenus`,
  `storage`, `tabs`, `<all_urls>`) kept.
- Chrome Web Store: Dark Reader 4.9.133 installed from its store page was
  "up to date" by Google's update service; with its catalog version set back
  to 4.9.100, the launch check downloaded 4.9.133 from Google, checked the
  CRX3 signature and installed it in place.
- The test extension 1.0 → 1.1 (`SUMMIT_EXTENSION_TEST_UPDATE`): `onInstalled`
  with `{"reason":"update","previousVersion":"1.0"}`, and `storage.local`,
  local storage and IndexedDB still held what 1.0 wrote. 1.1 → 1.2, which
  adds the `tabs` permission, waited ("Version 1.2 is ready. It asks for more
  access: tabs"); Update (clicked over VNC) installed it, `onInstalled` said
  `previousVersion: "1.1"`, and `tabs` was granted.
- Host tests: `summit_extension_catalog` (115 checks, with the update
  records) and `summit_extension_shortcut` (34 checks).

## Engine stage files

`ExtensionPackageRegistryHaiku.{h,cpp}`, `WebKitContext.{h,cpp}`,
`WebExtensionContext.h`, `WebExtensionContextCommandsHaiku.cpp`,
`WebExtension.cpp`, `WebExtensionAPINotificationsHaiku.{h,cpp}`,
`WebExtensionContextProxy.h` and `WebExtensionContextProxy.messages.in`.
