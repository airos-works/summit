// When a tab first finishes loading https://example.com/?zoom: zoom it to
// 150%, read the zoom back, restore the default, duplicate the tab and close
// the copy, check the zoom settings and an out-of-range zoom, then reload it
// without the cache. One console line per step: "[tab-zoom] ...".
const log = message => console.log(`[tab-zoom] ${message}`);
let ran = false;
chrome.tabs.onZoomChange.addListener(change => log(`onZoomChange ${change.tabId} ${change.oldZoomFactor} -> ${change.newZoomFactor} ${change.zoomSettings.mode}/${change.zoomSettings.scope}`));
chrome.tabs.onUpdated.addListener(async (tabId, change, tab) => {
    if (ran || change.status !== 'complete' || !tab.url || !tab.url.startsWith('https://example.com/?zoom') || tab.openerTabId)
        return;
    ran = true;
    try {
        log(`start ${await chrome.tabs.getZoom(tabId)}`);
        await chrome.tabs.setZoom(tabId, 1.5);
        log(`after setZoom(1.5) ${await chrome.tabs.getZoom(tabId)}`);
        await chrome.tabs.setZoom(tabId, 0);
        log(`after setZoom(0) ${await chrome.tabs.getZoom(tabId)}`);
        const copy = await chrome.tabs.duplicate(tabId);
        log(`duplicate ${copy.url} index ${copy.index} after ${tab.index} opener ${copy.openerTabId === tabId}`);
        await chrome.tabs.remove(copy.id);
        log(`detectLanguage ${await chrome.tabs.detectLanguage(tabId)}`);
        const settings = await chrome.tabs.getZoomSettings(tabId);
        log(`getZoomSettings ${settings.mode}/${settings.scope}/${settings.defaultZoomFactor}`);
        await chrome.tabs.setZoomSettings(tabId, { mode: 'automatic', scope: 'per-origin' });
        log('setZoomSettings automatic/per-origin ok');
        await chrome.tabs.setZoomSettings(tabId, { mode: 'manual' }).catch(error => log(`setZoomSettings manual rejected: ${error.message}`));
        try {
            await chrome.tabs.setZoom(tabId, 9);
        } catch (error) {
            log(`setZoom(9) rejected: ${error.message}`);
        }
        await chrome.tabs.reload(tabId, { bypassCache: true });
        log('reload({bypassCache: true}) resolved');
    } catch (error) {
        log(`error ${error.message}`);
    }
});
