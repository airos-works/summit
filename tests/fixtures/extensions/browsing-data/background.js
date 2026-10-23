// Sets cookies on example.com and example.org, removes example.com's by
// origin and checks which are left; then a timed removal, history and the
// errors. One console line per step: "[browsingData] ...".
const log = message => console.log(`[browsingData] ${message}`);
const step = async (name, run) => {
    try {
        log(`${name}: ${await run()}`);
    } catch (error) {
        log(`${name} failed: ${error.message}`);
    }
};
const cookie = async url => (await chrome.cookies.get({ url, name: 'bdfixture' })) ? 'kept' : 'gone';
const both = async () => `com=${await cookie('https://example.com/')} org=${await cookie('https://example.org/')}`;
const set = url => chrome.cookies.set({ url, name: 'bdfixture', value: '1', expirationDate: Date.now() / 1000 + 3600 });
(async () => {
    await step('settings', async () => {
        const settings = await chrome.browsingData.settings();
        return `since=${settings.options.since} passwords=${settings.dataRemovalPermitted.passwords} cookies=${settings.dataRemovalPermitted.cookies}`;
    });
    await step('before', async () => { await set('https://example.com/'); await set('https://example.org/'); return both(); });
    await step('origins', async () => { await chrome.browsingData.removeCookies({ origins: ['https://example.com'] }); return both(); });
    await step('excludeOrigins', async () => {
        await set('https://example.com/');
        await chrome.browsingData.remove({ excludeOrigins: ['https://example.com'] }, { cookies: true });
        return both();
    });
    await step('hostnames', async () => {
        await set('https://example.org/');
        await chrome.browsingData.remove({ hostnames: ['example.org'] }, { cookies: true });
        return both();
    });
    await step('since', async () => {
        await chrome.browsingData.remove({ since: Date.now() - 60000 }, { cookies: true, cache: true, localStorage: true });
        return both();
    });
    await step('removeHistory', async () => { await chrome.browsingData.removeHistory({ since: Date.now() - 60000 }); return 'done'; });
    await step('passwords (must fail)', () => chrome.browsingData.removePasswords({}));
    await step('history origins (must fail)', () => chrome.browsingData.removeHistory({ origins: ['https://example.com'] }));
    await step('both filters (must fail)', () => chrome.browsingData.remove({ origins: ['https://a.example'], excludeOrigins: ['https://b.example'] }, { cookies: true }));
    await step('unknown type (must fail)', () => chrome.browsingData.remove({}, { nonsense: true }));
})();
