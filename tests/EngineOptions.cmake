# cmake -DINIT_CACHE=engine/arm64/rpi4-gl/init-cache-rtc.cmake -P tests/EngineOptions.cmake
#
# The released engines (airos-ci summit/build-engine.sh, every architecture)
# are configured from this init cache, and its FORCE'd values win over the
# Haiku port's defaults in OptionsHaiku.cmake. A feature Summit relies on that
# is turned off there is missing from every package: the Fullscreen API
# (issue #55) and PDF.js (issue #78) both were.
set(required
    ENABLE_PDFJS                # PDFs in tabs, object, embed and iframe
    ENABLE_FULLSCREEN_API       # element.requestFullscreen, video fullscreen
    ENABLE_VIDEO
    ENABLE_MEDIA_SOURCE         # YouTube and other adaptive streams
    ENABLE_MEDIA_STREAM         # cameras and microphones
    ENABLE_WEB_RTC
    ENABLE_WEB_AUDIO
    ENABLE_WK_WEB_EXTENSIONS
    ENABLE_CONTENT_EXTENSIONS   # declarative content blocking
    ENABLE_WEB_AUTHN            # passkeys
    ENABLE_NOTIFICATIONS
    ENABLE_REMOTE_INSPECTOR     # Developer Tools and the MCP page tools
    ENABLE_RESOURCE_USAGE       # Inspector CPU and memory timelines
    USE_AVIF                    # AVIF images (libavif with dav1d, issue #115)
)
if(NOT EXISTS "${INIT_CACHE}")
    message(FATAL_ERROR "no init cache at '${INIT_CACHE}'")
endif()
file(READ "${INIT_CACHE}" cache)
set(missing)
foreach(option IN LISTS required)
    string(FIND "${cache}" "set(${option} \"ON\" CACHE BOOL \"\" FORCE)" at)
    if(at EQUAL -1)
        list(APPEND missing ${option})
    endif()
endforeach()
if(missing)
    list(JOIN missing ", " missing)
    message(FATAL_ERROR "${INIT_CACHE} does not turn on ${missing}")
endif()
list(LENGTH required count)
message(STATUS "engine init cache: all ${count} required options are on")
