cmake_minimum_required(VERSION 3.18)
project(pinfall C)

set(PINFALL_ROOT   ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
set(SDL2_SRC       ${PINFALL_ROOT}/android/third_party/src/SDL2-2.32.4)
set(DEPS_PREFIX    ${PINFALL_ROOT}/android/third_party/prefix/${ANDROID_ABI})

# SDL2 is built from source as part of the app (shared libSDL2.so + SDL2main).
set(SDL_SHARED ON  CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)
set(SDL_TEST   OFF CACHE BOOL "" FORCE)
set(SDL_TESTS  OFF CACHE BOOL "" FORCE)
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
add_subdirectory(${SDL2_SRC} ${CMAKE_CURRENT_BINARY_DIR}/SDL2)

file(GLOB PINFALL_SOURCES ${PINFALL_ROOT}/src/*.c)
list(APPEND PINFALL_SOURCES
     ${PINFALL_ROOT}/vendor/cJSON.c
     ${PINFALL_ROOT}/vendor/lz4/lz4.c)

add_library(main SHARED ${PINFALL_SOURCES})
target_compile_definitions(main PRIVATE PC_ANDROID=1 _GNU_SOURCE)
target_include_directories(main PRIVATE
    ${PINFALL_ROOT}/include
    ${PINFALL_ROOT}/vendor
    ${PINFALL_ROOT}/src
    ${PINFALL_ROOT}/vendor/lz4
    ${DEPS_PREFIX}/include
    ${DEPS_PREFIX}/include/freetype2
    ${SDL2_SRC}/include)
target_link_directories(main PRIVATE ${DEPS_PREFIX}/lib)

# Static deps first (order matters: SDL2_ttf -> freetype, curl -> mbedtls/zlib).
target_link_libraries(main PRIVATE
    ${DEPS_PREFIX}/lib/libSDL2_ttf.a
    ${DEPS_PREFIX}/lib/libfreetype.a
    ${DEPS_PREFIX}/lib/libcurl.a
    ${DEPS_PREFIX}/lib/libmbedtls.a
    ${DEPS_PREFIX}/lib/libmbedx509.a
    ${DEPS_PREFIX}/lib/libmbedcrypto.a
    ${DEPS_PREFIX}/lib/libz.a
    SDL2 SDL2main
    log android m)
