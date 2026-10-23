// Replies to other extensions' messages with what it saw of the sender.
chrome.runtime.onMessageExternal.addListener((message, sender, sendResponse) => {
    console.log(`[external-receiver] from ${sender.id}: ${JSON.stringify(message)}`);
    sendResponse({ echo: message, senderId: sender.id, receiver: chrome.runtime.id });
});
chrome.runtime.onMessage.addListener(() => console.log('[external-receiver] onMessage fired for an external message (wrong event)'));
