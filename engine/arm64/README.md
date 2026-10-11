# arm64 engine cross-build (Linux host)

These are the scripts, CMake toolchain and patches that built Summit's arm64
WebKit engine and its third-party libraries on the Linux workstation
(`/mnt/HaikuWork/build/summit-arm64`), committed so nothing the arm64 build
depends on lives outside git. They still assume the workstation layout
(`/mnt/HaikuWork/...`); the air/OS CI (jmgasper/airos-ci) drives the same
steps from its own paths, building the libraries from jmgasper's forks.

| File | What it is |
|---|---|
| `build-deps.sh` | cross-builds curl, OpenSSL, nghttp2, ICU, libxml2 and the other engine libraries into `deps/` |
| `dependency-sources.txt` | the source tarballs `build-deps.sh` used (versions and URLs) |
| `patches/deps-haiku-arm64.patch` | OpenSSL armcap SIGBUS fix and Haiku OS header; curl `SOCK_NONBLOCK` |
| `patches/webkit-arm64-haiku.patch` | the arm64 change still on top of `engine/patches/0001-haiku-port.patch`: WebExtensionContextAPITabsHaiku.cpp without Skia (the JavaScriptCore changes for ARM64Assembler.h, offlineasm/arm64.rb and MachineContext.h moved into the main patch on 8 October) |
| `execinfo/` | the small `backtrace()` library WebKit links on Haiku arm64 |
| `haiku-arm64.cmake` | CMake toolchain file for the cross build |
| `build-engine.sh`, `run-engine-build.sh` | memory-capped ninja runs of the engine build |
| `build-app.sh` | cross-builds the Summit browser against the engine |
| `hosttools-env.sh` | PATH for the host ninja/cmake/patchelf the scripts use |
| `rpi4-gl/haiku-arm64-gl.cmake`, `rpi4-gl/init-cache-rtc.cmake` | the Raspberry Pi GL + WebRTC engine configuration (Skia, GL compositing, WebGL, MEDIA_STREAM / WEB_RTC / MEDIA_RECORDER / WEB_AUDIO **on**). Use this cache, not the summit-gl one, which turns WebRTC off. |
