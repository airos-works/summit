// dom.openOrClosedShadowRoot from a content script: page.html's closed shadow root.
const host = document.getElementById("host");
const root = host && chrome.dom ? chrome.dom.openOrClosedShadowRoot(host) : undefined;
chrome.runtime.sendMessage({ dom: typeof chrome.dom, shadow: root ? root.textContent : null, open: host ? host.shadowRoot : "no host" });
