# Firefox content-script helpers: cloneInto, exportFunction, createObjectIn

29 October 2026, issue #106. Engine: `WebExtensionControllerProxy.cpp`
(`installFirefoxContentHelpersHaiku`, next to `wrappedJSObject`).

Firefox gives content scripts `cloneInto(obj, targetScope, options)`,
`exportFunction(func, targetScope, options)` and
`createObjectIn(targetScope, options)`. Extensions built for Firefox use them
to hand values and functions to the page. 1Password's Firefox build tests
`typeof cloneInto == "function"` and, when it is there, sends its
CustomEvents' init dictionaries through
`cloneInto(x, window, {cloneFunctions: true, wrapReflectors: true})`.

Summit now gives every extension's content scripts these three functions
(non-enumerable globals, like Firefox's). `SUMMIT_FIREFOX_CONTENT_HELPERS=0`
leaves them out.

## How they work in WebKit

Firefox's content scripts see the page through Xray wrappers, in a compartment
of their own. WebKit runs content scripts in an isolated world of the same JS
VM: both worlds share the DOM, each has its own wrappers and its own
`Object`, `Function` and so on. A JS object of one world can be handed to the
other in C++. Two rules follow:

- A page-world object may reach the content script. This is what Firefox's
  waived Xrays (`wrappedJSObject`) give too; the content script is trusting
  the page's code when it touches it.
- An extension-world object must never reach the page. From it the page could
  walk `constructor.constructor` to the extension world's `Function` and run
  code where `browser.*` is. Everything below keeps to this.

`targetScope` names a world, as in Firefox it names a compartment: a window,
a document or another node, or `window.wrappedJSObject`, mean the page's
world of that frame; any other object means its own world.

### cloneInto(obj, targetScope, {cloneFunctions, wrapReflectors})

Builds the copy in the target world (the page's `Object.prototype`,
`Array.prototype`, `Date` ...):

- plain objects (own enumerable string keys) and arrays (holes kept) are
  copied member by member; shared and circular references stay shared;
- functions: with `cloneFunctions`, exported functions (below); without,
  `DataCloneError: Function object could not be cloned.`;
- DOM nodes and windows: with `wrapReflectors`, the target world's wrapper of
  the same node or window; without, a `DataCloneError`;
- anything else (Date, RegExp, Map, Set, ArrayBuffer and views, Error, Blob,
  File, ImageData ...) goes through WebCore's structured clone
  (`SerializedScriptValue`, `CloneAcrossWorlds`), deserialised in the target
  world. Functions inside those are not cloned.

Why it matters even without functions: a CustomEvent's `detail` made in the
content script is structured-cloned when the page reads it
(`cloneAcrossWorlds`), and becomes `null` if that fails (a function inside,
for instance). A `detail` made by `cloneInto` is already in the page's world
and is passed as it is, functions included.

### exportFunction(func, targetScope, {defineAs})

Makes a real function of the target world (a `JSNativeStdFunction`: the
page's `Function.prototype`, so `call`, `apply` and `bind` work) that calls
`func`. With `defineAs` it is defined (writable, enumerable, configurable) on
the page's window, on the page's wrapper of a node, or on the target object
(for instance one from `createObjectIn`). It is returned in any case, so
`window.wrappedJSObject.name = exportFunction(f, window)` works too: the
`wrappedJSObject` setter now also accepts page-world objects (before, plain
values only).

When the page calls it:

- arguments and `this` reach `func` as they are (page objects stay page
  objects, as through waived Xrays), except DOM nodes and windows, which
  arrive as the content script's own wrappers (`node === document.body` holds
  in the content script);
- the result: primitives and page-world objects as they are; an
  extension-world object is cloned into the page (no functions; DOM objects as
  the page's wrappers); a promise gives the page a page promise that settles
  with the cloned value;
- an exception reaches the page as a page `Error` with the same message
  (primitives as they are); a result that cannot be cloned throws the clone
  error the same way.

### createObjectIn(targetScope, {defineAs})

An empty object of the target world, defined on the target as `defineAs`
says.

## Differences from Firefox

- No Xrays: the content script sees page objects (arguments, `cloneInto`
  results) with the page's own getters, setters and prototypes, as with
  Firefox's `wrappedJSObject`. Firefox would show it only the objects' own
  data properties.
- An extension-world object an exported function returns is cloned for the
  page (the same values, no functions); in Firefox the page gets a wrapper
  whose access depends on Xray rules. Returning a function throws in the
  page.
- `allowCrossOriginArguments` is ignored: arguments pass as they are.
- `exportFunction` into a scope that is not the page's (an extension-world
  object) also works, and is pointless.
- They exist for every extension, not only Firefox packages: the engine does
  not know where a package came from. A Chrome extension that tests
  `typeof cloneInto` to detect Firefox will think it runs in Firefox.
- `wrappedJSObject`'s getter still returns plain values only, so
  `window.wrappedJSObject.someObject` stays `undefined`.

## Checked

Laptop, 29 Oct, engine `ext106a` with an app built against it, the ext106
fixture (`page.html` plus an MV2 extension whose content script runs
`content.js`). What each side wrote into the page:

- typeof `cloneInto`/`exportFunction`/`createObjectIn`: `function` x3, not
  enumerable. Stock Summit: `undefined`, and the content script stopped at
  its first `exportFunction` (ReferenceError).
- `cloneInto({a, nested: {b: [1, 2, {c}]}, date, re, map, fn}, window,
  {cloneFunctions: true})` as a CustomEvent `detail`: the page saw page
  objects (`instanceof Object`/`Array`/`Date`/`RegExp`/`Map` true) with the
  same values, and `detail.fn()` returned the content function's result. The
  same object without `cloneInto` arrived as `null`.
- `new CustomEvent(type, cloneInto({detail: {...}}, window))` (1Password's
  form): the page read the detail.
- `cloneInto({node: document.body}, window, {wrapReflectors: true})`: the
  page's `detail.node === document.body`. Without the options: `DataCloneError`
  for a function and for a node. A circular object stayed circular.
- `exportFunction(..., window, {defineAs})`: `ext106Add(2, 3)` 5 (with `this`
  the content script's window); a page callback passed in was called; a
  node argument was the content script's `document.body`; an object result
  was a page object with a page `Date`; an `async` function gave a page
  `Promise` resolving to `{doubled: 42}`; a thrown `TypeError` arrived as a
  page `Error` "content failure"; returning `{f() {}}` threw "Function object
  could not be cloned." in the page; returning `browser` threw "The object can
  not be cloned." in the page.
- `window.wrappedJSObject.x = exportFunction(...)` and
  `createObjectIn(window, {defineAs})` + `exportFunction(..., obj, {defineAs:
  'hello'})` were callable from the page.
- From the page, `ext106Add.constructor.constructor('return typeof
  browser.storage ...')()` ran in the page's world (`undefined`; the page's
  `Function`).
- Cost: about 0.8 µs a call for `ext106Add` (100,000 calls from the page) and
  1 µs for a call returning a small object to clone.
- `SUMMIT_FIREFOX_CONTENT_HELPERS=0`: `undefined` again.

Not yet checked with 1Password's or another real Firefox extension's content
scripts.
