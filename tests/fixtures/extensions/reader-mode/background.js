// Opens an article, switches Reader view on and off and checks the refusal
// for a background tab and an unloaded tab's status (Summit's article test
// accepts even example.com, so there is no page without an article here). One console line per step: "[reader] ...".
const log = message => console.log(`[reader] ${message}`);
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const step = async (name, run) => {
    try {
        log(`${name}: ${await run()}`);
    } catch (error) {
        log(`${name} failed: ${error.message}`);
    }
};
chrome.tabs.onUpdated.addListener((id, change) => {
    if ('isArticle' in change || 'isInReaderMode' in change)
        log(`onUpdated ${id} ${JSON.stringify(change)}`);
});
(async () => {
    let article, other;
    await step('open', async () => {
        other = await chrome.tabs.create({ url: 'https://example.org/', active: false });
        article = await chrome.tabs.create({ url: 'https://en.wikipedia.org/wiki/BeOS', active: true });
        await wait(8000);
        const now = await chrome.tabs.get(article.id);
        return `isArticle=${now.isArticle} isInReaderMode=${now.isInReaderMode}`;
    });
    await step('toggle on', async () => {
        await chrome.tabs.toggleReaderMode(article.id);
        await wait(4000);
        return `isInReaderMode=${(await chrome.tabs.get(article.id)).isInReaderMode}`;
    });
    await step('toggle off', async () => {
        await chrome.tabs.toggleReaderMode(article.id);
        await wait(4000);
        return `isInReaderMode=${(await chrome.tabs.get(article.id)).isInReaderMode}`;
    });
    await step('background tab (must fail)', () => chrome.tabs.toggleReaderMode(other.id));
    await step('unloaded status', async () => (await chrome.tabs.discard(other.id)).status);
})();
