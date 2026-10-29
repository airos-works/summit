// onRuleMatchedDebug for the MCP evaluate tool: globalThis.debugEvents.
const dnr = chrome.declarativeNetRequest;
globalThis.debugEvents = [];
globalThis.hasDebugEvent = 'onRuleMatchedDebug' in dnr;
dnr.onRuleMatchedDebug.addListener((info) => { globalThis.debugEvents.push(info); });
globalThis.checks = {
    events: () => globalThis.debugEvents,
    summary: () => globalThis.debugEvents.map((e) => `${e.rule.rulesetId}:${e.rule.ruleId} ${e.request.type} ${e.request.method} tab=${e.request.tabId} frame=${e.request.frameId}/${e.request.parentFrameId} ${e.request.url.split('/').pop()} initiator=${e.request.initiator}`),
    matched: () => dnr.getMatchedRules(),
    clear: () => { globalThis.debugEvents = []; return true; },
    addSession: () => dnr.updateSessionRules({ addRules: [{ id: 10, priority: 1, action: { type: "block" }, condition: { urlFilter: "ext106-session", resourceTypes: ["image"] } }] }),
};
