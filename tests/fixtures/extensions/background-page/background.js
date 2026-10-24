// The MCP evaluate tool with "extension" reads these; console checks the log.
window.fixtureValue = { started: Date.now(), runtimeId: chrome.runtime.id };
console.log('[background-page] started');
