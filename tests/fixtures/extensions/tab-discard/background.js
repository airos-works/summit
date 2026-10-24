// Opens three background tabs, unloads them with tabs.discard, lists the
// unloaded ones, unloads the least recently used tab, selects one to load it
// again and checks the refusals. Waits 20 s before the discards so memory can
// be read first. One console line per step: "[discard] ...".
const log = message => console.log(`[discard] ${message}`);
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const step = async (name, run) => {
    try {
        log(`${name}: ${await run()}`);
    } catch (error) {
        log(`${name} failed: ${error.message}`);
    }
};
(async () => {
    const urls = ['https://en.wikipedia.org/wiki/Haiku_(operating_system)', 'https://www.haiku-os.org/', 'https://example.org/'];
    let tabs = [];
    await step('open', async () => {
        for (const url of urls)
            tabs.push(await chrome.tabs.create({ url, active: false }));
        await wait(20000);
        return tabs.map(tab => `${tab.id}:${tab.discarded}`).join(' ');
    });
    await step('discard', async () => {
        const replaced = [];
        for (const tab of tabs.slice(0, 2)) {
            const after = await chrome.tabs.discard(tab.id);
            replaced.push(`${tab.id}->${after.id} discarded=${after.discarded} url=${after.url}`);
        }
        tabs = (await chrome.tabs.query({})).filter(tab => urls.includes(tab.url) || tab.url.startsWith('https://en.wikipedia'));
        return replaced.join('; ');
    });
    await step('query discarded', async () => (await chrome.tabs.query({ discarded: true })).map(tab => tab.title).join(' | '));
    await step('discard least recently used', async () => {
        const after = await chrome.tabs.discard();
        return `${after.url} discarded=${after.discarded}`;
    });
    await wait(10000);
    await step('activate', async () => {
        const [first] = await chrome.tabs.query({ discarded: true });
        await chrome.tabs.update(first.id, { active: true });
        await wait(5000);
        const now = await chrome.tabs.get(first.id);
        return `${now.url} discarded=${now.discarded} status=${now.status}`;
    });
    await step('discard shown tab (must fail)', async () => {
        const [shown] = await chrome.tabs.query({ active: true, lastFocusedWindow: true });
        return chrome.tabs.discard(shown.id);
    });
    await step('discard missing tab (must fail)', () => chrome.tabs.discard(999999));
})();
