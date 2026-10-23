// Opens and closes a tab, finds it among the recently closed, restores it,
// then sets, reads and removes tab and window values. One console line per
// step: "[sessions] ...".
const log = message => console.log(`[sessions] ${message}`);
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
let changes = 0;
chrome.sessions.onChanged.addListener(() => ++changes);
(async () => {
    try {
        log(`MAX_SESSION_RESULTS=${chrome.sessions.MAX_SESSION_RESULTS} devices=${(await chrome.sessions.getDevices()).length}`);
        const tab = await chrome.tabs.create({ url: 'https://example.com/?sessions', active: false });
        await wait(3000);
        await chrome.sessions.setTabValue(tab.id, 'fixture', { n: 1 });
        log(`tab value ${JSON.stringify(await chrome.sessions.getTabValue(tab.id, 'fixture'))}`);
        await chrome.sessions.removeTabValue(tab.id, 'fixture');
        log(`tab value removed ${await chrome.sessions.getTabValue(tab.id, 'fixture')}`);
        await chrome.tabs.remove(tab.id);
        await wait(1000);
        const closed = await chrome.sessions.getRecentlyClosed({ maxResults: 5 });
        const first = closed[0];
        log(`getRecentlyClosed ${closed.length} first=${first && first.tab ? first.tab.url : JSON.stringify(first)} changes=${changes}`);
        const restored = await chrome.sessions.restore(first.tab.sessionId);
        await wait(2000);
        log(`restore ${restored.tab && restored.tab.url}; open=${(await chrome.tabs.query({ url: 'https://example.com/*' })).length}`);
        const window = await chrome.windows.getCurrent();
        await chrome.sessions.setWindowValue(window.id, 'fixture', 'w');
        log(`window value ${await chrome.sessions.getWindowValue(window.id, 'fixture')}`);
        await chrome.sessions.restore('999999').catch(error => log(`missing rejected: ${error.message}`));
        await chrome.sessions.getTabValue(999999, 'x').catch(error => log(`missing tab rejected: ${error.message}`));
        log(`changes=${changes}`);
    } catch (error) {
        log(`error ${error.message}`);
    }
})();
