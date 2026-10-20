# Native saved network-rule state

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku restores enabled static rulesets from its JSON extension state. Valid
saved booleans override the corresponding manifest defaults. Missing entries,
malformed values and unknown identifiers leave the declared defaults intact.
This preserves unaffected defaults when the saved map contains only a subset
of rulesets. Clearing the state removes the saved map and enabled-ID set.

The restore helper is used by the native context's load method. Cocoa's state
implementation is unchanged. Saving new overrides through the DNR API,
updating installed extensions, translating rules and loading compiled rules
remain separate work.

Evidence:

- `.vm/extension-state-tests.ELwr8PiT/result.json`: all 54 native checks pass.
  Twenty-one added checks cover default/override behavior, malformed and stale
  saved data, unchanged inputs and a native state-file round trip. The existing
  file tests also pass, including 100 atomic replacements with 2,940 concurrent
  reads and no partial objects. The process exits normally with no new crash
  log; sources, native inputs and frozen libraries remain unchanged.
- `.vm/extension-lifecycle-inputs.L8X6nISr/result.json`: the final helper and
  context units compile with both extension features enabled and regenerated
  IPC. Configuration and native input hashes remain unchanged.
- `.vm/extension-rule-state-validation.json`: final source hashes match both
  reports, including the helper shared by the runtime and compile checks.

No extension context was loaded, and no network request was matched or blocked
by an extension. These tests establish state-helper behavior, not DNR runtime
support.

Promoted patch: `c6ab469dacb1003eddb43c03903945104fb676de3ad58b835b6c2eaa69c70e1b`.
Probe baseline: `55798afee3c77e27b9dfc8ab1d46e7833d64568b5b6581a91fab79dfa6f3d5da`.
