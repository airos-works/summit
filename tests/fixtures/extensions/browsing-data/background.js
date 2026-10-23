// Sets cookies on example.com and example.org, removes example.com's by
// origin and checks which are left; then a timed removal, history and the
// errors. One console line per step: "[browsingData] ...".
const log = message => console.log(`[browsingData] ${message}`);
const cookie = async url => (await chrome.cookies.get({ url, name: 'bdfixture' })) ? 'kept' : 'gone';
(async () => {
    try {
        const settings = await chrome.browsingData.settings();
        log(`settings since=${settings.options.since} passwords=${settings.dataRemovalPermitted.passwords} cookies=${settings.dataRemovalPermitted.cookies}`);
        const expirationDate = Date.now() / 1000 + 3600;
        await chrome.cookies.set({ url: 'https://example.com/', name: 'bdfixture', value: '1', expirationDate });
        await chrome.cookies.set({ url: 'https://example.org/', name: 'bdfixture', value: '1', expirationDate });
        log(`before com=${await cookie('https://example.com/')} org=${await cookie('https://example.org/')}`);
        await chrome.browsingData.removeCookies({ origins: ['https://example.com'] });
        log(`origins com=${await cookie('https://example.com/')} org=${await cookie('https://example.org/')}`);
        await chrome.cookies.set({ url: 'https://example.com/', name: 'bdfixture', value: '1', expirationDate });
        await chrome.browsingData.remove({ excludeOrigins: ['https://example.com'] }, { cookies: true });
        log(`excludeOrigins com=${await cookie('https://example.com/')} org=${await cookie('https://example.org/')}`);
        await chrome.browsingData.remove({ since: Date.now() - 60000 }, { cookies: true, cache: true, localStorage: true });
        log(`since com=${await cookie('https://example.com/')}`);
        await chrome.browsingData.removeHistory({ since: Date.now() - 60000 });
        log('removeHistory done');
        await chrome.browsingData.removePasswords({}).catch(error => log(`passwords rejected: ${error.message}`));
        await chrome.browsingData.removeHistory({ origins: ['https://example.com'] }).catch(error => log(`history origins rejected: ${error.message}`));
        await chrome.browsingData.remove({ origins: ['https://a.example'], excludeOrigins: ['https://b.example'] }, { cookies: true })
            .catch(error => log(`both filters rejected: ${error.message}`));
        await chrome.browsingData.remove({}, { nonsense: true }).catch(error => log(`unknown type rejected: ${error.message}`));
    } catch (error) {
        log(`error ${error.message}`);
    }
})();
