# libWebKitLegacy on Summit's engine

The BWebView API that WebPositive and other Haiku programs are written
against (HaikuWebKit's `BWebView`, `BWebPage`, `BWebWindow`, `BWebDownload`,
`BWebSettings`, `BWebFrame`, `WebKitInfo`), implemented on Summit's WebKit
engine (`summit_webkit`: `BWebKitView`, `BWebKitContext`).

- `headers/` are HaikuWebKit 1.10.0's public headers. Class layouts and
  virtual functions are unchanged, so programs built for HaikuWebKit run on
  this library without being rebuilt. One difference: `BWebFrame::FindString()`
  takes option bits of its own instead of WebCore's `FindOptions`, and
  `WebFrame.h` no longer includes JavaScriptCore and WebCore headers.
- `src/` is the implementation. A `BWebView` holds the engine's `BWebKitView`
  as its child; its `BWebPage` is a handler of the application looper, as
  before, and turns the engine's notifications into the old listener
  messages (`LOAD_*`, `TITLE_CHANGED`, `UPDATE_NAVIGATION_INTERFACE`,
  `NEW_PAGE_CREATED`, `ICON_RECEIVED`, `B_DOWNLOAD_ADDED`...).
- WebPositive's cookie window reads the `BNetworkCookieJar` of its
  `BUrlContext`; `src/Cookies.cpp` keeps that jar in step with the engine's
  cookies and passes deletions back. The web inspector ("Inspect element"
  in the view's context menu, `src/Inspector.cpp`) is Summit's developer
  tools window, compiled from `../src` (with `../vendor/nlohmann`).
- `make` builds it on Haiku against the installed `summit_webkit`
  (it also needs `scintilla_devel`, `lexilla_devel` and Haiku's static
  `libnetservices.a`, `libshared.a` and `libcolumnlistview.a`);
  `tools/package-summit-webkitlegacy.sh` builds the `summit_webkitlegacy`
  package, which replaces `haikuwebkit` and `haikuwebkit_devel`.

The engine headers and library must include
`BWebKitView::ContinueAfterCertificateError(uint64)`, `SetPageSettings()` and
the constructors accepting initial settings (added 22 October 2026).
Certificate approval uses that operation to validate the failed navigation
before trusting its exact host/certificate and retrying. Build and install the
matching engine first; an older engine cannot load this shim.

Font and JavaScript setters take effect on `Apply()`. Application defaults
update existing and future pages while preserving explicit page overrides.
Summit's additional, nonvirtual `BWebSettings::SetUserAgent(const BString&)`
follows the same rules; an empty string restores the engine's user agent.
Existing public class layouts and virtual methods are unchanged.

Summit's `docs/legacy-webview.md` describes what maps to what, what is not
available and how it was tested.
