# Passkeys and Web Authentication in Summit

## Why the API is built at all

KunanyiOS has no platform authenticator: no biometric credential store, and no
CTAP transport for USB or NFC security keys. Summit still builds the Web
Authentication API, because passkeys arrive through a password manager rather
than the operating system.

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
Apple-specific.

## Status (September 20, 2026)

Verified on `bundle-dxx4tx8e`:

- The API surface is present. `tests/fixtures/webauthn/support.html` reports
  every entry point, including both response interfaces
  ([screenshot](screenshots/webauthn-surface.png)). The browser's own answers
  are `false` for a platform authenticator and for conditional mediation, and
  `getClientCapabilities()` returns nine capabilities, all unsupported.
- 1Password offers a passkey. On github.com and on Google's sign-in page its
  hooks install and it shows *"Sign in with a passkey — Select the 1Password
  icon in your browser's toolbar to unlock"*
  ([screenshot](screenshots/1password-passkey-prompt.png)). The previous
  "WebAuthn isn't supported" error is gone.
- Completing a passkey sign-in needs the vault unlocked, which only the owner
  can do; that last step is *unverified*.
- uBlock Origin (90 + 7 checks) and Dark Reader still pass on this build.

## What Summit implements

`UIProcess/WebAuthentication/haiku/WebAuthenticatorCoordinatorProxyHaiku.cpp`
answers the page's requests without an authenticator:

| Request | Answer |
| --- | --- |
| `create()` / `get()` | `NotAllowedError`, as a browser with no authenticator answers |
| `isUserVerifyingPlatformAuthenticatorAvailable()` | `false` |
| `isConditionalMediationAvailable()` | `false` |
| `getClientCapabilities()` | Every known capability, all `false` |
| `signalUnknownCredential` and the other signals | Succeed without doing anything: there is no credential store to correct |

Apple's authenticator manager (Touch ID service, NFC, the authenticator panel)
is not built. `WebsiteDataStore` and `WebPageProxy` keep their manager only off
Haiku; nothing else refers to it.

A passkey therefore works exactly when an extension provides it. The browser
contributes the API surface, the extension holds the credential, and the user
unlocks their vault.

## When a request reaches the browser (15 October 2026)

A password manager replaces `navigator.credentials` in the page, so a
request that reaches Summit's own coordinator was not taken by any
extension: there is none, it is not allowed in this window (an extension
installed without private-window access, in a private window), or it fell
back to the browser. The coordinator answers `NotAllowedError` as before,
and the page shows a failed passkey; since 15 October it also tells the
browser window (`B_WEBKIT_WEBAUTHN_UNHANDLED`, with the view and whether
the request was `create`), and the window's status line explains: "This
page asked for a passkey, but no password manager answered. Summit keeps no
passkeys of its own; an extension such as 1Password provides them", with
", and must be allowed in private windows (Extensions)" in a private
window. Conditional-mediation requests, which pages make at load for
passkey autofill, stay quiet. `tests/fixtures/webauthn/request.html` makes
both kinds of request from buttons.

## Checking the surface

Open `tests/fixtures/webauthn/support.html`. It lists each entry point and sets
the page title to `webauthn: present` or `webauthn: missing`. The asynchronous
answers come from Summit's own coordinator when no extension is installed, so
`false` there is correct; with 1Password active they come from the extension.

## Limits

- Summit has no authenticator of its own. Without a password manager, a site
  offering only passkeys cannot be used, and `create()` cannot enroll one.
- Conditional mediation (passkey autofill in the login field) is reported as
  unavailable by the browser. An extension may still offer its own.
- Security keys over USB or NFC need a CTAP transport, which needs Haiku device
  access for HID and NFC; none is implemented.
