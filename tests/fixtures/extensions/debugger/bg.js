// chrome.debugger on Summit (WebExtensionContextAPIDebuggerHaiku.cpp). The MCP
// evaluate tool (extension: "debugger@summit.test") calls these; page.html is
// the page they act on.
globalThis.events = [];
// What content.js found in the page (dom.openOrClosedShadowRoot).
globalThis.contentReports = [];
chrome.runtime.onMessage.addListener((message) => { if (message && message.shadow !== undefined) contentReports.push(message); });
globalThis.detached = [];
chrome.debugger.onEvent.addListener((source, method, params) => {
    events.push({ tabId: source.tabId, method, params });
    if (events.length > 500) events.shift();
});
chrome.debugger.onDetach.addListener((source, reason) => detached.push({ tabId: source.tabId, reason }));
const call = (promise) => promise.then((value) => value === undefined ? "ok" : value, (error) => "error: " + error.message);
const send = (tabId, method, params = {}) => call(chrome.debugger.sendCommand({ tabId }, method, params));
globalThis.checks = {
    present: () => ({
        debugger: typeof chrome.debugger,
        methods: ["attach", "detach", "sendCommand", "getTargets"].filter((name) => typeof chrome.debugger[name] === "function"),
        tabGroupsColor: chrome.tabGroups.Color,
        dnr: [chrome.declarativeNetRequest.RuleActionType.MODIFY_HEADERS, chrome.declarativeNetRequest.HeaderOperation.SET,
            chrome.declarativeNetRequest.ResourceType.WEBSOCKET],
        getContexts: typeof chrome.runtime.getContexts,
    }),
    contexts: (filter = {}) => call(chrome.runtime.getContexts(filter)),
    targets: () => call(chrome.debugger.getTargets()),
    tabs: async () => (await chrome.tabs.query({})).map((tab) => ({ id: tab.id, url: tab.url })),
    attach: (tabId) => call(chrome.debugger.attach({ tabId }, "1.3")),
    attachTarget: (targetId) => call(chrome.debugger.attach({ targetId }, "1.3")),
    detach: (tabId) => call(chrome.debugger.detach({ tabId })),
    send,
    // As Claude's javascript tool sends it: REPL mode, by value, awaited.
    repl: (tabId, code) => send(tabId, "Runtime.evaluate", { expression: `x:{{${code}\n};break x}`, returnByValue: true, awaitPromise: true, replMode: true }),
    click: async (tabId, x, y) => [
        await send(tabId, "Input.dispatchMouseEvent", { type: "mouseMoved", x, y }),
        await send(tabId, "Input.dispatchMouseEvent", { type: "mousePressed", x, y, button: "left", buttons: 1, clickCount: 1 }),
        await send(tabId, "Input.dispatchMouseEvent", { type: "mouseReleased", x, y, button: "left", buttons: 0, clickCount: 1 }),
    ],
    type: async (tabId, text) => send(tabId, "Input.insertText", { text }),
    key: async (tabId, key, code, windowsVirtualKeyCode, text = "") => [
        await send(tabId, "Input.dispatchKeyEvent", { type: text ? "keyDown" : "rawKeyDown", key, code, windowsVirtualKeyCode, text }),
        await send(tabId, "Input.dispatchKeyEvent", { type: "keyUp", key, code, windowsVirtualKeyCode }),
    ],
    screenshot: async (tabId, params = { format: "png" }) => {
        const result = await send(tabId, "Page.captureScreenshot", params);
        if (typeof result === "string")
            return result;
        const image = await createImageBitmap(await (await fetch(`data:image/${params.format || "png"};base64,${result.data}`)).blob());
        return { width: image.width, height: image.height, bytes: result.data.length };
    },
    events: (prefix = "") => events.filter((event) => event.method.startsWith(prefix)),
    // A sign-in ending at a page of the extension itself, as Claude in Chrome's
    // (chrome-extension://ID/oauth_callback.html): auth.html sends the window
    // to its ?redirect= with ?code=42.
    signIn: (authPage) => call(chrome.identity.launchWebAuthFlow({
        url: `${authPage}?redirect=${encodeURIComponent(chrome.runtime.getURL("oauth_callback.html"))}`, interactive: true })),
    // Chrome's favicon permission: the icon the browser shows for a page.
    favicon: async (pageUrl) => {
        const response = await fetch(chrome.runtime.getURL(`/_favicon/?pageUrl=${encodeURIComponent(pageUrl)}&size=32`)).catch((error) => error);
        if (!(response instanceof Response))
            return "error: " + response;
        if (!response.ok)
            return response.status;
        const image = await createImageBitmap(await response.blob());
        return { type: response.headers.get("content-type"), width: image.width, height: image.height };
    },
    // A dialog the page opens, answered with Page.handleJavaScriptDialog.
    dialog: async (tabId, kind, accept, promptText) => {
        events.length = 0;
        await send(tabId, "Runtime.evaluate", { expression: `setTimeout(() => { window.dialogResult = ${kind}("dialog from the fixture", "default"); }, 50); 0` });
        for (let i = 0; i < 50 && !events.some((event) => event.method === "Page.javascriptDialogOpening"); ++i)
            await new Promise((resolve) => setTimeout(resolve, 100));
        const opening = events.find((event) => event.method === "Page.javascriptDialogOpening");
        const handled = await send(tabId, "Page.handleJavaScriptDialog", { accept, promptText });
        await new Promise((resolve) => setTimeout(resolve, 300));
        const closed = events.find((event) => event.method === "Page.javascriptDialogClosed");
        const result = await send(tabId, "Runtime.evaluate", { expression: "window.dialogResult", returnByValue: true });
        return { opening: opening && opening.params, handled, closed: closed && closed.params, result: result.result };
    },
    clear: () => { events.length = 0; detached.length = 0; return "ok"; },
};
