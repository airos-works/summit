# Native runtime namespace bindings

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Both extension and webpage namespace IDLs still marked `runtime` as
Cocoa-only after the runtime implementation had been ported. The webpage
namespace also guarded its C++ getter and backing member. These gates are
removed. The existing runtime implementations and namespace permission
checks are retained.

Evidence:

- `.vm/extension-bindings-generated-06sk10v5/binding-generation.json`:
  the upstream generator produces all 37 interface/header pairs with
  unchanged inputs. Generated extension namespace enumeration, property
  lookup and the runtime getter no longer have the Cocoa gate. The webpage
  namespace includes its static runtime property and getter.
- `.vm/extension-lifecycle-inputs.WFdJ7c4G/result.json`: both generated
  namespace units and both actual C++ namespace units compile natively with
  both extension flags enabled and regenerated IPC. Native inputs and
  configuration remain unchanged; final source/header hashes match.
- `.vm/extension-runtime-namespaces-native-symbols.json`: both compiled
  objects define their runtime getter. Neither getter exists in the previous
  full build's unified binding object. New object hashes match the compile
  report, and all audited objects remain unchanged.
- `tools/test-engine-extension-lifecycle-compile.py` now accepts the two
  namespace binding units and the webpage namespace implementation.

These checks do not execute `browser.runtime`, external messaging, background
startup or a loaded extension. Cocoa compilation has not been run. Full
extension linking, native delegates and several API families remain incomplete.

Promoted patch: `75a0dfd1b1212c05e00e58ec22cd613289a90ac45284517fe47803024adf3b70`.
Probe baseline: `b85b0ea4c1b19d2bf6b0c421e1916c5a7d90e6f183f0888f1995bd4320763101`.
