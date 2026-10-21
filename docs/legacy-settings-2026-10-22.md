# Legacy font, JavaScript and user-agent settings — 22 October 2026

`BWebSettings` now passes the standard, serif, sans-serif and fixed font
families, standard and fixed font sizes, and JavaScript setting to the engine.
`BWebSettings::Default()` supplies application defaults. A page's own settings
can override individual fields without changing other pages. Setters stage
values; `Apply()` publishes them. Applying defaults does not publish another
page's still-pending edits or replace its committed overrides.

The added nonvirtual method `SetUserAgent(const BString&)` uses the same
default/override rules. An empty value restores the engine's own user agent.
This is an API for applications using the shim; it does not add a user-agent
control to the unmodified WebPositive preferences window. The old public
class layouts and virtual methods remain compatible.

Initial settings are supplied when the engine view is created, before the
web process initializes the page. This also covers the engine's opened-page
request: a popup must receive its script setting and user agent before being
accepted, instead of changing them after its first request or script starts.
Subsequent updates copy the page's preferences before modifying them, since
an opener and popup can initially share a preferences object.

Applying settings updates existing pages through their application-looper
handlers. Navigation also queues the committed settings before the load, so
an immediately following `LoadURL()` does not race a delayed Apply message.
Notifications left in the queue after view destruction cannot use its deleted
engine view. A page's settings are removed from the application registry when
the page is destroyed.

Disabling JavaScript also blocks an existing document's page event handlers.
Enabling it does not replay parser scripts or install inline handlers skipped
while scripting was disabled; reload the document to run those scripts.
Host Inspector evaluation remains available with page scripting disabled.

The engine API is `BWebKitView::SetPageSettings(const BMessage&)`, also accepted
by the new view constructors. Supported fields are `standardFont`, `serifFont`,
`sansSerifFont`, `fixedFont`, `standardFontSize`, `fixedFontSize`,
`javascriptEnabled` and `userAgent`. Unknown fields, duplicate values, wrong
types, invalid UTF-8, empty font names and sizes outside 1–512 CSS pixels
reject the entire update. `B_OK` from a live update means it was queued;
closed views ignore queued changes. Legacy font sizes retain the original
integer conversion of the floating-point setters.

## Native acceptance

The older `958ec59` shim reproduces both original defects on native airOS:
after requesting a 40px default font and disabling JavaScript, the page keeps
the engine's default font size and executes its parser script. The baseline
probe passes 11 assertions describing those missing behaviors, exits zero,
drains its process group without forced cleanup, has no new native crash
events and retains unchanged input/runtime hashes.

The candidate passes **76 native assertions and 14 runner assertions** on
airOS hrev60207+1733. The runner independently checks HTTP user-agent headers,
parser-script requests and native input requests, as well as the selected
engine and shim. The candidate exits zero, drains its process group without
forced cleanup, retains all 78 input/runtime hashes, and has a complete
native crash interval with no new events.

Regressions on the same candidate pass 50 direct-engine certificate assertions,
35 legacy certificate-dialog/lifetime assertions, and 28 HTTP-authentication
assertions, plus six runner assertions each. These three runs also exit zero,
drain without forced cleanup, keep their recorded hashes and have no new
native crash events. They exercise both existing engine constructors and the
legacy host using the new constructors.

The [machine-readable evidence](legacy-settings-2026-10-22.json) includes
the source/build manifest, frozen inputs, all native reports and logs, and
the retained failed attempt.

`tests/LegacySettingsTests.cpp` and `tools/test-legacy-settings.py` cover:

- Initial application defaults and page-specific script/font/user-agent values.
- Live font changes measured against rendered reference spans in the requested
  native font family, including generic serif, sans-serif and monospace.
- Preserving committed overrides and keeping uncommitted edits unapplied.
- Disabling existing page input handlers, enabling scripts for subsequent
  navigation, and checking parser-script execution.
- Independent HTTP user-agent observations at the loopback server.
- New pages and popups receiving application defaults before their first load.
- Popup preference changes leaving the opener's rendering unchanged.
- Invalid engine updates being rejected as a whole, clearing a user-agent
  override, and cleanup with Apply notifications still queued.

The baseline build uses `-DSUMMIT_LEGACY_SETTINGS_BASELINE` and does not call
the new API. Run it with `--expect-unavailable`. The candidate probe requires
the matching new engine and shim. Both use an owned temporary profile and
loopback-only fixtures; no installed browser settings are modified.

The engine is an unprofiled Release airOS build from upstream
`fb054d09146b113aeeabee5f67f4c8b6d9379809`, with patch SHA-256
`d194bf37832f5f1761b8142d0c4075d43019c86ecec05b0c134c03d6624f0856`.
All 1,483 patched source files match the owned build. The deployed engine
library digest is `2b4e01d6ba64b00313781046560bf828599dc4875ef8a40032ead04caf68cb39`;
the shim digest is
`359a14e26f09113760b4e88ad6cf4eca19c2404954c30e481e79f6d74db4a202`.
The engine, shim and probe were cross-compiled with the airOS SDK and then
executed natively. A separate audit matches all 35 frozen input files and
the four deployed engine binaries.

The first candidate attempt incorrectly expected enabling JavaScript to
install an inline handler that parsing had skipped while scripting was
disabled. WebCore's `JSLazyEventListener::create` deliberately returns no
listener in that case. The corrected test checks this behavior explicitly,
navigates with scripting enabled and then verifies the native click handler.
The failed run is retained; it exited normally with no new crash events.

This verifies native `BWebView` use. Installed WebPositive's preferences
window and a matching stock-Haiku engine/shim still need acceptance.

Proxy configuration, per-page storage paths, and persistent host preference
storage are outside this settings change. The host remains responsible for
saving its choices and calling the setters on its next launch.
