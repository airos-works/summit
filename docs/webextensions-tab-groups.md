# tabGroups without tab groups (#106)

Summit has no tab groups, so `chrome.tabGroups` (permission `tabGroups`)
answers as Chrome does for a browser without any: `query()` finds none,
`get`, `update` and `move` fail with Chrome's "No group with id: N.", and
`onCreated`, `onUpdated`, `onRemoved` and `onMoved` accept listeners but never
fire. `TAB_GROUP_ID_NONE` is -1, and every tab's `groupId` is -1.

In the tabs namespace, `tabs.query({groupId: -1})` matches every tab and
another `groupId` finds none, `tabs.ungroup()` (a tab id or an array of them)
succeeds without changing anything, and `tabs.group()` fails with "Summit has
no tab groups.".

Verified on the X399 (25 Oct, lf113 build): `query` gave `[]`, `get`,
`update` and `move` Chrome's error, every tab `groupId: -1`, `query({groupId:
-1})` every tab, `group` the refusal, and the four events took listeners.
`query({groupId: 3})` threw and `ungroup(id)` with one id was refused; both
were fixed after that build.

This lets extensions that look for the namespace start: Sidebery and Tree
Style Tab add tabGroups listeners at the top level and used to stop with a
TypeError. Real tab groups need Summit's tab strip to have them first.

## Fixture

`tests/fixtures/extensions/tab-groups/`: install it and read the `[groups]`
console lines (or `globalThis.results` in its service worker).
