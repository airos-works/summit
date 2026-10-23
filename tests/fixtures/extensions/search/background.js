// Lists the search engines, searches in a new tab with the default engine
// and with Bing in that tab, then checks the errors. One console line per
// step: "[search] ...".
const log = message => console.log(`[search] ${message}`);
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const step = async (name, run) => {
    try {
        log(`${name}: ${await run()}`);
    } catch (error) {
        log(`${name} failed: ${error.message}`);
    }
};
(async () => {
    let tab;
    await step('get', async () => (await chrome.search.get()).map(engine => `${engine.name}${engine.isDefault ? '*' : ''}`).join(','));
    await step('query NEW_TAB', async () => {
        await chrome.search.query({ text: 'summit fixture', disposition: 'NEW_TAB' });
        await wait(3000);
        [tab] = await chrome.tabs.query({ active: true, lastFocusedWindow: true });
        return tab && tab.url;
    });
    await step('search Bing in tab', async () => {
        await chrome.search.search({ query: 'haiku os', engine: 'Bing', tabId: tab.id });
        await wait(3000);
        return (await chrome.tabs.get(tab.id)).url;
    });
    await step('unknown engine (must fail)', () => chrome.search.search({ query: 'x', engine: 'Nope' }));
    await step('bad disposition (must fail)', () => chrome.search.query({ text: 'x', disposition: 'SIDEWAYS' }));
    await step('missing tab (must fail)', () => chrome.search.query({ text: 'x', tabId: 999999 }));
    await step('empty text (must fail)', () => chrome.search.query({ text: '  ' }));
})();
