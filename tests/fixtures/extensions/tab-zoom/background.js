// When a tab finishes loading https://example.com/?zoom, zoom it to 150%,
// read the zoom back, restore the default and read it again. One console line
// per step: "[tab-zoom] ...".
const log = message => console.log(`[tab-zoom] ${message}`);
chrome.tabs.onUpdated.addListener(async (tabId, change, tab) => {
    if (change.status !== 'complete' || !tab.url || !tab.url.startsWith('https://example.com/?zoom'))
        return;
    try {
        log(`start ${await chrome.tabs.getZoom(tabId)}`);
        await chrome.tabs.setZoom(tabId, 1.5);
        log(`after setZoom(1.5) ${await chrome.tabs.getZoom(tabId)}`);
        await chrome.tabs.setZoom(tabId, 0);
        log(`after setZoom(0) ${await chrome.tabs.getZoom(tabId)}`);
        await chrome.tabs.setZoom(tabId, 9).catch(error => log(`setZoom(9) rejected: ${error.message}`));
    } catch (error) {
        log(`error ${error.message}`);
    }
});
