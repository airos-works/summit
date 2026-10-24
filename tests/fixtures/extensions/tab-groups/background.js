// [groups] lines. Summit has no tab groups: listeners can be added at the
// top level (Sidebery's startup), query finds none, every tab has groupId -1.
chrome.tabGroups.onCreated.addListener((group) => log("onCreated", group));
chrome.tabGroups.onUpdated.addListener((group) => log("onUpdated", group));
chrome.tabGroups.onRemoved.addListener((group) => log("onRemoved", group));
chrome.tabGroups.onMoved.addListener((group) => log("onMoved", group));

globalThis.results = {};
function log(name, value) {
    globalThis.results[name] = value;
    console.log(`[groups] ${name} ${JSON.stringify(value)}`);
}
async function settle(name, promise) {
    try { log(name, await promise); } catch (error) { log(name, {error: String(error.message || error)}); }
}

(async () => {
    log("TAB_GROUP_ID_NONE", chrome.tabGroups.TAB_GROUP_ID_NONE);
    await settle("query", chrome.tabGroups.query({}));
    await settle("get", chrome.tabGroups.get(1));
    await settle("update", chrome.tabGroups.update(1, {title: "x"}));
    const tabs = await chrome.tabs.query({});
    log("groupIds", [...new Set(tabs.map((tab) => tab.groupId))]);
    await settle("query ungrouped", chrome.tabs.query({groupId: -1}).then((found) => found.length === tabs.length));
    await settle("query group 3", chrome.tabs.query({groupId: 3}));   // []
    await settle("group", chrome.tabs.group({tabIds: tabs[0].id}));
    await settle("ungroup", chrome.tabs.ungroup(tabs[0].id).then(() => "ok"));
})();
