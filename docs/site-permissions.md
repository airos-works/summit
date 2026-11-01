# Site permissions: notifications and location

Added 2 October 2026 for [issue #10](https://github.com/jmgasper/summit/issues/10)
(desktop notifications) and [issue #11](https://github.com/jmgasper/summit/issues/11)
(location). Before, `Notification.requestPermission()` was always refused,
granted notifications would not have shown anyway, `navigator.geolocation`
did not exist and `navigator.permissions` was missing.

Since 1 November (#141) a cross-site frame's `document.requestStorageAccess()`
is asked and remembered the same way, for a pair of sites ("storage-access",
shown as "Cross-site cookies"); see
[browser-privacy.md](browser-privacy.md#cross-site-tracking-prevention-141-1-november-2026).

## What the user sees

- **Asking.** A site that asks to show notifications or to know the location
  gets a small floating question over its window: *github.com wants to show you
  notifications.* with **Block**, **Not Now** and **Allow**. It floats rather
  than blocking the browser. Block and Allow are remembered for that site
  (scheme, host and port, as other browsers do); Not Now (and Escape) answers
  only this request, and the site may ask again later. Several requests from
  the same site while the question is open get the one answer.
- **Notifications** appear as Haiku notifications: the page's title and text,
  the site's name underneath and the site's icon when Summit has one. A
  notification with the same tag as an earlier one from that site replaces it.
  Clicking one brings the page's tab and window forward and tells the page
  (its `click` handler runs, which usually focuses or navigates the page).
  Private windows never show notifications (WebKit refuses them there, as
  other browsers do).
- **Location.** A site allowed to know the location gets a position from
  [BeaconDB](https://beacondb.net), the open successor of Mozilla Location
  Service that GeoClue also uses: Summit sends it the hardware addresses of the
  Wi-Fi networks in range (when it sees at least two) and BeaconDB answers with
  a position and its accuracy; without Wi-Fi the answer comes from the network
  address (city level, about 25 km). The question tells the user this before
  they allow it. A position is reused for a minute and refreshed every five
  minutes while a page watches it.
- **Preferences › Site Permissions** lists every site with a saved answer, the
  permission and whether it is allowed or blocked. **Allow**, **Block** and
  **Remove** change the selected one; a removed site asks again the next time.
  Changes apply at once to open pages (`Notification.permission`,
  `navigator.permissions.query()`).
- In a **private window** the saved answers apply, and what is answered there
  is forgotten when the last private window closes.

## How it works

Engine (`Source/WebKit/UIProcess/haiku/SitePermissionsHaiku.{h,cpp}`): one
object per `BWebKitContext`, installed on its process pool.

- It is the pool's `API::NotificationProvider` and (with `ENABLE_GEOLOCATION`,
  now on in `OptionsHaiku.cmake`) its `API::GeolocationProvider`.
- The Haiku `API::UIClient` (`WebView.cpp`) sends notification and geolocation
  permission requests to it, and answers `queryPermission` from it.
  `PermissionsAPIEnabled` is now set for every page.
- A request is answered at once from a saved decision; otherwise the host gets
  `B_WEBKIT_PERMISSION_REQUESTED` (identifier, permission, origin, the asking
  view) and answers with `BWebKitContext::RespondToPermissionRequest()`.
  Requests still open when the listener changes are refused.
- The host gives its saved decisions with `SetSitePermission()` (also after
  every start: the engine stores nothing). Notification decisions also reach
  every web process (`providerDidUpdateNotificationPolicy`), which is what
  `Notification.permission` reads.
- `B_WEBKIT_NOTIFICATION_SHOW` asks the host to show a notification;
  `NotificationClicked()`/`NotificationClosed()` report back to the page.
- `B_WEBKIT_GEOLOCATION_START`/`STOP` tell the host when pages watch the
  position; it answers with `SetGeolocationPosition()` or
  `GeolocationUnavailable()`.

Browser:

- `src/ui/SitePermissions.{h,cpp}`: `SitePermissionService`, one per context
  (normal and private), asks the questions (a `BAlert` with
  `B_FLOATING_APP_WINDOW_FEEL`), shows notifications (`BNotification`, whose
  on-click file is Summit's own executable with the arguments
  `--summit-notification-click normal|private ID`; the running Summit
  receives them in `ArgvReceived`) and looks positions up.
- `src/ui/Location.{h,cpp}`: the Wi-Fi scan (`BNetworkDevice::GetNetworks`,
  networks named `*_nomap` and locally administered addresses left out) and an
  HTTPS POST to `api.beacondb.net/v1/geolocate` through libcurl, which checks
  the certificate against the system's authorities. (`BSecureSocket` would have
  been the native choice, but on Haiku builds whose network kit lacks OpenSSL
  it is a stub that fails every connection with "Operation not supported".)
- `Profile::sitePermissions` (`profile.json`: permission → origin → allowed),
  `Profile::SetSitePermission()` (host-tested).

## What Haiku is missing for location

Haiku has no location service: no equivalent of GeoClue (Linux) or
CoreLocation (macOS) that applications ask for a position, no GPS or cellular
modem drivers or NMEA/GPSD support, and no system-wide setting for whether
applications may know the location. Summit therefore does the lookup itself
and only for sites the user allowed. A system service would let every
application share one position, one permission setting and better sources
(GPS receivers, Bluetooth phones). The same is recorded on issue #11.

## Checking it

`tools/bench/pages/permissions.html` (serve it from `localhost`: notifications
and location need a secure context) has buttons to ask for each permission,
show and replace a notification, ask for the position once or watch it, and
read the permission states; every event is logged in the page.
`SUMMIT_PERMISSION_TRACE=1` logs requests, answers, notifications and clicks on
stderr.

Checked on the X399 on 2 October 2026:

- Before asking, `Notification.permission` is `default` and
  `navigator.permissions.query()` answers `prompt` for both permissions.
- *Ask to notify* brings up the question over the page's window; *Allow* is
  remembered (a reload, and a restart of Summit, show `granted` without
  asking) and listed in Preferences › Site Permissions. *Block* there turns
  the page's state to `denied` at once.
- A notification appears in Haiku's notification area with the site's icon;
  clicking it brings Summit's window and the page's tab forward and the page's
  `onclick` runs (`notification N clicked (known)` in the trace).
- *Where am I?* after *Allow* returned a position within about 8 seconds.
  Summit sent the 18 Wi-Fi networks in the X399's scan list; BeaconDB did not
  know them and answered from the network address (`fallback: ipf`, accuracy
  25 km).
