#!/usr/bin/env bash
# Cross-build Summit's third-party libraries for arm64 Haiku into $W/deps.
# Each step leaves a stamp in $W/depbuild/.stamps and is skipped when present.
set -euo pipefail
W=/mnt/HaikuWork/build/summit-arm64
. $W/hosttools/env.sh
SYSROOT=$W/sysroot
P=$W/deps
B=$W/depbuild
X=/mnt/HaikuWork/build/arm64/cross-tools-arm64/bin/aarch64-unknown-haiku-
J=${JOBS:-16}
TC=$W/haiku-arm64.cmake
mkdir -p $B/.stamps $P/lib/pkgconfig

export CC="${X}gcc --sysroot=$SYSROOT" CXX="${X}g++ --sysroot=$SYSROOT"
export AR=${X}ar RANLIB=${X}ranlib STRIP=${X}strip NM=${X}nm LD=${X}ld
export CFLAGS="-O2 -fPIC -I$P/include" CXXFLAGS="-O2 -fPIC -I$P/include" CPPFLAGS="-I$P/include"
export LDFLAGS="-L$P/lib -Wl,-rpath-link,$P/lib -Wl,-rpath-link,$SYSROOT/boot/system/lib"
export PKG_CONFIG_LIBDIR=$P/lib/pkgconfig PKG_CONFIG_PATH= PKG_CONFIG_SYSROOT_DIR=
HOST=aarch64-unknown-haiku

step() { local name=$1; shift; [ -e $B/.stamps/$name ] && { echo "== $name (done)"; return; }
    echo "== $name"; ( "$@" ) > $B/$name.log 2>&1 || { echo "FAILED $name, see $B/$name.log"; tail -30 $B/$name.log; exit 1; }
    touch $B/.stamps/$name; }

ac() { # dir, extra configure args
    local d=$1; shift; cd $B/$d; rm -rf _b; mkdir _b; cd _b
    ../configure --host=$HOST --prefix=$P --enable-shared --disable-static "$@"
    make -j$J; make install; }

cm() { # dir, extra cmake args
    local d=$1; shift; cd $B/$d; rm -rf _b
    cmake -S . -B _b -G Ninja -DCMAKE_TOOLCHAIN_FILE=$TC -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=$P -DCMAKE_INSTALL_LIBDIR=lib -DBUILD_SHARED_LIBS=ON \
        -DCMAKE_POSITION_INDEPENDENT_CODE=ON "$@"
    ninja -C _b -j$J; ninja -C _b install; }

zlib() { # the sysroot has zlib 1.2.13; expose it in the prefix for every consumer
    cp -a $SYSROOT/boot/system/develop/lib/libz.so* $P/lib/ 2>/dev/null || cp -a $SYSROOT/boot/system/lib/libz.so* $P/lib/
    cp $SYSROOT/boot/system/develop/headers/zlib.h $SYSROOT/boot/system/develop/headers/zconf.h $P/include/
    printf 'prefix=%s\nlibdir=${prefix}/lib\nincludedir=${prefix}/include\nName: zlib\nDescription: zlib\nVersion: 1.2.13\nLibs: -L${libdir} -lz\nCflags: -I${includedir}\n' $P > $P/lib/pkgconfig/zlib.pc; }

openssl() {
    cd $B/openssl-3.3.2
    cat > Configurations/60-haiku-aarch64.conf <<'EOF'
my %targets = (
    "haiku-aarch64" => {
        inherit_from     => [ "haiku-common" ],
        bn_ops           => "SIXTY_FOUR_BIT_LONG RC4_CHAR",
        asm_arch         => 'aarch64',
        perlasm_scheme   => 'linux64',
    },
);
EOF
    make distclean >/dev/null 2>&1 || true
    CC=${X}gcc AR=${X}ar RANLIB=${X}ranlib CFLAGS="--sysroot=$SYSROOT -O2 -fPIC" LDFLAGS="--sysroot=$SYSROOT" \
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

icu() { # native tools first, then cross build against them
    cd $B/icu/source; rm -rf ../_host ../_cross; mkdir ../_host ../_cross
    (cd ../_host && env -u CC -u CXX -u AR -u RANLIB -u STRIP -u NM -u LD -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
        ../source/runConfigureICU Linux --disable-tests --disable-samples && make -j$J)
    cd ../_cross
    ../source/configure --host=$HOST --prefix=$P --with-cross-build=$B/icu/_host --disable-tests --disable-samples \
        --disable-extras --disable-tools --enable-shared --disable-static --with-data-packaging=library
    make -j$J; make install; }

step zlib zlib
step openssl openssl
step nghttp2 nghttp2
step libpsl libpsl
step curl curl
step sqlite sqlite
step libxml2 libxml2
step libxslt libxslt
step jpeg jpeg
step png png
step webp webp
step lcms2 lcms2
step brotli brotli
step woff2 woff2
step libzip libzip
step icu icu
echo ALL DEPS DONE
