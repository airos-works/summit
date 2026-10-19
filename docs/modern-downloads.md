# Modern downloads

The modern backend saves HTTP attachments, unsupported HTTP response types and
HTML download links into the native user's `Downloads` directory. The status
bar reports progress and completion. The toolbar's downloads button shows
their progress and opens a panel of this session's downloads, as Firefox's
does ([issue-fixes-2026-10-19.md](issue-fixes-2026-10-19.md)); the panel and
the Window menu open the folder. A window close first obtains provisional page approvals, then asks
whether to cancel active downloads. Keep Browsing resets those approvals and
preserves live documents, address drafts and selections. Confirmed quit keeps
the application loop and context alive until cancellation replies finish.

`BWebKitContext` configures the destination and native message listener on the
application thread. It exposes asynchronous cancellation and a pending-work
query. `BWebKitView::DownloadURL` starts an explicit transfer. Navigation policy
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
destination. Normal cancellation and transfer failures remove partial files.
A killed network process can leave its incomplete file, and the UI reports the
download as failed. The registry releases the failed operation and a subsequent
download can start with the same context. Crash recovery, a persistent download
manager and pause/resume remain unfinished; the Curl backend does not support
download resume data.

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
