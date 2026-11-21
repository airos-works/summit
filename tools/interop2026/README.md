# Interop 2026 tests against Summit

Runs the web-platform-tests of the [Interop 2026](https://wpt.fyi/interop-2026)
focus areas against Summit on an air/OS machine (the X399) through
`summitdriver` and wptrunner, and scores them as wpt.fyi does. Results and the
issues they produced: [#175](https://github.com/airos-works/summit/issues/175).

First run, 3 Nov 2026 (Summit 0.1.0~git20261103.0306): **90.9** best estimate
(89.3 plain), Safari Technology Preview 99.6, Safari 27.2 79.6. The whole
suite (1714 tests) takes about 30 minutes.

The November 19 follow-up lists results for every previously unresolved URL across
the 2025 and 2026 focus areas. [Follow-up report and per-URL results](../../docs/interop-2025-2026-followup-2026-11-19.md)

## Pieces

| File | What it does |
|---|---|
| `labels.py` | Test lists from wpt.fyi's `interop-data.json` and a wpt-metadata checkout: `labels.json` (area → tests), `all.txt` |
| `wpt-summit.patch` | Local changes to wpt (wptrunner) so it can drive Summit; see below |
| `summitdriver-shim` | The "WebDriver binary" for wptrunner: starts `WebKitWebDriver` on the test machine over SSH and forwards the port; resets the test profile each time |
| `make-profile.py` | The test profile: trusts wpt's certificate for all wpt hosts, allows pop-ups for wpt origins, HTTPS-First/session restore/update checks off, MCP on |
| `run.sh LIST NAME [wpt args]` | `wpt run webkit --webkit-port=summit ...`; results in `$INTEROP_ROOT/results/NAME.{json,log}` |
| `score.py RUN.json ...` | Per-area scores next to Safari TP and Safari stable (wpt.fyi summaries), and `results/tests.json` for triage |
| `gaps.py [RUN ...]` | Best result per test over several runs, and the points each failure group is worth |
| `mcp.py` | Small MCP client (Summit's MCP server, for on-screen screenshots) |
| `x399/size.sh` | Fallback for releases that need Haiku window scripting to set the viewport |
| `x399/noblank.sh` | Keeps screen_blanker away while tests run |
| `sp3.sh BUILD RUNS` | Speedometer 3.1 (browserbench.org) in a test copy, each run in a fresh Summit and profile, scores read over MCP; `SP3_ENV` adds settings, `SP3_PROFILE=SECONDS` takes a system profile 40 s in |

## Setting up

On the build host (`INTEROP_ROOT`, default `/data1/airos/interop2026`):

```sh
cd $INTEROP_ROOT
git clone --depth 1 https://github.com/web-platform-tests/wpt.git
git clone --depth 1 https://github.com/web-platform-tests/wpt-metadata.git
git -C wpt apply $KIT/wpt-summit.patch
python3 $KIT/labels.py wpt-metadata .            # labels.json, all.txt
(cd wpt && ./wpt make-hosts-file | sed 's/^127\.0\.0\.1/192.168.1.200/') > hosts.txt
curl -sL https://raw.githubusercontent.com/web-platform-tests/wpt.fyi/main/webapp/static/interop-data.json -o interop-data.json
# Safari TP / stable per-test summaries for score.py: the results_url of
# https://wpt.fyi/api/runs?product=safari&label=experimental&label=master&max-count=1
# (and label=stable), saved with curl --compressed as stp-summary.json / safari-summary.json
```

For the combined 2025 and 2026 review, use the saved dashboard configuration
and the metadata checkout from the review. The combined `labels.json` uses
year-qualified area names. `all.txt` lists each test URL once across both years.
`score.py` reports a mean for each year's focus areas. Each URL in `tests.json`
has an `areas` list with every matching focus area and retains the legacy
`area` field. `gaps.py` weights a URL in each area to which it belongs when
estimating the annual point gain. Its issue list includes each URL once.

```sh
python3 "$KIT/labels.py" wpt-metadata both-years --year 2025 --year 2026 --interop-data interop-data.json
```

On the test machine (`SUMMIT_REMOTE_DIR`, default `/boot/home/interop`):

1. Extract the Summit packages under test into `xh/` (`package extract` each
   `.hpkg`), and point the app at its own engine: the installed binary's RPATH
   is `/boot/system/lib/summit-webkit/lib`, so replace it in place with the
   same-length `$ORIGIN/../lib/summit-webkit/lib` (patchelf would drop the
   Haiku resources):
   `perl -0777 -pi -e 's{/boot/system/lib/summit-webkit/lib\0}{\$ORIGIN/../lib/summit-webkit/lib\0\0\0}' xh/apps/Summit`.
   Use a build that runs on the machine's OS (the 3 Nov air/OS build needed a
   newer libbe than the X399 had, #192; the stock-Haiku build ran).
2. Append `hosts.txt` to `/boot/system/settings/network/hosts` (wpt's host
   names → the build host). Remove the block afterwards.
3. Start that Summit once with `--profile` on an empty folder, copy its
   `profile.json` back, and make the test profile from it:
   `make-profile.py base.json hosts.txt profile.json.seed`; copy the seed to
   `$SUMMIT_REMOTE_DIR/profile.json.seed`.
4. Copy `x399/size.sh` and `x399/noblank.sh` there; start `noblank.sh` for the
   duration of the runs (a blanked desktop changes how frames are presented).

## Running

```sh
run.sh $INTEROP_ROOT/all.txt full                     # everything, WebDriver screenshots
SUMMIT_SCREEN_SHOTS=1 run.sh $INTEROP_ROOT/all.txt screen --test-types reftest
python3 score.py $INTEROP_ROOT/results/full.json $INTEROP_ROOT/results/screen.json
python3 gaps.py full screen
```

`WPT_SERVER_HOST` is the build host's address as the test machine sees it
(default 192.168.1.200), `SUMMIT_SSH` the ssh command reaching the test
machine (default the X399's lab config), `SUMMIT_REMOTE_BUILD` the folder
of the Summit to test in `SUMMIT_REMOTE_DIR` (default `xh`; e.g. a copy
with a test engine's libraries in `lib/summit-webkit/lib`) and
`SUMMIT_REMOTE_ENV` extra `NAME=VALUE` settings for it (its stderr lands in
the wpt log).
Every run also gets `SUMMIT_TEST_ENV` (default `SUMMIT_OVERLAY_SCROLLBARS=0`:
classic scroll bars, which never fade; set it empty for the overlay ones).

Without a board, a local air/OS VM works too (it reproduces the X399's
reftest pictures): make one from the latest x86_64 image with
`tools/haiku-vm/make-stock-vm.sh`, boot it with `run-stock-vm.sh`, set its
screen to 2560x1600 (`echo y | screenmode 2560 1600 24 60`), link
`/boot/home/interop` to the data volume, append `hosts.txt` with the build
host as `10.0.2.2`, and run with `SUMMIT_SSH` set to an ssh command for the
VM and `WPT_SERVER_HOST=127.0.0.1` (QEMU forwards 10.0.2.2 to the host's
loopback). For 2x runs give the window twice the frame (`size.sh` multiplies
by the number in `$SUMMIT_REMOTE_DIR/scale`, if present). A blank picture
passes every reftest, so check that failures and passes show real pages.

## What the wpt patch changes, and why

- `anchor-scroll-position-try-014.html` only shows a vertical scrollbar.
  With a 21 px horizontal scrollbar, the reversed flex scroller is only
  179 px tall inside. At `scrollTop = -116` the bottom fallback still fits
  by 5 px, so Summit correctly keeps it. Using the full 200 px height makes
  the bottom fallback overflow at the tested offset and exercises the flip.
  [Summit #238](https://github.com/airos-works/summit/issues/238)
- `wpt run` does not insist on wpt's names in the build host's `/etc/hosts`
  for the `webkit` product (no root here; the browser resolves them on the
  test machine). The server binds to `WPT_SERVER_HOST` (`env_options`).
- `--webkit-port=summit` sends `summit:browserOptions` (binary, args) and
  `acceptInsecureCerts`.
- After connecting, the runner measures the CSS viewport and uses WebDriver
  Set Window Rect to resize it to 800×600. Older builds can use the
  `x399/size.sh` fallback. On a 2× display, a viewport screenshot is 1600×1200 pixels.
- Closing a window waits up to two seconds for its handle to disappear on
  builds that acknowledge Close Window early.

- `SUMMIT_SCREEN_SHOTS=1`: reftest pictures come from Summit's MCP
  `screenshot` (what the screen shows) instead of WebDriver, whose pictures
  leave out composited content (#186). Neither is perfect: compare both runs.

## Parallel runs over several VMs

The November 21 run of all 4,615 labelled 2025 and 2026 URLs took about
30 minutes with `--processes=4` over four copy-on-write clones of one air/OS
VM. One VM with one process takes about 5 hours. The image already shipped the
Summit under test, so the runner's build folder was a link to `/boot/system`
(`SUMMIT_REMOTE_BUILD=sys`). Results and open work:
[#243](https://github.com/airos-works/summit/issues/243#issuecomment-7051838433).

- Give each wptrunner process its own VM. Use a wrapper as the
  `--webdriver-binary` that takes the first free VM with `flock` and runs
  `summitdriver-shim` with that VM's `SUMMIT_SSH`. The wrapper has to keep the
  lock itself and run the shim as a child process, because ssh closes
  inherited descriptors. If a lock is handed down through `exec`, it is
  released, two processes share a VM, and each kills the other's Summit.
- Start the clones without `-vnc`. With VNC enabled, QEMU 10.2.1 on the build
  host segfaulted at the same address in two guests at the same moment (other
  sessions' VMs crashed in the same way). wptrunner then drops those processes'
  queues ("Max restarts exceeded"). Rerun the URLs missing from the report and
  merge them.
- On this image, changes to the system volume do not survive a reboot. After
  each boot, append the hosts block again and recreate the `/boot/home/interop`
  link to the data volume.
- Run a single test with `--no-pause-after-test`, or the run waits for the
  browser to close.
- To compare with wpt.fyi, use the current WPT and wpt-metadata (labels change,
  and tests are renamed or updated) rather than the pinned review revisions.

## Traps

- Automated top-level pages can open pop-ups (#187). The test profile also
  allows pop-ups for each WPT origin on ports 8000, 8001, 8443, 8444 and 9000.
  In an embedded frame, check whether `window.open()` returned `null` before
  diagnosing a missing message from the new window. A call inside a trusted
  click handler can succeed even when the same call after WebDriver
  acknowledges the click is blocked.
- The wpt host names must be in the test machine's hosts file for every
  run; without them each testharness test ends in
  `__wptrunner_process_next_event is not a function`.
- `pkill -f PATTERN` matches the shell running it when the command line
  contains PATTERN: stop shims and servers in a separate command.
- An orphaned `wpt serve` keeps ports 8000/8443/9000; `ss -ltnp` shows it.
- wpt's Python is 3.14 here; the `webkit` product's accessibility requirements
  (PyGObject, pycairo) do not build without cairo headers, so `run.sh` leaves
  out `aamtest`.
