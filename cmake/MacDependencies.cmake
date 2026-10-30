# Build Mac dependencies for the same architecture and deployment target as the game.
# Prebuilt Homebrew libraries can require a newer macOS than the selected target.
include(FetchContent)
set(FETCHCONTENT_QUIET OFF)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ssr_sdl2
    URL https://www.libsdl.org/release/SDL2-2.32.10.tar.gz
    TIMEOUT 90
    TLS_VERIFY TRUE
    URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
    # (iOS: the window in a UIScene, so the app fills the whole display, a foldable iPhone's
    # folded-out one too: cmake/sdl2-uiscene.patch, with the scene manifest in tools/ios/build_all.sh.)
    PATCH_COMMAND patch -p1 -N -i "${CMAKE_CURRENT_LIST_DIR}/sdl2-uiscene.patch")
FetchContent_MakeAvailable(ssr_sdl2)

# (iOS, tools/ios/build_all.sh: SDL2 only. libpng is for the test harness's screenshots.)
if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

# zlib is supplied by macOS; use the SDK copy, never a newer Homebrew dylib.
set(SSR_MACOS_SDK "${CMAKE_OSX_SYSROOT}")
if(NOT IS_DIRECTORY "${SSR_MACOS_SDK}")
    if(NOT SSR_MACOS_SDK)
        set(SSR_MACOS_SDK macosx)
    endif()
    execute_process(COMMAND xcrun --sdk "${SSR_MACOS_SDK}" --show-sdk-path
        OUTPUT_VARIABLE SSR_MACOS_SDK OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE SSR_SDK_RESULT)
    if(NOT SSR_SDK_RESULT EQUAL 0)
        message(FATAL_ERROR "Install/select Xcode Command Line Tools before building")
    endif()
endif()
find_library(SSR_SDK_ZLIB NAMES z PATHS "${SSR_MACOS_SDK}/usr/lib" NO_DEFAULT_PATH REQUIRED)
set(ZLIB_LIBRARY "${SSR_SDK_ZLIB}" CACHE FILEPATH "SDK zlib" FORCE)
set(ZLIB_INCLUDE_DIR "${SSR_MACOS_SDK}/usr/include" CACHE PATH "SDK zlib headers" FORCE)
set(PNG_FRAMEWORK OFF CACHE BOOL "" FORCE)
set(PNG_SHARED OFF CACHE BOOL "" FORCE)
set(PNG_STATIC ON CACHE BOOL "" FORCE)
set(PNG_TESTS OFF CACHE BOOL "" FORCE)
set(PNG_TOOLS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(ssr_png
    URL https://download.sourceforge.net/libpng/libpng-1.6.59.tar.gz
    TIMEOUT 90
    TLS_VERIFY TRUE
    URL_HASH SHA256=86a3e4b501f7f50e392c4e456ea158893e9a595b1c63eb88c7bb9f6cf8772dad)
FetchContent_MakeAvailable(ssr_png)
add_library(PNG::PNG ALIAS png_static)
