// Signs in through a page that redirects to the extension's address (by
// script, then by a server redirect), checks non-interactive and closed
// windows. AUTH_PAGE is a page that navigates to its ?redirect= with ?code=42
// (docs/webextensions-identity.md). One console line per step: "[identity] ...".
const AUTH_PAGE = 'http://192.168.1.200:8796/fixtures/auth.html';
const log = message => console.log(`[identity] ${message}`);
const wait = ms => new Promise(resolve => setTimeout(resolve, ms));
const step = async (name, run) => {
    try {
        log(`${name}: ${await run()}`);
    } catch (error) {
        log(`${name} failed: ${error.message}`);
    }
};
(async () => {
    const redirect = chrome.identity.getRedirectURL('cb');
    log(`getRedirectURL ${redirect}`);
    await step('script redirect', () => chrome.identity.launchWebAuthFlow({ url: `${AUTH_PAGE}?redirect=${encodeURIComponent(redirect)}`, interactive: true }));
    await step('server redirect', () => chrome.identity.launchWebAuthFlow({
        url: `https://httpbin.org/redirect-to?url=${encodeURIComponent(redirect + '?code=7')}`, interactive: true }));
    await step('non-interactive (must fail)', () => chrome.identity.launchWebAuthFlow({ url: 'https://example.com/', interactive: false }));
    await step('closed window (must fail)', async () => {
        const flow = chrome.identity.launchWebAuthFlow({ url: 'https://example.com/?identity-close', interactive: true });
        await wait(3000);
        const tabs = await chrome.tabs.query({ url: 'https://example.com/*' });
        const tab = tabs.find(each => each.url.includes('identity-close'));
        log(`closing ${tab ? tab.id : 'nothing'}`);
        if (tab) await chrome.tabs.remove(tab.id);
        return flow;
    });
    await step('windows left', async () => (await chrome.windows.getAll()).length);
})();
