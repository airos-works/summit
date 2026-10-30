// [groups] lines: every tabGroups event, and tabs.onUpdated with a groupId,
// heard by the service worker (listeners at the top level, as Sidebery and
// Tree Style Tab add them). globalThis.events in the worker keeps them.
globalThis.events = [];
function record(name, value) {
    globalThis.events.push([name, value]);
    console.log(`[groups] ${name} ${JSON.stringify(value)}`);
}
chrome.tabGroups.onCreated.addListener((group) => record("onCreated", group));
chrome.tabGroups.onUpdated.addListener((group) => record("onUpdated", group));
chrome.tabGroups.onRemoved.addListener((group) => record("onRemoved", group));
chrome.tabGroups.onMoved.addListener((group) => record("onMoved", group));
chrome.tabs.onUpdated.addListener((tabId, changeInfo) => {
    if ("groupId" in changeInfo) record("tabs.onUpdated", {tabId, groupId: changeInfo.groupId});
});
