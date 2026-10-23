'use strict';

// chrome.userScripts fixture (#105). Open https://example.com/ and
// https://example.com/?skip in a tab; each user script marks the page, and the
// one in the content script world also messages this worker. See
// docs/webextensions-userscripts.md for the expected results.

const log = (...args) => console.log('[us-fixture]', ...args);

chrome.runtime.onMessage.addListener((message, sender) => {
    log('message', JSON.stringify(message), sender.url);
    chrome.storage.local.set({lastMessage: message});
});
// Tampermonkey listens here too; this port delivers user script messages to
// onMessage instead.
chrome.runtime.onUserScriptMessage?.addListener(message => log('user script message', JSON.stringify(message)));

async function setup() {
    await chrome.userScripts.unregister();
    await chrome.userScripts.configureWorld({messaging: true});
    await chrome.userScripts.register([
        {
            id: 'heading',
            matches: ['https://example.com/*'],
            runAt: 'document_end',
            js: [{code: `
                document.documentElement.dataset.heading = 'ran';
                const heading = document.querySelector('h1, p');
                if (heading)
                    heading.textContent = 'Changed by a user script';
                chrome.runtime.sendMessage({ran: 'heading', href: location.href});`}],
        },
        // Globs: the first never matches, the second matches unless ?skip.
        {
            id: 'glob-miss',
            matches: ['https://example.com/*'],
            includeGlobs: ['*://example.com/nothing*'],
            js: [{code: "document.documentElement.dataset.globMiss = 'ran';"}],
            runAt: 'document_end',
        },
        {
            id: 'glob-hit',
            matches: ['https://example.com/*'],
            includeGlobs: ['https://example.com/*'],
            excludeGlobs: ['*?skip*'],
            js: [{code: "document.documentElement.dataset.globHit = 'ran';"}],
            runAt: 'document_end',
        },
        {
            id: 'main-world',
            matches: ['https://example.com/*'],
            world: 'MAIN',
            js: [{code: 'window.summitUserScriptMainWorld = typeof chrome === "undefined" || !chrome.runtime ? "page" : "extension";'}],
            runAt: 'document_start',
        },
    ]);
    await chrome.userScripts.update([{id: 'glob-miss', excludeGlobs: ['*']}]);
    const scripts = await chrome.userScripts.getScripts();
    log('registered', scripts.map(script => `${script.id}:${script.world}:${script.js.length}`).join(' '));
    log('worlds', JSON.stringify(await chrome.userScripts.getWorldConfigurations()));
}

chrome.runtime.onInstalled.addListener(() => setup().catch(error => log('setup failed', error.message)));
