// declarativeNetRequest.getMatchedRules for the MCP evaluate tool. Open
// http://127.0.0.1:8798/dnr.html (the downloads-control fixture's serve.py):
// it loads summit-dnr-match-blocked.png (rule 1 blocks it) and
// summit-dnr-match-allowed.png (rule 2 allows it, above rule 1). Matches are
// recorded from the first getMatchedRules call on, so it is made at start.
const dnr = browser.declarativeNetRequest;
dnr.getMatchedRules().catch(() => { });

globalThis.checks = {
    all: () => dnr.getMatchedRules(),
    tab: (tabId) => dnr.getMatchedRules({ tabId }),
    since: (minTimeStamp) => dnr.getMatchedRules({ minTimeStamp }),
    // A session rule (id 3) blocking another image, to see it reported too.
    addSession: () => dnr.updateSessionRules({ addRules: [{ id: 3, priority: 1, action: { type: "block" }, condition: { urlFilter: "summit-dnr-session", resourceTypes: ["image"] } }] }),
    // What a matched rule looks like, rule by rule: "ruleset:id" -> count.
    async summary(tabId) {
        const { rulesMatchedInfo } = await dnr.getMatchedRules(tabId === undefined ? {} : { tabId });
        const counts = {};
        for (const info of rulesMatchedInfo) {
            const key = `${info.rule.rulesetId}:${info.rule.ruleId}`;
            counts[key] = (counts[key] || 0) + 1;
        }
        return counts;
    },
};
