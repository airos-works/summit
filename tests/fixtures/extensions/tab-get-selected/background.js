// [selected] lines: getSelected (callback and promise forms) agrees with
// tabs.query({active, currentWindow}); a bad window id throws.
globalThis.results = {};
function log(name, value) {
    globalThis.results[name] = value;
    console.log(`[selected] ${name} ${JSON.stringify(value)}`);
}

async function check() {
    const [active] = await browser.tabs.query({active: true, currentWindow: true});
    chrome.tabs.getSelected((tab) => log("callback", {id: tab && tab.id, same: !!tab && !!active && tab.id === active.id}));
    const tab = await browser.tabs.getSelected(active.windowId);
    log("window", {id: tab && tab.id, same: !!tab && tab.id === active.id});
    try { chrome.tabs.getSelected(-5, () => {}); log("bad window", "accepted"); }
    catch (error) { log("bad window", String(error)); }
}

browser.tabs.onActivated.addListener(check);
check();
