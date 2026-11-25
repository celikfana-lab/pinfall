#!/usr/bin/env bash
# =============================================================================
# PinFall -- Android native dependency build (one command, reproducible)
# =============================================================================
#
# Builds the complete native dependency stack for the PinFall SDL2 Android port:
#
#   zlib 1.3.1            static  libz.a
#   mbedTLS 3.6.2         static  libmbedtls.a libmbedx509.a libmbedcrypto.a
#   freetype 2.13.3       static  libfreetype.a        (no harfbuzz/png/bz2/brotli)
#   curl 8.12.0           static  libcurl.a            (mbedTLS + zlib backend)
#   SDL2 2.32.4           shared  libSDL2.so  + static libSDL2main.a
#   SDL2_ttf 2.24.0       static  libSDL2_ttf.a       (SDL2 + freetype)
#
# for arm64-v8a (default, phone) and x86_64 (optional, emulator), API level 24.
#
# Usage:
#   ./android/deps.sh                       # rebuild arm64-v8a
#   ./android/deps.sh arm64-v8a x86_64      # rebuild both ABIs
#   ./android/deps.sh all                   # same as above
#   ./android/deps.sh --clean               # wipe build/ + prefix/ first
#   ./android/deps.sh --jobs 8 arm64-v8a
#   ./android/deps.sh --verify-only          # re-run checks, no rebuild
#
# Environment overrides:
#   ANDROID_NDK_HOME   NDK root           (default $HOME/Android/Sdk/ndk/28.2.13676358)
#   PINFALL_CMAKE      cmake binary       (default: SDK cmake 3.22.1, else `cmake`)
#   PINFALL_ANDROID_API  API level        (default 24)
#   PINFALL_JOBS       parallel jobs      (default nproc)
#
# Output layout (per ABI):
#   android/third_party/build/<abi>/<package>/            build trees
#   android/third_party/prefix/<abi>/include|lib|...      install prefix
#
# No sudo, no device and no network access are required when the source
# tarballs are already in android/third_party/downloads/ (they are extracted
# automatically if android/third_party/src/<pkg> is missing).
# =============================================================================
set -euo pipefail

# ---------------------------------------------------------------------------
# Paths
# ---------------------------------------------------------------------------
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"          # <project>/android
PROJECT_ROOT="$(cd "${HERE}/.." && pwd)"                      # <project>
TP="${HERE}/third_party"
SRC="${TP}/src"
DOWNLOADS="${TP}/downloads"
BUILD_ROOT="${TP}/build"
PREFIX_ROOT="${TP}/prefix"

# ---------------------------------------------------------------------------
# Versions
# ---------------------------------------------------------------------------
ZLIB_DIR="zlib-1.3.1"
MBEDTLS_DIR="mbedtls-3.6.2"
FREETYPE_DIR="freetype-2.13.3"
CURL_DIR="curl-8.12.0"
SDL2_DIR="SDL2-2.32.4"
SDL2_TTF_DIR="SDL2_ttf-2.24.0"

# curl >= 8.12.0 is REQUIRED: 8.11.1 contains CVE-2025-0665 (eventfd double
# close) which fires Android fdsan SIGABRT inside image fetches on arm64.

TARBALL_ZLIB="zlib-1.3.1.tar.gz"
TARBALL_MBEDTLS="mbedtls-3.6.2.tar.bz2"
TARBALL_FREETYPE="freetype-2.13.3.tar.xz"
TARBALL_CURL="curl-8.12.0.tar.gz"
TARBALL_SDL2="SDL2-2.32.4.tar.gz"
TARBALL_SDL2_TTF="SDL2_ttf-2.24.0.tar.gz"

# ---------------------------------------------------------------------------
# Defaults / options
# ---------------------------------------------------------------------------
NDK="${ANDROID_NDK_HOME:-${ANDROID_NDK:-$HOME/Android/Sdk/ndk/28.2.13676358}}"
API="${PINFALL_ANDROID_API:-24}"
JOBS="${PINFALL_JOBS:-$( (command -v nproc >/dev/null && nproc) || echo 4 )}"
CMAKE_BIN="${PINFALL_CMAKE:-}"
DO_CLEAN=0
VERIFY_ONLY=0
ABIS=()

log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
die()  { printf '\n\033[1;31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

usage() {
  sed -n '2,45p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
  exit 0
}

while [ $# -gt 0 ]; do
  case "$1" in
    -h|--help)    usage ;;
    --clean)      DO_CLEAN=1; shift ;;
    --verify-only) VERIFY_ONLY=1; shift ;;
    --jobs|-j)    [ $# -ge 2 ] || die "$1 needs a value"; JOBS="$2"; shift 2 ;;
    --api)        [ $# -ge 2 ] || die "$1 needs a value"; API="$2"; shift 2 ;;
    --ndk)        [ $# -ge 2 ] || die "$1 needs a value"; NDK="$2"; shift 2 ;;
    --cmake)      [ $# -ge 2 ] || die "$1 needs a value"; CMAKE_BIN="$2"; shift 2 ;;
    all|ALL)      ABIS+=("arm64-v8a" "x86_64"); shift ;;
    arm64-v8a|x86_64) ABIS+=("$1"); shift ;;
    -*)           die "unknown option: $1 (try --help)" ;;
    *)            die "unknown ABI: $1 (supported: arm64-v8a, x86_64, all)" ;;
  esac
done
if [ ${#ABIS[@]} -eq 0 ]; then
  ABIS=("arm64-v8a")
fi
# de-duplicate while preserving order
UNIQ=()
for a in "${ABIS[@]}"; do
  case " ${UNIQ[*]-} " in *" $a "*) ;; *) UNIQ+=("$a") ;; esac
done
ABIS=("${UNIQ[@]}")

# ---------------------------------------------------------------------------
# Validate NDK
# ---------------------------------------------------------------------------
NDK="$(cd "${NDK}" 2>/dev/null && pwd)" || die "NDK not found: ${NDK}"
NDK_TOOLCHAIN_FILE="${NDK}/build/cmake/android.toolchain.cmake"
NDK_BIN="${NDK}/toolchains/llvm/prebuilt/linux-x86_64/bin"
[ -f "${NDK_TOOLCHAIN_FILE}" ] || die "not an NDK (missing build/cmake/android.toolchain.cmake): ${NDK}"
[ -x "${NDK_BIN}/llvm-ar" ]    || die "missing llvm-ar in ${NDK_BIN}"

# ---------------------------------------------------------------------------
# Pick a CMake that can still build pre-3.5 minimums (CMake 4 cannot).
# The Android SDK ships 3.22.1, which is the NDK r28 baseline.
# ---------------------------------------------------------------------------
pick_cmake() {
  local cands=()
  if [ -n "${CMAKE_BIN}" ]; then cands+=("${CMAKE_BIN}"); fi
  local sdk_root
  sdk_root="$(cd "${NDK}/../.." && pwd)"
  cands+=("${sdk_root}/cmake/3.22.1/bin/cmake")
  cands+=("$(command -v cmake 2>/dev/null || true)")
  local c v
  for c in "${cands[@]}"; do
    [ -n "$c" ] && [ -x "$c" ] || continue
    v="$("$c" --version 2>/dev/null | awk 'NR==1{print $3}')"
    [ -n "$v" ] || continue
    case "$v" in
      3.*) CMAKE_BIN="$c"; CMAKE_VER="$v"; return 0 ;;
    esac
    # remember a 4.x as a last resort candidate only if nothing else exists
    if [ -z "${CMAKE_BIN_FALLBACK:-}" ]; then CMAKE_BIN_FALLBACK="$c"; CMAKE_VER_FALLBACK="$v"; fi
  done
  if [ -n "${CMAKE_BIN_FALLBACK:-}" ]; then
    CMAKE_BIN="${CMAKE_BIN_FALLBACK}"; CMAKE_VER="${CMAKE_VER_FALLBACK}"
    printf '\033[1;33mWARNING: only CMake %s found; SDL2 2.32.4 declares a <3.5 minimum and\n' "${CMAKE_VER}" >&2
    printf '         will be rejected by CMake 4. Install 3.22.x or pass --cmake PATH.\033[0m\n' >&2
    return 0
  fi
  return 1
}
pick_cmake || die "no usable cmake found (need 3.22.x; use --cmake /path/to/cmake)"
[ "${CMAKE_VER%%.*}" = "3" ] || die "CMake ${CMAKE_VER} is too new; pinfall/deps needs 3.22.x"

NINJA="$(command -v ninja 2>/dev/null || true)"
if [ -z "${NINJA}" ] && [ -x "$(dirname "${CMAKE_BIN}")/ninja" ]; then
  NINJA="$(dirname "${CMAKE_BIN}")/ninja"
fi
[ -n "${NINJA}" ] || die "ninja not found (install ninja-build)"

log "PinFall Android dependency build"
info "project      : ${PROJECT_ROOT}"
info "NDK          : ${NDK}"
info "cmake        : ${CMAKE_BIN} (${CMAKE_VER})"
info "ninja        : ${NINJA}"
info "ABIs         : ${ABIS[*]}"
info "API level    : ${API}"
info "jobs         : ${JOBS}"
info "sources      : ${SRC}"
info "prefix root  : ${PREFIX_ROOT}"

# ---------------------------------------------------------------------------
# Source extraction / verification
# ---------------------------------------------------------------------------
ensure_src() {
  local dir="$1" tarball="$2"
  if [ -d "${SRC}/${dir}" ]; then
    info "src: ${dir} (present)"
    return 0
  fi
  local tb="${DOWNLOADS}/${tarball}"
  [ -f "${tb}" ] || die "missing source ${SRC}/${dir} and tarball ${tb}"
  info "extracting ${tarball}"
  mkdir -p "${SRC}"
  case "${tarball}" in
    *.tar.gz)  tar -xzf "${tb}" -C "${SRC}" ;;
    *.tar.xz)  tar -xJf "${tb}" -C "${SRC}" ;;
    *.tar.bz2) tar -xjf "${tb}" -C "${SRC}" ;;
    *) die "unknown tarball type: ${tarball}" ;;
  esac
  [ -d "${SRC}/${dir}" ] || die "extraction did not produce ${SRC}/${dir}"
}

# ---------------------------------------------------------------------------
# Per-ABI helpers
# ---------------------------------------------------------------------------
triple_for() {
  case "$1" in
    arm64-v8a) echo "aarch64-linux-android" ;;
    x86_64)    echo "x86_64-linux-android" ;;
    *) die "unsupported ABI: $1" ;;
  esac
}

# Build a package in a clean environment so host CC/CFLAGS never leak in.
pkg_env_begin() {  # abi
  local abi="$1" triple
  triple="$(triple_for "${abi}")"
  export _ABI="${abi}"
  export _TRIPLE="${triple}"
  export _PFX="${PREFIX_ROOT}/${abi}"
  export CC="${NDK_BIN}/${triple}${API}-clang"
  export CXX="${NDK_BIN}/${triple}${API}-clang++"
  export AR="${NDK_BIN}/llvm-ar"
  export RANLIB="${NDK_BIN}/llvm-ranlib"
  export NM="${NDK_BIN}/llvm-nm"
  export STRIP="${NDK_BIN}/llvm-strip"
  export CFLAGS="-O2 -fPIC -D_FORTIFY_SOURCE=2"
  export CXXFLAGS="-O2 -fPIC -D_FORTIFY_SOURCE=2"
  export CPPFLAGS="-I${_PFX}/include"
  export LDFLAGS="-L${_PFX}/lib"
  export PKG_CONFIG_LIBDIR="${_PFX}/lib/pkgconfig"
  export PKG_CONFIG_PATH="${_PFX}/lib/pkgconfig"
  mkdir -p "${BUILD_ROOT}/${abi}" "${_PFX}"
}

# ---- 1. zlib ---------------------------------------------------------------
build_zlib() {  # abi
  local abi="$1" b="${BUILD_ROOT}/$1/zlib"
  log "zlib ${ZLIB_DIR##*-} [${abi}]"
  rm -rf "${b}"; mkdir -p "${b}"
  (
    pkg_env_begin "${abi}"
    cd "${b}"
    "${SRC}/${ZLIB_DIR}/configure" --static --prefix="${_PFX}" > configure.log 2>&1 \
      || { tail -40 configure.log; exit 1; }
    make -j"${JOBS}" > build.log 2>&1 || { tail -40 build.log; exit 1; }
    make install > install.log 2>&1 || { tail -30 install.log; exit 1; }
  )
  info "installed ${PREFIX_ROOT}/${abi}/lib/libz.a"
}

# ---- 2. mbedTLS ------------------------------------------------------------
build_mbedtls() {  # abi
  local abi="$1" b="${BUILD_ROOT}/$1/mbedtls"
  log "mbedTLS 3.6.2 [${abi}]"
  rm -rf "${b}"; mkdir -p "${b}"
  (
    set +u
    unset CC CXX AR RANLIB NM CPPFLAGS LDFLAGS CFLAGS CXXFLAGS PKG_CONFIG_LIBDIR PKG_CONFIG_PATH
    set -u
    "${CMAKE_BIN}" -S "${SRC}/${MBEDTLS_DIR}" -B "${b}" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="${NDK_TOOLCHAIN_FILE}" \
      -DANDROID_ABI="${abi}" -DANDROID_PLATFORM="android-${API}" \
      -DCMAKE_INSTALL_PREFIX="${PREFIX_ROOT}/${abi}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DENABLE_TESTING=OFF -DENABLE_PROGRAMS=OFF \
      -DUSE_STATIC_MBEDTLS_LIBRARY=ON -DUSE_SHARED_MBEDTLS_LIBRARY=OFF \
      -DMBEDTLS_FATAL_WARNINGS=OFF > "${b}/configure.log" 2>&1 \
      || { tail -40 "${b}/configure.log"; exit 1; }
    "${CMAKE_BIN}" --build "${b}" -j "${JOBS}" > "${b}/build.log" 2>&1 \
      || { tail -40 "${b}/build.log"; exit 1; }
    "${CMAKE_BIN}" --install "${b}" > "${b}/install.log" 2>&1 \
      || { tail -30 "${b}/install.log"; exit 1; }
  )
  info "installed libmbedtls.a libmbedx509.a libmbedcrypto.a"
}

# ---- 3. freetype -----------------------------------------------------------
build_freetype() {  # abi
  local abi="$1" b="${BUILD_ROOT}/$1/freetype"
  log "freetype 2.13.3 [${abi}]"
  rm -rf "${b}"; mkdir -p "${b}"
  (
    set +u
    unset CXX CXXFLAGS
    set -u
    pkg_env_begin "${abi}"
    cd "${b}"
    "${SRC}/${FREETYPE_DIR}/configure" \
      --host="${_TRIPLE}" --prefix="${_PFX}" \
      --enable-static --disable-shared --with-pic \
      --without-harfbuzz --without-png --without-bzip2 --without-brotli \
      --with-zlib=yes \
      ZLIB_CFLAGS="-I${_PFX}/include" ZLIB_LIBS="-L${_PFX}/lib -lz" \
      > configure.log 2>&1 || { tail -40 configure.log; exit 1; }
    make -j"${JOBS}" > build.log 2>&1 || { tail -40 build.log; exit 1; }
    make install > install.log 2>&1 || { tail -30 install.log; exit 1; }
    rm -f "${_PFX}/lib/libfreetype.la"
  )
  info "installed ${PREFIX_ROOT}/${abi}/lib/libfreetype.a (zlib=yes, harfbuzz/png/bzip2/brotli=no)"
}

# ---- 4. curl ---------------------------------------------------------------
build_curl() {  # abi
  local abi="$1" b="${BUILD_ROOT}/$1/curl"
  log "curl 8.12.0 [${abi}] (static, mbedTLS + zlib)"
  rm -rf "${b}"; mkdir -p "${b}"
  (
    pkg_env_begin "${abi}"
    cd "${b}"
    # NOTE: curl 8.11 hard-errors when --without-ssl is combined with an
    # explicit TLS backend (--with-mbedtls), so OpenSSL is simply not selected.
    "${SRC}/${CURL_DIR}/configure" \
      --host="${_TRIPLE}" --prefix="${_PFX}" \
      --disable-shared --enable-static \
      --with-mbedtls="${_PFX}" --with-zlib="${_PFX}" \
      --disable-ldap --disable-ldaps --disable-rtsp --disable-dict \
      --disable-telnet --disable-tftp --disable-pop3 --disable-imap \
      --disable-smtp --disable-gopher --disable-mqtt \
      --disable-manual \
      --without-libpsl --without-brotli --without-zstd --without-libidn2 \
      --without-nghttp2 \
      --with-ca-path=/system/etc/security/cacerts \
      > configure.log 2>&1 || { tail -50 configure.log; exit 1; }
    make -j"${JOBS}" > build.log 2>&1 || { tail -50 build.log; exit 1; }
    make install > install.log 2>&1 || { tail -30 install.log; exit 1; }
  )
  info "installed ${PREFIX_ROOT}/${abi}/lib/libcurl.a (ssl=$( "${PREFIX_ROOT}/${abi}/bin/curl-config" --ssl-backends ))"
}

# ---- 5. SDL2 ---------------------------------------------------------------
build_sdl2() {  # abi
  local abi="$1" b="${BUILD_ROOT}/$1/sdl2"
  log "SDL2 2.32.4 [${abi}] (shared libSDL2.so + static libSDL2main.a)"
  rm -rf "${b}"; mkdir -p "${b}"
  (
    set +u
    unset CC CXX AR RANLIB NM CPPFLAGS LDFLAGS CFLAGS CXXFLAGS PKG_CONFIG_LIBDIR PKG_CONFIG_PATH
    set -u
    "${CMAKE_BIN}" -S "${SRC}/${SDL2_DIR}" -B "${b}" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="${NDK_TOOLCHAIN_FILE}" \
      -DANDROID_ABI="${abi}" -DANDROID_PLATFORM="android-${API}" \
      -DCMAKE_INSTALL_PREFIX="${PREFIX_ROOT}/${abi}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DSDL_SHARED=ON -DSDL_STATIC=ON -DSDL_STATIC_PIC=ON \
      -DSDL_TEST=OFF -DSDL_TESTS=OFF -DSDL_INSTALL_TESTS=OFF \
      > "${b}/configure.log" 2>&1 || { tail -40 "${b}/configure.log"; exit 1; }
    "${CMAKE_BIN}" --build "${b}" -j "${JOBS}" > "${b}/build.log" 2>&1 \
      || { tail -50 "${b}/build.log"; exit 1; }
    "${CMAKE_BIN}" --install "${b}" > "${b}/install.log" 2>&1 \
      || { tail -30 "${b}/install.log"; exit 1; }
  )
  info "installed ${PREFIX_ROOT}/${abi}/lib/libSDL2.so + libSDL2main.a"
}

# ---- 6. SDL2_ttf -----------------------------------------------------------
build_sdl2_ttf() {  # abi
  local abi="$1" b="${BUILD_ROOT}/$1/sdl2_ttf" pfx="${PREFIX_ROOT}/$1"
  log "SDL2_ttf 2.24.0 [${abi}] (static, SDL2 + freetype)"
  rm -rf "${b}"; mkdir -p "${b}"
  (
    set +u
    unset CC CXX AR RANLIB NM CPPFLAGS LDFLAGS CFLAGS CXXFLAGS
    set -u
    export PKG_CONFIG_LIBDIR="${pfx}/lib/pkgconfig"
    export PKG_CONFIG_PATH="${pfx}/lib/pkgconfig"
    "${CMAKE_BIN}" -S "${SRC}/${SDL2_TTF_DIR}" -B "${b}" -G Ninja \
      -DCMAKE_TOOLCHAIN_FILE="${NDK_TOOLCHAIN_FILE}" \
      -DANDROID_ABI="${abi}" -DANDROID_PLATFORM="android-${API}" \
      -DCMAKE_INSTALL_PREFIX="${pfx}" \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_PREFIX_PATH="${pfx}" \
      -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
      -DBUILD_SHARED_LIBS=OFF \
      -DSDL2TTF_SAMPLES=OFF -DSDL2TTF_VENDORED=OFF -DSDL2TTF_HARFBUZZ=OFF \
      -DSDL2_DIR="${pfx}/lib/cmake/SDL2" \
      -DFREETYPE_INCLUDE_DIRS="${pfx}/include/freetype2" \
      -DFREETYPE_INCLUDE_DIR_freetype2="${pfx}/include/freetype2" \
      -DFREETYPE_LIBRARY="${pfx}/lib/libfreetype.a" \
      -DFREETYPE_LIBRARY_RELEASE="${pfx}/lib/libfreetype.a" \
      > "${b}/configure.log" 2>&1 || { tail -40 "${b}/configure.log"; exit 1; }
    "${CMAKE_BIN}" --build "${b}" -j "${JOBS}" > "${b}/build.log" 2>&1 \
      || { tail -50 "${b}/build.log"; exit 1; }
    "${CMAKE_BIN}" --install "${b}" > "${b}/install.log" 2>&1 \
      || { tail -30 "${b}/install.log"; exit 1; }
  )
  info "installed ${pfx}/lib/libSDL2_ttf.a"
}

# ---------------------------------------------------------------------------
# Verification (hard requirements; exits non-zero on failure)
# ---------------------------------------------------------------------------
FAILED=0
check() {  # description, command...
  local desc="$1"; shift
  if "$@" > /dev/null 2>&1; then
    printf '    [ok]   %s\n' "${desc}"
  else
    printf '    [FAIL] %s\n' "${desc}"
    FAILED=1
  fi
}

verify_abi() {  # abi
  local abi="$1" pfx="${PREFIX_ROOT}/$1" machine expected
  local readelf="${NDK_BIN}/llvm-readelf" nm="${NDK_BIN}/llvm-nm" ar="${NDK_BIN}/llvm-ar"
  case "${abi}" in
    arm64-v8a) machine="AArch64"; expected="aarch64" ;;
    x86_64)    machine="X86-64";  expected="x86-64" ;;
  esac

  log "verify [${abi}]"

  # headers + libraries present
  for f in include/SDL2/SDL.h include/SDL2/SDL_ttf.h include/freetype2/ft2build.h \
           include/curl/curl.h include/mbedtls/ssl.h include/zlib.h \
           lib/libz.a lib/libmbedtls.a lib/libmbedx509.a lib/libmbedcrypto.a \
           lib/libfreetype.a lib/libcurl.a lib/libSDL2.so lib/libSDL2main.a \
           lib/libSDL2_ttf.a; do
    check "exists ${f}" test -f "${pfx}/${f}"
  done

  # ELF machine of the delivered .so
  check "libSDL2.so machine is ${machine}" \
    bash -c "'${readelf}' -h '${pfx}/lib/libSDL2.so' | grep -q 'Machine:.*${machine}'"
  check "libSDL2.so is for Android ${API}" \
    bash -c "file '${pfx}/lib/libSDL2.so' | grep -q 'for Android ${API}'"
  check "libSDL2.so exports Java_org_libsdl_app_SDLActivity_nativeRunMain" \
    bash -c "'${nm}' -D --defined-only '${pfx}/lib/libSDL2.so' | grep -q Java_org_libsdl_app_SDLActivity_nativeRunMain"
  check "libSDL2.so links Android platform libs" \
    bash -c "'${readelf}' -d '${pfx}/lib/libSDL2.so' | grep -q 'libandroid\\.so'"

  # curl -> mbedTLS ; SDL2_ttf -> freetype
  check "libcurl.a references mbedTLS" \
    bash -c "'${nm}' --undefined-only '${pfx}/lib/libcurl.a' | grep -q 'U mbedtls_ssl_init'"
  # CVE-2025-0665 (eventfd double close -> fdsan SIGABRT) is fixed in 8.12.0.
  check "libcurl is ${CURL_DIR#curl-} (CVE-2025-0665 fix)" \
    bash -c "'${pfx}/bin/curl-config' --version | grep -q '${CURL_DIR#curl-}'"
  check "libSDL2_ttf.a defines TTF_OpenFontRW" \
    bash -c "'${nm}' --defined-only '${pfx}/lib/libSDL2_ttf.a' | grep -qE ' T TTF_OpenFontRW$'"
  check "libSDL2_ttf.a references freetype" \
    bash -c "'${nm}' --undefined-only '${pfx}/lib/libSDL2_ttf.a' | grep -q 'U FT_Init_FreeType'"
  check "libfreetype.a references zlib" \
    bash -c "'${nm}' --undefined-only '${pfx}/lib/libfreetype.a' | grep -q 'U inflate'"

  # no host artifacts: every archive member must match the ABI
  local bad=0 f m tmp
  for f in "${pfx}"/lib/*.a; do
    m="$("${ar}" t "${f}" | sed -n '1p')"
    tmp="$(mktemp /tmp/pinf-mem.XXXXXX)"
    "${ar}" p "${f}" "${m}" > "${tmp}" 2>/dev/null || continue
    file -b "${tmp}" | grep -qi "${expected}" || { printf '    [FAIL] host/wrong-arch object in %s\n' "${f}"; bad=1; }
    rm -f "${tmp}"
  done
  if [ "${bad}" -eq 0 ]; then
    printf '    [ok]   every .a member is %s (no host objects)\n' "${expected}"
  else
    FAILED=1
  fi

  # curl feature summary (informational)
  printf '    info: curl features: %s\n' "$("${pfx}/bin/curl-config" --features | tr '\n' ' ')"
  printf '    info: curl TLS    : %s\n' "$("${pfx}/bin/curl-config" --ssl-backends | tr '\n' ' ')"
}

# ---------------------------------------------------------------------------
# Run
# ---------------------------------------------------------------------------
if [ "${DO_CLEAN}" -eq 1 ]; then
  log "cleaning build/ and prefix/"
  rm -rf "${BUILD_ROOT}" "${PREFIX_ROOT}"
fi

if [ "${VERIFY_ONLY}" -eq 1 ]; then
  log "verify-only mode (no rebuild)"
  for ABI in "${ABIS[@]}"; do
    verify_abi "${ABI}"
  done
  if [ "${FAILED}" -ne 0 ]; then die "verification failed for one or more ABIs"; fi
  log "verification passed"
  exit 0
fi

mkdir -p "${SRC}" "${DOWNLOADS}" "${BUILD_ROOT}" "${PREFIX_ROOT}"
ensure_src "${ZLIB_DIR}"       "${TARBALL_ZLIB}"
ensure_src "${MBEDTLS_DIR}"    "${TARBALL_MBEDTLS}"
ensure_src "${FREETYPE_DIR}"   "${TARBALL_FREETYPE}"
ensure_src "${CURL_DIR}"       "${TARBALL_CURL}"
ensure_src "${SDL2_DIR}"       "${TARBALL_SDL2}"
ensure_src "${SDL2_TTF_DIR}"   "${TARBALL_SDL2_TTF}"

for ABI in "${ABIS[@]}"; do
  log "############ ${ABI} ############"
  build_zlib       "${ABI}"
  build_mbedtls    "${ABI}"
  build_freetype   "${ABI}"
  build_curl       "${ABI}"
  build_sdl2       "${ABI}"
  build_sdl2_ttf   "${ABI}"
done

for ABI in "${ABIS[@]}"; do
  verify_abi "${ABI}"
done

log "done"
for ABI in "${ABIS[@]}"; do
  info "prefix: ${PREFIX_ROOT}/${ABI}"
done
cat <<'EOF'

    Link order for the app's native library (static deps + shared SDL2):
      -Wl,--start-group
          -lSDL2_ttf -lfreetype -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lz
      -Wl,--end-group
      -lSDL2 -lm -ldl -llog -landroid -lGLESv1_CM -lGLESv2 -lOpenSLES

    (libSDL2.so is shared and must be packaged in the APK;
     everything else is linked statically into the app .so.)
EOF

if [ "${FAILED}" -ne 0 ]; then
  die "verification failed for one or more ABIs"
fi
