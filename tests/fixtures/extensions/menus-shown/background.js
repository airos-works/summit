// Right-click a page: the item's title changes while the menu is open (it
// counts the menus shown and names the context), [menus] lines log each
// onShown and onHidden. globalThis.events keeps them for the MCP evaluate.
const menus = browser.menus;
globalThis.events = [];
let shown = 0;

function log(name, value) {
    globalThis.events.push({name, value});
    console.log(`[menus] ${name} ${JSON.stringify(value)}`);
}

menus.create({id: "count", title: "Not shown yet", contexts: ["all"]});
menus.create({id: "link", title: "A link item", contexts: ["link"]});

menus.onShown.addListener(async (info, tab) => {
    ++shown;
    log("shown", {info, tab: tab && {id: tab.id, url: tab.url}});
    const where = info.linkUrl ? "a link" : info.selectionText ? "a selection" : info.contexts.join(",");
    await menus.update("count", {title: `Shown ${shown} times, on ${where}`});
    await menus.refresh();
    log("refreshed", shown);
});

menus.onHidden.addListener(() => log("hidden", shown));

menus.onClicked.addListener((info) => log("clicked", info.menuItemId));
