# =============================================================================
# PinFall -- Android cross-compilation toolchain wrapper
# =============================================================================
#
# Usage (from the project's android/ app build):
#
#   cmake -S android -B build/android-arm64 \
#         -DCMAKE_TOOLCHAIN_FILE=android/toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a
#
#   # x86_64 emulator build:
#   cmake -S android -B build/android-x86_64 \
#         -DCMAKE_TOOLCHAIN_FILE=android/toolchain.cmake \
#         -DANDROID_ABI=x86_64
#
# This wrapper only sets PinFall-specific defaults and then includes the
# NDK's own android.toolchain.cmake, so all normal NDK build behaviour
# (compiler selection, sysroot, API level, STL, sanitizers, ...) is preserved.
#
# Prebuilt dependency stack (produced by android/deps.sh) lives at:
#   android/third_party/prefix/<abi>/{include,lib}
#
# Variables exported to the project:
#   PINFALL_DEPS_PREFIX   absolute path of the per-ABI install prefix
#   PINFALL_ANDROID_API   API level used for the dependency build (default 24)
#   PINFALL_ANDROID_NDK   NDK root actually used
# =============================================================================

cmake_minimum_required(VERSION 3.22)

get_filename_component(PINFALL_ANDROID_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(PINFALL_ROOT "${PINFALL_ANDROID_DIR}/.." ABSOLUTE)

# ---------------------------------------------------------------------------
# NDK location: explicit -DANDROID_NDK / -DANDROID_NDK_HOME, then $ANDROID_NDK_HOME,
# then the known local install used by this project.
# ---------------------------------------------------------------------------
if(NOT ANDROID_NDK)
  if(DEFINED ANDROID_NDK_HOME AND NOT "${ANDROID_NDK_HOME}" STREQUAL "")
    set(ANDROID_NDK "${ANDROID_NDK_HOME}")
  elseif(DEFINED ENV{ANDROID_NDK_HOME} AND NOT "$ENV{ANDROID_NDK_HOME}" STREQUAL "")
    set(ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
  elseif(DEFINED ENV{ANDROID_NDK_ROOT} AND NOT "$ENV{ANDROID_NDK_ROOT}" STREQUAL "")
    set(ANDROID_NDK "$ENV{ANDROID_NDK_ROOT}")
  else()
    set(ANDROID_NDK "$ENV{HOME}/Android/Sdk/ndk/28.2.13676358")
  endif()
endif()
get_filename_component(ANDROID_NDK "${ANDROID_NDK}" ABSOLUTE)
if(NOT EXISTS "${ANDROID_NDK}/build/cmake/android.toolchain.cmake")
  message(FATAL_ERROR
    "PinFall: no Android NDK at '${ANDROID_NDK}'.\n"
    "Pass -DANDROID_NDK=/path/to/ndk or set ANDROID_NDK_HOME.")
endif()
set(PINFALL_ANDROID_NDK "${ANDROID_NDK}" CACHE PATH
    "Android NDK root used by the PinFall build")

# ---------------------------------------------------------------------------
# Defaults (any of these can be overridden on the command line).
# ---------------------------------------------------------------------------
set(PINFALL_ANDROID_API "24" CACHE STRING
    "Android API level targeted by the prebuilt dependency stack")

if(NOT DEFINED ANDROID_ABI OR "${ANDROID_ABI}" STREQUAL "")
  set(ANDROID_ABI "arm64-v8a" CACHE STRING "Android ABI (arm64-v8a, x86_64)" FORCE)
endif()

if(NOT DEFINED ANDROID_PLATFORM OR "${ANDROID_PLATFORM}" STREQUAL "")
  set(ANDROID_PLATFORM "android-${PINFALL_ANDROID_API}" CACHE STRING
      "Android platform level (android-${PINFALL_ANDROID_API})" FORCE)
endif()

if(NOT DEFINED ANDROID_STL OR "${ANDROID_STL}" STREQUAL "")
  # PinFall itself is pure C; c++_static is only linked if some dependency
  # (none of ours) actually needs the C++ runtime. Keep it self-contained so
  # the APK does not need libc++_shared.so alongside the app .so.
  set(ANDROID_STL "c++_static" CACHE STRING "Android STL" FORCE)
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE "Release" CACHE STRING "Build type" FORCE)
endif()

# CMAKE 4.x refuses projects that declare cmake_minimum_required(<3.5). Keep the
# NDK toolchain on a 3.22-compatible path and fail with a clear message early.
if(CMAKE_VERSION VERSION_GREATER_EQUAL 4.0)
  message(WARNING
    "PinFall: CMake ${CMAKE_VERSION} is newer than the tested 3.22.1. "
    "The NDK r28 toolchain works, but some third-party sources require "
    "cmake_minimum_required < 3.5, which CMake 4 rejects. "
    "android/deps.sh selects a 3.22.x CMake automatically.")
endif()

# ---------------------------------------------------------------------------
# Prebuilt dependency prefix for the selected ABI.
# ---------------------------------------------------------------------------
set(PINFALL_DEPS_PREFIX
    "${PINFALL_ANDROID_DIR}/third_party/prefix/${ANDROID_ABI}"
    CACHE PATH "PinFall prebuilt Android dependency prefix (per ABI)")

# ---------------------------------------------------------------------------
# Delegate everything else to the NDK toolchain file.
# ---------------------------------------------------------------------------
include("${ANDROID_NDK}/build/cmake/android.toolchain.cmake")

# Let find_package()/find_library()/find_path() see the prebuilt prefix while
# keeping every result inside the NDK sysroot + this prefix (target-only).
list(APPEND CMAKE_FIND_ROOT_PATH "${PINFALL_DEPS_PREFIX}")
list(REMOVE_DUPLICATES CMAKE_FIND_ROOT_PATH)
list(APPEND CMAKE_PREFIX_PATH "${PINFALL_DEPS_PREFIX}")

# Config-mode packages installed by deps.sh: SDL2 (and MbedTLS).
if(EXISTS "${PINFALL_DEPS_PREFIX}/lib/cmake/SDL2/SDL2Config.cmake")
  if(NOT SDL2_DIR)
    set(SDL2_DIR "${PINFALL_DEPS_PREFIX}/lib/cmake/SDL2" CACHE PATH
        "SDL2 CMake package directory (PinFall prebuilt)" FORCE)
  endif()
endif()
if(EXISTS "${PINFALL_DEPS_PREFIX}/lib/cmake/MbedTLS/MbedTLSConfig.cmake")
  if(NOT MbedTLS_DIR)
    set(MbedTLS_DIR "${PINFALL_DEPS_PREFIX}/lib/cmake/MbedTLS" CACHE PATH
        "MbedTLS CMake package directory (PinFall prebuilt)" FORCE)
  endif()
endif()

# pkg-config must only see the cross-compiled .pc files, never the host ones.
# (FindFreetype and any pkg_check_modules() call rely on this.)
set(ENV{PKG_CONFIG_LIBDIR} "${PINFALL_DEPS_PREFIX}/lib/pkgconfig")
set(ENV{PKG_CONFIG_PATH} "${PINFALL_DEPS_PREFIX}/lib/pkgconfig")
set(ENV{PKG_CONFIG_SYSROOT_DIR} "")

# Report once (CMake processes the toolchain file twice on the first configure).
if(NOT PINFALL_TOOLCHAIN_REPORTED)
  set(PINFALL_TOOLCHAIN_REPORTED TRUE)
  message(STATUS "PinFall Android toolchain: ABI=${ANDROID_ABI} "
                 "platform=${ANDROID_PLATFORM} NDK=${PINFALL_ANDROID_NDK}")
  message(STATUS "PinFall deps prefix: ${PINFALL_DEPS_PREFIX}")
  if(NOT EXISTS "${PINFALL_DEPS_PREFIX}/lib")
    message(WARNING
      "PinFall: '${PINFALL_DEPS_PREFIX}/lib' does not exist. "
      "Run android/deps.sh ${ANDROID_ABI} first.")
  endif()
endif()
