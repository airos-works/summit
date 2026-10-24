// Talks to the summit.echo host (host/: copy both files to
// ~/config/settings/Summit/NativeMessagingHosts/). One console line per step:
// "[native] ...".
const log = message => console.log(`[native] ${message}`);
const step = async (name, run) => {
    try {
        log(`${name}: ${await run()}`);
    } catch (error) {
        log(`${name} failed: ${error.message}`);
    }
};
const portSteps = () => new Promise(resolve => {
    const port = chrome.runtime.connectNative('summit.echo');
    const replies = [];
    port.onMessage.addListener(reply => {
        replies.push(reply.count);
        if (reply.count === 2)
            port.postMessage({ quit: true });
    });
    port.onDisconnect.addListener(() => resolve(`replies=${replies.join(',')} disconnected: ${chrome.runtime.lastError && chrome.runtime.lastError.message}`));
    port.postMessage({ n: 1 });
    port.postMessage({ n: 2 });
});
(async () => {
    await step('sendNativeMessage', async () => JSON.stringify(await chrome.runtime.sendNativeMessage('summit.echo', { hello: 'haiku' })));
    await step('connectNative', portSteps);
    await step('over the limit (must fail)', () => chrome.runtime.sendNativeMessage('summit.echo', { big: true }));
    await step('missing host (must fail)', () => chrome.runtime.sendNativeMessage('summit.nothere', {}));
    await step('bad name (must fail)', () => chrome.runtime.sendNativeMessage('../etc', {}));
})();
