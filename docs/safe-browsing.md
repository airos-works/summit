# Fraudulent and malicious website warnings

Issue #150. Summit warns before showing a page that is on a list of phishing
or malware sites, as Safari ("Warn when visiting a fraudulent website"),
Chrome and Firefox (Google Safe Browsing) do.

## What the user sees

- A page on a list is replaced in its tab by a red warning: "Deceptive Website
  Warning" (phishing) or "Dangerous Website Warning" (malware), why, the
  address, and two buttons. **Go Back** (also the toolbar's Back button)
  returns to the page the tab had, which never went away, or the home page in
  a new tab. **Visit This Site Anyway** loads the page; its host is not checked
  again until Summit quits.
- Preferences › Security: "Warn about fraudulent and malicious websites" (on
  by default), when the lists were last brought up to date, how many entries
  they have, **Update Now**, and the lists' sources.

## The lists

Downloaded in the background every 4 hours (conditional requests: an
unchanged list costs one request), 10 seconds after the first start, and kept
in the profile's `SafeBrowsing` folder. Nothing about the pages visited leaves
the computer.

| List | Source | Terms | Size |
|---|---|---|---|
| phishing | [Phishing Army](https://phishing.army/) `phishing_army_blocklist.txt` (domains; aggregates PhishTank, OpenPhish, CERT.PL, urlscan.io and others) | free for personal and individual use, no extra restriction from Phishing Army | ~150,000 domains, 1.1 MB compressed, expires every 4 h |
| malware | abuse.ch [URLhaus](https://urlhaus.abuse.ch/) online malware URLs, as [malware-filter](https://gitlab.com/malware-filter/urlhaus-filter) republishes them (`urlhaus-filter-online.txt`, mirror on github.io) | CC0 (URLhaus data) / CC0 + MIT (malware-filter) | ~110,000 addresses and hosts, 1.4 MB compressed, every 12 h |

Not used, as checked on 1 November 2026:

- URLhaus's own downloads (`/downloads/text_online/` and the rest) answer
  410 Gone; its exports now need a free abuse.ch account's Auth-Key
  (`https://urlhaus-api.abuse.ch/v2/files/exports/KEY/…`). A field for a
  user's own key could be added; malware-filter needs none.
- OpenPhish's community feed: its terms allow personal research only and
  forbid "customer protection" and "automation" without written consent.
- PhishTank's `online-valid.csv` still downloads without a key (10 MB), but
  PhishTank asks applications to register a key, and registration has been
  closed for years; Phishing Army already includes it.
- Google Safe Browsing (Update API with the user's own API key): not done. It
  would be a third list filled by the same updater thread; the lists already
  key on SHA-256 prefixes of Safe Browsing's expressions.

## How it works

- `src/core/SafeBrowsing.{h,cpp}` (tested on the host,
  `tests/SafeBrowsingTests.cpp`): Safe Browsing's URL canonicalisation (lower
  case host without port, user or fragment, percent-escapes resolved
  repeatedly, IPv4 addresses in any spelling as four numbers, dot segments and
  repeated slashes gone) and its lookup expressions (the host and its last 5
  to 2 names, times the full path with and without the query and the first 4
  folders). List lines can be hosts, hosts-file lines, addresses or Adblock
  `||host/path^$all` rules. Each list keeps the first 8 bytes of the SHA-256
  of each expression in lower case (malware-filter lowercases its URLs),
  sorted: `phishing.bin` and `malware.bin`, 8 bytes an entry (about 2 MB in
  all). A lookup takes a few microseconds.
- `src/ui/SafeBrowsingService.{h,cpp}`: the lists, their updater thread
  (libcurl; a download with fewer than 1,000 entries never replaces a list),
  `settings.json` (on/off, validators, last update) and Preferences ›
  Security.
- The engine (`BWebKitSetNavigationCheck`, `B_WEBKIT_HAS_NAVIGATION_CHECK` in
  `WebKitView.h`) asks the browser about every main-frame http(s)
  navigation and each of its server redirects in
  `decidePolicyForNavigationAction`, before the request is made. A listed one
  is ignored and the view's target gets `B_WEBKIT_NAVIGATION_BLOCKED`
  ("url", "reason", "source_url", "redirect"); the window shows the warning
  (`src/ui/BrowserWindowSafeBrowsing.cpp`).
- With an engine without the check, the window stops a listed page when the
  tab's state first names it (before it commits) and shows the same warning.

## Tests

- `SUMMIT_SAFE_BROWSING_TEST_URLS="phishing:http://host/phish/ malware:http://host/x.html"`:
  addresses (or hosts) treated as listed, so the warning can be seen without
  visiting a malicious site. Never load real listed addresses in tests;
  `amiprotected.it` is Phishing Army's harmless test domain.
- `SUMMIT_SAFE_BROWSING_PHISHING_FEED` / `SUMMIT_SAFE_BROWSING_MALWARE_FEED`:
  another list address (http allowed, any size), to test the updater.
- `stderr` says each list update ("safe browsing phishing list updated from …:
  N entries", "unchanged"), each warning and each "visiting … despite".
