// The window's tabs, kept up to date; clicking one selects it. #count says
// how many there are (what a test reads).
async function show() {
    const tabs = await browser.tabs.query({ currentWindow: true });
    document.getElementById("count").textContent = `${tabs.length} tabs`;
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
