# Summit: repository rules

These add to the workspace rules (`/mnt/HaikuWork/AGENTS.md`).

## Keep air/OS and Android in step

Summit builds for air/OS (WebKit's Haiku port) and for Android (WPE), from the
same engine tree and the same `src/core`. See `docs/android.md`, "What is
shared, and how".

- In shared engine files, guard Summit's work with `#if USE(SUMMIT)` (on for
  both builds): web behaviour, layout, style, paint and loading changes,
  Interop fixes, performance work, Summit's diagnostics.
- Use `#if PLATFORM(HAIKU)` only for code that needs Haiku itself (BeAPI,
  `<OS.h>`, `system_time()`, `snooze()`, the Haiku network, media and speech
  backends, `*Haiku.cpp` files) or that works around a Haiku-only build
  setting. A `PLATFORM(HAIKU)` guard in shared code keeps the change off
  Android.
- Browser logic belongs in `src/core`, which both builds compile.
