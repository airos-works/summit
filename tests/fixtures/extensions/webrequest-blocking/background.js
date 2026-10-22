'use strict';

// Blocking webRequest fixture. Every decision is logged with a stable prefix so
// that the extension console (or SUMMIT extension console diagnostics) shows
// which listener acted. Serve tests/fixtures/extensions/webrequest-blocking/server.py
// and open http://127.0.0.1:8765/ in a tab; see docs/webextensions-webrequest-blocking.md.

const api = globalThis.browser ?? globalThis.chrome;
const filter = {urls: ['http://127.0.0.1/*', 'http://localhost/*']};
const log = (...args) => console.log('[wr-fixture]', ...args);
const has = (details, marker) => new URL(details.url).pathname.includes(marker);

// 1. onBeforeRequest: cancel, redirect, Promise cancel, never-settling Promise.
api.webRequest.onBeforeRequest.addListener(details => {
    if (has(details, '/wr-block/')) {
        // Also WebSockets (type "websocket") and beacons (type "beacon").
        log('cancel', details.type, details.url);
        return {cancel: true};
    }
    if (has(details, '/wr-redirect/')) {
        const target = new URL('/wr-redirected.html?from=' + encodeURIComponent(details.url), details.url).href;
        log('redirect', details.type, details.url, '->', target);
        return {redirectUrl: target};
    }
    if (has(details, '/wr-promise-block/')) {
        log('promise cancel', details.type, details.url);
        return new Promise(resolve => setTimeout(() => resolve({cancel: true}), 100));
    }
    if (has(details, '/wr-surrogate-js/')) {
        // uBlock Origin's redirect= surrogates: a web-accessible extension file.
        log('surrogate script', details.type, details.url);
        return {redirectUrl: api.runtime.getURL('surrogates/noop.js')};
    }
    if (has(details, '/wr-surrogate-data/')) {
        log('surrogate data: URL', details.type, details.url);
        return {redirectUrl: 'data:text/plain;base64,' + btoa('surrogate data')};
    }
    if (has(details, '/wr-surrogate-hidden/')) {
        // Not web-accessible: the request must be blocked, not served.
        log('surrogate not web-accessible', details.type, details.url);
        return {redirectUrl: api.runtime.getURL('background.js')};
    }
    if (has(details, '/wr-slow/')) {
        // Never settles: the load must continue unmodified after the listener
        // timeout (20 s) instead of hanging.
        log('never settles', details.url);
        return new Promise(() => {});
    }
    if (has(details, '/wr-invalid/')) {
        // Invalid (cancel mixed with other keys): must be ignored, load continues.
        log('invalid reply', details.url);
        return {cancel: true, redirectUrl: 'http://127.0.0.1:1/'};
    }
    return undefined;
}, filter, ['blocking']);

// 2. onBeforeSendHeaders: add a request header the server echoes back.
api.webRequest.onBeforeSendHeaders.addListener(details => {
    if (!has(details, '/wr-headers/'))
        return undefined;
    const requestHeaders = details.requestHeaders.filter(header => header.name.toLowerCase() !== 'x-summit-webrequest');
    requestHeaders.push({name: 'X-Summit-WebRequest', value: 'request-header-added'});
    log('request headers', details.url);
    return {requestHeaders};
}, filter, ['blocking', 'requestHeaders']);

// 3. onHeadersReceived: add a response header and a restrictive CSP.
api.webRequest.onHeadersReceived.addListener(details => {
    if (!has(details, '/wr-headers/'))
        return undefined;
    const responseHeaders = details.responseHeaders.slice();
    responseHeaders.push({name: 'X-Summit-Response', value: 'response-header-added'});
    log('response headers', details.url);
    return {responseHeaders};
}, filter, ['blocking', 'responseHeaders']);

// 4. onAuthRequired: fill HTTP Basic credentials (asyncBlocking callback form).
api.webRequest.onAuthRequired.addListener((details, callback) => {
    if (!has(details, '/wr-auth/')) {
        callback({});
        return;
    }
    log('auth', details.url, details.realm);
    callback({authCredentials: {username: 'summit', password: 'webrequest'}});
}, filter, ['asyncBlocking']);

// Non-blocking observer: must still see every request exactly once.
api.webRequest.onCompleted.addListener(details => log('completed', details.statusCode, details.url), filter);
api.webRequest.onErrorOccurred.addListener(details => log('error', details.error, details.url), filter);

log('ready');
