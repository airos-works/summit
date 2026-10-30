// Run by opening test.html in a tab. window.results: [name, passed, detail];
// window.done becomes true at the end; window.events: what this page heard.
window.results = [];
window.events = [];
window.done = false;
const keep = location.search.includes("keep");
for (const name of ["onCreated", "onUpdated", "onRemoved", "onMoved"])
    chrome.tabGroups[name].addListener((group) => window.events.push([name, group.id, group.title, group.color, group.collapsed]));
chrome.tabs.onUpdated.addListener((tabId, info) => {
    if ("groupId" in info) window.events.push(["tabs.onUpdated", tabId, info.groupId]);
});
function check(name, passed, detail) {
    window.results.push([name, !!passed, detail]);
    const item = document.createElement("li");
    item.className = passed ? "pass" : "fail";
    item.textContent = `${passed ? "PASS" : "FAIL"} ${name}${detail === undefined ? "" : ": " + JSON.stringify(detail)}`;
    document.getElementById("results").append(item);
}
async function error(promise) {
    try { await promise; return null; } catch (e) { return String(e.message || e); }
}
const wait = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

(async () => {
    check("TAB_GROUP_ID_NONE is -1", chrome.tabGroups.TAB_GROUP_ID_NONE === -1);
    const self = await chrome.tabs.getCurrent();
    const page = chrome.runtime.getURL("blank.html");
    const tabs = [];
    for (let i = 0; i < 3; ++i) tabs.push(await chrome.tabs.create({url: `${page}?${i}`, active: false, windowId: self.windowId}));
    const [a, b, c] = tabs.map((tab) => tab.id);
    check("new tabs are in no group", tabs.every((tab) => tab.groupId === -1), tabs.map((tab) => tab.groupId));

    const group = await chrome.tabs.group({tabIds: [a, b]});
    check("tabs.group answers a group id", Number.isInteger(group) && group > 0, group);
    let info = await chrome.tabGroups.get(group);
    check("tabGroups.get", info.id === group && info.windowId === self.windowId && info.collapsed === false
        && typeof info.color === "string" && info.title === "", info);
    const [ta, tb] = await Promise.all([chrome.tabs.get(a), chrome.tabs.get(b)]);
    check("Tab.groupId", ta.groupId === group && tb.groupId === group, [ta.groupId, tb.groupId]);
    check("grouped tabs are adjacent", Math.abs(ta.index - tb.index) === 1, [ta.index, tb.index]);

    info = await chrome.tabGroups.update(group, {title: "Fixture", color: "green"});
    check("tabGroups.update title and colour", info && info.title === "Fixture" && info.color === "green", info);
    check("tabGroups.update refuses a colour", !!(await error(chrome.tabGroups.update(group, {color: "beige"}))));

    await chrome.tabs.group({tabIds: c, groupId: group});
    let members = await chrome.tabs.query({groupId: group});
    check("tabs.group into an existing group; tabs.query({groupId})", members.length === 3
        && members.map((tab) => tab.id).sort().join() === [a, b, c].sort().join(), members.map((tab) => tab.id));
    // (Another extension, such as a group-by-domain one, may group other tabs.)
    const ungroupedCount = (await chrome.tabs.query({groupId: -1, windowId: self.windowId})).length;
    const all = await chrome.tabs.query({windowId: self.windowId});
    const expected = all.filter((tab) => tab.groupId === -1).length;
    check("tabs.query({groupId: -1})", ungroupedCount === expected && ungroupedCount <= all.length - 3, [ungroupedCount, expected, all.length]);

    check("tabGroups.query({title})", (await chrome.tabGroups.query({title: "Fix*"})).some((found) => found.id === group));
    check("tabGroups.query({color})", !(await chrome.tabGroups.query({color: "red"})).some((found) => found.id === group));
    check("tabGroups.query({windowId})", (await chrome.tabGroups.query({windowId: self.windowId})).some((found) => found.id === group));

    info = await chrome.tabGroups.update(group, {collapsed: true});
    check("collapse", info.collapsed === true, info);
    info = await chrome.tabGroups.update(group, {collapsed: false});
    check("expand", info.collapsed === false, info);

    const pinned = (await chrome.tabs.query({windowId: self.windowId, pinned: true})).length;
    await chrome.tabGroups.move(group, {index: 0});
    let moved = await chrome.tabs.query({groupId: group});
    check("tabGroups.move to the start (after the pinned tabs)", Math.min(...moved.map((tab) => tab.index)) === pinned, {pinned, indexes: moved.map((tab) => tab.index)});
    await chrome.tabGroups.move(group, {index: -1});
    moved = await chrome.tabs.query({groupId: group});
    const last = (await chrome.tabs.query({windowId: self.windowId})).length - 1;
    check("tabGroups.move to the end", Math.max(...moved.map((tab) => tab.index)) === last, {indexes: moved.map((tab) => tab.index)});

    await chrome.tabs.ungroup(c);
    const tc = await chrome.tabs.get(c);
    check("tabs.ungroup", tc.groupId === -1, tc.groupId);
    const second = await chrome.tabs.group({tabIds: [c]});
    const secondInfo = await chrome.tabGroups.get(second);
    check("a second group has its own id and colour", second !== group && secondInfo.color !== "green", secondInfo);

    check("tabGroups.get of an unknown group", (await error(chrome.tabGroups.get(987654))) === "No group with id: 987654.");
    check("tabs.group of an unknown tab", /No tab with id/.test(await error(chrome.tabs.group({tabIds: [987654321]})) || ""));

    await wait(300);
    const names = window.events.map((event) => event[0]);
    check("events: onCreated, onUpdated, onMoved, tabs.onUpdated",
        names.includes("onCreated") && names.includes("onUpdated") && names.includes("onMoved") && names.includes("tabs.onUpdated"), window.events);
    if (!keep) {
        await chrome.tabs.remove([a, b, c]);
        await wait(300);
        check("events: onRemoved when the tabs closed", window.events.some((event) => event[0] === "onRemoved" && event[1] === group), window.events.filter((event) => event[0] === "onRemoved"));
    }
    document.getElementById("events").textContent = window.events.map((event) => JSON.stringify(event)).join("\n");
    window.done = true;
    console.log(`[groups] ${window.results.filter((result) => result[1]).length}/${window.results.length} passed`);
})().catch((e) => { check("unexpected error", false, String(e && e.message || e)); window.done = true; });
