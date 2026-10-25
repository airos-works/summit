// The window's tabs, kept up to date; clicking one selects it. #count says
// how many there are (what a test reads).
async function show() {
    const tabs = await browser.tabs.query({ currentWindow: true });
    document.getElementById("count").textContent = `${tabs.length} tabs${location.search}`;
    const list = document.getElementById("tabs");
    list.replaceChildren(...tabs.map((tab) => {
        const item = document.createElement("li");
        item.textContent = tab.title || tab.url;
        item.className = tab.active ? "active" : "";
        item.onclick = () => browser.tabs.update(tab.id, { active: true });
        return item;
    }));
}
for (const event of ["onCreated", "onRemoved", "onUpdated", "onActivated"]) browser.tabs[event].addListener(show);
show();
// The background page asks what the panel shows ({sidebar: "state"}).
browser.runtime.onMessage.addListener((message) => {
    if (message?.sidebar === "state")
        return Promise.resolve({ count: document.getElementById("count").textContent, url: location.href, width: innerWidth, height: innerHeight });
});
