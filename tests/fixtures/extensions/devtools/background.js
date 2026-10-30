// What the devtools page reported, for the MCP evaluate tool
// ({"extension": ID, "expression": "checks.results()"}), and a way to ask
// the open devtools page to run a call now (checks.ask({...})).
const results = [];
browser.runtime.onMessage.addListener((message) => {
    if (message && message.fixture === "report") {
        results.push(message.entry);
        if (results.length > 500) results.shift();
    }
});
globalThis.checks = {
    results: () => results.slice(),
    last: (kind) => results.filter((entry) => entry.kind === kind).pop() || null,
    count: (kind) => results.filter((entry) => entry.kind === kind).length,
    clear: () => { results.length = 0; return true; },
    // {call: "eval", expression, options} | {call: "getHAR"} | {call: "reload"}
    // | {call: "openResource", url, line}: the devtools page answers.
    ask: (request) => browser.runtime.sendMessage({ fixture: "ask", request }),
};
