'use strict';
const cookieURL = 'https://localhost/partition-test/page';
const firstPartition = {topLevelSite: 'https://192.168.1.247'};
const secondPartition = {topLevelSite: 'https://127.0.0.1'};
const persistentName = 'summit-chips-persistent-fixture';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
window.runCookiePartitionTest = async () => {
    const checks = [], errors = [], events = [];
    const name = 'summit-chips-' + Date.now();
    const check = (label, ok) => { checks.push({label, ok: !!ok}); if (!ok) throw new Error(label); };
    const details = (value, partitionKey) => ({url: cookieURL, name, value, path: '/partition-test', secure: true, httpOnly: true,
        sameSite: 'no_restriction', ...(partitionKey ? {partitionKey} : {})});
    const query = partitionKey => ({url: cookieURL, name, ...(partitionKey ? {partitionKey} : {})});
    const listener = change => { if (change.cookie.name === name) events.push(change); };
    const waitEvents = async count => {
        const deadline = Date.now() + 5000;
        while (events.length < count && Date.now() < deadline) await sleep(20);
        check('received ' + count + ' committed cookie events', events.length === count);
    };
    browser.cookies.onChanged.addListener(listener);
    try {
        const plain = await browser.cookies.set(details('ordinary'));
        const first = await browser.cookies.set(details('first', firstPartition));
        const second = await browser.cookies.set(details('second', secondPartition));
        check('write receipts retain separate partition identities', !plain.partitionKey
            && first.partitionKey.topLevelSite === firstPartition.topLevelSite
            && second.partitionKey.topLevelSite === secondPartition.topLevelSite);
        await waitEvents(3);
        check('change events identify the stored partition', !events[0].cookie.partitionKey
            && events[1].cookie.partitionKey.topLevelSite === firstPartition.topLevelSite
            && events[2].cookie.partitionKey.topLevelSite === secondPartition.topLevelSite);
        check('default reads select the ordinary jar', (await browser.cookies.get(query())).value === 'ordinary');
        check('explicit partition reads stay isolated', (await browser.cookies.get(query(firstPartition))).value === 'first'
            && (await browser.cookies.get(query(secondPartition))).value === 'second');
        check('default enumeration excludes partitioned rows', (await browser.cookies.getAll(query())).length === 1);
        const all = await browser.cookies.getAll({...query(), partitionKey: {}});
        check('explicit wildcard enumeration returns all three identities', all.length === 3 && new Set(all.map(c => c.value)).size === 3);
        check('URL path filtering still applies with a wildcard partition', (await browser.cookies.getAll({...query(), url: 'https://localhost/elsewhere', partitionKey: {}})).length === 0);
        let denied = false;
        try { await browser.cookies.getAll({url: 'https://denied.example/', partitionKey: {}}); } catch (_) { denied = true; }
        check('wildcard partition access still enforces host permissions', denied);
        const removed = await browser.cookies.remove(query(firstPartition));
        check('removal receipt includes the selected partition', removed.partitionKey.topLevelSite === firstPartition.topLevelSite);
        check('partition removal preserves ordinary and other-partition cookies', await browser.cookies.get(query(firstPartition)) === null
            && (await browser.cookies.get(query())).value === 'ordinary'
            && (await browser.cookies.get(query(secondPartition))).value === 'second');
        await waitEvents(4);
        check('removal event retains the old partition and value', events[3].removed && events[3].cookie.value === 'first'
            && events[3].cookie.partitionKey.topLevelSite === firstPartition.topLevelSite);
        const expired = await browser.cookies.set({...details('expired', secondPartition), expirationDate: 0});
        check('expired partition write returns no cookie and preserves the ordinary jar', expired === null
            && await browser.cookies.get(query(secondPartition)) === null && (await browser.cookies.get(query())).value === 'ordinary');
        await waitEvents(5);
        check('expiry event keeps partition identity', events[4].cause === 'expired_overwrite'
            && events[4].cookie.partitionKey.topLevelSite === secondPartition.topLevelSite);
        for (const [value, partitionKey] of [['persistent-first', firstPartition], ['persistent-second', secondPartition]]) {
            await browser.cookies.set({...details(value, partitionKey), name: persistentName, expirationDate: Date.now() / 1000 + 3600});
        }
        check('two partitioned cookies are stored for restart verification', (await browser.cookies.getAll({url: cookieURL, name: persistentName, partitionKey: {}})).length === 2);
    } catch (error) { errors.push(String(error)); }
    finally {
        browser.cookies.onChanged.removeListener(listener);
        for (const partitionKey of [undefined, firstPartition, secondPartition]) await browser.cookies.remove(query(partitionKey));
    }
    const result = {checks, passed: checks.filter(c => c.ok).length, errors};
    document.getElementById('result').textContent = JSON.stringify(result, null, 2);
    return result;
};
window.resumeCookiePartitionTest = async () => {
    const checks = [];
    for (const [expected, partitionKey] of [['persistent-first', firstPartition], ['persistent-second', secondPartition]]) {
        const query = {url: cookieURL, name: persistentName, partitionKey};
        const cookie = await browser.cookies.get(query);
        checks.push({label: expected + ' survives browser restart', ok: cookie?.value === expected && cookie.partitionKey.topLevelSite === partitionKey.topLevelSite});
        await browser.cookies.remove(query);
    }
    const result = {checks, passed: checks.filter(c => c.ok).length};
    document.getElementById('result').textContent = JSON.stringify(result, null, 2);
    return result;
};
