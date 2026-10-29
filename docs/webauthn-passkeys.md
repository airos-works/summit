# Passkeys and Web Authentication in Summit

## Why the API is built at all

KunanyiOS has no platform authenticator of its own: no biometric credential
store, and no CTAP transport for USB or NFC security keys. Summit builds the
Web Authentication API for two reasons: password managers provide passkeys
through it from their content scripts, and since October 2026 (#111) Summit
answers it itself with passkeys it keeps in the system key store.

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
passkey is for.

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
  sheet, which a page cannot draw or click. That is how the UV flag is
  earned; there is no PIN or biometric step.
- **Discoverable credentials.** Every passkey is discoverable: it keeps the
  user handle, name and display name, so `get()` without `allowCredentials`
  (username-less sign-in) offers all of the site's passkeys, and
  `credProps.rk` is `true`. A site keeps one passkey per account (user
  handle): a new one replaces the old.
- **Allow and exclude lists** are honoured by credential ID. Entries for other
  authenticators (security keys, phones) simply do not match.
- **clientDataJSON** is WebKit's `buildClientDataJson` from the frame's
  origin; the RP ID check is WebKit's
  (`SecurityOrigin::isMatchingRegistrableDomainSuffix`, after the web process's
  own checks: secure context, no IP addresses, `user.id` 1-64 bytes). Related
  origins (`/.well-known/webauthn`) are not consulted. Frames of another origin
  than their parents are declined, as upstream's own authenticators do.
- **Timeouts and aborts.** The page's `timeout` is honoured between 30 s and
  10 minutes (5 minutes without one); an `AbortSignal` or the timeout closes the
  sheet. A request whose key is already being made or used completes.
- **Availability.** `PublicKeyCredential.isUserVerifyingPlatformAuthenticatorAvailable()`
  is `true`, and `getClientCapabilities()` reports
  `passkeyPlatformAuthenticator` and `userVerifyingPlatformAuthenticator`.
  Conditional mediation (passkeys in a sign-in field's autofill) and
  conditional creation are not offered: `isConditionalMediationAvailable()` is
  `false`, and such requests are declined quietly.
- The signal methods (`signalUnknownCredential` and the others) succeed
  without changing anything: Summit's passkeys are removed only by the user.

### Where the passkeys are kept

In the Haiku key store (keystore_server), keyring **"Summit passkeys"**, one
generic key per passkey (purpose `B_KEY_PURPOSE_WEB`): identifier = RP ID,
secondary identifier = credential ID in hexadecimal, data = a flattened
`BMessage` with `version`, `algorithm` (-7), `credential_id`, `user_handle`,
`user_name`, `display_name`, `origin` (the page that made it), `created` and
`private_key` (ECPrivateKey DER). Nothing is written to Summit's profile.

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

- Private keys exist only in the key store and, briefly, in the browser
  process while a key is made or a signature computed; those copies are
  cleared (`OPENSSL_cleanse`). Web processes never see them: they get the
  public key, the attestation and signatures.
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
UI comes first and the platform's is the fallback. Not verified with 1Password
itself (its vault needs the owner).

### Turning it off and the API

`SUMMIT_PLATFORM_PASSKEYS=0` turns the authenticator off: requests are then
declined with `NotAllowedError` as before, `isUserVerifyingPlatformAuthenticatorAvailable()`
is `false`, and the window's status line explains (below).

Hosts opt in per view with `BWebKitView::SetPasskeyRequests(true)` (Summit does
for every page view, not extension pages). The engine then sends
`B_WEBKIT_PASSKEY_REQUESTED` ('wpky') with `view`, `identifier`, `create`,
`rp_id`, `origin`, and either `user_name`, `user_display_name`, `exists`,
`replaces` (create) or one `account_name`/`account_display_name` per passkey
(get); the host answers with `BWebKitView::AnswerPasskeyRequest(identifier,
account)` (the passkey's index, 0 to create, negative to decline). A request
that goes away first is followed by a notice with `withdrawn`. A view that did
not opt in behaves as before #111.

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
| `create()` / `get()` | Summit's authenticator (above); `NotAllowedError` when it is off or the view did not opt in, for conditional mediation, and when the site asks for a cross-platform authenticator (a security key) |
| `isUserVerifyingPlatformAuthenticatorAvailable()` | `true` (`false` when off) |
| `isConditionalMediationAvailable()` | `false` |
| `getClientCapabilities()` | Every known capability; `passkeyPlatformAuthenticator` and `userVerifyingPlatformAuthenticator` `true` (when on), the rest `false` |
| `signalUnknownCredential` and the other signals | Succeed without doing anything |

Apple's authenticator manager (Touch ID service, NFC, the authenticator panel)
is not built. `WebsiteDataStore` and `WebPageProxy` keep their manager only off
Haiku; nothing else refers to it.

## When a request reaches the browser unanswered (15 October 2026)

A request that Summit's authenticator does not take (it is off, the view did
not opt in, or the site wants a security key) is declined with
`NotAllowedError`, and the browser window is told
(`B_WEBKIT_WEBAUTHN_UNHANDLED`, with the view and whether the request was
`create`). The window's status line explains: "This page asked for a passkey,
but neither Summit nor a password manager answered. Security keys (USB) are
not supported yet; an extension such as 1Password provides its own passkeys",
with ", and must be allowed in private windows (Extensions)" in a private
window. Conditional-mediation requests, which pages make at load for passkey
autofill, stay quiet. `tests/fixtures/webauthn/request.html` makes both kinds
of request from buttons (it needs a secure context with a domain, so it shows
errors from `file:`).

## Checking the surface

Open `tests/fixtures/webauthn/support.html`. It lists each entry point and sets
the page title to `webauthn: present` or `webauthn: missing`. The asynchronous
answers come from Summit's own coordinator when no extension is installed; with
1Password active they come from the extension.

## Limits

- Security keys over USB or NFC (CTAP2/U2F) are not supported: there is no
  HID transport yet, and no key was available to test one.
- No passkey autofill (conditional mediation) or automatic passkey upgrade
  after a password sign-in (conditional create).
- Passkeys are not synced and cannot be exported; removing Summit's key store
  data removes them. Hybrid sign-in with a phone (caBLE/QR) is not offered.
- Related origins and cross-origin iframes (`publickey-credentials-get` in a
  frame of another origin) are declined.
- No PRF, largeBlob or other authenticator extensions; only `credProps`.
