// [detect] lines: i18n.detectLanguage on a few texts, then tabs.detectLanguage
// for every tab that loads (open tests/fixtures/detect-language.html, which
// declares no language, and a page that does). globalThis.results keeps them.
globalThis.results = {};
const texts = {
    german: "Die Stadtbibliothek öffnet jeden Morgen um neun Uhr. Besucher können Bücher ausleihen, Zeitungen lesen und die Computer neben dem Eingang benutzen.",
    mixed: "The public library opens every morning at nine. Visitors can borrow books and read newspapers. La biblioteca municipal abre cada mañana a las nueve. Los visitantes pueden pedir libros prestados.",
    japanese: "市立図書館は毎朝九時に開館します。来館者は本を借りたり、新聞を読んだりすることができます。",
    empty: "",
};

function log(name, value) {
    globalThis.results[name] = value;
    console.log(`[detect] ${name} ${JSON.stringify(value)}`);
}

(async () => {
    for (const [name, text] of Object.entries(texts)) {
        try { log(name, await browser.i18n.detectLanguage(text)); }
        catch (error) { log(name, {error: String(error)}); }
    }
    // The callback form, as Chrome extensions use it.
    chrome.i18n.detectLanguage(texts.german, (result) => log("callback", result));
})();

browser.tabs.onUpdated.addListener(async (tabId, change, tab) => {
    if (change.status !== "complete" || !/^https?:/.test(tab.url)) return;
    try { log("tab " + tab.url, await browser.tabs.detectLanguage(tabId)); }
    catch (error) { log("tab " + tab.url, {error: String(error)}); }
});

browser.runtime.onMessage.addListener((message) => { if (message.content) log("content " + message.url, message.content); });
