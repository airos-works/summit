// Chrome's sidePanel on Summit's sidebar (#106). The MCP evaluate tool calls
// these; panel.html reports itself with a runtime message.
globalThis.shown = [];
browser.runtime.onMessage.addListener((message) => { shown.push(message); });
const sp = chrome.sidePanel;
const call = (promise) => promise.then((value) => value === undefined ? "ok" : value, (error) => "error: " + error.message);
globalThis.checks = {
    present: () => ({ sidePanel: typeof chrome.sidePanel, sidebarAction: typeof browser.sidebarAction, methods: Object.keys(Object.getPrototypeOf(sp) || {}).length }),
    open: (options = {}) => call(sp.open(options)),
    close: (options = {}) => call(sp.close(options)),
    isOpen: () => call(sp.isOpen({})),
    options: (options = {}) => call(sp.getOptions(options)),
    set: (options) => call(sp.setOptions(options)),
    behavior: () => call(sp.getPanelBehavior()),
    setBehavior: (opens) => call(sp.setPanelBehavior({ openPanelOnActionClick: opens })),
    badBehavior: () => call(sp.setPanelBehavior({ openPanelOnActionClick: "yes" })),
    outside: () => call(sp.setOptions({ path: "https://example.com/" })),
    tabs: async () => (await browser.tabs.query({})).map((tab) => tab.id),
};
globalThis.clicked = 0;
browser.browserAction.onClicked.addListener(() => { ++clicked; });
