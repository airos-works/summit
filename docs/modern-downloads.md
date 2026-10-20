# Modern downloads

The modern backend saves HTTP attachments, unsupported HTTP response types and
HTML download links into the native user's `Downloads` directory. The status
bar reports progress and completion. The toolbar's downloads button shows
their progress and opens a panel of the downloads, as Firefox's does
([issue-fixes-2026-10-19.md](issue-fixes-2026-10-19.md)); the panel and the
Window menu open the folder. Downloads can be paused and resumed, a failed one
continues where it stopped, and the list is kept in the profile
([below](#pause-resume-and-the-kept-list-107)). Quitting pauses the downloads
in progress; it first obtains provisional page approvals, then asks only when
a download would be lost (its server cannot resume it, or it belongs to a
private window). Keep Browsing resets those approvals and preserves live
documents, address drafts and selections. Confirmed quit keeps the application
loop and context alive until the pause replies finish.

`BWebKitContext` configures the destination and native message listener on the
application thread. It exposes asynchronous cancellation and pausing, resuming
and a pending-work query. `BWebKitView::DownloadURL` starts an explicit transfer. Navigation policy
and download callbacks register attachment, download-attribute and context-menu
operations in the same context-owned registry. The browser owns its listener
at application scope and forwards value-only messages to its window.

Each download has one identifier and a client that weakly references its
registry. The registry retains its proxy through cancellation replies and
releases it during process invalidation. A process crash must perform that
cleanup even if an earlier finish callback was observed while cancellation was
pending. Terminal messages contain the result, byte counts, path and any error.
An engine completion that wins a cancellation race retains its actual result.

The shared filename helper strips path components and controls and bounds names
without splitting UTF-8 characters. Collisions receive a numbered filename;
the network process creates files exclusively and cannot overwrite an existing
destination, except the partial file the registry hands it for a resumed
download. Cancelling removes the partial file; pausing keeps it; a transfer
failure or a killed network process removes it unless the host asked to keep
failed downloads (`SetKeepFailedDownloads`, which Summit does). The network
process never deletes a download's file itself: the UI process's registry
decides, after the transfer has stopped. The UI reports a killed network
process as a failure; the registry releases the failed operation and a
subsequent download can start with the same context.

## Pause, resume and the kept list (#107)

What the user sees. Each row of the downloads panel has round buttons at its
end: **Pause** (two bars) and **Cancel** (×) while it downloads; **Resume** (a
triangle) and **Cancel** once paused; **Retry** after a failure; the folder
once done. The row's menu has the same (Pause, Resume or Restart, Retry,
Cancel). A paused row says "Paused — 400 of 1000 MiB" with its bar greyed
where it stopped. Resume continues from there. If the server sends the whole
file again (it does not do ranges, or the file changed on it since), the
download starts over from zero, and the row says "Starting over — …" and the
status bar "The server could not resume the download, so it started over". A
paused download whose server said it cannot send parts of a file reads
"Paused — will start over" and offers **Restart**; its partial file is
removed. A failed download (a dropped connection, a server that stopped
answering) keeps what it had: **Retry** resumes it. Cancelling a paused
download removes its partial file.

The list is kept in the profile (`downloads.json`): finished, failed,
cancelled and paused downloads are there after a restart until Clear List
(which leaves paused ones) or Remove from List (which removes a failed
download's partial file too). Show in Folder and Open are greyed out, and the
row says "File moved or missing", for a file that is no longer there.
Quitting pauses what downloads, so the next start lists it as paused, ready to
resume. A download that a crash cut off comes back as "Interrupted — …" with
Resume. Private windows' downloads are not written and are forgotten, with
their partial files, when the private session ends.

How it works. Pausing is a cancellation that keeps the file:
`BWebKitContext::PauseDownload` (any looper) asks the registry
(`UIProcess/haiku/DownloadRegistryHaiku.cpp`) to cancel with the result
`B_WEBKIT_DOWNLOAD_PAUSED`; FINISHED says `partial` when the file is kept.
On Haiku `NetworkDataTaskCurl` only closes a download's file when it is
cancelled or fails; the registry removes it afterwards unless it is kept
(paused, or failed with keep-failed on). Once the server has answered, every
notification carries `request_url` (the address first asked for), `validator`
(the strong ETag, else Last-Modified) and `accepts_ranges`.

`BWebKitContext::ResumeDownload(url, path, validator, expectedSize, view,
&identifier)` (application thread) starts a page-less download
(`WebViewContextHaiku::resumeDownload`) of `url` with
`Range: bytes=N-` (N the partial file's size) and `If-Range: validator`, and
registers it with that path as its destination (the path must be a regular
file directly in the download directory, not written by another download; a
missing file or an empty path starts afresh with a new name). When the answer
arrives the registry decides: 206 from offset N continues (`resumed_from` N,
the counters start at N, the total from `Content-Range`); another 2xx starts
over (`restarted`); 416 with the file's size means it was complete; anything
else, or a 206 from another offset, fails and keeps the file. Without a
validator, a 206 whose total is not `expectedSize` fails too, so two versions
of a file are never joined. The registry then lets the network process write
to the existing file (`AllowOverwrite::Yes`): for a 206 that matches the
`Range` it sent, `NetworkDataTaskCurl` opens it read-write, truncates it to N
and appends; for a 200 it truncates it as before. The curl layer already asks
for `Accept-Encoding: identity` with a `Range`, so offsets are in the stored
bytes.

In the browser, `DownloadList` (`src/ui/DownloadList.cpp`) gives each
download an identifier of its own that survives pausing and resuming (each
transfer is another engine download), and writes the list, but private
entries, to `downloads.json` whenever something other than the byte count
changes (`core/DownloadHistory.cpp`, host tests in `tests/CoreTests.cpp`). An
entry still active in the file was cut off by a crash: it is read back paused
and interrupted, its byte count taken from the partial file's size. The panel
sends Pause, Resume and Cancel to the application (`kDownloadPause`,
`kDownloadResume`, `kDownloadCancel`), which calls the engine context the
download belongs to; cancelling a paused download only removes its file.
`downloads.pause`/`resume` of the extension API (#106) are still missing: an
extension's download item is one engine download, and resuming makes a new
one.

Summit's MCP server has a `downloads` tool (the list with ids, states and
status lines; `do` = start, pause, resume, cancel, retry, remove, clear), which
the tests below used.

Checked on the X399 (20 October) with a test copy of the browser on a test
engine (`claude-s36/bundle-dlr2`: CI's tree with the lead's engine changes of
that afternoon and these), against a local server that generates files from
their offsets, with `Accept-Ranges`, `ETag`, `Last-Modified`, `Range` and
`If-Range`, at 8–30 MB/s, and variants: no range support, an ETag changed
between pause and resume, a connection dropped part way, a server that stops
sending. Each finished file's SHA-256 on the X399 was that of the generated
content:

- A 1 GiB file paused at 40% (433,324,032 bytes, the partial file exactly
  that size) and resumed: the request had `Range: bytes=433324032-`,
  `If-Range` with the ETag and `Accept-Encoding: identity`, the server
  answered 206 from that byte, and the file's checksum was right.
- No range support: Pause (a real click on the panel's button) removed the
  partial file and the row said "Paused — will start over"; Restart
  downloaded the whole file again, to the same name.
- The ETag changed while paused: the server answered the `If-Range` request
  with 200 and the new file; the row said "Starting over — …", the status bar
  that the server could not resume, and the file was the new version.
- A connection dropped at 40%: "Failed — Transferred a partial file", the
  partial file kept; Retry (a real click) resumed with a 206. The same after
  the network process was killed ("Download network process exited"), and
  after a server that stopped sending was paused and resumed. A failed
  download from a server without ranges retried with a 200 and started over
  in the same file.
- Quitting during a 1 GiB download (no question: it can resume) paused it at
  493,092,864 bytes and wrote it to `downloads.json`; after a relaunch the
  list had it and the earlier downloads, and Resume in the row's menu
  finished it with the right checksum.
- Summit killed (`kill -9`) during a 1 GiB download: the network process
  exited, the partial file stayed; after a relaunch the row said
  "Interrupted — 0.43 of 1.00 GiB" and Resume finished it, checksum right.
- Quitting during a download from a server without ranges asked "A download
  is still in progress whose server cannot resume it: quitting pauses it,
  but it will have to start over"; Quit removed its partial file, and after
  the relaunch it was "Paused — will start over" and Restart downloaded it.
- Cancel removed the partial file of an active and of a paused download;
  Clear List left the paused one; Remove from List removed a failed
  download's partial file; rows of deleted files said "File moved or
  missing", their Open and Show in Folder greyed out.

Not tried: unplugging a cable (the X399 has to stay reachable). A transfer
whose server stops sending is not given up while the network is up (as in
Firefox; CurlRequest gives up on a request that stays quiet once the network
has gone away), so such a download is paused and resumed by hand. A Save As…
download that is paused and resumed stays in the Downloads folder.

Run against a completed frozen native bundle:

```sh
bash tools/test-download-names-in-vm.sh
python3 tools/test-modern-downloads.py --bundle /boot/home/summit/build-modern-browser/bundle-ID
python3 tools/test-modern-downloads.py --browser --bundle /boot/home/summit/build-modern-browser/bundle-ID
```

The API harness exercises eight real HTTP scenarios, including cancellation,
truncation, an exact owned network-process interruption, restart and completion
after the originating view closes. The browser harness uses a real HTML link,
native input and actual page/download dialogs. It verifies repeated page
approval, preserved edits and address selection, cancellation drain, normal
browser exit and partial-file removal. Test downloads use unique names; cleanup
removes only the verified fixture content. The HTTP servers use separate random
ports and tokens. Frozen inputs and binary hashes are checked before and after
each run. Exact results are recorded in [STATUS.md](STATUS.md).
