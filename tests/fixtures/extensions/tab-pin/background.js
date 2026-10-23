// When a tab finishes loading https://example.com/?pin, pin it, check the
// state three ways, and unpin it. One console line per step: "[tab-pin] ...".
const log = message => console.log(`[tab-pin] ${message}`);
let running = false;
chrome.tabs.onUpdated.addListener(async (tabId, change, tab) => {
    if ('pinned' in change)
        log(`onUpdated ${tabId} pinned=${change.pinned}`);
    if (running || change.status !== 'complete' || !tab.url || !tab.url.startsWith('https://example.com/?pin'))
        return;
    running = true;
    try {
        log(`start pinned=${tab.pinned}`);
        const pinned = await chrome.tabs.update(tabId, { pinned: true });
        await new Promise(resolve => setTimeout(resolve, 500));
        log(`after update pinned=${pinned.pinned} get=${(await chrome.tabs.get(tabId)).pinned} query=${(await chrome.tabs.query({ pinned: true })).map(t => t.id).includes(tabId)}`);
        await chrome.tabs.update(tabId, { pinned: false });
        await new Promise(resolve => setTimeout(resolve, 500));
        log(`after unpin get=${(await chrome.tabs.get(tabId)).pinned}`);
    } catch (error) {
        log(`error ${error.message}`);
    }
});
