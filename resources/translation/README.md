# Local translation resources

`document.js` runs in Summit's persistent isolated content world and retains
original text nodes for restoration. `service.js` runs in a private, hidden
WebKit view and communicates with the upstream translation worker. Website
scripts cannot read either controller's private state.

The three files in `bergamot/` are unmodified from
`@browsermt/bergamot-translator` 0.4.9. `sources.json` records the package
integrity, source revision and file hashes. The MPL-2.0 license is included
there and in `docs/bergamot-license.txt`; `docs/translation-runtime-notices.txt`
contains the corresponding source location and dependency notices. Build
instructions are in the upstream revision's `wasm/module/package.json`.

`models.json` is a pinned copy of the upstream Bergamot model registry.
Summit downloads only a selected language pair's files after the user chooses
Translate. Downloads are verified by size and SHA-256 before use. Translation
runs locally; no page text is sent to the model host.
