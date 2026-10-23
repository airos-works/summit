# chrome.browsingData (#106)

Extensions with the `browsingData` permission (Cookie AutoDelete and other
cleaners) remove data of normal browsing as Preferences › History and Data
does; private windows' data is untouched.

| Data type | Removed |
| --- | --- |
| `cookies` | cookies (by their times with `since`) |
| `cache`, `appcache` | disk and memory caches |
| `localStorage` | local and session storage |
| `indexedDB` | IndexedDB |
| `serviceWorkers`, `cacheStorage` | service worker registrations and their caches |
| `history` | visits since `since` (all: History › Clear History, reported by `history.onVisitRemoved`) |
| `downloads` | the finished entries of the download list (all of them, whatever `since`) |
| `formData`, `fileSystems`, `pluginData`, `webSQL`, `serverBoundCertificates` | nothing: Summit keeps none |
| `passwords` | refused: saved passwords are removed in Summit's Passwords only |

`remove(options, dataToRemove)`, the `removeCookies`-style shortcuts and
`settings()` are supported. Options: `since` (milliseconds); `origins` or
Firefox's `hostnames` (only those sites), or `excludeOrigins` (all others).
Filters work on whole sites (registrable domains, as the Site data list
names them), ignore `since`, and are refused with `history` or `downloads`.
Storage keeps no times, so with `since` it is removed for the sites visited
since then, as Summit's own time ranges do.

## How

`WebExtensionAPIBrowsingData` validates the options (origins become hosts)
and sends `BrowsingDataQueryHaiku`, forwarded as a
`B_WEBKIT_BROWSER_BROWSING_DATA` browser command.
`BrowserWindow::BrowsingDataCommand` (`BrowserWindowFeatures.cpp`) clears
history and downloads itself and site data with the context's
`RemoveWebsiteData`, `RemoveWebsiteDataSince` or (filters)
`FetchWebsiteDataSizes` then `RemoveWebsiteDataOfSites`, answering when
`B_WEBKIT_WEBSITE_DATA_REMOVED` arrives (`BrowsingDataReply`).

## Fixture

`tests/fixtures/extensions/browsing-data/`: install it and read the
`[browsingData]` lines.

Verified on the X399 (24 Oct, bm106 build) with cookies on example.com and
example.org: `origins`, `excludeOrigins`, `hostnames` and `since` each
removed exactly the expected cookies; `removeHistory`, `settings` and the
refusals (passwords, filtered history, both filters, unknown type) behaved
as above.
