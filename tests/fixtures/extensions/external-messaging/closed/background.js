chrome.runtime.onMessageExternal.addListener((message, sender, sendResponse) => {
    console.log(`[external-closed] unexpected message from ${sender.id}`);
    sendResponse('should not happen');
});
