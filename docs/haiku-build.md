# Summit for air/OS and for Haiku

Summit is built twice from the same source:

| Build | For | Packages |
|---|---|---|
| **air/OS** | air/OS images (x86_64 and arm64), Summit's main platform | the `latest` release |
| **Haiku R1/beta6** | a stock Haiku R1/beta6 installation, x86_64, without air/OS's changes | the `latest-haiku` release |

Both builds have the same browser: tabs, sessions, bookmarks, private
windows, extensions, developer tools, the PDF viewer, reader mode, site
permissions and the same WebKit engine with JIT, WebRTC, MediaSource and
WebAssembly. They differ in what the system under them can do. air/OS
carries graphics drivers, an app_server, video decoders and a Bluetooth
stack that stock Haiku does not have, and Summit uses them where it finds
them.

## What is better on air/OS

| Area | air/OS | Haiku R1/beta6 |
|---|---|---|
| Page rendering | GL compositing on the GPU (NVIDIA through NVK and Zink, Panfrost on the ROCK 5, V3D on the Raspberry Pi 4) | in software on the CPU. Stock Haiku has no 3D drivers, and the engine declines GL compositing on a software renderer (llvmpipe) |
| WebGL | on the GPU | on llvmpipe (the CPU): works, but slowly |
| Presenting frames | straight into the screen (`B_DIRECT_DEVICE_PIXELS`, air/OS app_server) | through app_server |
| High-density screens | desktop and pages at the monitor's density, such as 200% ([hidpi.md](hidpi.md)) | 100%, with Haiku's font scaling |
| Video decoding | hardware decoders where the machine has them: NVDEC (NVIDIA), UVD (AMD Radeon Pro WX 5100), Rockchip MPP (ROCK 5), MMAL (Raspberry Pi 4) | software decoding through the Media Kit's FFmpeg add-on |
| Web Bluetooth | the air/OS Bluetooth LE stack ([bluetooth-transport.md](bluetooth-transport.md)) | not available: Haiku's Bluetooth stack has no LE connections |
| Default browser | set when the package is installed | your choice: Preferences → *Make Summit the Default Browser* |
| arm64 | ROCK 5 and Raspberry Pi 4 images | x86_64 only (Haiku has no arm64 release) |

Everything else is the same code in both builds. Pages, scripts and media
were checked on R1/beta6 ([below](#what-was-checked-on-stock-r1beta6));
native extension installation, permissions, storage, toolbar actions, popups,
removal and shutdown were also checked on 22 October 2026.

## Installing on Haiku R1/beta6

Download `summit_webkit-…-x86_64.hpkg` and `summit-…-x86_64.hpkg` from the
[`latest-haiku` release](https://github.com/jmgasper/summit/releases/tag/latest-haiku),
then install both in one command, so that `pkgman` can resolve what they
need:

```sh
pkgman install ./summit_webkit-*.hpkg ./summit-*.hpkg
```

`pkgman` also installs four HaikuPorts packages, about 55 MB to download:
`llvm21_libs` and `vulkan` (the engine's private Mesa), and `scintilla` and
`lexilla` (the developer tools' text views). Summit then appears in Deskbar's
Applications menu. The packages need Haiku R1/beta6 (hrev59866_79) or later.

Summit bundles [color and monochrome emoji fonts](emoji.md). For pages in
scripts beyond the fonts installed with Haiku, install more Noto fonts from
HaikuDepot, as you would for WebPositive.

The air/OS packages are not meant for Haiku. They do not declare the
HaikuPorts libraries above, they make Summit the default browser when they
are installed, and they are built against air/OS's newer system.

## How the Haiku build is made

air/OS CI builds it after every air/OS build of Summit's `main` branch
(`jmgasper/airos-ci`, `AIROS_TARGET=r1beta6`). It shares no binaries with
the air/OS build.

- **SDK** (`sdk/build-release-sdk.sh x86_64 r1beta6`): the sysroot is a
  fresh R1/beta6 installation. It holds every package of the release's ISO
  (checked against its published SHA-256), the release's `haiku_devel`
  (checked against the Haiku repository) and the private headers of the
  release commit (874f0cea9d). It also has HaikuPorts' devel packages for
  the release's libraries that Summit links (zlib, OpenSSL, curl, HarfBuzz,
  Fontconfig and the rest). Their headers may be a little newer, but the
  links lead to the release's own files, so a symbol the release lacks fails
  the build. Scintilla and Lexilla, which the release lacks, come whole. The
  cross compiler is GCC 13.3.0, the same release as the system's
  `gcc_syslibs`.
- **Engine**: the same WebKit, Haiku port and options as the air/OS x86_64
  engine, compiled against that SDK, in trees of its own. It links the
  release's own libraries: OpenSSL, curl, nghttp2, libpsl, SQLite, libxml2,
  libxslt, libjpeg-turbo, libpng, libwebp, Little CMS, Brotli, WOFF2,
  FreeType, Expat, Fontconfig, HarfBuzz and libexecinfo. The package
  carries only what the release lacks: ICU 78, libzip, libepoxy, and a
  private Mesa (EGL, llvmpipe, softpipe, Zink), because Haiku's Mesa has no
  EGL. A second copy of a system library would clash in Summit's processes.
  The Media Kit's FFmpeg reader, for example, loads the system's libass,
  which needs the system's newer Fontconfig, and does not load at all next
  to an older copy. The air/OS x86_64 engine has done the same with the
  image's own libraries since 12 October 2026. arm64 air/OS images have no
  HaikuPorts; there the engine carries the same jmgasper builds as the
  images' `airos_*` packages.
- **Browser**: compiled with `SUMMIT_AIROS=0` (`src/core/BuildTarget.h`).
  It asks app_server for nothing that only air/OS has. Air/OS's
  `B_DIRECT_DEVICE_PIXELS` window flag is left out, because a stock
  app_server refuses every `SetFlags()` call that includes it. Its About box
  says *Built for Haiku R1/beta6*.
- **Packages**: they require `haiku >= r1~beta6_hrev59866_79`, each
  library they link from the release at the release's version (worked out
  from the binaries, as HaikuPorts does), and the HaikuPorts libraries
  above. They have no post-install script, and they never go into the
  air/OS package pool that images are made from.

The engine needs no switch for the platform: each air/OS feature is looked
up when it is used, and is skipped when it is missing.

## What was checked on stock R1/beta6

The first Haiku build (Summit 833cd3d, `summit_webkit-1.10.1~git20261011.0655`
and `summit-0.1.0~git20261011.0655`) was checked on 11 October 2026 on an
untouched R1/beta6 (hrev59866_79) machine made from the release ISO. The
machine was a QEMU/KVM PC with 8 cores, 8 GB, VESA graphics, HD Audio and
no GPU.

- `pkgman install` of the two files installed `llvm21_libs`, `vulkan`,
  `lexilla` and `scintilla` from HaikuPorts and changed nothing else. The
  default browser stayed WebPositive. The About box says *Built for Haiku
  R1/beta6*.
- Every library and symbol of the browser, the web and network processes
  and the private Mesa resolves against the release and those four
  packages. The runtime loader logged nothing for Summit's processes except
  the `nss_files`/`nss_dns` lookups that libnetwork makes for every program.
- These worked:
  - pages over HTTPS (example.com, Wikipedia, YouTube);
  - wheel scrolling;
  - WebGL (drawn on llvmpipe) and WebAssembly;
  - MediaSource: YouTube played a video to the end;
  - local H.264 MP4 and VP9 WebM video, decoded in software.

  `navigator.bluetooth` reports itself unavailable, as expected. Quitting
  ends every Summit process within a second, and the next launch restores
  the session.
- For comparison, the air/OS package of 7 October started on the same
  machine only after Scintilla and Lexilla were installed by hand, and it
  could not open any video. Its own older Fontconfig kept the Media Kit's
  FFmpeg reader from loading (libass needs `FcConfigSetDefaultSubstitute`).
  The air/OS image of 11 October failed the same way on the same kind of
  test machine. The air/OS x86_64 packages built since 12 October link the
  image's libraries. On that image they load the FFmpeg reader, play the VP9
  test file and leave libbnetapi and libavformat on the system's OpenSSL.

The first page in a new profile waits a few seconds while Fontconfig builds
its cache, as WebPositive's first start does. Only Latin, Greek, Cyrillic and
CJK fonts come with Haiku, so other scripts need more fonts installed.

### Extension acceptance, 22 October 2026

The published `20261021.1632` browser and engine packages (Summit `cd9de3e`)
pass **252 native checks** and four rounds of **58 JavaScript badge checks**
on a fresh R1/beta6 hrev59866+79 VM, with four virtual CPUs, 4 GiB RAM and
standard VGA. `pkgman` installed the packages and their Lexilla, Scintilla,
LLVM 21 and Vulkan dependencies. The engine's installed build metadata names
the stock Haiku target and the tested commit.

`tests/ModernExtensionManagerTests.cpp`, compiled against the R1/beta6 SDK,
operates the actual manager, file picker, consent controls and popups. The
`startup` and `actions` extension fixtures verify required versus optional
permissions, duplicate rejection, saved storage across disable/enable and
browser restart, popup JavaScript and pointer input, toolbar icons and badge
pixels, action scopes, and rejection of stale invocations. Removal covers both
Cancel and Remove in the confirmation dialog, then checks that the extension
stays absent after another restart. The badge fixture waits for its page's
navigation to complete before testing per-tab overrides, which navigation
correctly clears.

All four browser processes and their helpers exited normally, with no forced
cleanup or new debugger events. Eight additional launches followed by an
immediate quit also exited cleanly. Runtime and fixture hashes remained
unchanged. The [machine-readable acceptance record](haiku-extensions-2026-10-22.json)
contains package and runtime hashes, per-phase results and the native crash-log
checks. The raw evidence is retained in `.vm/issue120-stock-20261022/` and in
the dedicated VM under `/Data/issue120/`.

These tests found a shutdown defect in the preceding published build: after
extension removal, a WebProcess could still be inside `load_image()` when
Summit exited, leaving its main thread suspended indefinitely. This reproduced
three times. `cd9de3e` tracks pending launches through their main-loop completion
and keeps the application loop running until they finish, including failure
and cancellation. The updated packages passed the same removal/shutdown
scenario and [release CI](https://github.com/jmgasper/summit/actions/runs/39635703321).
This fixture acceptance does not establish compatibility with every third-party
extension or account-backed feature.

## Testing on stock Haiku

`tools/haiku-vm/` makes a test machine from a release ISO. The machine has no
GPU, like most Haiku installations. Its disk is the ISO's image, with only
an SSH server added and no first-boot prompt or screen blanker, plus an
empty data volume. `check-packages.sh` installs two packages there with
`pkgman` and runs the checks above.

```sh
mkdir -p .vm/haiku-r1beta6 && ssh-keygen -t ed25519 -N '' -f .vm/haiku-r1beta6/id_ed25519
tools/haiku-vm/make-stock-vm.sh haiku-r1beta6-x86_64-anyboot.iso .vm/haiku-r1beta6
tools/haiku-vm/run-stock-vm.sh .vm/haiku-r1beta6        # VNC 127.0.0.1:5941
tools/haiku-vm/ssh.sh 'uname -a'
SUMMIT_VM_QMP=.vm/haiku-r1beta6/qmp.sock python3 tools/vm.py screenshot shot.png
# test videos, then the checks
ffmpeg -f lavfi -i testsrc=duration=12:size=640x360:rate=30 -c:v libx264 -pix_fmt yuv420p videos/test-h264.mp4
ffmpeg -f lavfi -i testsrc=duration=12:size=640x360:rate=30 -c:v libvpx-vp9 -b:v 500k videos/test-vp9.webm
SUMMIT_TEST_VIDEO_DIR=videos tools/haiku-vm/check-packages.sh summit_webkit-….hpkg summit-….hpkg
```
