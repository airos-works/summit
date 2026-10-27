#!/usr/bin/env bash
# Rebuild Summit's arm64 third-party libraries (build/summit-arm64/build-deps.sh
# and rpi4 tools/rpi4/summit/build-gl-deps.sh) into a prefix of their own,
# linked with GNU hash tables and their own functions bound at link time:
# Haiku's runtime_loader resolves every symbol at load, and these libraries
# were SysV-hash only, so each lookup that passed one of them walked a hash
# chain with strcmp(). Sources are the already patched trees of the originals.
set -euo pipefail
W=/mnt/HaikuWork/build/summit-arm64
G=/mnt/HaikuWork/rpi4/summit-gl
R=${SUMMIT_GNU_DEPS:-/mnt/HaikuWork/build/summit-arm64/deps-gnu}
. $W/hosttools/env.sh
SYSROOT=$W/sysroot
P=$R/prefix
B=$R/src
X=/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-
J=${JOBS:-12}
LINK="-Wl,--hash-style=both -Wl,-Bsymbolic-functions"
mkdir -p $B/.stamps $P/lib/pkgconfig $P/include
TC=$R/haiku-arm64.cmake
sed -e "s|$W/deps|$P|g" -e "s|^set(CMAKE_SHARED_LINKER_FLAGS_INIT \"\(.*\)\")|set(CMAKE_SHARED_LINKER_FLAGS_INIT \"\1 $LINK\")|" \
    $W/haiku-arm64.cmake > $TC
grep -q Bsymbolic $TC || { echo "toolchain file edit failed"; exit 1; }

export CC="${X}gcc --sysroot=$SYSROOT" CXX="${X}g++ --sysroot=$SYSROOT"
export AR=${X}ar RANLIB=${X}ranlib STRIP=${X}strip NM=${X}nm LD=${X}ld
export CFLAGS="-O2 -fPIC -I$P/include" CXXFLAGS="-O2 -fPIC -I$P/include" CPPFLAGS="-I$P/include"
export LDFLAGS="-L$P/lib -Wl,-rpath-link,$P/lib -Wl,-rpath-link,$SYSROOT/boot/system/lib $LINK"
export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR=
HOST=aarch64-unknown-haiku

step() { local name=$1; shift; [ -e $B/.stamps/$name ] && { echo "== $name (done)"; return; }
    echo "== $name $(date +%T)"; ( "$@" ) > $B/$name.log 2>&1 || { echo "FAILED $name, see $B/$name.log"; tail -30 $B/$name.log; exit 1; }
    touch $B/.stamps/$name; }
src() { [ -d $B/$1 ] || rsync -a --exclude=/_b --exclude=/_host --exclude=/_cross $W/depbuild/$1/ $B/$1/; }
ac() { local d=$1; shift; src $d; cd $B/$d; rm -rf _b; mkdir _b; cd _b
    ../configure --host=$HOST --prefix=$P --enable-shared --disable-static "$@"; make -j$J; make install; }
cm() { local d=$1; shift; src $d; cd $B/$d; rm -rf _b
    cmake -S . -B _b -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@"
    ninja -C _b -j$J; ninja -C _b install; }

zlib() { cp -a $SYSROOT/boot/system/develop/lib/libz.so* $P/lib/ 2>/dev/null || cp -a $SYSROOT/boot/system/lib/libz.so* $P/lib/
    cp $SYSROOT/boot/system/develop/headers/zlib.h $SYSROOT/boot/system/develop/headers/zconf.h $P/include/
    printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: zlib\nDescription: zlib\nVersion: 1.2.13\nLibs: -L${libdir} -lz\nCflags: -I${includedir}\n' $P > $P/lib/pkgconfig/zlib.pc; }
openssl() { src openssl-3.3.2; cd $B/openssl-3.3.2; make distclean >/dev/null 2>&1 || true
    CC=${X}gcc AR=${X}ar RANLIB=${X}ranlib CFLAGS="--sysroot=$SYSROOT -O2 -fPIC" LDFLAGS="--sysroot=$SYSROOT -Wl,--hash-style=both" \
        ./Configure haiku-aarch64 shared no-tests no-docs --prefix=$P --libdir=lib --openssldir=/boot/system/data/ssl
    make -j$J; make install_sw; }
nghttp2() { cm nghttp2-1.64.0 -DENABLE_LIB_ONLY=ON -DBUILD_STATIC_LIBS=OFF -DENABLE_DOC=OFF; }
# The Public Suffix List built in: PublicSuffixStoreCurl asks psl_builtin(), and
# without it every domain is a public suffix and no cookie is ever stored.
libpsl() { ac libpsl-0.21.5 --disable-runtime --enable-builtin --disable-man --disable-gtk-doc LIBS=-lnetwork; }
curl() { ac curl-8.10.1 --with-openssl=$P --with-nghttp2=$P --with-zlib=$P --without-libpsl \
    --without-brotli --without-zstd --without-libidn2 --disable-ldap --disable-manual --disable-docs \
    --with-ca-bundle=/boot/system/non-packaged/data/ssl/CARootCertificates.pem --without-ca-path LIBS=-lnetwork; }
sqlite() { CFLAGS="$CFLAGS -DSQLITE_ENABLE_COLUMN_METADATA=1 -DSQLITE_ENABLE_FTS3=1 -DSQLITE_ENABLE_FTS5=1 -DSQLITE_ENABLE_UNLOCK_NOTIFY=1 -DSQLITE_SECURE_DELETE=1" \
    ac sqlite-autoconf-3460100 --disable-readline --enable-threadsafe; }
libxml2() { cm libxml2-2.14.5 -DLIBXML2_WITH_PYTHON=OFF -DLIBXML2_WITH_ICONV=OFF -DLIBXML2_WITH_MODULES=OFF -DLIBXML2_WITH_ICU=OFF \
    -DLIBXML2_WITH_LZMA=OFF -DLIBXML2_WITH_ZLIB=ON -DLIBXML2_WITH_TESTS=OFF -DLIBXML2_WITH_PROGRAMS=OFF -DLIBXML2_WITH_READLINE=OFF; }
libxslt() { cm libxslt-1.1.43 -DLIBXSLT_WITH_PYTHON=OFF -DLIBXSLT_WITH_TESTS=OFF -DLIBXSLT_WITH_PROGRAMS=OFF -DLIBXSLT_WITH_CRYPTO=OFF -DLIBXSLT_WITH_MODULES=OFF; }
jpeg() { cm libjpeg-turbo-3.0.4 -DENABLE_STATIC=OFF -DWITH_TURBOJPEG=OFF -DWITH_JPEG8=OFF; }
png() { cm libpng-1.6.44 -DPNG_STATIC=OFF -DPNG_TESTS=OFF -DPNG_TOOLS=OFF -DPNG_ARM_NEON=on; }
webp() { cm libwebp-1.4.0 -DWEBP_BUILD_ANIM_UTILS=OFF -DWEBP_BUILD_CWEBP=OFF -DWEBP_BUILD_DWEBP=OFF \
    -DWEBP_BUILD_GIF2WEBP=OFF -DWEBP_BUILD_IMG2WEBP=OFF -DWEBP_BUILD_VWEBP=OFF -DWEBP_BUILD_WEBPINFO=OFF \
    -DWEBP_BUILD_WEBPMUX=OFF -DWEBP_BUILD_EXTRAS=OFF; }
lcms2() { ac lcms2-2.16 --without-jpeg --without-tiff; }
brotli() { cm brotli-1.1.0 -DBROTLI_DISABLE_TESTS=ON; }
woff2() { cm woff2-1.0.2 -DCANONICAL_PREFIXES=ON -DNOISY_LOGGING=OFF; }
libzip() { cm libzip-1.11.4 -DENABLE_COMMONCRYPTO=OFF -DENABLE_GNUTLS=OFF -DENABLE_MBEDTLS=OFF -DENABLE_OPENSSL=OFF \
    -DENABLE_BZIP2=OFF -DENABLE_LZMA=OFF -DENABLE_ZSTD=OFF -DBUILD_TOOLS=OFF -DBUILD_REGRESS=OFF -DBUILD_OSSFUZZ=OFF \
    -DBUILD_EXAMPLES=OFF -DBUILD_DOC=OFF; }
icu() { [ -d $B/icu ] || rsync -a --exclude=/_host --exclude=/_cross $W/depbuild/icu/ $B/icu/
    cd $B/icu; rm -rf _cross; mkdir _cross; cd _cross
    ../source/configure --host=$HOST --prefix=$P --with-cross-build=$W/depbuild/icu/_host --disable-tests --disable-samples \
        --disable-extras --disable-tools --enable-shared --disable-static --with-data-packaging=library
    make -j$J; make install; }
execinfo() { cd $W/depbuild/execinfo
    ${X}gcc --sysroot=$SYSROOT -O2 -fPIC -shared execinfo.c -o $P/lib/libexecinfo.so.1.1 -Wl,-soname,libexecinfo.so.1 $LINK
    ln -sf libexecinfo.so.1.1 $P/lib/libexecinfo.so.1; ln -sf libexecinfo.so.1 $P/lib/libexecinfo.so; cp execinfo.h $P/include/; }

# --- GL configuration: FreeType, Expat, Fontconfig, HarfBuzz, libepoxy (into the same prefix)
GS=$G/src
gcm() { local d=$1; shift; rm -rf $B/gl-$d
    cmake -S $GS/$d -B $B/gl-$d -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$P \
        -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@"
    ninja -C $B/gl-$d -j$J; ninja -C $B/gl-$d install; }
freetype() { gcm freetype-2.13.3 -DFT_DISABLE_HARFBUZZ=ON -DFT_DISABLE_BROTLI=ON -DFT_DISABLE_BZIP2=ON -DFT_REQUIRE_PNG=ON -DFT_REQUIRE_ZLIB=ON; }
expat() { gcm expat-2.6.4 -DEXPAT_BUILD_TOOLS=OFF -DEXPAT_BUILD_EXAMPLES=OFF -DEXPAT_BUILD_TESTS=OFF -DEXPAT_BUILD_DOCS=OFF; }
fontconfig() { rm -rf $B/gl-fontconfig; mkdir -p $B/gl-fontconfig; cd $B/gl-fontconfig
    CC="${X}gcc --sysroot=$SYSROOT" CFLAGS="-O2 -fPIC -I$P/include" LDFLAGS="-L$P/lib -Wl,-rpath-link,$P/lib -Wl,-rpath-link,$SYSROOT/boot/system/lib $LINK" \
    PKG_CONFIG_LIBDIR=$P/lib/pkgconfig PKG_CONFIG_PATH= \
    $GS/fontconfig-2.15.0/configure --host=$HOST --prefix=$P --enable-shared --disable-static --disable-docs --disable-nls \
        --disable-cache-build --sysconfdir=/boot/system/lib/summit-webkit/etc --localstatedir=/boot/system/var \
        --with-default-fonts=/boot/system/data/fonts \
        --with-add-fonts=/boot/system/non-packaged/data/fonts,/boot/home/config/data/fonts,/boot/home/config/non-packaged/data/fonts \
        --with-cache-dir=/boot/home/config/cache/fontconfig
    make -j$J; make install sysconfdir=$P/etc fc_cachedir=$B/unused-fontconfig-cache; }
harfbuzz() { gcm harfbuzz-10.1.0 -DHB_HAVE_FREETYPE=ON -DHB_HAVE_ICU=ON -DHB_BUILD_UTILS=OFF -DHB_BUILD_SUBSET=OFF -DHB_HAVE_GLIB=OFF; }
epoxy() { local cross=$B/haiku-aarch64-gl.ini
    cat > $cross <<INI
[binaries]
c = '${X}gcc'
cpp = '${X}g++'
ar = '${X}ar'
strip = '${X}strip'
pkg-config = '/usr/bin/pkg-config'
[host_machine]
system = 'haiku'
cpu_family = 'aarch64'
cpu = 'aarch64'
endian = 'little'
[properties]
needs_exe_wrapper = true
sys_root = '$SYSROOT'
pkg_config_libdir = ['$P/lib/pkgconfig']
[built-in options]
c_args = ['--sysroot=$SYSROOT', '-I$SYSROOT/boot/system/develop/headers/os/opengl']
c_link_args = ['--sysroot=$SYSROOT', '-Wl,--hash-style=both', '-Wl,-Bsymbolic']
INI
    rm -rf $B/gl-epoxy
    env -u CC -u CXX -u CFLAGS -u CXXFLAGS -u LDFLAGS PATH=/mnt/HaikuWork/toolchains/mesa-python/bin:$PATH \
    meson setup $B/gl-epoxy $GS/libepoxy-1.5.10 --cross-file=$cross --prefix=$P --libdir=lib --buildtype=release \
        -Degl=yes -Dglx=no -Dx11=false -Dtests=false -Ddocs=false
    ninja -C $B/gl-epoxy -j$J; ninja -C $B/gl-epoxy install; }

step zlib zlib; step openssl openssl; step nghttp2 nghttp2; step libpsl libpsl; step curl curl
step sqlite sqlite; step libxml2 libxml2; step libxslt libxslt; step jpeg jpeg; step png png
step webp webp; step lcms2 lcms2; step brotli brotli; step woff2 woff2; step libzip libzip
step icu icu; step execinfo execinfo
step freetype freetype; step expat expat; step fontconfig fontconfig; step harfbuzz harfbuzz; step epoxy epoxy
echo ALL DEPS DONE $(date +%T)
