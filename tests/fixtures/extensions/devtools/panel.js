// The panel's own page: devtools is there too (Vue's panel uses it).
window.marker = "fixture-panel";
document.getElementById("tab").textContent = "inspected tab " + chrome.devtools.inspectedWindow.tabId;
