// When a tab finishes loading https://example.com/?geometry: report the
// window's frame, move and resize it, report it again, restore it, and open
// a window with a given frame.
// One console line per step: "[window-geometry] ...".
const log = message => console.log(`[window-geometry] ${message}`);
const frame = w => `${w.left},${w.top} ${w.width}x${w.height}`;
let ran = false;
chrome.windows.onBoundsChanged.addListener(w => log(`onBoundsChanged ${w.id} ${frame(w)}`));
chrome.tabs.onUpdated.addListener(async (tabId, change, tab) => {
    if (ran || change.status !== 'complete' || !tab.url || !tab.url.startsWith('https://example.com/?geometry'))
        return;
    ran = true;
    try {
        const before = await chrome.windows.get(tab.windowId);
        log(`before ${frame(before)}`);
        const moved = await chrome.windows.update(tab.windowId, { left: 120, top: 90, width: 900, height: 640 });
        log(`update returned ${frame(moved)}`);
        await new Promise(resolve => setTimeout(resolve, 500));
        log(`after ${frame(await chrome.windows.get(tab.windowId))}`);
        const resized = await chrome.windows.update(tab.windowId, { width: 1000 });
        log(`width only ${frame(resized)}`);
        await chrome.windows.update(tab.windowId, { left: before.left, top: before.top, width: before.width, height: before.height });
        log(`restored ${frame(await chrome.windows.get(tab.windowId))}`);
        const created = await chrome.windows.create({ url: 'https://example.com/?created', left: 200, top: 150, width: 700, height: 500 });
        await new Promise(resolve => setTimeout(resolve, 1000));
        log(`created ${frame(await chrome.windows.get(created.id))}`);
        await chrome.windows.remove(created.id);
        try {
            await chrome.windows.update(tab.windowId, { width: 0 });
        } catch (error) {
            log(`width 0 rejected: ${error.message}`);
        }
    } catch (error) {
        log(`error ${error.message}`);
    }
});
