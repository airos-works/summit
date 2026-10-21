# Wheel targeting and scroll position synchronization

Wheel bursts sent through Summit's MCP server preserve the requested page
coordinates, horizontal and vertical deltas, and tab activation. Previously,
supplying `count` selected a path that discarded the coordinates and horizontal
delta. A burst intended for a snap container could therefore scroll the page
behind it. Repeated individual notches already reached both horizontal and
vertical snap containers correctly.

The engine also keeps each composed frame's scroll position consistent with
what page scripts read. After pending wheel input is processed, the scrolling
thread resamples animations before the main thread synchronizes the DOM scroll
position. Timer refreshes do not sample that frame again after scripts have
read it. Input arriving later contributes to the following frame. The scrolling
thread continues to advance independently when the main thread is busy.

This fixes a timing-dependent disagreement between the page and scroll-linked
effects. It does not make a smooth wheel animation linear: its speed still
changes between notches. The correctness check compares the rendered page
position with the position used by the script, rather than requiring identical
pixel increments on every frame.

## Reproduction and tests

Serve `tests/fixtures/scroll-snap.html`. It contains horizontal and vertical
mandatory snap containers with six items, a scrollable page, and optional
stripes positioned by `requestAnimationFrame` using `scrollY`. Those stripes
should align with the document's row boundaries throughout scrolling.

Use an isolated native browser with MCP enabled. Set `SUMMIT_TEST_MCP_TOKEN`
to its access token and run:

```sh
python3 tools/test-scroll-snap.py \
  --mcp-url http://HOST:PORT/mcp --tab 1.1 \
  --fixture-url http://FIXTURE_HOST:8766/scroll-snap.html
```

The test checks forward/reverse notches, both snap axes, bursts with one and
three events, unchanged page position, and rejected invalid coordinates/deltas.

For a visual timing check, call `beginScrollRecording(true)` in the fixture
through the inspector and send wheel bursts over the page outside the snap
containers. Use 40 notches with 37, 50 and 53 ms spacing to vary input timing
relative to the compositor. `scrollSamples` records both scroll events and rAF
positions. Compile the native screen reader:

```sh
c++ -std=c++23 -O1 -Wall -Wextra tests/ScrollPositionCapture.cpp \
  -lbe -o ScrollPositionCapture
```

It takes logical screen coordinates for a document column and the script's
stripe column, followed by the capture's top, height, sample count and interval
in microseconds. For a page view at `(75, 186)` sized `1121 × 538`, this captures
both columns below the snap containers:

```sh
./ScrollPositionCapture 925 1156 466 240 180 16667 > positions.csv
```

Start capture before sending the wheel burst. Keep the test window visible
until capture exits; allow extra time for screen reads on large desktops.
The reader compares actual app_server pixels, rejects missing stripes, and
fails on more than one pixel of displacement or inconsistent stripe shapes.

## Native results, 21 October 2026

At 200%, the old engine (`4db8e46`) reproduced up to nine pixels of disagreement
at 37 ms notch spacing and three pixels at 53 ms. With engine `31a91fe`, all
2,520 captured samples aligned exactly: eight 50 ms runs and three runs each
at 37 and 53 ms. The snap suite also passed at 200%.

The snap suite passed at native 100% as well. At that scale, direct-presentation
readback occasionally returned all-white columns; those runs were rejected.
With `SUMMIT_DIRECT_PRESENT=0` (GPU composition presented through the native
BView), all 1,260 captured samples aligned exactly across the same three notch
intervals. Scroll events and rAF also reported the same positions. The 200%
checks used the default direct presentation path. Display settings and the
original pointer were restored, and the isolated test browser exited normally.
