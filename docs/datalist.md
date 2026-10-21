# Native datalist suggestions

The modern engine displays `<input list>` suggestions in a native, borderless
list below the field. The popup belongs to the field's window and leaves keyboard
focus in the input. Values and optional labels appear together; WebCore supplies
the filtered results, including label matches.

Up and Down select rows, Return accepts the selected value, and Escape dismisses
the list. Clicking a row accepts it. Acceptance uses WebCore's existing datalist
path, which updates the input and dispatches input/change events. An unselected
Return or dismissal does not choose a value.

The popup closes when its page scrolls, its view geometry or focus changes, the
user clicks outside it, or its owner disappears. Native results carry the filter
generation; a result from an older list cannot fill the field after a newer
filter has reached the engine. The native window owns its views, cancellation
does not wait for its looper, and engine callbacks run only on the WebKit main
loop. Browser shutdown waits for outstanding native windows.

The field rectangle and screen origin use logical coordinates. The native
window is positioned on the field's screen and constrained to its bounds;
app_server applies the display scale.

## Verification

`tests/NativeDataListTests.cpp` exercises the native window without an engine
build. It moves and restores the pointer. On a Haiku test desktop, using the
prepared WebKit source tree:

```sh
c++ -std=c++23 -O1 -Wall -Wextra -Wno-multichar \
  -I.cache/WebKit/Source/WebKit/UIProcess/haiku \
  tests/NativeDataListTests.cpp -lbe -lgame -o NativeDataListTests
./NativeDataListTests
```

On 2026-10-21, all 12 native cases passed on stock Haiku at 100% and on the
air/OS workstation at 200%: keyboard selection, Up wrapping,
single-click selection, filtered replacement, Escape, unselected Return,
owner destruction, cancellation before startup, invalid owner, empty results,
invalid geometry, and keys queued before startup. The four changed engine
translation units also compile with the current cross toolchain.

Full browser acceptance is still pending. Serve `tests/fixtures/datalist.html`
and check its five initial choices, value and label filtering, mouse/keyboard
selection, and event log. Repeat at 100% and 200% display scaling. Check dismissal
on page scroll, window movement, tab switching, and closing the tab while the
list is visible. Issue #91 stays open until these integrated checks pass.
