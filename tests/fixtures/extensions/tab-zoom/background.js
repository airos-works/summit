// When a tab finishes loading https://example.com/?zoom, zoom it to 150%,
// read the zoom back, restore the default and read it again, then duplicate
// the tab and close the copy. One console line per step: "[tab-zoom] ...".
const log = message => console.log(`[tab-zoom] ${message}`);
chrome.tabs.onZoomChange.addListener(change => log(`onZoomChange ${change.tabId} ${change.oldZoomFactor} -> ${change.newZoomFactor} ${change.zoomSettings.mode}/${change.zoomSettings.scope}`));
chrome.tabs.onUpdated.addListener(async (tabId, change, tab) => {
    if (change.status !== 'complete' || !tab.url || !tab.url.startsWith('https://example.com/?zoom') || tab.openerTabId)
        return;
    try {
        log(`start ${await chrome.tabs.getZoom(tabId)}`);
        await chrome.tabs.setZoom(tabId, 1.5);
        log(`after setZoom(1.5) ${await chrome.tabs.getZoom(tabId)}`);
        await chrome.tabs.setZoom(tabId, 0);
        log(`after setZoom(0) ${await chrome.tabs.getZoom(tabId)}`);
        const copy = await chrome.tabs.duplicate(tabId);
        log(`duplicate ${copy.url} index ${copy.index} after ${tab.index} opener ${copy.openerTabId === tabId}`);
        await chrome.tabs.remove(copy.id);
        await chrome.tabs.setZoom(tabId, 9);
    } catch (error) {
        log(`error ${error.message}`);
    }
});
