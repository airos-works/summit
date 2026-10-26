// Session rules that name tabs (tabIds, excludedTabIds), #106. The MCP
// evaluate tool calls these.
const dnr = browser.declarativeNetRequest;
globalThis.checks = {
    tabs: async () => (await browser.tabs.query({})).map((tab) => [tab.id, tab.url]),
    // Rule 1 blocks ?summit-dnr-tab images in the given tabs only; rule 2
    // blocks ?summit-dnr-excl images everywhere but the given tabs.
    add: (only, except) => dnr.updateSessionRules({ removeRuleIds: [1, 2], addRules: [
        { id: 1, action: { type: "block" }, condition: { urlFilter: "summit-dnr-tab", resourceTypes: ["image"], tabIds: only } },
        { id: 2, action: { type: "block" }, condition: { urlFilter: "summit-dnr-excl", resourceTypes: ["image"], excludedTabIds: except } },
    ] }).then(() => "added", (error) => error.message),
    clear: () => dnr.updateSessionRules({ removeRuleIds: [1, 2] }).then(() => "cleared"),
    session: () => dnr.getSessionRules(),
    dynamic: () => dnr.updateDynamicRules({ addRules: [{ id: 9, action: { type: "block" }, condition: { urlFilter: "x", tabIds: [1] } }] }).then(() => "accepted", (error) => error.message),
    invalid: () => dnr.updateSessionRules({ addRules: [{ id: 8, action: { type: "block" }, condition: { urlFilter: "x", tabIds: [] } }] }).then(() => "accepted", (error) => error.message),
    both: () => dnr.updateSessionRules({ addRules: [{ id: 7, action: { type: "block" }, condition: { urlFilter: "x", tabIds: [1], excludedTabIds: [2] } }] }).then(() => "accepted", (error) => error.message),
    test: async (tabId) => {
        const result = {};
        for (const marker of ["summit-dnr-tab", "summit-dnr-excl"]) {
            const request = { url: `http://192.168.1.200:8796/fixtures/px.png?${marker}`, type: "image" };
            if (tabId !== undefined)
                request.tabId = tabId;
            result[marker] = (await dnr.testMatchOutcome(request)).matchedRules.map((rule) => rule.ruleId);
        }
        return result;
    },
};
