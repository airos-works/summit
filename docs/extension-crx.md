# Signed CRX3 extension import

Summit's modern extension-enabled browser imports CRX3 packages through
**Window → Extensions… → Add extension…**. Before presenting consent, the engine
verifies the package signature, extracts its authenticated ZIP payload, and
prepares an immutable resource snapshot. A verified signature does not establish
Chrome Web Store provenance or extension compatibility. Requested access still
requires the user's approval.

The verified developer public key determines the 32-letter Chrome extension ID.
This identity takes precedence over a manifest key or Gecko ID inside that CRX.
The public SDK returns it as `verified_crx_id` in the preparation receipt and
rejects activation under any other identity. ZIP/XPI packages and folders return
an empty value and retain the existing manifest/local identity rules. Neither
format nor the choice of `chrome` versus `browser` namespace selects a complete
API compatibility profile yet.

The installation retains the original CRX bytes. Startup prepares and verifies
that package again, then restores saved approval only if its resource fingerprint
matches. Corruption is rejected. A valid signature over changed resources still
requires fresh approval; this does not implement automatic extension updates.

## Verification and extraction

The verifier follows the CRX3 wire format and
[Chromium's verifier implementation](https://github.com/chromium/chromium/blob/main/components/crx_file/crx_verifier.cc).
RSA proofs use PKCS#1 v1.5 with SHA-256; ECDSA proofs use SHA-256. The historical
PSS comment in [crx3.proto](https://github.com/chromium/chromium/blob/main/components/crx_file/crx3.proto)
does not describe the RSA algorithm used by Chromium's implementation.
Every supplied RSA/ECDSA proof must verify, and at least one developer key must
match the signed ID. Signatures cover the exact signed-header bytes and complete
ZIP payload. The reader bounds header size, proof count, key/signature sizes,
varints, field lengths and nested unknown protobuf groups.

Extraction passes only the authenticated ZIP slice to libzip. CRX input is read
into an immutable buffer and checked for source changes. The existing path,
entry-type, duplicate-name, CRC, expansion and cleanup checks remain enforced.
Ordinary ZIP/XPI inputs retain `ZIP_CHECKCONS`. Signed CRX uses libzip's normal
read mode: the published uBlock CRX has local/central ZIP version metadata that
libzip's additional consistency mode rejects, although its payload is readable.
The signed hostile-archive tests cover this separate path.

Store publisher authentication and complete browser-specific API semantics
remain unfinished. Mozilla's XPI signature is checked since 27 Oct (below),
and so are CRX2 packages.

## CRX2

Chrome's packages before CRX3 (version 2 in the `Cr24` header) carry the
developer's RSA key and its PKCS #1 v1.5 SHA-1 signature over the ZIP that
follows them. `verifyWebExtensionCRXHaiku` checks that signature
(`verifyCRX2`), and the extension ID is the key's SHA-256 prefix as for
CRX3, so a CRX2 package installs as a verified CRX3 one does ("Package
signature verified"). Chrome stopped accepting CRX2 in 2019; old saved
packages still use it. Checked on the host with the verifier alone: a CRX2
built and signed with OpenSSL verified with the ID its key gives; one byte
changed in its ZIP, and a zeroed signature, were refused; Honey's CRX3 still
verified. On the X399 (27 Oct, an20 engine) that CRX2 installed with
"Package signature verified." and the ID `ofkikkbcffofmmncfcmnojdjefdhfkmb`
and ran; the changed one was refused ("Bad data", as an invalid CRX3 is).

## Mozilla XPI signatures

A Firefox package signed by addons.mozilla.org carries Mozilla's signature in
`META-INF` (Firefox's PKCS #7 form: `mozilla.rsa`, `mozilla.sf`,
`manifest.mf`). `verifyWebExtensionXPISignatureHaiku`
(`WebExtensionCRXHaiku.cpp`) checks it over the unpacked files as Firefox
does: the signature over `mozilla.sf` chains to Mozilla's add-on root
`root-ca-production-amo` (built in, from Firefox's
`security/manager/ssl/addons-public.pem`; the intermediate comes with the
signature), `mozilla.sf` holds the SHA-256 of `manifest.mf`, and
`manifest.mf` lists the SHA-256 of every other file, with none missing or
added (the COSE files are checked when listed). The signing certificate's
common name is the add-on's ID. The package registry reports
`xpi_signature` (`verified` or `invalid`, absent when unsigned),
`xpi_signer` and `xpi_signature_error` with the prepared package.

The installer refuses a package whose signature does not verify ("The
package's Mozilla signature is not valid: …"), or that was signed for
another ID than its manifest's Gecko ID, and says "Signed by Mozilla
(Firefox Add-ons) for ID; signature verified." in the review. An unsigned
package installs as before, with the unverified-package warning. The
signature is checked at installation, not on every launch (the installed
files' own fingerprint covers later changes).

Checked on the host with the verifier alone: Dark Reader 4.9.133,
SponsorBlock, uBlock Origin, 1Password and Bitwarden from AMO verify, each
signed for its own ID; a changed `manifest.json`, an added file, a missing
file and a changed `mozilla.sf` are refused with the reason; a package
without `META-INF` is unsigned. On the X399 (27 Oct, an19 engine): Dark
Reader's AMO package showed "Signed by Mozilla (Firefox Add-ons) for
addon@darkreader.org; signature verified." in the review and installed; the
same package with a changed name was refused with "The package's Mozilla
signature is not valid: manifest.json does not match its signed digest.";
without `META-INF` it showed the unverified-package warning and installed.

## Safari web extensions

A Safari web extension ships inside a macOS app: `Name.app/Contents/PlugIns/`
holds an app extension (`Name Extension.appex`) whose `Contents/Resources`
folder is the web extension (`manifest.json`, scripts, images). Choosing the
`.app` or the `.appex` in **Add extension…** imports that Resources folder as
an unpacked extension (`SafariWebExtensionResources`, called by
`ExtensionCatalog::Stage`); app extensions without a `manifest.json` (share or
widget extensions) are passed over, in name order. The extension gets the
identity an unpacked folder gets (its manifest's key or browser ID, else a
local one), not its macOS bundle identifier, and the app's native part (the
Swift handler `browser.runtime.sendNativeMessage` reaches) does not run on
Haiku. The browser's user agent stays Summit's. Checked by the package tests
(`tests/ExtensionPackageTests.cpp`: an app, its `.appex`, a share extension
before it, an unpacked folder and an archive).

## Native results

The verified engine patch is
`d5887e4f3e31b5c580a90cd501a9917853bfffccd32fe5a239fe6a8da6bd809c`.
The exported bundle is `artifacts/modern-browser/bundle-d0h_6xzg/`;
[copy provenance and source archive digests](modern-bundle-copy.md#verified-crx3-import)
identify its 399 verified artifact entries. The full engine and browser built
with unchanged inputs as native bundle
`/SummitExtensions/summit/build-modern-browser/bundle-d0h_6xzg`.
The frozen build report is `.vm/modern-browser-crx-import-bundle-result.json`
(SHA-256 `09a7a136584e8e57178efd81988b24cd40ae71a123e01d89df87526d481b6310`).

The production verifier/extractor passes **620 native checks**: 201 signature
checks, 308 CRX extraction checks, and all 111 existing ZIP/XPI checks. These
include independent RSA/ECDSA signatures, invalid extra proofs, tampering,
malformed protobuf, signed hostile ZIPs, and all 779 resources of the unmodified
published uBlock Origin 1.74.0 CRX. The native result is
`.vm/native-crx-f2f77ff6607354eb7945469a/result.json`.

The public SDK and actual browser pass **205 checks**:

- 72 SDK checks establish signer propagation, rejection of conflicting caller
  IDs, immutable prepared snapshots, RSA/ECDSA execution, resource bytes,
  storage across reload and complete snapshot cleanup. The unmodified uBlock
  package prepares with ID `fkgkibajhfbepljeaefdnfnegdcjomkh`; this run does not
  execute uBlock or test its filtering.
- 78 installation checks exercise the native picker, corrupt-package rejection,
  signature/provenance consent text, cancellation, installation despite a
  conflicting manifest ID, actual runtime ID and popup execution.
- Four further browser processes pass 17, 10, 11 and 17 checks respectively:
  startup restores saved state; corruption prevents startup; a newly signed
  update requires approval; restoring the original bytes restores the runtime
  without an extra background boot from either rejected package.

All six process groups exit normally and drain without forced cleanup. The
native debugger-log interval is clean. Source, bundle, fixture, original CRX
and approved catalog checks pass. Consent and restored-popup screenshots were
also inspected. The runtime result is
`.vm/modern-extension-crx-cbeaee46835bcb92b29d31eb/result.json`
(SHA-256 `e36bcdd03c6686514cb92e311d53991b88f68be6f63853d2e0d4b7281c2f6b57`).

All three host CTest suites pass, including 34 identity checks. The production
signature verifier passes its 201 host checks under AddressSanitizer and
UndefinedBehaviorSanitizer; the runtime fixture envelopes add 16 passing checks.

The unchanged published Chromium MV2 Dark Reader 4.9.131 package also passes
all **78 native checks** on this bundle, including initial injection into an
already-open page, popup On/Off, and disable/re-enable. Its result is
`.vm/modern-darkreader-ef4aa1354e5979354fb9a707/result.json`
(SHA-256 `c64cd8b4237c0596c30ba2c0e271519ce3b699ad9d1aaea2c2d6b1bdffa11988`),
with unchanged package/bundle inputs, a clean debugger-log interval and no
forced cleanup.

## Reproduce

The host fixture generators require Python's `cryptography` package (tested
with 41.0.7). Each generation creates fresh ephemeral signing keys; freeze the
resulting directories for the duration of a run. The published package is
checked against its existing corpus pin.

```sh
python3 tools/fetch-extension-corpus.py
python3 tools/test-engine-extension-crx-fixtures.py .vm/crx-fixtures
python3 tools/test-modern-extension-crx-fixtures.py .vm/crx-runtime-fixtures
python3 tools/test-engine-extension-crx.py \
  --fixtures .vm/crx-fixtures --bundle /absolute/guest/bundle
python3 tools/test-modern-extension-crx.py \
  --fixtures .vm/crx-runtime-fixtures --archive-fixtures .vm/crx-fixtures \
  --bundle /absolute/guest/bundle --watch
```

Run native integration suites serially with other browser/crash-log tests.
`--suite sdk` or `--suite browser` selects a narrower runtime run;
`--compile-only` verifies harness compilation without executing extensions.
The broader [browser and extension requirements](REQUIREMENTS.md) remain active.
