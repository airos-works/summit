# Passkeys and Web Authentication in Summit

## Why the API is built at all

KunanyiOS has no platform authenticator of its own: no biometric credential
store, and no CTAP stack for security keys. Summit builds the Web
Authentication API for three reasons: password managers provide passkeys
through it from their content scripts, since October 2026 (#111) Summit
answers it itself with passkeys it keeps in the system key store, and it
talks to USB security keys (CTAP2 and U2F over HID, below).

1Password's page script replaces `navigator.credentials.create`,
`navigator.credentials.get` and the static methods on `PublicKeyCredential`
(`isUserVerifyingPlatformAuthenticatorAvailable`,
`isConditionalMediationAvailable`, `getClientCapabilities`), and returns
credentials whose prototype it sets to `PublicKeyCredential.prototype`. None of
that is possible when the interfaces are absent: the hooks cannot attach, and
the extension reports *"WebAuthn isn't supported. Use a different browser or
device to use your passkey."* Relying parties check the same interfaces before
offering passkey sign-in.

The engine was built with `ENABLE_WEB_AUTHN` off, so those interfaces did not
exist. It is now on for the Haiku port. WebKit's WebAuthn module (the IDL
interfaces, CBOR, FIDO structures) is portable C++; only the authenticators are
Apple-specific, and Summit has its own (below).

## Summit's own passkeys (30 October 2026, #111)

A page's `navigator.credentials.create({publicKey})` or
`navigator.credentials.get({publicKey})` that reaches the browser is answered
by a platform authenticator built into the engine
(`UIProcess/WebAuthentication/haiku/WebAuthenticatorCoordinatorProxyHaiku.cpp`,
modelled on Apple's `LocalAuthenticator`). The browser asks the user first, in
a sheet over the window (`src/ui/PasskeyPrompt.cpp`):

| Request | The sheet | Confirming | Declining or closing |
| --- | --- | --- | --- |
| `create()` | "Create a passkey for *site*?" with the account the site names; notes when it replaces the account's saved passkey, and in a private window that the passkey outlives the window | A new passkey; the page gets its attestation | `NotAllowedError` |
| `create()` naming a saved passkey in `excludeCredentials` | "A passkey for *site* is already saved" | `InvalidStateError` (after the user saw it, as the spec asks) | `NotAllowedError` |
| `get()` | "Sign in to *site* with a passkey?" with the account, or an Account menu when there are several | The chosen passkey signs; the page gets the assertion | `NotAllowedError` |
| `get()` with no suitable passkey | "No passkey for *site*" | `NotAllowedError` | `NotAllowedError` |

The sheet also says which page asks when it is a subdomain of the site the
passkey is for. A `get()` with `mediation: "conditional"` gets no sheet: its
passkeys are offered in the sign-in field's autofill (below).

What the authenticator does:

- **Keys.** ES256 (ECDSA P-256 with SHA-256) only, made with OpenSSL
  (`EVP_PKEY_Q_keygen`); a site whose `pubKeyCredParams` has no `-7` gets
  `NotSupportedError`. The credential ID is 16 random bytes. Signatures are DER,
  over `authenticatorData || SHA-256(clientDataJSON)`.
- **Attestation** is always `"none"`, with a zero AAGUID.
- **Authenticator data.** The RP ID hash, then the flags: UP and UV always,
  AT on creation; BE and BS are clear, because the passkey never leaves this
  computer. The signature counter is always 0, as for synced passkeys (a
  counter that never moves tells the site not to rely on it).
- **User verification** is the user's confirmation in the browser's own
  sheet, or the choice of a passkey in its autofill list, neither of which a
  page can draw or click. That is how the UV flag is earned; there is no PIN
  or biometric step.
- **Discoverable credentials.** Every passkey is discoverable: it keeps the
  user handle, name and display name, so `get()` without `allowCredentials`
  (username-less sign-in) offers all of the site's passkeys, and
  `credProps.rk` is `true`. A site keeps one passkey per account (user
  handle): a new one replaces the old.
- **Allow and exclude lists** are honoured by credential ID. Entries for other
  authenticators (security keys, phones) simply do not match; security keys
  are asked as well (below).
- **clientDataJSON** is WebKit's `buildClientDataJson` from the frame's
  origin; the RP ID check is WebKit's
  (`SecurityOrigin::isMatchingRegistrableDomainSuffix`, after the web process's
  own checks: secure context, no IP addresses, `user.id` 1-64 bytes), and
  when that fails, the site's list of related origins (below). A `get()` from
  a frame of another origin than the page carries `crossOrigin: true` and
  `topOrigin` (the page's origin) in clientDataJSON (below).
- **Timeouts and aborts.** The page's `timeout` is honoured between 30 s and
  10 minutes (5 minutes without one); an `AbortSignal` or the timeout closes the
  sheet. A request whose key is already being made or used completes.
- **Availability.** `PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable()`
  and `isConditionalMediationAvailable()` are `true`, and
  `getClientCapabilities()` reports `passkeyPlatformAuthenticator`,
  `userVerifyingPlatformAuthenticator`, `conditionalGet`, `conditionalCreate`,
  `relatedOrigins`, the three signals and `extension:prf`,
  `extension:largeBlob` and `extension:credProps`.
- **Extensions** (below): `credProps`, `prf` and `largeBlob`.

### Passkey autofill (conditional mediation, 30 October 2026)

A page that calls `navigator.credentials.get({mediation: "conditional",
publicKey})` (webauthn.io and most passkey sites do at load) gets its
passkeys offered in the autofill of its sign-in field, as other browsers do:

- The engine looks up the site's passkeys (allow list and hidden ones
  applied) and tells the window (`B_WEBKIT_PASSKEY_REQUESTED` with
  `conditional`); the request then waits, without a timeout.
- A page script (`kPasskeyFieldScript`, main frame only) reports where a field
  marked `autocomplete="… webauthn"` is when the user goes into it or clicks
  it. The window opens a list under it (`PasskeyAutofill`): a borderless window
  that takes no focus, so the field keeps the caret and typing goes on. Each
  row is an account and "Passkey saved in Summit".
- A click on a row signs with that passkey and completes the page's `get()`.
  The page only says where its field is; it cannot open, read or click the
  list, which is the browser's.
- The list closes when the field loses focus, the page scrolls or resizes, the
  tab or window changes, or the request ends. The request ends when a passkey
  is chosen, the page aborts it (`AbortError`; webauthn.io aborts it before a
  modal `get()`), the frame goes to another document (the engine withdraws it
  on commit), the tab closes, or the page process goes.
- No passkeys for the site: nothing is shown and the request just waits.

For tests, the list takes `hey Summit 'ALTB' of Window N with
account=int32(K)` (the window titled "Passkeys").

### Related origins (WebAuthn Level 3 §5.11, 30 October 2026)

A page may use an RP ID that is not its registrable domain or a suffix of it
when the RP ID's site lists the page's origin at
`https://<rp id>/.well-known/webauthn` (`{"origins": [...]}`): Microsoft's
`login.microsoft.com` lists `https://login.live.com`, for example. When
WebKit's RP ID rule fails, the coordinator fetches that list and looks for the
caller's origin with WebCore's `findOriginInWellKnownList` (upstream's: at most
five registrable domains, `eTLD+1` labels, are looked at, 100 entries, 64 KiB;
the response must be `200` with `application/json`). The caller must be a
secure origin and the RP ID a domain (no IP address, port or path).

Upstream fetches the list through the network process
(`WellKnownResourceFetcher`), but the curl network session has no data tasks
(`NetworkSession::dataTaskWithRequest` does nothing there and never answers),
so the Haiku coordinator fetches it itself with libcurl on a queue of its own:
https only, redirects too (at most 5), no cookies or credentials, the page's
user agent, 10 seconds. It does not go through Summit's proxy settings.
The browser's stderr says what came of it ("Summit passkeys: related origins
of login.microsoft.com for https://login.live.com: listed", "not listed",
"HTTP 404, text/html", or curl's error). Not listed or not fetched:
`SecurityError`, as before. An abort while the list is fetched ends the
request at once (`AbortError`). The sheet then says which page asks ("Asked
by login.live.com"). `SUMMIT_RELATED_ORIGINS=0` turns this off (and
`relatedOrigins` is reported `false`).

### Conditional create: a passkey after a password sign-in (30 October 2026)

`navigator.credentials.create({mediation: "conditional", publicKey})` is how
a site adds a passkey to an account right after the user signed in with a
password (Chrome's "automatic passkey upgrade"). Summit makes one without a
prompt only when all of these hold, and otherwise declines quietly with
`NotAllowedError` ("No passkey was created."), as the spec asks, so that a page
learns nothing:

- The user signed in to the site, in this window and within the last 5
  minutes, with a password Summit's password manager knows: a saved password
  submitted unchanged (autofilled or typed), or one just saved from the
  password bar. The sign-in's host must be the RP ID or under it, and its
  account name the `user.name` of the request (ignoring case). Each sign-in
  allows one passkey. Private windows neither save nor fill passwords, so
  they never get one.
- The account (user handle) has no passkey in Summit yet, and none of the
  site's `excludeCredentials` is saved here: a conditional create never
  replaces a passkey.
- The site does not require user verification (`userVerification:
  "required"` is declined).

The passkey's authenticator data then has neither UP nor UV set (flags `0x40`:
AT only), as the spec has it for conditional creation: there was no gesture or
confirmation for this passkey. The window's status line says what happened
("Summit saved a passkey for *site* (*account*) after your password sign-in.
Preferences › Browsing › Passwords lists it."), where it can be removed. The
engine asks the host (`B_WEBKIT_PASSKEY_REQUESTED` with `create` and
`conditional`), which answers at once (`BrowserWindow::ConditionalCreateRequested`,
`TakePasswordSignIn`). `SUMMIT_PASSKEY_CONDITIONAL_CREATE=0` declines them all
(and `conditionalCreate` is reported `false`).

### Frames of another origin (30 October 2026)

A `get()` from a frame of another origin than its ancestors is allowed when
the page lets the frame use passkeys (`<iframe allow="publickey-credentials-get">`;
the web process checks the permissions policy and declines otherwise with
`NotAllowedError`). The browser checks that the parent origin the web
process names is one of the frame's ancestors (upstream's check), and builds
clientDataJSON with `crossOrigin: true` and `topOrigin`, the main frame's
origin as the browser knows it. The sheet names both ("Asked by
localhost:8001, in a frame of 127.0.0.1:8000"; `top_origin` in
`B_WEBKIT_PASSKEY_REQUESTED`). `create()` in such a frame is still declined
by the web process (WebKit implements Level 2's rule there).
`SUMMIT_PASSKEY_FRAMES=0` declines frames of another origin again.

Passkey autofill works in frames too: `kPasskeyFieldScript` runs in every
frame. A frame asks its parent where it is (`window.postMessage` with a
`__summitPasskeyFrame` member, answered by the same script in the parent,
which adds the frame element's content box and asks its own parent, up to the
page), and reports its field in the page's coordinates. The script stops
those messages from reaching the page's own listeners. The window offers the
passkeys of the conditional request that the field's frame origin made, so a
sign-in frame of another site offers that site's passkeys. An embedding page
could only misplace the list by answering for its frame, never choose in it.

### Extensions: prf and largeBlob

- **prf** (`extensions: {prf: {eval: {first, second?}, evalByCredential}}`):
  every passkey made from now on has a 32-byte PRF secret in its record
  (`prf_secret`). An output is HMAC-SHA-256(secret, SHA-256("WebAuthn PRF" ||
  0x00 || input)), the salt construction CTAP2's hmac-secret uses. On
  `create()` the result is `prf: {enabled: true}`, with `results` at once when
  `eval` is given; on `get()`, `results.first` (and `second`) for the chosen
  passkey, from its `evalByCredential` entry if there is one, else `eval`. The
  client checks: `evalByCredential` without `allowCredentials` →
  `NotSupportedError`; an entry naming a credential not in `allowCredentials`
  (or not base64url) → `SyntaxError`; on `create()` → `NotSupportedError`.
  Passkeys made before PRF have no secret and give no `prf` output.
- **largeBlob**: `create()` with `largeBlob.support` reports `supported: true`.
  `get()` with `read: true` returns the passkey's blob (absent when there is
  none); `write` needs exactly one credential in `allowCredentials`
  (`NotSupportedError` otherwise), is stored only after that passkey signed,
  and reports `written` (false over 16 KiB). The blob is the record's
  `large_blob`, rewritten in the key store.

### Signals from the site

The signals change Summit's passkeys only for the site the page shows: the
origin the web process names must be a secure one, of the main frame's site,
and the RP ID must be its registrable domain or a suffix (WebCore checks the
RP ID too). The page's promise resolves at once, and the key store changes
after, on the passkey queue.

| Signal | What Summit does |
| --- | --- |
| `signalUnknownCredential({rpId, credentialId})` | Removes that passkey: the site does not know it (a sign-in with it failed) |
| `signalAllAcceptedCredentials({rpId, userId, allAcceptedCredentialIds})` | The account's passkeys missing from the list are **hidden**, not removed (a site's mistake, such as an empty list, would otherwise lose them): not offered or matched, and listed in Preferences as "passkey, hidden by the site". Hidden ones the site lists again come back. A malformed ID makes the whole list ignored. |
| `signalCurrentUserDetails({rpId, userId, name, displayName})` | Renames the account's passkeys |

When something changed, the window's status line says so ("The site
webauthn.io hid 1 passkey (Preferences › Browsing › Passwords).",
`B_WEBKIT_PASSKEYS_CHANGED`).

### Where the passkeys are kept

In the Haiku key store (keystore_server), keyring **"Summit passkeys"**, one
generic key per passkey (purpose `B_KEY_PURPOSE_WEB`): identifier = RP ID,
secondary identifier = credential ID in hexadecimal, data = a flattened
`BMessage` with `version`, `algorithm` (-7), `credential_id`, `user_handle`,
`user_name`, `display_name`, `origin` (the page that made it), `created` and
`private_key` (ECPrivateKey DER), and since PRF `prf_secret`; `large_blob` and
`hidden` when set. Nothing is written to Summit's profile. The key store has
no update: a changed passkey (blob, hidden, names) is removed and added again,
and put back as it was if adding fails.

The engine reads and writes the keyring on a queue of its own, never the main
thread, because keystore_server asks the user the first time Summit uses the
keyring for each kind of access ("Application keyring access": adding keys,
listing them, removing them; *Allow always* remembers the answer for that
Summit binary). Reading always lists the keyring rather than fetching one key,
so that using passkeys needs one approval, not two.

**Preferences › Browsing › Passwords** lists Summit's passkeys with the saved
passwords ("*account* — passkey"); Show says what it is, and Remove deletes
it. The private key is never shown or exported.

### Security model

- Private keys and PRF secrets exist only in the key store and, briefly, in
  the browser process while a key is made or a signature or PRF output
  computed; those copies are cleared (`OPENSSL_cleanse`). Web processes never
  see them: they get the public key, the attestation, signatures and PRF
  outputs.
- The key store is what protects them at rest, exactly as for Summit's saved
  passwords (#67). Haiku's keystore_server does not encrypt keyrings yet (its
  `_EncryptToFlatBuffer` is a TODO), so
  `~/config/settings/system/keystore/keystore_database` holds them readable to
  anything running as the user. A keyring unlock password (keystore_server
  supports one) is the place to add protection; Summit does not set one.
- Which site may use a passkey is WebKit's RP ID rule, checked in the browser
  process against the frame's real origin (a web process that lies about its
  frame's origin is marked invalid).

### Password managers first

A password manager extension (1Password) replaces `navigator.credentials` in
the page, so it sees every request first and Summit's authenticator is never
asked. Summit answers what no extension took: no extension is installed or
allowed (in a private window, one installed without private-window access), or
the extension handed the request back to the browser (1Password's "use another
device or passkey" path calls the original API). That order needs no
arbitration in the engine and matches other browsers, where the extension's
UI comes first and the platform's is the fallback.

Checked with 1Password 8.12.41.1 (30 October 2026, laptop, the owner's
extension package copied into a fresh test profile, never signed in; no
desktop app, so `NativeHostNotFound`): on https://webauthn.io its page script
had replaced `navigator.credentials.get`/`create`,
`isConditionalMediationAvailable` and `getClientCapabilities` (their source is
1Password's, not `[native code]`; its capabilities say `conditionalCreate:
false`, `hybridTransport: true`, the signals `false`).

- The page's conditional `get()` at load went to 1Password first
  ("[Webauthn] Could not complete _handleGetCredential: no-available-accounts")
  and was then handed to the browser: Summit's autofill offered its passkeys.
- A modal `get()`: 1Password again found no accounts and handed it on, and
  only then did Summit's sheet open ("Sign in to webauthn.io with a passkey").
- A `create()`: 1Password showed its own "Save passkey? Select the 1Password
  icon in your browser's toolbar to unlock" in the page and Summit showed
  nothing. Closing 1Password's panel ("_handleCreateCredential while locked:
  disconnected") handed the request to Summit, whose sheet then opened.

So the two never ask at once: whatever 1Password takes stays with it, and
Summit's sheet appears only for what it gives back. Signing in with a passkey
from an unlocked vault still needs the owner.

### Turning it off and the API

`SUMMIT_PLATFORM_PASSKEYS=0` turns the authenticator off: requests are then
declined with `NotAllowedError` as before, `isUserVerifyingPlatformAuthenticatorAvailable()`
is `false`, and the window's status line explains (below).

Hosts opt in per view with `BWebKitView::SetPasskeyRequests(true)` (Summit does
for every page view, not extension pages). The engine then sends
`B_WEBKIT_PASSKEY_REQUESTED` ('wpky') with `view`, `identifier`, `create`,
`conditional`, `rp_id`, `origin`, and either `user_name`, `user_display_name`, `exists`,
`replaces` (create) or one `account_name`/`account_display_name` per passkey
(get); the host answers with `BWebKitView::AnswerPasskeyRequest(identifier,
account)` (the passkey's index, 0 to create, negative to decline). A request
that goes away first is followed by a notice with `withdrawn`. Signals that
changed passkeys send `B_WEBKIT_PASSKEYS_CHANGED` ('wpkc') with `rp_id` and
the counts `removed`, `hidden`, `restored`, `renamed`. A view that did not
opt in behaves as before #111.

The sheet takes BAlert's scripting message, for tests: `hey Summit 'ALTB' of
Window N with which=int32(1)` confirms (`which=int32(0)` declines), and
`account=int32(K)` chooses the K-th passkey.

### Verified (30 October 2026, laptop, engine passkey2/passkey3 + app)

On https://webauthn.io, profile `passkey-prof`:

- Default settings (discoverable *preferred*, UV *preferred*), user
  `summit-passkey-1`: Register → sheet → Create → *"Success! Now try to
  authenticate..."*; Authenticate (the site sends `allowCredentials` with the
  new ID, transports `internal`) → sheet → Sign In → *"You're logged in!"*.
- Discoverable *required*, UV *required*, attachment *platform*, user
  `summit-passkey-2`: Register → *"Success!"*; Authenticate with the username
  empty → the sheet offered three passkeys in an Account menu → the third →
  *"You're logged in!"*.
- From the page with script (`navigator.credentials` on webauthn.io):
  `create()` returned type `public-key`, attachment `platform`, algorithm -7,
  transports `["internal"]`, `credProps {rk: true}`, flags `0x45` (UP, UV,
  AT), counter 0, 148-byte authenticator data; `get()` returned flags `0x05`,
  the user handle, and a signature that WebCrypto verified (`verified: true`)
  with the public key from `getPublicKey()`; the RP ID hash matched.
  Choosing the third account returned its user handle
  (`webauthnio-summit-passkey-2`).
- Cancel → `NotAllowedError: The user declined.`; an excluded credential →
  "already saved" sheet → `InvalidStateError`; an unknown `allowCredentials`
  → "No passkey" sheet → `NotAllowedError`; creating again for the same user
  handle → "It replaces..." → the old key was removed (Preferences listed
  three passkeys, not four); `AbortController.abort()` after 3 s closed the
  sheet and the page got `AbortError`.
- Key store: keyring "Summit passkeys" present in `keystore_database` with the
  three passkeys; keystore_server asked twice on the first creation (listing
  and adding keys) and once on the first replacement (removing), and not
  again.
- Repeated on engine passkey3 (requests stay on the main thread while the key
  queue works): webauthn.io register and sign-in as `summit-passkey-3`, and the
  scripted `create()` + `get()` with a verified signature.
- `SUMMIT_PLATFORM_PASSKEYS=0`: `isUserVerifyingPlatformAuthenticatorAvailable()`
  `false`, capabilities all `false`, `create()` → `NotAllowedError: No
  authenticator is available on this device.` and the status line notice.

### Verified (30 October 2026, second round, engine passkey4 + app)

On the laptop, https://webauthn.io, the same profile:

- `isConditionalMediationAvailable()` → `true`; `getClientCapabilities()`
  lists `conditionalGet`, the signals and the extensions as `true`.
- **Autofill.** At load webauthn.io made a conditional `get()` ("offers 5
  passkeys … in its sign-in field"). A click in the username field opened the
  list under it (five passkeys, the field kept focus and caret); choosing
  `summit-passkey-3` → *"You're logged in!"* (`/profile`). The choice was sent
  to the list with `hey` (no pointer input on the laptop). Choosing an account
  other than the username typed gave the site's own "Credential does not
  belong to user", and two webauthn.io tabs loading at once gave its "Client
  data challenge was not expected challenge" (one challenge per session):
  both are the site's checks.
- Going to another page while the list was open: "passkey request 4
  withdrawn" and the list closed. `AbortController.abort()` on a conditional
  `get()` → `AbortError`. Quitting with the list open quit normally.
- **prf/largeBlob** (script on webauthn.io): `create()` with `prf.eval.first =
  "one"` and `largeBlob.support` → `prf.enabled: true`, `results.first`
  `gz00rtIx…`, `largeBlob.supported: true`, `credProps.rk: true`; `get()` with
  `eval {first: "one", second: "two"}` and `largeBlob.write` → the same
  `first` (`gz00rtIx…`), a different `second`, `written: true`; `get()` with
  `evalByCredential` for that passkey (`"other"`) and `read` → a different
  `first` and the blob back ("hello blob from Summit"). The three client
  checks gave `NotSupportedError`, `SyntaxError` and `NotSupportedError`. A
  passkey made before PRF gave no `prf` output.
- **Signals**: `signalCurrentUserDetails` → "renamed the account of 1
  passkey"; `signalAllAcceptedCredentials` with an empty list → "hid 1
  passkey", after which a `get()` naming it got the "No passkey" sheet and the
  autofill offered 5 instead of 6; listing it again → "showed again 1 passkey"
  and it signed, its blob intact; `signalUnknownCredential` → "removed 1
  passkey" and its record was gone from `keystore_database`; an `rpId` of
  another site → `SecurityError` (WebCore's check). No new keystore_server
  questions (removing was allowed in the first round).
- An earlier build used a native `BPopUpMenu` for the list; menus nobody
  clicked away kept the window's close lock and quitting hung, so the list is
  a window Summit closes itself.

### Verified (30 October 2026, third round, engine pk111c + app)

On the laptop, with local pages served by `python3 -m http.server` on
`127.0.0.1:8000` and `:8001` (`http://localhost` and `http://127.0.0.1` are
secure contexts; the fixtures are a page with a cross-site frame, the frame
with a `webauthn` field, and a password form):

- **Related origins.** On https://login.live.com the site's own conditional
  `get()` for RP ID `login.microsoft.com` passed ("related origins of
  login.microsoft.com for https://login.live.com: listed"); a scripted `get()`
  opened "No passkey for login.microsoft.com — Asked by login.live.com", and
  a `create()` "Create a passkey for login.microsoft.com?" (cancelled). From
  https://example.com the same RP ID gave `SecurityError` after 113 ms ("not
  listed"); `webauthn.io` (HTTP 404, text/html) after 568 ms; `localhost`
  after the 10 s timeout, or `AbortError` 1001 ms after the page aborted at
  1 s.
- **Frames.** `http://127.0.0.1:8000` embedding `http://localhost:8001` with
  `allow="publickey-credentials-get"`: the frame's conditional `get()`
  reached the window, a click in the frame's field opened the list right
  under that field, and a real click on the passkey signed: flags `0x05`,
  clientDataJSON `{"origin":"http://localhost:8001","crossOrigin":true,
  "topOrigin":"http://127.0.0.1:8000"}`. A modal `get()` from the frame showed
  "Asked by localhost:8001, in a frame of 127.0.0.1:8000". Without `allow`:
  `NotAllowedError` from the web process.
- **Conditional create.** Without a sign-in → `NotAllowedError`; after typing
  a new password and submitting (not yet saved) → `NotAllowedError`; after
  Save in the password bar → a passkey, flags `0x40`, and the status line
  notice; again for the same account → `NotAllowedError` (it has one); with
  `userVerification: "required"` → `NotAllowedError`. Then, with that passkey
  removed (`signalUnknownCredential`), the saved password autofilled and
  submitted unchanged → a passkey again.
- **Regression** on https://webauthn.io: `create()` (flags `0x45`, -7) and a
  `get()` whose signature WebCrypto verified (`crossOrigin` absent); the
  main-frame autofill list under the field as before.
- **Switches.** With `SUMMIT_RELATED_ORIGINS=0 SUMMIT_PASSKEY_CONDITIONAL_CREATE=0
  SUMMIT_PASSKEY_FRAMES=0`: the frame's `get()` → `NotAllowedError`,
  capabilities `conditionalCreate`/`relatedOrigins` `false`, login.live.com's
  related RP ID → `SecurityError` in 1 ms, conditional create →
  `NotAllowedError`.
- keystore_server asked for the test binary's access to the "Summit
  passkeys" keyring (listing, adding, removing) and to "Summit" (passwords),
  once each. The test passkeys and the test password were removed afterwards.

## Status (September 20, 2026)

Verified on `bundle-dxx4tx8e`:

- The API surface is present. `tests/fixtures/webauthn/support.html` reports
  every entry point, including both response interfaces
  ([screenshot](screenshots/webauthn-surface.png)). The browser's own answers
  were then `false` for a platform authenticator and for conditional
  mediation, and `getClientCapabilities()` returned nine capabilities, all
  unsupported.
- 1Password offers a passkey. On github.com and on Google's sign-in page its
  hooks install and it shows *"Sign in with a passkey — Select the 1Password
  icon in your browser's toolbar to unlock"*
  ([screenshot](screenshots/1password-passkey-prompt.png)). The previous
  "WebAuthn isn't supported" error is gone.
- Completing a passkey sign-in needs the vault unlocked, which only the owner
  can do; that last step is *unverified*.
- uBlock Origin (90 + 7 checks) and Dark Reader still pass on this build.

## What the coordinator answers

`UIProcess/WebAuthentication/haiku/WebAuthenticatorCoordinatorProxyHaiku.cpp`:

| Request | Answer |
| --- | --- |
| `create()` / `get()` | Summit's authenticator (above), conditional `get()` through autofill, conditional `create()` after a password sign-in; and USB security keys for modal requests that allow them (below); `NotAllowedError` when neither may answer or the view did not opt in |
| `isUserVerifyingPlatformAuthenticatorAvailable()` | `true` (`false` when off) |
| `isConditionalMediationAvailable()` | `true` (`false` when off) |
| `getClientCapabilities()` | Every known capability; the platform authenticator, `conditionalGet`, the signals and `extension:prf`/`largeBlob`/`credProps` `true` (when on), the rest `false` |
| `signalUnknownCredential`, `signalAllAcceptedCredentials`, `signalCurrentUserDetails` | Remove, hide/show, rename Summit's passkeys (above); resolve at once |

Apple's authenticator manager (Touch ID service, NFC, the authenticator panel)
is not built; its USB part is replaced by `SecurityKeysHaiku.cpp` (below). `WebsiteDataStore` and `WebPageProxy` keep their manager only off
Haiku; nothing else refers to it.

## When a request reaches the browser unanswered (15 October 2026)

A request that neither Summit's authenticator nor a security key may take
(both are off, the view did not opt in, or the site asks for one that is off)
is declined with
`NotAllowedError`, and the browser window is told
(`B_WEBKIT_WEBAUTHN_UNHANDLED`, with the view and whether the request was
`create`). The window's status line explains: "This page asked for a passkey,
but neither Summit nor a password manager answered. An extension such as
1Password provides its own passkeys",
with ", and must be allowed in private windows (Extensions)" in a private
window. Conditional-mediation requests that are declined (the authenticator is
off) stay quiet. `tests/fixtures/webauthn/request.html` makes both kinds
of request from buttons (it needs a secure context with a domain, so it shows
errors from `file:`).

## Checking the surface

Open `tests/fixtures/webauthn/support.html`. It lists each entry point and sets
the page title to `webauthn: present` or `webauthn: missing`. The asynchronous
answers come from Summit's own coordinator when no extension is installed; with
1Password active they come from the extension.

## Limits

- Security keys: USB only (no NFC or Bluetooth stack), and not yet tried
  with a real key (below).
- Autofill is offered only in fields marked `autocomplete="… webauthn"`; the
  list is not part of the saved-password list of the same field, which still
  shows its own.
- Passkeys are not synced and cannot be exported; removing Summit's key store
  data removes them. Hybrid sign-in with a phone (caBLE/QR) is not offered.
- `create()` in a frame of another origin is declined (Level 3's
  `publickey-credentials-create` policy is not implemented in WebCore).
- The related-origins list is fetched without Summit's proxy settings, and
  is not cached (each request that needs it fetches it).
- Extensions: `credProps`, `prf` and `largeBlob` only. PRF needs a passkey
  made with this build or later (older ones have no secret).

## Security keys (USB, CTAP2 and U2F, 31 October 2026, #111)

Summit talks to FIDO security keys (YubiKey, SoloKey, Titan and the like)
over USB HID, with upstream WebKit's portable CTAP code on a transport of its
own:

- **Finding keys** (`UIProcess/WebAuthentication/haiku/HidServiceHaiku.cpp`):
  while a request waits, the USB device nodes are looked at once a second
  through the WebHID device code (`HIDHaiku::enumerateSecurityKeys`, which
  reads each device's report descriptors once and caches them). A key is a
  HID interface with a top-level collection of usage page `0xF1D0`, usage
  `0x01` (CTAPHID) and 64-byte input and output reports. WebHID never offers
  these to pages (`protectedUsagePage`); only the browser opens them
  (`HIDHaiku::Connection::open(..., securityKey = true)`), off the main
  thread. A key plugged in after the prompt opened is found within a second;
  one pulled out and plugged in again is a new key.
- **The transport** (`HidConnectionHaiku.cpp`, upstream's `HidConnection`
  interface): output reports go to the key's interrupt-out endpoint (or
  SET_REPORT) on the connection's device thread, input reports come from the
  interrupt-in transfer it keeps pending; both are handed to the main run
  loop, where upstream's `CtapHidDriver` does the CTAPHID framing (INIT with a
  nonce, channel IDs, CBOR/MSG, KEEPALIVE, CANCEL).
- **The protocol**: upstream's `FidoService` (getInfo), `CtapAuthenticator`
  (makeCredential, getAssertion and getNextAssertion, exclude and allow lists
  in batches, clientPIN protocols 1 and 2, setPIN, `hmac-secret` for prf,
  `credProps`) and `U2fAuthenticator` (CTAP1 keys, and CTAP2 keys for U2F-only
  requests such as `appid`).
- **One request's keys** (`SecurityKeysHaiku.cpp`, what upstream's
  `AuthenticatorManager` does for USB): gives every key found the request
  (the clientDataJSON hash and the options), and reports to the coordinator
  what the keys do. Only one request looks for keys at a time (a newer one
  takes them). When the request ends, or the user chooses one of Summit's
  passkeys instead, the keys are told to cancel (CTAPHID_CANCEL) and closed.

### Which requests go to security keys

A modal `create()` or `get()` (not conditional mediation) is given to
security keys as well as to Summit's own passkeys, unless the site asks for
this computer's authenticator (`authenticatorAttachment: "platform"`), or,
for `get()`, its allow list names only other transports (Summit's own
passkeys say `internal`). The key answers only once touched (or its PIN
given), so the user chooses by using it; whichever answers first answers the
page. A request for a security key only (`authenticatorAttachment:
"cross-platform"`, or a `create()` with algorithms Summit's passkeys lack)
gets the security key prompt alone; before this it was declined.

As upstream's manager does, a key's answer ends the request, and so do the
errors that say the request cannot succeed (`InvalidStateError`: the key
already holds a credential the site excludes; `NotSupportedError`); other
errors (this key has none of the site's credentials, a PIN given up) are
shown, and the request waits for another key, or this one plugged in again.

### The prompts

In the passkey sheet (`src/ui/PasskeyPrompt.cpp`, `BrowserWindowAuthentication.cpp`):

| What | The sheet |
| --- | --- |
| Summit's passkey and keys both possible | The usual "Create a passkey…" or "Sign in…" sheet, with "Or … on a security key: insert it and touch it." |
| A key only (or `get()` with no passkey of Summit's) | "Use your security key": "Insert your security key and touch it to create a passkey for *site* on it" (or "to sign in to *site*"), Cancel |
| A key took the request | "Security key found. Touch it to continue." in the sheet showing |
| The key's PIN | "Enter your security key's PIN": a hidden PIN field; "Wrong PIN." and the tries left after a wrong one (or when 3 or fewer remain) |
| The key has no PIN and the site wants verification | "Set a PIN for your security key": new PIN twice, at least the key's minimum (4) |
| The key holds several of the site's passkeys | "Sign in to *site* with your security key" with an Account menu |
| After a PIN | "Checking the PIN…", then "PIN accepted. Touch your security key." |
| The key refused | "This security key has no passkey for *site*. Try another one.", "…is locked after too many wrong PINs…", "Too many wrong PINs. Remove the security key and insert it again…", "…has no room for another passkey." |

Cancel (or Escape) in any of them declines the whole request
(`NotAllowedError`). A key's final error (an excluded credential) closes the
sheet and the window's status line says "The security key could not be used
for *site*: …". For tests the sheets take `hey Summit 'ALTB' of Window
Passkey with which=int32(1) and pin=1234` (`account=int32(K)` for the menu).

### The API

`B_WEBKIT_PASSKEY_REQUESTED` has two more fields: `security_key` (bool: a key
may answer too) and `platform` (bool, false: only a key may). While the
request waits, notices with `identifier` and `security_key_state` say what a
key wants: `touch`, `pin` (`retries`), `new_pin` (`min_length`), `account`
(`account_name`/`account_display_name`), `failed` (`error`), or `status`,
each with `status` (`pin_invalid`, `pin_blocked`, `pin_auth_blocked`,
`no_credentials`, `key_store_full`, `pin_too_short`, `pin_too_long`,
`pin_accepted`) when there is one. The host answers `pin`, `new_pin` and
`account` with `BWebKitView::AnswerSecurityKey(identifier, pin, account)`.
When a key answered, the `withdrawn` notice has `security_key` and, when the
page got an error, `error`.

### Switches and test keys

- `SUMMIT_SECURITY_KEYS=0`: no security keys (requests as before).
- `SUMMIT_SECURITY_KEY_TRACE=1`: every HID interface looked at is logged once
  per request, with its top-level collections.
- `SUMMIT_WEBAUTHN_SOFT_KEY=1`: a working CTAP2.1 key in software in place of
  real keys (`SoftSecurityKeyHaiku.cpp`), at the CTAPHID packet level under
  `CtapHidDriver`: ES256 credentials (discoverable ones too), clientPIN
  protocols 1 and 2 (key agreement, PIN token, setPIN, 8 retries, blocking
  after 3 wrong in a row), a 0.4 s "touch", self attestation. Its signatures
  verify, so real sites accept it. `SUMMIT_WEBAUTHN_SOFT_KEY_PIN=1234` gives
  it a PIN; `SUMMIT_WEBAUTHN_SOFT_KEY_PROTOCOL=1` offers PIN protocol 1 only.
  Its credentials last as long as the browser.
- `SUMMIT_WEBAUTHN_MOCK_HID=<JSON or file>`: upstream's mock HID key
  (`MockHidConnection`, what layout tests set with
  `internals.setMockWebAuthenticationConfiguration({hid: …})`), canned
  answers at the packet level. The value is that `hid` object (or
  `{"hid": …}`), inline or the path of a file read again for each request.
  A configuration without an answer for a request gets CTAP2_ERR_OTHER
  (upstream's mock would read past its payloads).

### A WebCore fix the PIN needed

WebCore's OpenSSL AES-CBC (`crypto/openssl/CryptoAlgorithmAESCBCOpenSSL.cpp`)
ignored `Padding::No`, which only the CTAP2 PIN protocol asks for: the
encrypted PIN hash sent to a key had a padding block more than CTAP2 allows,
and the PIN token a key returned could not be decrypted (found with the mock:
"Unknown internal error. Error code: 0" after the PIN). It now turns padding
off for those calls (input must be whole blocks) and takes an empty IV as a
zero IV (PIN protocol 1). Web Crypto always pads and is unchanged.

### Verified (31 October 2026, laptop and X399, engine ctap111c + app)

Fixture: a page on `http://localhost:8011` (python's http.server on the
laptop) whose script makes the requests and verifies signatures with Web
Crypto; driven through Summit's MCP (`evaluate`) and `hey` for the sheets.

- **Upstream's mock (canned CTAPHID traffic, the payloads of WebKit's own
  tests):** CTAP2 `create()` (attachment `cross-platform`, transports
  `["usb"]`, ES256, flags `0x41`) and `get()`; U2F `create()` (authData built
  for `localhost`) and `get()` with an allow list; a key with two accounts →
  the Account sheet → the second account; PIN `create()` and `get()` (wink,
  getRetries 8, key agreement, the PIN sheet, PIN token, then the request
  with `pinUvAuthParam`); a wrong PIN then the right one ("Wrong PIN. 8 tries
  left"); PIN blocked and PIN auth blocked (the matching note; Cancel →
  `NotAllowedError`); no credentials (CTAP2 and U2F: note "has no passkey…",
  request goes on); excluded credential → `InvalidStateError` and the status
  line; key store full → retried without a discoverable credential →
  success; internal UV; PIN protocol 2 offered; user verification required
  on a key without a PIN → the "Set a PIN" sheet; Cancel →
  `NotAllowedError: The user declined.`; a key that never answers → the
  page's 30 s timeout. The mock's packets cross `CtapHidDriver` as a key's
  would; it does not check PIN cryptography.
- **The software key (real cryptography):** a discoverable `create()` (flags
  `0x41`, `credProps.rk` true, SHA-256(`localhost`) RP ID hash) and a
  username-less `get()` whose signature Web Crypto verified (`verified:
  true`, user handle back); with a PIN, protocol 2: a wrong PIN (the key
  decrypted a well-formed 32-byte hash and refused it, 7 tries left) and the
  right one → flags `0x45` (UV); protocol 1: `create()` and `get()` with
  `userVerification: "required"` → flags `0x45`/`0x05`, signature verified
  (this failed before the AES fix); no PIN with verification required →
  "Set a PIN" (a 2-digit PIN refused in the sheet, then `4321`) → setPIN →
  PIN token → credential with UV. **https://webauthn.io** with the software
  key and its PIN: Register → *"Success! Now try to authenticate..."*,
  Authenticate → *"You're logged in!"*.
- **Real device enumeration (no key attached):** laptop: "23 USB devices, 5
  HID interfaces, 0 security keys" (SMSL USB Audio `ff00:0001`, Keychron Q6
  `ff60:0061` and `0001:0002 0001:0080 000c:0001 0001:0006`, a hub
  `00ff:0001`, the Realtek dock HID `ffda:00da`); X399: "7 USB devices, 1 HID
  interface, 0 security keys" (Melody96 keyboard); an `AbortController` abort
  after 5 s ended the request and the polling (`AbortError`). No errors, no
  crash; the user's keyboards kept working (WebHID's protections unchanged).
- **Summit's passkeys (regression):** on webauthn.io with no key, Register →
  "Create a passkey…" (with the security key line) → Create → *"Success!"*,
  Authenticate (allow list `internal`: no key polling started) → Sign In →
  *"You're logged in!"*. The test passkey was removed afterwards
  (`signalUnknownCredential`).

### What needs a real key

- Opening a real key's interface through usb_raw while Haiku's usb_hid
  driver is attached to it (WebHID works this way; usb_hid has no handler
  for usage page `0xF1D0`, so it should not read its reports), and the
  interrupt transfers' timing with a key that blinks, sends KEEPALIVE and
  waits for a touch.
- A real key's PIN, its getInfo (extensions, `minPINLength`,
  `maxMsgSize`), its attestation certificate, `hmac-secret` (prf),
  `credProtect`, and pulling it out in the middle of a request (the request
  then waits for another key until its timeout).
- The issue's done-when: webauthn.io with a YubiKey 5 with and without a
  PIN, and GitHub's security key second factor (U2F `appid` path included).
