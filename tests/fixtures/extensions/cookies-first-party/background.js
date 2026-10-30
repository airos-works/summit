// checks.run() for the MCP evaluate tool ({"extension": ID}): what each call
// with firstPartyDomain answered. Firefox with first-party isolation off:
// "" and null are ordinary cookies, cookies report firstPartyDomain "", and
// another domain matches none (Summit refuses to set one; Firefox would keep
// it in a jar of its own).
const attempt = async (call) => {
    try {
        return await call();
    } catch (error) {
        return "error: " + error.message;
    }
};
globalThis.checks = {
    run: async () => {
        const url = "https://example.com/";
        const out = {};
        const set = await attempt(() => browser.cookies.set({ url, name: "fpd", value: "1", firstPartyDomain: "" }));
        out.setEmpty = set && set.firstPartyDomain;
        out.fields = set && typeof set === "object" ? Object.keys(set).sort() : set;
        out.setNull = await attempt(async () => (await browser.cookies.set({ url, name: "fpd2", value: "2", firstPartyDomain: null })).value);
        out.setDomain = await attempt(() => browser.cookies.set({ url, name: "fpd3", value: "3", firstPartyDomain: "example.com" }));
        out.getEmpty = await attempt(async () => (await browser.cookies.get({ url, name: "fpd", firstPartyDomain: "" })).value);
        out.getNull = await attempt(async () => (await browser.cookies.get({ url, name: "fpd", firstPartyDomain: null })).value);
        out.getOther = await attempt(() => browser.cookies.get({ url, name: "fpd", firstPartyDomain: "other.example" }));
        out.allDefault = await attempt(async () => (await browser.cookies.getAll({ url })).length);
        out.allNull = await attempt(async () => (await browser.cookies.getAll({ firstPartyDomain: null, domain: "example.com" })).length);
        out.allEmpty = await attempt(async () => (await browser.cookies.getAll({ url, firstPartyDomain: "" })).length);
        out.allOther = await attempt(async () => (await browser.cookies.getAll({ url, firstPartyDomain: "example.com" })).length);
        out.badType = await attempt(() => browser.cookies.getAll({ firstPartyDomain: 5 }));
        out.removeOther = await attempt(() => browser.cookies.remove({ url, name: "fpd", firstPartyDomain: "other.example" }));
        out.removeEmpty = await attempt(() => browser.cookies.remove({ url, name: "fpd", firstPartyDomain: "" }));
        out.removeNull = await attempt(() => browser.cookies.remove({ url, name: "fpd2", firstPartyDomain: null }));
        out.after = await attempt(async () => (await browser.cookies.getAll({ url })).length);
        return out;
    },
};
