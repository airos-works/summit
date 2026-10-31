# KunanyiOS / Haiku platform issues found while porting Summit

These are operating-system behaviours that Summit currently works around in its
WebKit port. Each one has a proper fix in the OS; the workarounds can be removed
once KunanyiOS ships those fixes. Found on Haiku R1/beta6 hrev59866+79 x86_64.

## Locale backend publishes uninitialized ctype tables (22 October 2026)

The first time conversion loads libroot's ICU backend, which publishes its ctype
table pointers before filling them. A concurrent `sscanf()` can reject ASCII
digits, causing libepoxy to abort on a valid Mesa version string. A standalone
native reproducer also fails on the airOS workstation. Summit initializes this
backend during engine startup before starting its workers. The OS still needs
to initialize the tables before publishing them and safely publish the backend.
See [the diagnosis, reproducer and validation](locale-startup-2026-10-22.md).

## 1. app_server drops text and bitmaps inside `BView::BeginLayer()` layers

**Symptom.** Any page content inside an element with `opacity < 1` lost its text
and images; only lines and rectangles were painted. `https://example.com/`
(whose body `div` has `opacity: 0.8`) rendered as an empty page with a lone
link underline.

**Cause.** `Layer::RenderToBitmap()` sizes the layer bitmap from the bounding
box of the recorded picture (`PictureBoundingBoxPlayer`). In
`src/servers/app/PictureBoundingBoxPlayer.cpp` two callbacks are still stubs:

```cpp
void BoundingBoxCallbacks::DrawPixels(...)           { BRect dest = _dest; /* TODO */ }
void BoundingBoxCallbacks::DrawStringLocations(...)  { /* TODO */ }
```

WebKit draws every glyph run with `BView::DrawString(string, locations, count)`
and every image with `DrawBitmap`, so neither contributes to the bounding box
and both are clipped away when the layer is composited.

**Workaround in Summit.** `GraphicsContextHaiku::beginTransparencyLayer()`
records a fully transparent `FillRect(clipBounds())` as the first operation of
each layer, so the layer always covers the clipped area
(`Source/WebCore/platform/graphics/haiku/GraphicsContextHaiku.cpp`). This costs a
larger layer bitmap than strictly necessary.

**Proper OS fix (suggested, untested).** Implement both callbacks:

```cpp
void
BoundingBoxCallbacks::DrawPixels(const BRect&, const BRect& _dest, uint32, uint32,
	size_t, color_space, uint32, const void*, size_t)
{
	BRect dest = _dest;
	fState->PenToLocalTransform().Apply(&dest);
	fState->IncludeRect(dest);
}

void
BoundingBoxCallbacks::DrawStringLocations(const char* string, size_t length,
	const BPoint locations[], size_t locationCount)
{
	ServerFont font = fState->GetDrawState()->Font();
	font_height height;
	font.GetHeight(height);
	// Conservative per-glyph box: one em wide around every glyph origin.
	const float size = font.Size();
	for (size_t i = 0; i < locationCount; i++) {
		BPoint origin = locations[i];
		fState->PenToLocalTransform().Apply(&origin);
		BRect rect(origin.x - size, origin.y - ceilf(height.ascent) - 1,
			origin.x + 2 * size, origin.y + ceilf(height.descent) + 1);
		fState->IncludeRect(rect);
	}
}
```

## 2. System `malloc` is slow under multi-threaded load

mimalloc's own `test-stress 8 20 25` workload takes **6.1 s** with the system
allocator (5.9 s of that in the kernel) against **0.79 s** with mimalloc built
from WebKit's bundled copy — about 7.8x. Summit's engine is switching from
`USE_SYSTEM_MALLOC` to WebKit's bundled mimalloc; other allocation-heavy
KunanyiOS software would benefit from the same, or from a faster libroot
allocator.

## 3. BFS has no hard links

WebKit's network disk cache stores large response bodies once and hard-links
them from each record. Every such store failed on BFS ("Failed to create hard
link…"), so large resources were never served from the disk cache. Summit now
stores the body with its record on Haiku (`NetworkCacheBlobStorage.cpp`), giving
up de-duplication of identical bodies.

## 4. Native notifications cannot call back into the sender

`BNotification` can launch an application or open a file on click, but offers no
activation callback, and its notifications have no buttons. Summit works
around it (#102): an extension's notification starts the browser again with
`--summit-extension-notification-click` and the notification's identity, the
running instance routes the click to the extension, and a small window
offers the buttons. Real per-button actions and a "closed by the user" report
would still need the notification server's help.

## 5. Stock Mesa 22.0.5 EGL does not initialize

`eglInitialize()` fails with `EGL_NOT_INITIALIZED` on the stock x86_64 packages,
so GL compositing cannot be exercised there. KunanyiOS' own Mesa 25.3 port has a
working Haiku EGL platform (pbuffers and windows); see `docs/vm-egl-stack.md`
once the private VM build is recorded.

## 6. The audio mixer crashes when a sound player connects and no audio output exists

**Symptom.** Opening https://www.reddit.com/ (which autoplays video) crashed
`media_addon_server` on a machine without sound hardware (the QEMU VM):
`Segment violation` in `AudioMixer::Connected` (`mixer.media_addon` + 0x126).
Every later sound playback in the session failed until the server was restarted.

**Cause.** In `src/add-ons/media/media-add-ons/mixer/AudioMixer.cpp`,
`AudioMixer::Connected()` auto-starts the mixer when its first input connects:

```cpp
if (fAutoStop && fCore->CountInputs() == 1) {
	...
	roster->NodeIDFor(fCore->Output()->MediaOutput().source.port)   // Output() is NULL
```

`fCore->Output()` is null when no output (sound card) is connected, so any
application that creates a `BSoundPlayer` crashes the add-on server.

**Workaround in Summit.** The media player creates a sound player only when
`BMediaRoster::GetAudioOutput()` succeeds and the decoded audio format has no
wildcards; otherwise the media plays without sound (`MediaPlayerPrivateHaiku.cpp`).

**Proper OS fix (suggested).** Skip the auto-start while there is no output:

```cpp
if (fAutoStop && fCore->CountInputs() == 1 && fCore->Output() != NULL) {
```

## libnetwork: `nsswitch_conf_file_path()` is not thread-safe, and the parser exits the process

`src/system/libnetwork/netresolv/net/nsdispatch.c`: `nsswitch_conf_file_path()`
builds the settings path in a `static char path[256]` on every call
(`find_directory()` then `strlcat()`), and `_nsconfigure()` reads it before
taking `_nsconflock`. When several threads resolve names at once (libcurl's
threaded resolver starts one thread per lookup; a session of 36 tabs starts a
dozen), one thread can `stat()` the buffer while another has just written the
settings directory into it: the directory exists, its mtime is newer than the
last parse, `fopen()` of a directory succeeds on Haiku, and the flex scanner
`_nsyylex` fails its first read with "input in flex scanner failed" and calls
`exit()` -- from a resolver thread, so the whole process dies; Summit's
network process then segfaults in `BMessage::_SendMessage` because libbe's
statics are already gone (reports `NetworkProcess-137701-debug-28-09-2026-11-04-31`
and the two older NetworkProcess reports). `/boot/system/settings/network/nsswitch.conf`
does not exist on the X399, so no parse should ever happen there. Fix: compute
the path once (`pthread_once`), or take `_nsconflock` before computing it; the
scanner should also fail the lookup rather than exit the process.

Summit resolves `localhost` at network process start-up so the first
`nsdispatch` happens on the main thread, which does not close this race
(it still killed the network process in most 36-tab starts once four curl
workers resolved names concurrently). What does close it, until libnetwork
is fixed: a real `/boot/system/settings/network/nsswitch.conf` (`hosts:
files dns`, the same sources as without a file) whose mtime is newer than
`/boot/system/settings`. The first lookup parses it and records its mtime;
a later racy `stat()` of the half-built path finds the directory, which is
older, and returns without parsing. `tools/install-on-workstation.sh` now
creates that file when it is missing.

**29 September 2026.** The same exit takes web processes: their media
loaders fetch with libcurl (crash report of 29 September, thread
`libcurl.so.4 pthread`: `_kern_exit_team` from `_nsyylex`, `_nsyyparse`,
`nsdispatch`, `getaddrinfo`). 3 of 57 runs of the build installed that
morning ended with it. Summit now makes the first lookup on the main thread
of the browser and of every web process, and the browser creates the file
and keeps it newer than `/boot/system/settings` and its `network`
directory at every start. Anything that changes those directories while a
browser runs reopens the race, so the fix in libnetwork is still wanted;
the OS session has one built (path assembled once, non-regular files
refused, the scanner's fatal error returned instead of `exit()`), not yet
installed.

## app_server copies a presented frame twice, with a slow `memcpy`

A browser window that presents 3840x1756 frames costs its app_server
window thread 8 ms a frame in `memcpy` (`profile -a -k`, 29 September):
`Painter::BitmapPainter` copies the bitmap into the back buffer row by row
and `HWInterface::_CopyToFront` copies that to the screen. libroot's
x86_64 `memcpy` is `rep movsb` above 2 KiB, and the 1950X has no ERMS. The
same 27 MB take 4.1 ms with that `memcpy`, 2.3 ms with SSE2 non-temporal
stores on one thread and 1.3 ms on four
(`tools/mesa-vm/mesa-25.3.6-summit-05-frame-copies.patch`,
`summit_parallel_copy.c`). Direct windows are stopped at render scales
other than 100%, so there is no way around these copies at 200%.

## NVK: a copy narrower than the image is slower per pixel

`glReadPixels` through zink, which becomes `vkCmdCopyImageToBuffer`, of a
3840x1756 BGRA8 target on the GTX 1070 (`tools/mesa-vm/video-probe.cpp`):

| rectangle | megapixels | time |
| --- | --- | --- |
| 0,0 3840x1756 | 6.74 | 4.0 ms |
| 0,136 3840x1498 | 5.75 | 3.2 ms |
| 32,136 2664x1498 | 3.99 | 5.9 ms |
| 0,0 2664x1498 | 3.99 | 5.9 ms |
| 0,0 3776x1756 | 6.63 | 6.1 ms |
| 0,0 1920x878 | 1.69 | 2.6 ms |
| 0,0 3840x878 | 3.37 | 2.2 ms |

Rows that span the image cost 0.56 ms a megapixel, narrower ones 1.5,
whatever the offset. The CPU's share is the same in both (the copy out of
the staging buffer). Summit reads full-width bands.

## BFS: a new file costs 6 ms

**Measured** on the workstation's `/boot` (bfs, 238 GiB, 80% used), 30
September 2026, nothing else running, Python `open`, one `write`, `close`:

| | in all | each |
| --- | --- | --- |
| 300 new files of 2 KB | 1832 ms | 6.11 ms |
| 300 new files of 24 KB | 1859 ms | 6.20 ms |
| 100 new files of 200 KB | 624 ms | 6.24 ms |
| removing 300 files | 12 ms | |

**Where it shows.** WebKit's network cache writes one file for each
resource, a few hundred for a page. While The Guardian loads, the network
process's cache thread is on a processor 1533 to 1707 ms of 4000, three
quarters of it in `BlockAllocator::AllocateBlocks()` (through
`Inode::SetFileSize()`, `Inode::_GrowStream()`, `BlockAllocator::Allocate()`).

**Not measured**: why. The OS session reads the allocator's source as a
search, one bit at a time, for a free run of 64 KB (what every file is
given when it gets its first data), through allocation groups that have
none; it proposes the same test on a fresh volume to tell this volume's
state from a fixed cost. Whether the writes hold up other processes was not
measured either.

**In Summit**: nothing. The thread has the lowest priority and the machine
has processors to spare.

## The GPU's performance state

`NV2080_CTRL_CMD_PERF_GET_CURRENT_PSTATE` returns P0 idle, under a browser
and under a GL probe (170 samples at 10 a second), and
`NV2080_CTRL_CMD_PERF_BOOST` with `BOOST_TO_MAX` returns success and
changes no timing (`tools/mesa-vm/rmperf.c`). Earlier notes that the card
stays at its boot clocks were inferred from frame rates. The headers on
disk have no control that returns clock frequencies, so the clocks
themselves are unread.


## Raspberry Pi 4 (air/OS, hrev60097+621, 5 October 2026)

### A kernel panic in `VMAnonymousCache::Commit()` from `madvise()`

```
PANIC: ASSERT FAILED (../haiku/src/system/kernel/vm/VMAnonymousCache.cpp:756):
  size >= (page_count * 4096); cache 0xffff0000d6c4f368
  VMAnonymousCache::Commit <- VMAnonymousCache::Discard <- _user_memory_advice
  <- libroot _kern_memory_advice <- libJavaScriptCore (thread exit:
  __pthread_key_call_destructors) on "JSC Heap Collector Thread"
CACHE: type RAM, virtual_end 0x4000a000, temporary, 84773 pages,
  one area "libJavaScriptCore.so.18 mmap ar", 1 GiB
```

`Discard()` lowers the commitment by the bytes `VMCache::Discard()` reports
(`Commit(fCommittedSize - discarded)`); here more pages stayed in the cache
than the lowered commitment covers. It happened with the system nearly out
of memory (95%: two hung RAM disks held 2.3 GB, below), as a web process's
mimalloc returned memory with `MADV_FREE` at a thread's exit. Summit uses
`MADV_FREE` since 27 September (Haiku ignores `MADV_DONTNEED`). Captured from
the NanoKVM: `.vm/pi-panic-2026-10-05/kdl.jpg`. The board was restarted from
the kernel debugger over its serial console (`reboot` on the NanoKVM's
`/dev/ttyS1`); the NanoKVM's keyboard does not reach the kernel debugger.

### The VideoCore decoder needs 3 MB of contiguous memory below 1 GB

`rpi_mmal` receives each picture through a VCHIQ bulk transfer into a bounce
buffer the driver allocates per service and direction, physically contiguous
and below 1 GB (`ensure_bulk_buffer()`, `B_CONTIGUOUS`, `high_address = 1 GB`),
about 3 MB for a 1080p picture. After an hour and a half of use (3.6 GB
free) it failed: `rpi_mmal: receiving a picture: Out of memory`. The
firmware's side of that transfer then never completes, the decoder's later
requests time out (5 s each) and its mmal service answers nothing until the
next boot. Reserving the bounce buffers when the driver loads (or giving the
firmware page lists of the caller's buffer) would remove both.

**In Summit**: a decoder that fails is closed on a thread of its own, and when
closing takes seconds (or a request times out) the add-on is not used again
until the system restarts (a marker in `/tmp` that names the boot); videos go
to libavcodec.

### A helper whose parent dies inside `load_image()` stays suspended for good

`load_image()` returns the new team's main thread suspended; the parent
resumes it. A parent killed while `load_image()` loads the child's libraries
(0.3-0.6 s on the Pi) leaves a team with one suspended thread that nothing
will ever resume (~10 MiB each). The kernel could kill a team whose main
thread was never resumed when its parent dies.

**In Summit**: the first helper launch of a browser removes helpers of its own
engine whose parent is gone and whose threads are all suspended.

### RAM disks hang after a force-killed program ran from them

A network process force-killed while it ran from a `ramfs` (5 October 00:25)
stays in a kernel wait with that RAM disk; `ls`, `df`, `rm`, `profile -a`
then hang on it and its memory (2.3 GB of bundles here) is never freed. Only
a restart clears it.

### V3D cannot present into the screen, and hands buffers to the CPU whole (31 October 2026)

On zink (NVK, radeonsi) Summit's web process copies each frame into the
frame buffer and app_server's back copy with the GPU
(`summit_haiku_present_framebuffer`). The Pi's V3D driver cannot: it maps
only buffers it allocated into the V3D MMU, so neither the frame buffer nor
app_server's back copy can be a blit's destination, and Summit reads every
frame back (at 3840x2160, 6.9 Mpx: 57-98 ms a frame, 6-9 fps scrolling).
Needed: an ioctl that imports an area's pages as a buffer (with cache
maintenance for cached ones), and a Mesa v3d `summit_haiku_present_*`.

Its `CPU_PREPARE` invalidates (or cleans) the whole buffer: 12 ms for a
28 MB frame even when a caret changed. A prepare limited to a range would
make small reads and copies cheap.

**In Summit**: frames go into shared V3D buffers (Mesa
`haiku_mesa_shared_target_*`) instead of being copied by the CPU; small
damage is copied by the CPU so the buffer stays the CPU's.

### Writes to the SD card hold up other processes for seconds

The card is run at 25 MHz default speed and writes at ~1.4 MB/s. While
YouTube's first visit writes its HTTP cache and a 44 MB bytecode cache,
another process's small write (a profile save, a line to a log file on the
card) waits 4 to 11 s, and a team's teardown after `exit()` 12 to 14 s.
