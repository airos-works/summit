// Logs history events, and when https://example.com/?history has been
// visited: searches for it, reads its visits, adds and deletes a made-up URL,
// and deletes a range. One console line per step: "[history] ...".
const log = message => console.log(`[history] ${message}`);
const fake = 'https://summit.invalid/history-fixture';
let ran = false;
chrome.history.onVisitRemoved.addListener(removed => log(`onVisitRemoved all=${removed.allHistory} urls=${removed.urls.join(',')}`));
chrome.history.onVisited.addListener(async item => {
    log(`onVisited ${item.url} count=${item.visitCount}`);
    if (ran || !item.url.startsWith('https://example.com/?history'))
        return;
    ran = true;
    try {
        const found = await chrome.history.search({ text: 'example.com/?history' });
        log(`search found ${found.length}: ${found.map(page => `${page.url} (${page.title}, ${page.visitCount})`).join('; ')}`);
        const visits = await chrome.history.getVisits({ url: item.url });
        log(`getVisits ${visits.length} transition=${visits[0] && visits[0].transition}`);
        await chrome.history.addUrl({ url: fake, title: 'Fixture' });
        log(`addUrl, search ${(await chrome.history.search({ text: 'summit.invalid', startTime: 0 })).length}`);
        await chrome.history.deleteUrl({ url: fake });
        log(`deleteUrl, search ${(await chrome.history.search({ text: 'summit.invalid', startTime: 0 })).length}`);
        await chrome.history.search({ text: '' }).then(all => log(`all recent ${all.length}`));
        await chrome.history.getVisits({ url: 'not a url' }).catch(error => log(`bad url rejected: ${error.message}`));
    } catch (error) {
        log(`error ${error.message}`);
    }
});
