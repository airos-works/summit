// Exercise real promise/callback bindings, IPC validation and scoped action state.
async function verifyBadgeColors() {
    const action = browser.browserAction;
    const checks = [];
    const equal = (actual, expected, label) => {
        if (JSON.stringify(actual) !== JSON.stringify(expected))
            throw new Error(label + ": " + JSON.stringify(actual));
        checks.push(label);
    };
    const background = details => action.getBadgeBackgroundColor(details);
    const foreground = details => action.getBadgeTextColor(details);
    equal(await background({}), [217, 0, 0, 255], "default background");
    equal(await foreground({}), [255, 255, 255, 255], "default contrast");
    for (const [color, expected] of [
        ["rebeccapurple", [102, 51, 153, 255]],
        ["#abc", [170, 187, 204, 255]],
        ["#12345680", [18, 52, 86, 128]],
        ["rgb(20 40 60 / 50%)", [20, 40, 60, 128]],
        ["hsl(120 100% 50%)", [0, 255, 0, 255]],
        [[1, 2, 3, 4], [1, 2, 3, 4]],
        ["transparent", [0, 0, 0, 0]],
    ]) {
        await action.setBadgeBackgroundColor({color});
        equal(await background({}), expected, "background " + JSON.stringify(color));
    }
    await action.setBadgeBackgroundColor({color: "white"});
    equal(await foreground({}), [0, 0, 0, 255], "automatic dark text");
    await action.setBadgeBackgroundColor({color: "black"});
    equal(await foreground({}), [255, 255, 255, 255], "automatic light text");
    await action.setBadgeTextColor({color: "#12345680"});
    equal(await foreground({}), [18, 52, 86, 128], "CSS text alpha");
    await action.setBadgeBackgroundColor({color: "white"});
    equal(await foreground({}), [18, 52, 86, 128], "explicit text survives background change");
    await action.setBadgeTextColor({color: null});
    equal(await foreground({}), [0, 0, 0, 255], "reset text restores automatic contrast");
    await new Promise(resolve => chrome.browserAction.setBadgeBackgroundColor({color: [7, 8, 9, 255]}, resolve));
    equal(await new Promise(resolve => chrome.browserAction.getBadgeBackgroundColor({}, resolve)), [7, 8, 9, 255], "Chrome callback round trip");

    // Restoring a session and opening the fixture URL can still be committing
    // navigation when the background starts. A commit correctly clears tab
    // action overrides, so wait for the test page before checking their scope.
    let tab;
    const deadline = Date.now() + 10000;
    while (Date.now() < deadline) {
        const tabs = await browser.tabs.query({});
        tab = tabs.find(item => item.active && item.status === "complete"
            && item.url?.includes("/page?run=@NONCE@"));
        if (tab) break;
        await new Promise(resolve => setTimeout(resolve, 50));
    }
    if (!tab || !(tab.id > 0) || !(tab.windowId > 0)) throw new Error("missing native tab/window for badge scope");
    const tabScope = {tabId: tab.id}, windowScope = {windowId: tab.windowId};
    await action.setBadgeBackgroundColor({color: "black"});
    await action.setBadgeBackgroundColor({...windowScope, color: "blue"});
    equal(await background(tabScope), [0, 0, 255, 255], "tab inherits window background");
    await action.setBadgeBackgroundColor({...tabScope, color: "white"});
    equal(await background(tabScope), [255, 255, 255, 255], "tab background overrides window");
    equal(await foreground(tabScope), [0, 0, 0, 255], "tab contrast uses tab background");
    equal(await foreground(windowScope), [255, 255, 255, 255], "window contrast remains independent");
    await action.setBadgeTextColor({color: "green"});
    equal(await foreground(tabScope), [0, 128, 0, 255], "tab inherits explicit global text");
    await action.setBadgeTextColor({...windowScope, color: "yellow"});
    equal(await foreground(tabScope), [255, 255, 0, 255], "tab inherits explicit window text");
    await action.setBadgeTextColor({...tabScope, color: [10, 20, 30, 255]});
    equal(await foreground(tabScope), [10, 20, 30, 255], "tab text overrides window");
    equal(await foreground({}), [0, 128, 0, 255], "tab changes preserve global text");
    await action.setBadgeTextColor({...tabScope, color: null});
    equal(await foreground(tabScope), [255, 255, 0, 255], "null tab text restores window override");
    await action.setBadgeTextColor({...windowScope, color: null});
    equal(await foreground(tabScope), [0, 128, 0, 255], "null window text restores global override");
    await action.setBadgeTextColor({});
    equal(await foreground(tabScope), [0, 0, 0, 255], "omitted text color restores automatic contrast");
    await action.setBadgeBackgroundColor({...tabScope, color: null});
    equal(await background(tabScope), [0, 0, 255, 255], "null tab background restores window");
    await action.setBadgeBackgroundColor({...windowScope, color: null});
    equal(await background(tabScope), [0, 0, 0, 255], "null window background restores global");
    await action.setBadgeBackgroundColor({color: null});
    equal(await background(tabScope), [217, 0, 0, 255], "null global background restores default");

    const reject = async (operation, label, promiseRequired = false) => {
        const before = [await background({}), await foreground({}), await background(tabScope), await foreground(tabScope)];
        let rejected = false;
        if (promiseRequired) {
            const promise = operation();
            if (!promise || typeof promise.then !== "function") throw new Error("CSS validation did not return a promise");
            try { await promise; } catch { rejected = true; }
        } else {
            try { await operation(); } catch { rejected = true; }
        }
        if (!rejected) throw new Error("accepted invalid badge input: " + label);
        equal([await background({}), await foreground({}), await background(tabScope), await foreground(tabScope)], before, "atomic rejection " + label);
    };
    for (const color of ["", "not-a-color", "#zzzzzz", "red trailing", "red\u0000", "currentColor", "var(--badge)"])
        await reject(() => action.setBadgeBackgroundColor({color}), "CSS " + JSON.stringify(color), true);
    for (const color of [[], [0, 0, 0], [0, 0, 0, 0, 0], [-1, 0, 0, 255], [256, 0, 0, 255], [0.5, 0, 0, 255], [true, 0, 0, 255], ["1", 0, 0, 255], [null, 0, 0, 255], true, 123, {}])
        await reject(() => action.setBadgeBackgroundColor({color}), "shape " + JSON.stringify(color));
    for (const color of ["transparent", [0, 0, 0, 0]])
        await reject(() => action.setBadgeTextColor({color}), "transparent text " + JSON.stringify(color));
    for (const details of [{}, {color: "red", tabId: 0}, {color: "red", tabId: null}, {color: "red", tabId: 9007199254740992}, {color: "red", tabId: 99999999}, {color: "red", windowId: 99999999}, {color: "red", ...tabScope, ...windowScope}, {color: "red", unknown: true}])
        await reject(() => action.setBadgeBackgroundColor(details), "target " + JSON.stringify(details));

    await action.setBadgeBackgroundColor({color: [24, 72, 120, 255]});
    await action.setBadgeTextColor({color: [255, 224, 128, 255]});
    return {checks, background: await background({}), foreground: await foreground({}), tabId: tab.id, windowId: tab.windowId};
}
