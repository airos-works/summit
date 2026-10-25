// sidebarAction from the background page; the MCP evaluate tool calls
// these (each returns what the API answered).
globalThis.checks = {
    open: () => browser.sidebarAction.open(),
    close: () => browser.sidebarAction.close(),
    toggle: () => browser.sidebarAction.toggle(),
    isOpen: () => browser.sidebarAction.isOpen({}),
    panel: () => browser.sidebarAction.getPanel({}),
    title: () => browser.sidebarAction.getTitle({}),
    rename: (title) => browser.sidebarAction.setTitle({ title }),
    // The panel again, with ?second (sidebar.js shows it in #count).
    retarget: () => browser.sidebarAction.setPanel({ panel: "sidebar.html?second" }),
    // What the open panel shows (its #count, address and size).
    shown: () => browser.runtime.sendMessage({ sidebar: "state" }),
};
