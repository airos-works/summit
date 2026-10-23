// Replies to other extensions' messages with what it saw of the sender.
chrome.runtime.onMessageExternal.addListener((message, sender, sendResponse) => {
    console.log(`[external-receiver] from ${sender.id}: ${JSON.stringify(message)}`);
    sendResponse({ echo: message, senderId: sender.id, receiver: chrome.runtime.id });
});
chrome.runtime.onMessage.addListener(() => console.log('[external-receiver] onMessage fired for an external message (wrong event)'));
// Ports from other extensions: answer each ping, disconnect after "bye".
chrome.runtime.onConnectExternal.addListener(port => {
    console.log(`[external-receiver] port "${port.name}" from ${port.sender.id}`);
    port.onMessage.addListener(message => {
        if (message === 'bye') {
            port.disconnect();
            return;
        }
        port.postMessage(`pong ${message}`);
    });
});
chrome.runtime.onConnect.addListener(() => console.log('[external-receiver] onConnect fired for an external port (wrong event)'));
