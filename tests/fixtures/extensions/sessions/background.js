// Opens and closes a tab, finds it among the recently closed, restores it,
// then sets, reads and removes tab and window values. One console line per
// step: "[sessions] ...".
const log = message => console.log(`[sessions] ${message}`);
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const step = async (name, run) => {
    try {
        log(`${name}: ${await run()}`);
    } catch (error) {
        log(`${name} failed: ${error.message}`);
    }
};
let changes = 0;
chrome.sessions.onChanged.addListener(() => ++changes);
(async () => {
    let tab, first;
    await step('constants', async () => `MAX_SESSION_RESULTS=${chrome.sessions.MAX_SESSION_RESULTS} devices=${(await chrome.sessions.getDevices()).length}`);
    await step('tab values', async () => {
        tab = await chrome.tabs.create({ url: 'https://example.com/?sessions', active: false });
        await wait(3000);
        await chrome.sessions.setTabValue(tab.id, 'fixture', { n: 1 });
        const set = JSON.stringify(await chrome.sessions.getTabValue(tab.id, 'fixture'));
        await chrome.sessions.removeTabValue(tab.id, 'fixture');
        return `set=${set} removed=${await chrome.sessions.getTabValue(tab.id, 'fixture')}`;
    });
    await step('getRecentlyClosed', async () => {
        await chrome.tabs.remove(tab.id);
        await wait(1000);
        const closed = await chrome.sessions.getRecentlyClosed({ maxResults: 5 });
        first = closed[0];
        return `${closed.length} first=${first && first.tab ? first.tab.url : JSON.stringify(first)} changes=${changes}`;
    });
    await step('restore', async () => {
        const restored = await chrome.sessions.restore(first.tab.sessionId);
        await wait(2000);
        return `${restored.tab && restored.tab.url} open=${(await chrome.tabs.query({ url: 'https://example.com/*' })).length}`;
    });
    await step('window value', async () => {
        const window = await chrome.windows.getCurrent();
        await chrome.sessions.setWindowValue(window.id, 'fixture', 'w');
        return await chrome.sessions.getWindowValue(window.id, 'fixture');
    });
    await step('missing session (must fail)', () => chrome.sessions.restore('999999'));
    await step('missing tab (must fail)', () => chrome.sessions.getTabValue(999999, 'x'));
    await step('changes', async () => changes);
})();
