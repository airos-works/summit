'use strict';
const prefix = new URL('.', location.href).pathname;
self.addEventListener('install', event => event.waitUntil((async () => {
    const cache = await caches.open('shutdown-cache');
    await cache.put(prefix + 'cache-seed', new Response('persistent-cache'));
    await self.skipWaiting();
})()));
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));
self.addEventListener('fetch', event => {
    if (new URL(event.request.url).pathname === prefix + 'controlled')
        event.respondWith(new Response('service-worker-response'));
});
self.addEventListener('message', event => event.waitUntil((async () => {
    try {
        const response = await fetch(event.data.url);
        if (!response.ok) throw new Error('service-worker transfer failed');
        event.source.postMessage({kind: 'service-headers', round: event.data.round});
        const reader = response.body.getReader();
        while (!(await reader.read()).done) { }
    } catch (error) {
        event.source.postMessage({error: String(error)});
    }
})()));
