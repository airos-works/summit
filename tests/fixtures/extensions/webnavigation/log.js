function show({ log = [] }) {
    document.getElementById('log').textContent = log.map(entry => JSON.stringify(entry)).join('\n');
}
chrome.storage.local.get('log').then(show);
chrome.storage.onChanged.addListener(() => chrome.storage.local.get('log').then(show));
