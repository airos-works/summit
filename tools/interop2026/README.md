# Interop 2026 tests against Summit

Runs the web-platform-tests of the [Interop 2026](https://wpt.fyi/interop-2026)
focus areas against Summit on an air/OS machine (the X399) through
`summitdriver` and wptrunner, and scores them as wpt.fyi does. Results and the
issues they produced: [#175](https://github.com/airos-works/summit/issues/175).

First run, 3 Nov 2026 (Summit 0.1.0~git20261103.0306): **90.9** best estimate
(89.3 plain), Safari Technology Preview 99.6, Safari 27.2 79.6. The whole
suite (1714 tests) takes about 30 minutes.

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
| `x399/size.sh` | Gives the automated window an 800x600 viewport with `hey` (Set Window Rect does nothing, #187) |
| `x399/noblank.sh` | Keeps screen_blanker away while tests run |

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

## What the wpt patch changes, and why

- `wpt run` does not insist on wpt's names in the build host's `/etc/hosts`
  for the `webkit` product (no root here; the browser resolves them on the
  test machine). The server binds to `WPT_SERVER_HOST` (`env_options`).
- `--webkit-port=summit` sends `summit:browserOptions` (binary, args) and
  `acceptInsecureCerts`.
- After connecting, the window is sized with `x399/size.sh` (Set Window Rect
  is ignored, #187) and the runner reloaded so the viewport is 800x600 CSS
  pixels. The X399 runs at 2x, so WebDriver screenshots are 1600x1200; test
  and reference are taken the same way.
- Closing a window waits up to 2 s for its handle to go (Close Window answers
  early, #187).
- `SUMMIT_SCREEN_SHOTS=1`: reftest pictures come from Summit's MCP
  `screenshot` (what the screen shows) instead of WebDriver, whose pictures
  leave out composited content (#186). Neither is perfect: compare both runs.

## Traps

- Pop-ups are blocked in automation sessions (#187): the profile allows them
  per origin on the fixed ports only (8000, 8001, 8443, 8444, 9000).
- The wpt host names must be in the test machine's hosts file for every
  run; without them each testharness test ends in
  `__wptrunner_process_next_event is not a function`.
- `pkill -f PATTERN` matches the shell running it when the command line
  contains PATTERN: stop shims and servers in a separate command.
- An orphaned `wpt serve` keeps ports 8000/8443/9000; `ss -ltnp` shows it.
- wpt's Python is 3.14 here; the `webkit` product's accessibility requirements
  (PyGObject, pycairo) do not build without cairo headers, so `run.sh` leaves
  out `aamtest`.
