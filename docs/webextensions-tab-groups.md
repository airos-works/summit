# Tab groups (#106)

Summit has tab groups, as Chrome and Firefox (138+) do, and extensions see
them through `chrome.tabGroups` (permission `tabGroups`) and the tab group
parts of `chrome.tabs`. The browser owns the groups; the engine asks it for
changes and hears of every change, so the strip and the API stay in step both
ways.

## What the user sees

- A group has a title (it may be empty), one of Chrome's nine colours (grey,
  blue, red, yellow, green, pink, purple, cyan, orange) and a collapsed state.
  Its tabs are always adjacent; pinned tabs are never in a group (pinning a
  grouped tab takes it out; grouping a pinned tab unpins it, as in Chrome).
- The strip shows a label before the group's tabs: its title on its colour,
  or a dot of the colour when it has no title, and a line of the colour under
  its tabs. A click on the label collapses or expands the group; a collapsed
  group shows only its label, and its selected tab if it has it (as in
  Firefox). The label's menu: Name Group… (a small window asks for the name),
  Color (the nine colours, the current one marked), Collapse/Expand Group,
  New Tab in Group, Ungroup, Close Group.
- The tab menu has Add Tab to Group (New Group, then the window's other
  groups with their colours) and, for a grouped tab, Remove from Group (the
  tab goes to the place after its group). Add Tab to Group is off for pinned
  tabs. A new group takes the first colour no group of the window has.
- Dragging a tab between two tabs of a group puts it in the group; next to its
  own group it stays in it; held over its group's label (before the group's
  first tab) or past the group's last tab, it leaves the group.
- A link opened in a tab next to the opener (middle click, `target=_blank`)
  from a grouped tab joins the group, and so does any tab put between two
  tabs of a group.
- List All Tabs lists a group's tabs under the group's name and colour
  (indented, "(collapsed)" when it is); choosing the group's line collapses or
  expands it, choosing a tab selects it.
- Both looks (Haiku and Safari-like) draw the labels and lines; the scrolled
  strip (#135) scrolls labels with the tabs, and the arrows step from tab or
  label to the next.
- Groups are saved with the session (`groups` and `tabGroups` in each window
  of `profile.json`) and come back with new identifiers.

## The API

- `tabs.group({tabIds, groupId?, createProperties?: {windowId}})` answers the
  group's id. With `groupId` the tabs join that group (after its last tab);
  without, a new group is made in `createProperties.windowId`, else in the
  first tab's window (Chrome's default is the current window). Tabs of other
  windows are moved there first, as `tabs.move` does.
- `tabs.ungroup(tabIds)`: each tab leaves its group, to the place after it.
- `Tab.groupId` (-1, `TAB_GROUP_ID_NONE`, for none); `tabs.query({groupId})`
  (-1 finds the tabs in no group); `tabs.onUpdated` with `changeInfo.groupId`
  when a tab joins or leaves a group (not in other changeInfos).
- `tabGroups.get(id)`, `tabGroups.query({collapsed, color, title (a pattern,
  as tabs.query's), windowId (-2: the last focused window), shared})`,
  `tabGroups.update(id, {title, color, collapsed})` (answers the group; an
  unknown colour is refused), `tabGroups.move(id, {index, windowId?})`
  (answers the group; -1 is the end; never into another group or among the
  pinned tabs; to another window the tabs move one by one and keep the group).
  A `TabGroup` is `{id, collapsed, color, title, windowId, shared: false}`.
- Events: `tabGroups.onCreated`, `onUpdated` (title, colour or collapsed
  changed), `onMoved` (its first tab moved while the window kept as many tabs)
  and `onRemoved` (its last tab left or closed, or its window closed). As
  Chrome documents, a group that moves to another window is removed from one
  and created in the other.
- Unknown groups get Chrome's "No group with id: N.", unknown tabs "No tab with
  id: N.". Groups of private windows are seen only by extensions allowed there.

## How it works

- App (`src/ui/BrowserWindowFeatures.cpp`, `Chrome.cpp`): each window keeps
  `fTabGroups` (id, title, colour, collapsed) and each tab its `group`.
  Identifiers come from one counter for all windows. `NormalizeTabGroups()`
  keeps the rules (pinned tabs in none, a group's longest run of tabs keeps
  it, a tab between two of a group joins it, empty groups go) and runs before
  every `SyncBrowserWindow()` and `RefreshChrome()`. `SyncBrowserWindow()`
  reports the groups after the tabs with
  `BWebKitContext::SetBrowserWindowTabGroups`. `TabStrip` gets
  `TabGroupLabel`s; its `Layout` now places every unpinned tab and label in
  pixels (`tabLeft`, `tabWidth`, `chips`), and the scroll offset is in pixels.
  A drag sends `kMoveTab` with the tab's `group` afterwards.
- Public API (`WebKitContext.h`): `BWebKitTabGroup` and
  `SetBrowserWindowTabGroups(window, groups)`; the browser command
  `B_WEBKIT_BROWSER_TAB_GROUPS` (methods `group`, `ungroup`, `update`, `move`;
  answered with `RespondToBrowserCommandWithData` after the change was
  reported).
- Engine: `BrowserTabRegistryHaiku` keeps each window's groups and publishes
  the groups whose tabs are still there (after the tab observer, so a closed
  tab or window ends its group); its group observer calls
  `WebExtensionContext::nativeTabGroupsDidChangeHaiku`, which fires the
  events from the difference. `WebExtensionTab::parameters()` fills the new
  `WebExtensionTabParameters::groupIdentifier` (serialized, Haiku only), and
  `ChangedProperties::GroupIdentifier` puts it in `tabs.onUpdated`.
  `WebExtensionContext::tabGroupsHaiku` answers reads from the registry and
  sends changes as browser commands.
- IPC: no new messages (a new one changes `MessageNames.h`, which is in the
  WebKit precompiled header and rebuilds the whole engine). Requests travel in
  `SidebarActionHaiku` as `tabGroups.get`, `.query`, `.update`, `.move`,
  `.groupTabs`, `.ungroupTabs`; the events in `DispatchHistoryEventHaiku`
  with the tabGroups listener types. Both can become their own messages when
  the engine is rebuilt anyway.

## Fixture

`tests/fixtures/extensions/tab-groups/`: install it and open its `test.html`
in a tab (`?keep` leaves the groups open). It makes three tabs and checks
grouping, `Tab.groupId`, `tabs.query({groupId})`, get/query/update/move,
ungroup, a second group, the error texts and the events; `window.results`
holds `[name, passed, detail]` and `window.done` turns true at the end. The
service worker logs every event as `[groups] …` (`globalThis.events`).

## Verified

On the Framework laptop (30 October 2026, engine and app built from one
tree, stage `groups106`):

- The fixture: 23/23 checks passed (grouping, `Tab.groupId`, `tabs.query`
  with a group and with -1, get/query by title, colour and window,
  update/collapse/expand, move to the start and to the end, ungroup, a second
  group in another colour, both error texts) and its service worker logged
  onCreated, onUpdated, onMoved, onRemoved and `tabs.onUpdated {groupId}`.
- A real extension, "Group Tabs By Domain" 0.0.1.9 (Chrome Web Store
  `icmbnkmjhdapliaologdghjfpcmcimln`, uses tabs.group/ungroup/move,
  tabGroups.query/get/update/move and the three tabGroups events), unpacked
  into the profile: it grouped tabs by site by itself ("example" red,
  "wikipedia" orange, "iana" blue, moving same-site tabs together), and when
  the strip's label collapsed its group it renamed it "wikipedia (2)" from
  its `tabGroups.onUpdated` listener, so changes made in the strip reach it.
- With the real pointer (an input_server test device): a click on a label
  collapsed the group; the label's menu and its Color submenu chose purple
  (`tabGroups.query` then said purple); the tab menu's Add Tab to Group ›
  New Group made a grey dot group; dragging a tab between two tabs of a
  group put it in, dragging a group's last tab past its end and a group's
  only tab over its own label took them out; List All Tabs listed the
  groups (collapsed marked) with their tabs.
- 19 tabs with a pinned tab (pinning a grouped tab took it out) scrolled with
  their labels (arrows step from tab to label); the session came back after
  a restart, in the Safari-like look, with its groups (one holding an
  extension page, which is made later than the other tabs) and the collapsed
  state; the profile's `groups`/`tabGroups` were checked by a session
  round-trip test on air/OS.
- Across windows: `tabGroups.move` of a 7-tab group into another window
  (extensions heard one onRemoved, one onCreated and a `tabs.onUpdated` per
  tab), and `tabs.group` of a tab of another window into a group (the tab
  moved there and joined).

## Not done

- `tabs.onUpdated` changes of one tab within 25 ms are reported together
  (as for every tab property): a tab that joins and leaves a group at once
  reports only where it ended up.
- `onMoved` is inferred (the group's first tab moved while the window kept
  as many tabs); moving an ungrouped tab across a group reports it too.
- A group cannot be dragged by its label, nor moved to another window from
  its menu (the API can); closed groups are not in Reopen Closed Tab or
  `sessions`; private windows' groups are not saved (nor are their tabs).
- `tabGroups.query({windowId: -2})` takes the last focused window, not the
  calling page's window.
