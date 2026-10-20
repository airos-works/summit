# Native stale extension website-data cleanup

> **Historical:** a design note from the porting of WebKit's extension runtime
> (14-16 September 2026), written before any extension ran in Summit. What runs
> today is described in [browser-extensions-default.md](browser-extensions-default.md),
> the `modern-extension-*.md` and `webextensions-*.md` notes, and the issues.

Haiku's persistent extension context now fetches website-data records from the
controller's default data store and applies the existing Cocoa cleanup policy.
It selects origins whose schemes are registered extension schemes and whose
lowercased scheme/host/port is absent from the controller's active-origin set.
That set includes loaded contexts and readable saved extension state.

The record-selection loop is shared with Cocoa, with only indentation changed.
Selected records contain only stale extension origins and the original data-type
mask. Cookie hosts, HSTS hosts and other host/domain metadata from a mixed record
are not copied into the deletion request. The native fetch mask matches the
current public/private Cocoa website-data types, including their feature guards.

Cleanup preserves the upstream once-per-controller gate and persistent sentinel.
It writes the sentinel after finding no stale records or after deletion completes.
This does not strengthen the upstream saved-state recovery policy: unreadable
state can omit an installed origin, and an aborted asynchronous cleanup does not
retry in the same controller. Those cases need integration tests and review
before enabling extension installation in the browser.

Evidence:

- `.vm/extension-website-data-tests.iDAnGqzh/result.json`: all 29 native checks
  pass using actual WebKit scheme registration, WebCore origin conversion and
  the shared selector. Coverage includes mixed records, ordinary websites,
  exact origin/port matching, custom scheme changes and unchanged inputs. The
  process exits normally with no new debugger events. Sources, native inputs
  and frozen JavaScriptCore/WTF libraries remain unchanged.
- `.vm/extension-lifecycle-inputs.HnCbjrQc/result.json`: the final selector
  compiles. The context initially failed for a missing fetch-option header;
  its corrected final source passes in
  `.vm/extension-lifecycle-inputs.hSPSsY89/result.json`. Both extension flags are
  enabled in isolated configurations with regenerated IPC.
- `.vm/extension-website-data-validation.json`: final source hashes match the
  relevant integration units and the successful runtime probe.

The isolated runtime probe hides WebCore exports to discard unused functions
when linking Haiku's shared-ELF executable. It links frozen JavaScriptCore/WTF,
not WebCore or WebKit libraries. Initial harness failures are retained: native
header shadowing, unwanted exported dependencies, and an invalid test that
inserted the reserved empty-origin hash sentinel. None counts as a passing run.

No website-data store, extension context, fetch/delete IPC or sentinel behavior
was executed. No extension was loaded and no user data was deleted. Cocoa's
project entries are updated, but a Cocoa build was not run.

Promoted patch: `adb8b391c6e42031b5ccd084b4898216e9634bfc929214306c7a58f63ec23ac6`.
Probe baseline: `c6ab469dacb1003eddb43c03903945104fb676de3ad58b835b6c2eaa69c70e1b`.
