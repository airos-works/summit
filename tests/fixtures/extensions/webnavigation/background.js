// Every webNavigation event on fixture pages, kept in storage.local "log"
// and shown by log.html.
const log = [];
function record(name, details) {
    if (!/\/(page|target|frame)\.html/.test(details.url))
        return;
    const { url, tabId, frameId, parentFrameId, sourceTabId, sourceFrameId } = details;
    const entry = { name, url: url.replace(/^.*\//, ''), tabId, frameId, parentFrameId, sourceTabId, sourceFrameId };
    for (const key of Object.keys(entry)) {
        if (entry[key] === undefined)
            delete entry[key];
    }
    log.push(entry);
    console.log(`${name} ${JSON.stringify(entry)}`);
    chrome.storage.local.set({ log });
}
for (const name of ['onBeforeNavigate', 'onCommitted', 'onDOMContentLoaded', 'onCompleted', 'onHistoryStateUpdated', 'onReferenceFragmentUpdated', 'onCreatedNavigationTarget'])
    chrome.webNavigation[name].addListener(details => record(name, details));
chrome.storage.local.set({ log });
