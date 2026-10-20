# Copying a frozen modern bundle

> **Retired (20 October 2026, #120):** `tools/copy-modern-browser-bundle.py`
> needed the VM's exact CMake configuration and rejected every bundle built on
> the workstations, and releases now come from CI's packages (the README's
> "Latest builds"; docs/updates.md). The tool is gone; this page stays as the
> record of the bundles it copied and their digests, which other notes link to.

After a modern browser bundle has completed and its runtime checks have passed,
run this on the host from the Summit checkout:

```sh
python3 tools/copy-modern-browser-bundle.py
# A saved report may be selected explicitly:
python3 tools/copy-modern-browser-bundle.py .vm/modern-browser-bundle.json
# The separate preview bundle is also supported:
python3 tools/copy-modern-browser-bundle.py --preview
# An extension-enabled browser has a separate report:
python3 tools/copy-modern-browser-bundle.py .vm/modern-extensions-browser-bundle.json
```

The default report is `.vm/modern-browser-bundle.json`; `--preview` selects
`.vm/modern-preview-bundle.json`. The tool uses the existing VM SSH connection
in `tools/haiku.sh`. It does not build, synchronize, prepare, or launch anything.
It requires Python 3.9 or newer, Git, and enough host disk space for the native
transport archive, extracted bundle and compressed engine source.

The result is a new `artifacts/modern-browser/bundle-<native ID>` directory.
An existing directory, file or symlink at that destination causes a failure.
Extraction and verification happen in a private temporary directory; a failed
copy removes only that temporary directory. Completed bundles are never replaced.

The native path and manifest must match the selected host report. Every declared
binary, private library, launcher and frozen app input is checked by SHA-256.
Only declared library aliases pointing to their regular library in `lib/` are
accepted; archive traversal, hard links, special files and undeclared source
files are rejected. Summit and WebKit licenses are checked against their matching
source; the bundled private ICU license is preserved and its digest recorded.
Extension-enabled bundles also contain the locked private libzip library and
its complete license notice from the locked `zip.h` header. Their manifest
records the selected engine variant and requires both extension CMake options
to be ON; ordinary modern bundles require both options OFF. Older manifests
remain readable with their original configuration and header mapping.

The cached `.cache/WebKit` must already have the bundled upstream commit and
patch. The exact `git diff --binary --full-index HEAD` digest must equal the
bundled engine lock, and the lock's patch file must have that same digest.
Untracked and ignored source files are rejected except `__pycache__` entries,
which native source synchronization also excludes. Git flags that hide source
changes are rejected. For Git-mandated CRLF files, the normalized blob is also
checked against the index; the archive retains and hashes the actual CRLF bytes. The copier never repairs or reapplies a patch. A mismatch
requires selecting a genuinely matching checkout/bundle before trying again.

`WebKit-sources.tar.xz` contains the complete native build input trees
`CMakeLists.txt`, `Configurations`, `Source` and `Tools`, with the applied patch.
It preserves source symlinks within those trees. It excludes Git metadata,
generated builds and Python caches. This is the same source scope transferred
by `tools/build-webkit-in-vm.sh`, rather than a complete upstream Git clone.
`WebKit-source-manifest.json` records every archived file, link and directory.
All source bytes and the locks/build support are fingerprinted before and after
archiving; changes abort publication. Bytes sent to the source archive are also
checked while writing it.

The original `build-manifest.json` and `host-bundle-report.json` are preserved.
`copy-provenance.json` records their digests, the source archive digest, matching
before/after fingerprints and a file inventory of the final payload excluding
the provenance file itself. The final command output reports the provenance
digest. These hashes establish the copied inputs; they do not establish browser
runtime behavior. The latest extension-enabled copy is `bundle-d0h_6xzg`,
with [verified CRX3 import and published Dark Reader regression evidence](extension-crx.md).
The earlier `bundle-p214ja9w` retains its
[native action/popup evidence and provenance](modern-extension-actions.md).
The earlier identity-enabled copy is `bundle-mo8zo6d4`,
including the native extension manager, real XPI/folder picker, Chrome manifest
key identities and approved extension startup across browser process restarts.
Its provenance digest is
`f0fc447700311f16763ae5f28f85b3f4fc8175b0a7b715c516da16dbe2cd6fbd`.
Its engine source archive digest is
`47cfdc4589a682fe7d987d0f177b6276b55e62b5b7d0f591bb02b4ee35e70c0a`.
The copy records host revision `9c67f33`; avoid committing while copying,
because changing that recorded revision also invalidates the before/after
provenance comparison. The previous manager bundle `bundle-tz1tfyzh` remains
available with provenance digest
`b034304aa77ca4c250ad718c0b4dedd3e014ed81823ed8a93ed7b7c869b0bb3e`.
Its first copy attempt overlapped commit `858221f` and was discarded; the
subsequent successful copy and the current copy have matching source and
support fingerprints.
The ordinary modern copy `bundle-yqhmejfw` also remains available with its
recorded native process, download, navigation and history checks.
Earlier artifacts are preserved with their recorded limitations.
See [the verification record](STATUS.md) for exact runtime results and remaining
limitations.

## Building from the preserved inputs

The copied bundle runs natively on Haiku with `./run-browser.sh` or
`./run-preview.sh`. Keep the helpers and private libraries beside it. Runtime
results must come from that frozen bundle, with its matching helpers and ICU.

`source/` is the exact app source and public headers frozen by the native build.
`rebuild/` contains the engine/ICU locks, complete port patch and host build
scripts captured when copying. Its `tools/build-modern-browser.py` comes from
the frozen app inputs. The other host scripts have separate provenance and are
not represented as original native compilation inputs.

To set up a fresh build workspace, copy the two trees into an empty directory:

```sh
mkdir summit-rebuild
cp -a source/. summit-rebuild/
cp -a rebuild/. summit-rebuild/
cp summit-rebuild/LICENSE-Summit summit-rebuild/LICENSE
cd summit-rebuild
```

Configure your own VM access in `.vm/` for `tools/haiku.sh` (Haiku SSH on host
port 2225, user `user`, key `id_ed25519`, known hosts file `known_hosts`). No SSH
credentials are distributed. The native VM needs the original compiler and
development dependencies; the native manifest records the compiler and original
commands. Use a separate VM or ensure no existing build is active, because the
build scripts use `/boot/home/summit-webkit` and `/boot/home/summit-deps/icu78`.

```sh
# Downloads and verifies ICU 78.3 using engine/icu.lock.json, then builds/tests it.
bash tools/build-icu-in-vm.sh
# Fetches the locked upstream commit and applies the preserved patch in a new cache.
python3 tools/prepare-webkit.py
# Builds the modern engine and both process helpers with the private ICU.
SUMMIT_WEBKIT_JOBS=8 bash tools/build-webkit-in-vm.sh --modern
# Compile and freeze the app corresponding to the preserved source tree:
bash tools/build-modern-browser-in-vm.sh --browser
# For a preview source bundle, use the following command instead:
# bash tools/build-modern-browser-in-vm.sh
```

For an extension-enabled bundle, use `--modern-extensions` on both the engine
build and app bundler instead:

```sh
SUMMIT_WEBKIT_JOBS=3 bash tools/build-webkit-in-vm.sh --modern-extensions
bash tools/build-modern-browser-in-vm.sh --browser --modern-extensions
```

This uses `/boot/home/summit-webkit-extensions`, preserves the private libzip
dependency, and writes `.vm/modern-extensions-browser-bundle.json` without
replacing the ordinary modern report. The bundler checks that both process
targets are fully rebuilt before copying them. `--compile-only` can compile the
browser against isolated staged headers while an engine build is running; it
does not link, bundle, or establish runtime behavior. Extension-enabled browser
`bundle-w5i95j6u` passes approved startup and storage checks across separate
browser processes; [the startup record](modern-extension-startup.md) describes
that evidence and the preceding navigation/close regressions. The older
`bundle-9jahww_j` remains preserved with its recorded cookie-observer lifecycle
failure. See [STATUS.md](STATUS.md) for tested scopes and current limitations.

When the boot volume cannot allocate another engine library, new bundles can
be built on the mounted `SummitExtensions` volume:

```sh
SUMMIT_NATIVE_BUILD_ROOT=/SummitExtensions/summit \
  bash tools/build-modern-browser-in-vm.sh --browser --modern-extensions
```

This creates new compile and bundle directories under
`/SummitExtensions/summit/build-modern-browser`. Existing bundles retain their
paths. The volume must be mounted, and build paths must be direct rather than
symlinks. The selected report records the actual native bundle path; runtime
tests and the copy command consume that report/path with the same hash checks.
The default root remains `/boot/home/summit`. Library copy failures report the
source, destination, partial size and remaining space, since a fragmented BFS
volume can report `File too large` before its free space is exhausted.

The source archive also supplies those full patched trees for a direct native
CMake build without fetching Git. It is already patched: do not apply the patch
again. It has no `.git`, so extracting it into `.cache/WebKit` does not create
the Git checkout expected by `prepare-webkit.py`. Consult the preserved
`tools/build-webkit-in-vm.sh` for the exact native CMake configuration and paths.
Rebuilding is separate from copying and does not promise identical binary hashes
or substitute for testing the rebuilt browser.

The toolbar/action/popup/overflow bundle `artifacts/modern-browser/bundle-7ve4k1a6`
is copied and verified. Its provenance digest is
`1e64738d2b33617b4546e3762330bccc6de216b9efedaa1d4f0af4765cc87992`
and its matching engine source archive digest is
`40b83599ce48235f5dc8b30dc2d743290081140a16cb3c22c976fafa64eae62e`.
Runtime evidence and feature limits are in [native extension actions](modern-extension-actions.md).

The newer badge-color bundle `artifacts/modern-browser/bundle-lzht66nf` is also
copied and verified, with source commit `466f4b1` recorded in copy support.
Its provenance digest is
`4892ae8ab988b1f8bc57c24256a5b293f1cfbc4df27ac2545821410b5b812b23`;
its matching WebKit source archive digest is
`f92213370e7b2ab90373c317b9c2cfe2b8ef3525f5348de7ee69f16e402ea1dc`.
All 399 artifact entries match their recorded hashes. Native/host reports,
source fingerprints and build-support fingerprints match. The native action,
color, popup and overflow results are in [native badge colors](webextensions-native-badge-colors.md).

## Dynamic action icons

The earlier verified dynamic-icon bundle is
`artifacts/modern-browser/bundle-_inqiw0r`, with copy source revision `6cc95e9`
and engine patch `e71fe35768f9d28d5475179f0d782b7820e3027bdfb48f2bea1036ff583b65ca`.
Its provenance digest is
`1474b16a3a8c78464c5067e44d7c4769d1fed85af7959c5566b0cdf5b19a4dbb`.
All 399 artifact entries were checked, with matching source/support fingerprints
before and after copying. The native run passes 47 icon cases, 53 pixel
observations (439 checks), 238 action/popup checks and 169 overflow checks;
process exits and crash monitors are clean. See
[dynamic icon evidence](webextensions-native-action-icons.md).

The newer `bundle-ptx4moys` is built directly on the mounted larger volume at
`/SummitExtensions/summit/build-modern-browser/bundle-ptx4moys`. It passes
49 icon cases, 55 pixel observations (455 checks), 238 action/popup checks and
169 overflow checks. The selected report is
`.vm/modern-browser-action-icons-malformed-svg-bundle-result.json`;
[the icon record](webextensions-native-action-icons.md) includes malformed SVG
rejection and the successful native results.

Its verified host copy is `artifacts/modern-browser/bundle-ptx4moys`, recording
source revision `a807a22` and 399 checked artifact entries. Copy provenance is
`800941a35e53700d2b83c77721bcd9d4d9d645f6c89a195b1c3f33ef705af6d6`;
the matching WebKit source archive digest is
`beba5136830221cd40d24865288ab2f29fb59e97fc5d49f896c93da497a96b54`.

## Programmatic action popups

The verified copy is `artifacts/modern-browser/bundle-en85evku`, from native
bundle `/SummitExtensions/summit/build-modern-browser/bundle-en85evku`, recording
host source revision `9f4cc5b` and engine patch
`591548f4b18259ad3de2ac4eeac855d882176d083f6718b15f41f5b2fd424c34`.
Its copy provenance digest is
`091676aa7f4624f88e1e5cd9132ac77e1eb87e4216fa09b75df8b0481e333fb6`;
the matching WebKit source archive digest is
`6e64763b4c7fd117b821cc2670b30bd461b5044beac088db269a2da9fa758d26`.

All 399 artifact entries match their recorded hashes, modes, and link targets.
The native inventory contains 375 entries and 177 files, and source/support
fingerprints match before and after copying. The selected report is
`.vm/modern-browser-action-open-popup-bundle-result.json`; independent host
validation is `.vm/action-open-popup-artifact-validation.json`.
The bundle passes 342 programmatic popup checks on each of three fresh profiles,
455 icon checks, 238 toolbar-action/popup checks, 29 identity checks, and 169
overflow checks. See [popup behavior and runtime evidence](webextensions-native-open-popup.md).

## Extension access and native extension tabs

`artifacts/modern-browser/bundle-7v5zjcgj` contains the September 16 extension
access and tab configuration changes, with copy support recording source
revision `b549420` and engine patch
`6bd3c0367b53d84b356f22fc9ca4fac930c9f1312810adf4bd7917377ad220e9`.
All 399 artifact entries match their hashes, modes and link targets; the
native inventory contains 375 entries. Native/host manifests and the source
and build-support fingerprints match.

Copy provenance SHA-256 is
`660bb53b43de9955734f5c36d735261aa549bb2cee3e49965bcab833ebe99ad6`;
the complete patched WebKit source archive SHA-256 is
`24b5a76009f460b0806bd81908406da379b535a325002c8d2a5751ff7cb8b2be`.
Validation is `.vm/extension-access-artifact-validation.json`. The five native
suites pass 1,500 checks after the additional Dark Reader On/Off regression;
see [extension access evidence and limits](webextensions-extension-access.md).

## Native extension script injection

`artifacts/modern-browser/bundle-zwmpgvis/` is the verified script-injection
bundle, copied at host commit `1364589` with engine patch
`0ebce0e871f549817c218e0e8a92a4f441408201d46d3aedd2f60e66a6ace206`.
All 399 artifact entries and 375 native entries match their inventories.
The native/host manifests match, and source/support fingerprints stayed
unchanged during copying. `.vm/tab-script-artifact-validation.json` records
independent artifact validation.

Copy provenance SHA-256:
`fcc40a10273a9cf02dd13184ccb989ce03074f55ea9d69bf0dc7733886a474c2`.
Complete WebKit source archive SHA-256:
`5d3b9e91c512b757c44d37c2b5dc55aa0adbf906e4c6fa798ddcec321dc096e2`.
Six native suites pass 2,026 checks, including 113 unique script-injection
cases and published Dark Reader theming of an already-open tab. See
[scope and remaining compatibility work](webextensions-tab-script.md).

## Thenable script results

`artifacts/modern-browser/bundle-o7hhvrm9/` includes the thenable correction,
copied at host commit `1d498a9` with engine patch
`5922459b937953f3744459c266e3cc5baf5d0240e92ef0c5c171c48fd83db54b`.
All 399 artifact entries match their inventory, including the 375 native
entries. Source and build-support fingerprints remained unchanged during
copying, and the native and host manifests match. Validation is
`.vm/thenable-artifact-validation.json`.

Copy provenance SHA-256:
`cc5d72e814022f41e3ee2cc6a6636b97508b36b8a3065fbbe7235bebb5c5eba0`.
Complete WebKit source archive SHA-256:
`4b81f372de5996901d021cae7136092375036383d6b0520dc19b7f2ae8ac2b9c`.
This bundle passes 129 unique injection cases / 585 native checks and 78
published Dark Reader checks. See [the result contract and remaining work](webextensions-script-results.md).

## Verified CRX3 import

`artifacts/modern-browser/bundle-d0h_6xzg/` contains the CRX3 signature verifier,
signer identity propagation, native consent and retained-package startup checks.
It was copied at host commit `16c277222b6fc19b03f52c069934982ca8fbc6eb`, with
engine patch `d5887e4f3e31b5c580a90cd501a9917853bfffccd32fe5a239fe6a8da6bd809c`.
All 399 artifact entries and 375 native entries match their inventories. The
source/support fingerprints remain unchanged and both manifests agree.
Independent validation is `.vm/crx-artifact-validation.json`.

Copy provenance SHA-256:
`1e9f4a8b8c4237623d7fdf9c5b6b37cf1372432da68197667c88aa9f628b840b`.
Complete WebKit source archive SHA-256:
`927a2b2ef5ab569a7f474d12b2481cd6ad99a75badada723f3bab43e10df9142`.
The new bundle passes 205 SDK/browser CRX checks and 78 published Dark Reader
checks. The production verifier/extractor separately passes 620 native checks.
See [the implementation, exact evidence and remaining compatibility work](extension-crx.md).
