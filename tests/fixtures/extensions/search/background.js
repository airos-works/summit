// Lists the search engines, searches in a new tab with the default engine
// and with Bing in that tab, then checks the errors. One console line per
// step: "[search] ...".
const log = message => console.log(`[search] ${message}`);
(async () => {
    try {
        const engines = await chrome.search.get();
        log(`get ${engines.map(engine => `${engine.name}${engine.isDefault ? '*' : ''}`).join(',')}`);
        await chrome.search.query({ text: 'summit fixture', disposition: 'NEW_TAB' });
        await new Promise(resolve => setTimeout(resolve, 3000));
        const [tab] = await chrome.tabs.query({ active: true, lastFocusedWindow: true });
        log(`query NEW_TAB -> ${tab && tab.url}`);
        await chrome.search.search({ query: 'haiku os', engine: 'Bing', tabId: tab.id });
        await new Promise(resolve => setTimeout(resolve, 3000));
        log(`search Bing in tab -> ${(await chrome.tabs.get(tab.id)).url}`);
        await chrome.search.search({ query: 'x', engine: 'Nope' }).catch(error => log(`unknown engine rejected: ${error.message}`));
        await chrome.search.query({ text: 'x', disposition: 'SIDEWAYS' }).catch(error => log(`bad disposition rejected: ${error.message}`));
        await chrome.search.query({ text: 'x', tabId: 999999 }).catch(error => log(`missing tab rejected: ${error.message}`));
    } catch (error) {
        log(`error ${error.message}`);
    }
})();
