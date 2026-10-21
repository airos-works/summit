'use strict';
window.shutdownProbe = {ready: false, errors: []};
const probe = shutdownProbe;
const round = new URL(location.href).searchParams.get('round');
const prefix = new URL('.', location.href).pathname;
const held = kind => `${prefix}held?round=${round}&kind=${kind}`;
const consume = async response => {
    const reader = response.body.getReader();
    while (!(await reader.read()).done) { }
};
const failed = error => probe.errors.push(String(error));
const waitFor = predicate => new Promise((resolve, reject) => {
    const deadline = Date.now() + 20000;
    const tick = () => predicate() ? resolve() : Date.now() > deadline
        ? reject(new Error('probe readiness deadline')) : setTimeout(tick, 25);
    tick();
});
(async () => {
    probe.secure = isSecureContext;
    probe.previousCookie = document.cookie;
    probe.visit = Number(localStorage.getItem('shutdown-visits') || 0) + 1;
    localStorage.setItem('shutdown-visits', String(probe.visit));
    document.cookie = `shutdown-visit=${probe.visit}; Path=${prefix}; Max-Age=3600; Secure; SameSite=Strict`;
    probe.serviceWorkerAvailable = 'serviceWorker' in navigator;
    if (!probe.serviceWorkerAvailable) throw new Error('service workers are unavailable');
    const registration = await navigator.serviceWorker.register('sw.js', {scope: prefix});
    await navigator.serviceWorker.ready;
    await waitFor(() => !!navigator.serviceWorker.controller);
    probe.controlled = await (await fetch('controlled')).text();
    probe.cacheSeed = await (await caches.match(prefix + 'cache-seed')).text();
    const cache = await caches.open('shutdown-cache');
    const previousCache = await cache.match(prefix + 'cache-visits');
    probe.cacheVisit = Number(previousCache ? await previousCache.text() : 0) + 1;
    await cache.put(prefix + 'cache-visits', new Response(String(probe.cacheVisit)));
    navigator.serviceWorker.addEventListener('message', event => {
        if (event.data.kind === 'service-headers' && event.data.round === round) probe.serviceHeaders = true;
        if (event.data.error) failed(event.data.error);
    });
    navigator.serviceWorker.controller.postMessage({round, url: held('service')});
    fetch(held('page')).then(response => {
        probe.pageHeaders = response.ok;
        return consume(response);
    }).catch(failed);
    const worker = new Worker('worker.js');
    worker.onmessage = event => {
        if (event.data.kind === 'worker-headers') probe.workerHeaders = true;
        if (event.data.error) failed(event.data.error);
    };
    worker.onerror = event => failed(event.message);
    worker.postMessage(held('worker'));
    const socket = new WebSocket(`wss://${location.host}${prefix}socket?round=${round}`);
    socket.onopen = () => socket.send(`round-${round}`);
    socket.onmessage = event => { probe.websocketEcho = event.data; };
    socket.onerror = () => failed('secure WebSocket error');
    window.shutdownRegistration = registration;
    await waitFor(() => probe.pageHeaders && probe.workerHeaders && probe.serviceHeaders
        && probe.websocketEcho === `round-${round}`);
    probe.ready = true;
})().catch(failed);
