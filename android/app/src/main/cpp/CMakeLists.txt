# libsummit: Summit's shared core (src/core, every file, as the air/OS Makefile
# builds it) and its JNI bridge for the Android app.
cmake_minimum_required(VERSION 3.22)
project(summit_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
get_filename_component(SUMMIT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../.." ABSOLUTE)
set(SUMMIT_IMPORTED "${SUMMIT_ROOT}/android/wpeview/src/main/cpp/imported")

file(STRINGS "${SUMMIT_ROOT}/vendor/cld2/sources.list" SUMMIT_CLD2_SOURCES)
list(TRANSFORM SUMMIT_CLD2_SOURCES PREPEND "${SUMMIT_ROOT}/vendor/cld2/")
add_library(summit_cld2 STATIC ${SUMMIT_CLD2_SOURCES})
# CLD2 is C++98-era code: GCC (air/OS) only warns about its narrowing initializers, Clang refuses them.
target_compile_options(summit_cld2 PRIVATE -w -Wno-c++11-narrowing)

file(GLOB SUMMIT_CORE_SOURCES CONFIGURE_DEPENDS "${SUMMIT_ROOT}/src/core/*.cpp")
add_library(summit_core STATIC ${SUMMIT_CORE_SOURCES})
target_include_directories(summit_core PUBLIC "${SUMMIT_ROOT}/src" "${SUMMIT_ROOT}/vendor" "${SUMMIT_IMPORTED}/include")
target_compile_options(summit_core PRIVATE -Wall -Wno-unused-parameter)
target_link_libraries(summit_core PRIVATE summit_cld2)

# The engine's OpenSSL (staged by tools/android/stage-engine.py).
file(GLOB SUMMIT_CRYPTO "${SUMMIT_IMPORTED}/lib/${ANDROID_ABI}/libcrypto*.so")
if (NOT SUMMIT_CRYPTO)
    message(FATAL_ERROR "no libcrypto in ${SUMMIT_IMPORTED}: run tools/android/stage-engine.py")
endif ()
add_library(summit_crypto SHARED IMPORTED)
set_target_properties(summit_crypto PROPERTIES IMPORTED_LOCATION "${SUMMIT_CRYPTO}")

# libcurl, for the extension stores (core/ExtensionStoreDownload), as on air/OS.
set(SUMMIT_CURL "${SUMMIT_IMPORTED}/lib/${ANDROID_ABI}/libcurl.so")
if (NOT EXISTS "${SUMMIT_CURL}")
    message(FATAL_ERROR "no libcurl in ${SUMMIT_IMPORTED}: run tools/android/stage-engine.py")
endif ()
add_library(summit_curl SHARED IMPORTED)
set_target_properties(summit_curl PROPERTIES IMPORTED_LOCATION "${SUMMIT_CURL}")

add_library(summit SHARED SummitCoreJni.cpp SummitExtensionsJni.cpp)
target_link_libraries(summit PRIVATE summit_core summit_crypto summit_curl log)
target_link_options(summit PRIVATE "-Wl,-z,max-page-size=16384")
