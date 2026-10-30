// The extension's devtools_page: reports what devtools.* gives it.
const report = (kind, data) => browser.runtime.sendMessage({ fixture: "report", entry: Object.assign({ kind, time: Date.now() }, data) });
const devtools = chrome.devtools;
report("loaded", {
    tabId: devtools.inspectedWindow.tabId,
    theme: devtools.panels.themeName,
    hasElements: !!(devtools.panels.elements && devtools.panels.elements.onSelectionChanged),
    hasNetwork: typeof devtools.network.getHAR === "function",
});
devtools.panels.create("Fixture", "icon.png", "panel.html", (panel) => {
    report("created", { error: chrome.runtime.lastError ? chrome.runtime.lastError.message : null });
    panel.onShown.addListener((panelWindow) => {
        report("shown", {
            hasWindow: !!panelWindow,
            marker: panelWindow ? panelWindow.marker : null,
            title: panelWindow ? panelWindow.document.title : null,
        });
        // As React DevTools does: draw into the panel's document from here.
        if (panelWindow) panelWindow.document.getElementById("from-devtools-page").textContent = "drawn by the devtools page";
    });
    panel.onHidden.addListener(() => report("hidden", {}));
});
devtools.network.onNavigated.addListener((url) => report("navigated", { url }));
devtools.network.onRequestFinished.addListener((entry) => report("request", {
    url: entry && entry.request ? entry.request.url : null,
    status: entry && entry.response ? entry.response.status : null,
}));
devtools.panels.elements.onSelectionChanged.addListener(() => {
    devtools.inspectedWindow.eval("$0 ? $0.tagName + (($0.id && '#' + $0.id) || '') : null", (result, error) => report("selection", { result, error }));
});
devtools.panels.onThemeChanged.addListener((theme) => report("theme", { theme }));
devtools.inspectedWindow.eval("document.title", (result, error) => report("eval-title", { result, error }));
devtools.inspectedWindow.eval("throw new Error('fixture failure')", (result, error) => report("eval-throw", { result, error }));
devtools.inspectedWindow.eval("({ answer: 42, list: [1, 2], nothing: null })", (result, error) => report("eval-object", { result, error }));
browser.devtools.inspectedWindow.eval("location.href").then((pair) => report("eval-promise", { pair }));

browser.runtime.onMessage.addListener((message) => {
    if (!message || message.fixture !== "ask") return undefined;
    const request = message.request || {};
    if (request.call === "eval")
        return new Promise((resolve) => devtools.inspectedWindow.eval(request.expression, request.options || {}, (result, error) => resolve({ result, error })));
    if (request.call === "getHAR")
        return new Promise((resolve) => devtools.network.getHAR((log) => resolve({
            version: log && log.version, creator: log && log.creator && log.creator.name,
            entries: log && log.entries ? log.entries.length : -1,
            urls: log && log.entries ? log.entries.slice(0, 5).map((entry) => entry.request.url) : [],
        })));
    if (request.call === "reload") {
        devtools.inspectedWindow.reload({ ignoreCache: !!request.ignoreCache });
        return Promise.resolve({ reloaded: true });
    }
    if (request.call === "openResource")
        return new Promise((resolve) => devtools.panels.openResource(request.url, request.line || 0, (result) => resolve({ result: result || null })));
    if (request.call === "resources")
        return new Promise((resolve) => devtools.inspectedWindow.getResources((resources) => resolve({ resources })));
    return Promise.resolve({ unknown: request.call });
});
