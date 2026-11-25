# PinFall — Android native build

Cross-compiled dependency stack for the PinFall SDL2 Android port (pure C).
Everything here is produced by [`android/deps.sh`](deps.sh) and consumed through
[`android/toolchain.cmake`](toolchain.cmake).

No sudo, no device and no network access are required once the source tarballs
are in `android/third_party/downloads/`.

---

## 1. Toolchain

| Item | Value |
|---|---|
| NDK | `~/Android/Sdk/ndk/28.2.13676358` (r28c) |
| Compiler | `aarch64-linux-android24-clang`, `x86_64-linux-android24-clang` |
| Binutils | `$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-{ar,ranlib,nm,readelf,strip}` |
| CMake | `~/Android/Sdk/cmake/3.22.1/bin/cmake` (3.22.1) |
| Ninja | `~/Android/Sdk/cmake/3.22.1/bin/ninja` (or system ninja) |
| API level | **24** (`ANDROID_PLATFORM=android-24`) |
| ABIs | **arm64-v8a** (primary, phone), **x86_64** (secondary, emulator) |

> CMake 3.22.x is required: SDL2 2.32.4 declares `cmake_minimum_required(3.0...3.10)`
> and CMake 4.x rejects any minimum below 3.5. `deps.sh` picks the SDK's 3.22.1
> automatically (`--cmake PATH` to override).

## 2. Versions & flags

| Library | Version | Output | Notes |
|---|---|---|---|
| zlib | 1.3.1 | `libz.a` static | `-O2 -fPIC`, zlib `configure --static` |
| mbedTLS | 3.6.2 | `libmbedtls.a`, `libmbedx509.a`, `libmbedcrypto.a` static | CMake + NDK toolchain, `ENABLE_TESTING=OFF ENABLE_PROGRAMS=OFF`, shared libs OFF |
| freetype | 2.13.3 | `libfreetype.a` static | autotools, `--without-harfbuzz --without-png --without-bzip2 --without-brotli --with-zlib`, `--with-pic` |
| curl | 8.12.0 | `libcurl.a` static | autotools, `--with-mbedtls --with-zlib`, HTTP(S)/FTP/WS only, no LDAP/RTSP/IMAP/etc., `--disable-manual`, `--without-libpsl/brotli/zstd/libidn2/nghttp2`, `--with-ca-path=/system/etc/security/cacerts` |
| SDL2 | 2.32.4 | **`libSDL2.so` shared** + `libSDL2main.a` static (+ `libSDL2.a` static, `libSDL2_test.a`) | CMake + NDK toolchain, `SDL_SHARED=ON SDL_STATIC=ON SDL_STATIC_PIC=ON SDL_TEST=OFF SDL_TESTS=OFF` |
| SDL2_ttf | 2.24.0 | `libSDL2_ttf.a` static | CMake, `BUILD_SHARED_LIBS=OFF`, `SDL2TTF_VENDORED=OFF`, `SDL2TTF_HARFBUZZ=OFF`, freetype from the prefix |

Installed prefixes: `android/third_party/prefix/<abi>/{include,lib,lib/cmake,lib/pkgconfig,bin}`.

Headers live at `include/SDL2/` (SDL2, SDL_ttf), `include/freetype2/`,
`include/curl/`, `include/mbedtls/`, `include/zlib.h`.

SDL2 and SDL2_ttf are installed with their CMake package configs
(`lib/cmake/SDL2/SDL2Config.cmake`), so `find_package(SDL2 CONFIG)` works; the
toolchain wrapper also sets `SDL2_DIR` and pins pkg-config so
`find_package(Freetype)` resolves the cross-built 2.13.3, not the host copy:

```
-- find_package(SDL2)      -> 2.32.4 (.../prefix/arm64-v8a/lib/cmake/SDL2)
-- find_package(Freetype)  -> 2.13.3 lib=.../prefix/arm64-v8a/lib/libfreetype.a
```

## 3. Rebuild — one command

```bash
cd pinfall

./android/deps.sh                      # arm64-v8a (default)
./android/deps.sh arm64-v8a x86_64     # both ABIs (or: ./android/deps.sh all)
./android/deps.sh --clean all          # wipe build/ + prefix/ and rebuild
./android/deps.sh --jobs 8 x86_64      # limit parallelism
./android/deps.sh --verify-only        # re-run the checks only, no rebuild
./android/deps.sh --ndk /path/to/ndk --api 24
```

The script:

1. validates the NDK and picks a 3.22.x CMake,
2. extracts any missing sources from `android/third_party/downloads/`,
3. rebuilds zlib → mbedTLS → freetype → curl → SDL2 → SDL2_ttf for every ABI
   (build trees in `android/third_party/build/<abi>/<pkg>/`, logs next to them),
4. **verifies** the result and exits non-zero on failure:
   - `llvm-readelf -h libSDL2.so` → `Machine: AArch64` (arm64) / `X86-64`,
   - `file libSDL2.so` → `for Android 24 ... built by NDK r28c`,
   - `Java_org_libsdl_app_SDLActivity_nativeRunMain` exported by `libSDL2.so`,
   - `libcurl.a` references `mbedtls_ssl_init`, `libSDL2_ttf.a` defines
     `TTF_OpenFontRW` and references `FT_Init_FreeType`, `libfreetype.a`
     references `inflate`,
   - every object inside every `.a` matches the target ABI (no host x86-64
     objects in the arm64 prefix).

Sources can also be re-fetched by dropping the official tarballs into
`android/third_party/downloads/`:

```
zlib-1.3.1.tar.gz  mbedtls-3.6.2.tar.bz2  freetype-2.13.3.tar.xz
curl-8.11.1.tar.gz  SDL2-2.32.4.tar.gz  SDL2_ttf-2.24.0.tar.gz
```

## 4. Using it from the app build

```bash
cmake -S android -B build/android-arm64 \
      -DCMAKE_TOOLCHAIN_FILE=android/toolchain.cmake \
      -DANDROID_ABI=arm64-v8a \
      -DCMAKE_BUILD_TYPE=Release

cmake --build build/android-arm64

# x86_64 emulator
cmake -S android -B build/android-x86_64 \
      -DCMAKE_TOOLCHAIN_FILE=android/toolchain.cmake -DANDROID_ABI=x86_64
```

`toolchain.cmake`:

* defaults to NDK 28.2.13676358, API 24, `arm64-v8a`, `Release`, `c++_static`
  (only linked if something actually uses C++ — none of these deps do),
* includes the NDK's own `android.toolchain.cmake` (all standard `ANDROID_*`
  variables still work),
* exports `PINFALL_DEPS_PREFIX = android/third_party/prefix/${ANDROID_ABI}`,
  appends it to `CMAKE_FIND_ROOT_PATH` / `CMAKE_PREFIX_PATH`, sets `SDL2_DIR`,
  and pins `PKG_CONFIG_LIBDIR/PKG_CONFIG_PATH` to that prefix so no host
  pkg-config module can leak in.

Link order matters (static archives, circular references between curl ↔ mbedTLS):

```cmake
target_include_directories(app PRIVATE
    ${PINFALL_DEPS_PREFIX}/include
    ${PINFALL_DEPS_PREFIX}/include/freetype2
    ${PINFALL_DEPS_PREFIX}/include/SDL2)

target_link_libraries(app PRIVATE
    -Wl,--start-group
      ${PINFALL_DEPS_PREFIX}/lib/libSDL2_ttf.a
      ${PINFALL_DEPS_PREFIX}/lib/libfreetype.a
      ${PINFALL_DEPS_PREFIX}/lib/libcurl.a
      ${PINFALL_DEPS_PREFIX}/lib/libmbedtls.a
      ${PINFALL_DEPS_PREFIX}/lib/libmbedx509.a
      ${PINFALL_DEPS_PREFIX}/lib/libmbedcrypto.a
      ${PINFALL_DEPS_PREFIX}/lib/libz.a
    -Wl,--end-group
    ${PINFALL_DEPS_PREFIX}/lib/libSDL2.so
    m dl log android GLESv1_CM GLESv2 OpenSLES)
```

`libSDL2.so` is the only shared dependency and must be packaged in the APK
(`jniLibs/<abi>/libSDL2.so`). Everything else is linked into the app's `.so`.

`libSDL2main.a` supplies the `SDL_main` → `main` shim used by the Android
`SDLActivity` launcher (link it **before** `libSDL2.so` if the app defines
`main()` rather than `SDL_main`).

The Java glue that loads `libSDL2.so` and calls
`Java_org_libsdl_app_SDLActivity_nativeRunMain` is not built here; copy it from
the SDL2 source tree:

```
android/third_party/src/SDL2-2.32.4/android-project/app/src/main/java/org/libsdl/app/*.java
android/third_party/src/SDL2-2.32.4/android-project/app/src/main/AndroidManifest.xml
```

## 5. Verification snapshot (arm64-v8a)

```
$ llvm-readelf -h prefix/arm64-v8a/lib/libSDL2.so
  Class:   ELF64
  Machine: AArch64
  Type:    DYN (Shared object file)

$ file prefix/arm64-v8a/lib/libSDL2.so
ELF 64-bit LSB shared object, ARM aarch64, ... for Android 24,
built by NDK r28c (13676358), with debug_info, not stripped

$ llvm-nm -D --defined-only prefix/arm64-v8a/lib/libSDL2.so | grep SDLActivity_native
T Java_org_libsdl_app_SDLActivity_nativeRunMain
T Java_org_libsdl_app_SDLActivity_nativeSetupJNI          # + HIDDeviceManager_* JNI exports

$ llvm-readelf -d prefix/arm64-v8a/lib/libSDL2.so | grep NEEDED
NEEDED libm.so  libOpenSLES.so  libdl.so  liblog.so
NEEDED libandroid.so  libGLESv1_CM.so  libGLESv2.so  libc.so

$ llvm-nm --undefined-only prefix/arm64-v8a/lib/libcurl.a | grep -c mbedtls_
72

$ prefix/arm64-v8a/bin/curl-config --ssl-backends
mbedTLS
$ prefix/arm64-v8a/bin/curl-config --features
alt-svc AsynchDNS HSTS HTTPS-proxy IPv6 Largefile libz NTLM SSL threadsafe UnixSockets

$ llvm-nm --defined-only prefix/arm64-v8a/lib/libSDL2_ttf.a | grep TTF_OpenFontRW
T TTF_OpenFontRW
$ llvm-nm --undefined-only prefix/arm64-v8a/lib/libSDL2_ttf.a | grep FT_Init_FreeType
U FT_Init_FreeType
```

`android/deps.sh` re-runs all of the above on every build and fails loudly.

End-to-end link test (also proves the toolchain wrapper works) — a throwaway
CMake project under `/tmp/pinfall-android-sanity` compiles a `SHARED` library
that calls into SDL2, SDL2_ttf, freetype, curl, mbedTLS and zlib with
`-Wl,--no-undefined`, for both ABIs:

```
[2/2] Linking C shared library libpinfall_sanity.so     # arm64-v8a and x86_64
$ file libpinfall_sanity.so
ELF 64-bit LSB shared object, ARM aarch64, ... dynamically linked
$ llvm-readelf -d libpinfall_sanity.so | grep NEEDED
NEEDED libSDL2.so  libm.so  libdl.so  liblog.so  libandroid.so ...
```

Only libc/libSDL2 imports remain undefined; every static-stack symbol resolves.

## 6. Known quirks / gotchas

* **curl 8.11.1 + mbedTLS:** `--without-ssl` cannot be combined with
  `--with-mbedtls` — curl aborts with
  `--without-ssl has been set together with an explicit option to use an ssl library`.
  Do not pass it; `--with-mbedtls` alone is the TLS backend (OpenSSL is simply
  not selected). This is why the task's original flag list was adjusted.
* **curl CA path warning:** `ca cert path: /system/etc/security/cacerts
  (warning: certs not found)` is expected while cross-compiling — the path only
  exists on-device. That is the Android system CA store; apps may still override
  with `CURLOPT_CAINFO`.
* **CMake 4.x** cannot configure SDL2 2.32.4 (`cmake_minimum_required(3.0...3.10)`).
  Always use the SDK's 3.22.1; `deps.sh` does.
* **Static + shared SDL2 in one prefix:** `libSDL2.a` and `libSDL2.so` coexist;
  link the `.so` for the APK (it carries the Android video/audio/Java glue).
  `nm -D libSDL2.so | grep android` / the `Java_org_libsdl_app_*` exports
  confirm the Android backend is compiled in.
* **`-fPIC` everywhere:** all static libs are built with `-fPIC` (zlib `-fPIC`,
  freetype `--with-pic`, mbedTLS/SDL2 `CMAKE_POSITION_INDEPENDENT_CODE=ON`,
  curl `CFLAGS=-fPIC`) so they can be linked into the app's shared library.
* **mbedTLS installs extra `libeverest.a` / `libp256m.a`:** only needed when
  `MBEDTLS_ECDH_VARIANT_EVEREST_ENABLED` is on (it is not). `curl-config
  --static-libs` (the authoritative link line) lists only
  `-lmbedtls -lmbedx509 -lmbedcrypto -lz`.
* **No host contamination:** `deps.sh` verify walks every archive member and
  checks its ELF machine; the arm64 prefix contains only AArch64 objects.
* **Disk:** full trees for both ABIs are ~1.5 GB. `deps.sh --clean` reclaims
  build trees; prefixes are ~80 MB per ABI.
* **Rebuilds are idempotent:** packages are rebuilt in their own directories;
  re-running `deps.sh` without `--clean` is safe (it recreates each build dir).

## 7. Layout

```
android/
├── deps.sh                       # one-command rebuild + verify (both ABIs)
├── toolchain.cmake               # CMAKE_TOOLCHAIN_FILE wrapper for the app build
├── BUILD-ANDROID.md              # this file
└── third_party/
    ├── downloads/                # upstream tarballs (input)
    ├── src/                      # extracted sources
    ├── build/<abi>/<pkg>/        # per-ABI build trees + logs
    └── prefix/<abi>/
        ├── include/              # SDL2, SDL_ttf, freetype2, curl, mbedtls, zlib
        ├── lib/                  # lib*.a, libSDL2.so, lib/cmake/SDL2, lib/pkgconfig
        └── bin/                  # curl-config
```

## Notes

- **curl 8.12.0** (updated from 8.11.1): fixes CVE-2025-0665, an eventfd
  double-close in the threaded resolver that crashed the Android app with
  `fdsan: double-close of file descriptor ...` during concurrent image fetches.
  `deps.sh --verify-only` asserts the version.
