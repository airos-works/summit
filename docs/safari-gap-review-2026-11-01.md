# Safari gap review, 1–2 November 2026

Goal: find the biggest functionality gaps between Summit and Safari (and Chrome
and Firefox), log them, and fix the biggest first. OS-side work goes to
airos-works/haiku.

## How the gaps were found

- A feature probe page (JS APIs, CSS properties, selectors, at-rules, media
  and image formats) loaded in Summit on the X399, compared with what Safari
  26 ships.
- WebKit's `UnifiedWebPreferences.yaml`: preferences on for Cocoa or GTK/WPE
  but off for Haiku.
- The engine's compile flags (`engine/arm64/rpi4-gl/init-cache-rtc.cmake`).
- Summit's menus, Preferences and internal pages against Safari's features.

## Issues

| # | Gap | State |
|---|-----|-------|
| 141 | Cross-site tracking prevention, Storage Access, link tracking protection | done |
| 142 | Gamepad API | done (mock-tested; no pad attached) |
| 143 | Pointer Lock | done |
| 144 | Media Session, media keys, tab media controls | done |
| 145 | speechSynthesis (eSpeak), Edit › Speech | done |
| 146 | Origin private file system | done |
| 147 | Smaller features (CookieStore, scoped registries, switch, …) | mostly; change events, P3 canvas, MP4 recording open |
| 148 | Web Push | design note only |
| 149 | AutoFill: addresses, cards, form history | done |
| 150 | Fraudulent and malicious website warnings | done |
| 151 | Back/Forward history menus | done |
| 152 | Save Page As | done |
| 153 | Web Share | done |
| 154 | JPEG XL | needs libjxl in the x86_64 image (OS side) |
| 155 | Responsive Design Mode | done (no DPR override) |
| 156 | Reading List | done |
| 157 | Profiles | done, one process per profile |
| 158 | WebGPU | planning note only |
| 159 | WebDriver / speech recognition | WebDriver done; recognition open |
| 160 | HTTPS-First | done |
| 161 | SVG in saved web archives | done |

OS side: airos-works/haiku#52 (text-to-speech engine in the images), #53
(Xbox One/Series, Bluetooth pads, rumble), #54 (screen reader and an
accessibility interface), #55 (system-wide media keys / now playing).

Feature details and their switches: `docs/web-apis.md`, `docs/browser-privacy.md`,
`docs/safe-browsing.md`, `docs/form-controls.md`, `docs/browser-profiles.md`,
`docs/developer-tools.md`, `docs/webdriver.md`, `docs/browser-ui.md`.

## Performance

Speedometer 3.1 on the X399 (official site, through Summit's MCP server):

| Build | Score |
|---|---|
| Installed 0.1.0~git20261031.0352 | 8.00, 8.04 |
| ff915ec release engine | 7.85 |
| Today, before the AutoFill fix | 6.24–6.48 |
| Today, final (d0e7874) | 8.24 |

The AutoFill/form-history page script classified every field on the page
(labels included) on each Enter and button click: the Complex-DOM TodoMVCs
ran 2–3× slower (React-Complex-DOM 162 → 555 ms). It now works only after the
user's own key or pointer input. A profile (`profile` MCP tool on the tab's
web process) pointed straight at `LabelsNodeList::length` under event
listeners. Lesson: run Speedometer before shipping a page script.

Tracking prevention costs no measurable page-load time (warm loads within
noise on Wikipedia, GitHub, BBC and MDN with it on or off).

## Traps found

- `ResourceRequestBase::setAsIsolatedCopy` dropped
  `wasSchemeOptimisticallyUpgraded`; the curl backend copies every request.
- curl's request timeout is an idle-speed watcher; handshakes need
  `CURLOPT_CONNECTTIMEOUT_MS`.
- WebKit's MHTML parser files SVG parts as frames only.
- The socket remote inspector had never run on Haiku (no `SO_REUSEPORT`,
  `SO_ACCEPTCONN` unset, int buffer sizes).
- airos-ci compiled the Summit app with one job per core (48); two runs died
  with a silent `exit code 4` (likely OOM-killed cc1plus). Compile jobs now
  follow available memory.
