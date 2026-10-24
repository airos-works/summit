// i18n.detectLanguage from a content script, on the page's own text.
browser.i18n.detectLanguage(document.body ? document.body.innerText.slice(0, 5000) : "").then(
    (content) => browser.runtime.sendMessage({url: location.href, content}),
    (error) => browser.runtime.sendMessage({url: location.href, content: {error: String(error)}}));
