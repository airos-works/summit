// Firefox content-script helpers, observed from both sides: this script writes
// what it sees to #cs-results, the page (page.html) writes to #page-results.
const results = {};
const note = (key, value) => { results[key] = value; document.getElementById('cs-results').textContent = JSON.stringify(results, null, 1); };
// Control: an object of this world with a function as a CustomEvent's detail.
document.dispatchEvent(new CustomEvent('ext106-plain', { detail: { a: 1, f() { } } }));
try {
    note('typeof', [typeof cloneInto, typeof exportFunction, typeof createObjectIn].join(','));
    note('enumerable', Object.keys(globalThis).includes('cloneInto'));

    // exportFunction with defineAs on the window.
    exportFunction(function (a, b) { note('addThis', this === window); return a + b; }, window, { defineAs: 'ext106Add' });
    // The page's arguments: a callback (called here), a node (seen as this world's wrapper), an object.
    exportFunction(function (callback, node, object) {
        note('argNodeIsOwnWrapper', node === document.body);
        note('argObject', JSON.stringify(object));
        return callback('from content');
    }, window, { defineAs: 'ext106CallBack' });
    // Results: an object (cloned into the page), a promise, an exception, a node.
    exportFunction(() => ({ list: [1, 2, 3], when: new Date(86400000), nested: { s: 'x' } }), window, { defineAs: 'ext106Object' });
    exportFunction(async (x) => { await null; return { doubled: x * 2 }; }, window, { defineAs: 'ext106Async' });
    exportFunction(() => { throw new TypeError('content failure'); }, window, { defineAs: 'ext106Throw' });
    exportFunction(() => ({ f() { } }), window, { defineAs: 'ext106Uncloneable' });
    exportFunction(() => document.body, window, { defineAs: 'ext106Node' });
    exportFunction(() => browser, window, { defineAs: 'ext106Leak' });
    // Through wrappedJSObject, as Firefox extensions often do.
    window.wrappedJSObject.ext106Wrapped = exportFunction(() => 'via wrappedJSObject', window);
    // createObjectIn + exportFunction onto it.
    const api = createObjectIn(window, { defineAs: 'ext106Api' });
    exportFunction(() => 'api hello', api, { defineAs: 'hello' });
    note('apiIsContentObject', api instanceof Object); // false: it is the page's

    // cloneInto: data and a function for a CustomEvent, as 1Password's Firefox build does.
    const source = { a: 1, nested: { b: [1, 2, { c: 'x' }] }, date: new Date(0), re: /ab+c/gi, map: new Map([[1, 'one']]), fn() { return 'cloned function called'; } };
    const cloned = cloneInto(source, window, { cloneFunctions: true, wrapReflectors: true });
    note('clonedIsCopy', cloned !== source && cloned.nested !== source.nested);
    note('clonedA', cloned.a);
    document.dispatchEvent(new CustomEvent('ext106-clone', { detail: cloned }));
    document.dispatchEvent(new CustomEvent('ext106-init', cloneInto({ detail: { validated: true, origin: location.origin } }, window)));
    // wrapReflectors: a node goes as the page's wrapper.
    document.dispatchEvent(new CustomEvent('ext106-node', { detail: cloneInto({ node: document.body }, window, { wrapReflectors: true }) }));
    // Without the options, functions and nodes are refused.
    try { cloneInto({ f() { } }, window); note('noCloneFunctions', 'no error'); } catch (e) { note('noCloneFunctions', `${e.name}: ${e.message}`); }
    try { cloneInto({ n: document.body }, window); note('noWrapReflectors', 'no error'); } catch (e) { note('noWrapReflectors', `${e.name}: ${e.message}`); }
    try { cloneInto(1, 5); note('badScope', 'no error'); } catch (e) { note('badScope', `${e.name}: ${e.message}`); }
    // Circular references stay circular.
    const ring = { name: 'ring' }; ring.self = ring;
    const ringClone = cloneInto(ring, window);
    note('cycle', ringClone.self === ringClone);
} catch (e) {
    note('error', `${e.name}: ${e.message} ${e.stack}`);
}
document.dispatchEvent(new CustomEvent('ext106-ready'));
