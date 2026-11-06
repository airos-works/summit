# Unloading tabs (#116, #106)

A background tab can be unloaded: its page and what it holds (the web
process, if no other page uses it, its GPU contexts, tiles and decoded
images) are freed, and the tab keeps its place, title and icon until it is
selected, when its address loads again. Restored sessions' tabs already
waited this way; unloading puts a loaded tab back in that state.

- **Unload Tab** in a tab's menu.
- `tabs.discard(tabId?)` for extensions (Chrome's form): without a tab,
  the least recently selected background tab. The tab gets a new id, as in
  Chrome; `Tab.discarded` (and `status: "unloaded"`) and
  `tabs.query({discarded})` report unloaded tabs, restored-session tabs
  included.

Not unloaded: the shown tab, tabs playing sound, capturing the camera,
microphone or screen or keeping the screen on, Summit's own pages and
extension pages, and tabs with Developer Tools, a crash or load-error page,
Reader view, Picture in Picture or full screen. Unloading forgets the tab's
back and forward lists and its translation.

## How

`BrowserWindow::DiscardTab` (`BrowserWindow.cpp`) puts a new `BWebKitView`
in the tab's card (set up by `AttachPageView`, as `CreateTab` sets up its
views), deletes the old view and sets `Tab::deferredURL`, which
`SelectTab` loads. `BWebKitContext::SetBrowserTabDiscarded` tells the
engine's tab registry (`BrowserTabRegistryHaiku::setDiscarded`), which
`WebExtensionTab::isDiscarded` reads. `tabs.discard` sends
`TabsDiscardHaiku`, forwarded as a `B_WEBKIT_BROWSER_DISCARD_TAB` browser
command answered with the new view.

- **By itself when memory is low:** every 5 s (30 s until 6 November)
  each window checks what the RAM has left (free pages and the file cache:
  `max_pages - used_pages`); below a tenth of the total (and at least 512
  MiB) it unloads its background tab unseen the longest, if that has been
  at least five minutes, one tab a check while memory stays low, and says
  so in the status line. Until #163 this read `system_info.free_memory`,
  which counts the swap file, so with swap on it never fired while the RAM
  filled. `SUMMIT_UNLOAD_BELOW_MIB` and `SUMMIT_UNLOAD_AFTER_S` change the
  limit and the five minutes; 0 for either turns it off.

- **Before memory runs out:** below half that limit (about 780 MiB on a
  16 GB machine) the longest-unseen background tabs go however
  recently they were looked at, as long as each has been behind for 10 s
  (`SUMMIT_UNLOAD_CRITICAL_AFTER_S`), until their processes' memory covers
  what is missing to the limit, at most four a check and then none for
  10 s while those processes end. Haiku has no out-of-memory killer: on
  6 November the 16 GB laptop froze (it still answered ping,
  but nothing in user land replied) with ten tabs (six of manage.auth0.com at about
  490 MiB of process and 150-250 MiB of GPU memory each, AWS, Proton Mail,
  Confluence) and a few application pages more opened at once. All of
  them had been looked at in the last minutes, so the five-minute (then
  one-minute) rule unloaded nothing, and the 30 s check came too late
  anyway: six new tabs of a heavy site took 4 GB in under 10 s.

- **By itself when memory is getting tight (#172):** below a quarter of
  the RAM left, a background tab unseen for 30 minutes whose web process has
  grown past 1 GiB (resident, without app_server's shared screen buffer) is
  unloaded, the biggest first, one a check; pinned tabs and tabs sharing
  their process with another tab stay. `SUMMIT_UNLOAD_TIGHT_PERCENT` (25),
  `SUMMIT_UNLOAD_IDLE_S` (1800), `SUMMIT_UNLOAD_IDLE_MIB` (1024); 0 turns it off.

Preferences › Browsing › Memory has a switch for it ("Unload tabs left in
the background for 5 minutes when memory runs low", on by default; the
profile's `unloadWhenMemoryIsLow`); the environment variables still
override the limits.

Not done yet: unloading after a long time in the background regardless of
memory.

## Fixture

`tests/fixtures/extensions/tab-discard/`: install it, read memory during the
first 20 s (three loaded background tabs) and after the discards, and read
the `[discard]` lines.

Verified on the X399 (24 Oct, td116 build): unloading three loaded
background tabs (Wikipedia, haiku-os.org, example.org) let their web
processes exit, and Summit's resident memory went from 490 to 236 MiB
(about 85 MiB a tab). `tabs.discard` answered with new ids, `discarded:
true` and the tabs' addresses; `tabs.query({discarded: true})` listed their
titles; the least recently used tab was chosen without an id; selecting one
loaded it again (`status: complete`); the shown tab and a missing tab were
refused; the tab strip kept the unloaded tabs' titles.

Unloading by itself was checked on the X399 (24 Oct, au116 build) with
`SUMMIT_UNLOAD_BELOW_MIB=10000000 SUMMIT_UNLOAD_AFTER_S=20`: three
background tabs were unloaded within 95 s, one a check, the shown tab kept
its page and the unloaded ones their titles.

The step before memory runs out was checked on the laptop (6 November,
Summit 98f7978 + this change on engine git20261105.1055) with
`SUMMIT_UNLOAD_BELOW_MIB=12000`, so that it acts below 6,000 MiB left: the
owner's ten tabs selected one after the other, 10 s apart. After the
fourth and again after the sixth was selected, the machine went below
6,000 MiB left between two samples and four background tabs were unloaded
each time (the shown one stayed); no 10 s sample saw less than 6,241 MiB
left, and the unloaded tabs loaded again when selected. A first build with
no pause between checks unloaded seven tabs in two checks 5 s apart, as the
first four processes had not ended yet.
