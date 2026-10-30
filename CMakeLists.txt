cmake_minimum_required(VERSION 3.16)
# Set the SDK floor before compiler detection, including melonDS and generated ASM.
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin" AND NOT DEFINED CMAKE_OSX_DEPLOYMENT_TARGET)
    if(DEFINED ENV{MACOSX_DEPLOYMENT_TARGET})
        set(CMAKE_OSX_DEPLOYMENT_TARGET "$ENV{MACOSX_DEPLOYMENT_TARGET}" CACHE STRING "Minimum macOS version")
    else()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS version")
    endif()
endif()
project(ssrecomp LANGUAGES C CXX)
set(SSR_GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/gen" CACHE PATH "Generated game sources")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

# melonDS core is the hardware runtime; its own frontend is not used. Its OpenGL 3D renderer
# is, on Linux and the Switch, to draw the 3D scene on the graphics chip (SSR_GPU3D:
# melonDS/src/GPU3D_GLThread.h, frontend/glcontext.cpp; through EGL, desktop OpenGL 3.3; on
# Windows through WGL, frontend/glcontext_wgl.cpp, for the PC all-games program).
set(BUILD_QT_SDL OFF CACHE BOOL "" FORCE)
set(SSR_GPU3D_DEFAULT OFF)
if(NINTENDO_SWITCH OR CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    set(SSR_GPU3D_DEFAULT ON)
elseif(UNIX AND NOT APPLE AND NOT ANDROID)
    find_library(SSR_EGL_LIBRARY EGL)
    find_path(SSR_EGL_INCLUDE EGL/egl.h)
    if(SSR_EGL_LIBRARY AND SSR_EGL_INCLUDE)
        set(SSR_GPU3D_DEFAULT ON)
    endif()
endif()
option(SSR_GPU3D "Draw the 3D scene on the graphics chip when chosen (OpenGL through EGL)" ${SSR_GPU3D_DEFAULT})
set(ENABLE_OGLRENDERER ${SSR_GPU3D} CACHE BOOL "" FORCE)
# The JIT is only for games that are not recompiled (Pokemon Black 2: --jit); the recompiled
# SoulSilver runs on its own code either way.
option(SSR_JIT "Build melonDS's JIT in (for --jit)" OFF)
set(ENABLE_JIT ${SSR_JIT} CACHE BOOL "" FORCE)
set(ENABLE_GDBSTUB OFF CACHE BOOL "" FORCE)
set(ENABLE_LTO_RELEASE OFF CACHE BOOL "" FORCE)
add_subdirectory(melonDS EXCLUDE_FROM_ALL)

# Nintendo Switch homebrew: built with devkitPro's toolchain file ($DEVKITPRO/cmake/Switch.cmake),
# which sets NINTENDO_SWITCH. See tools/switch/build.sh.
if(NINTENDO_SWITCH OR CMAKE_SYSTEM_NAME STREQUAL "NintendoSwitch")
    set(SSR_SWITCH ON)
    if(NOT DEVKITPRO)
        set(DEVKITPRO "$ENV{DEVKITPRO}")
    endif()
    if(NOT DEVKITPRO)
        set(DEVKITPRO /opt/devkitpro)
    endif()
else()
    set(SSR_SWITCH OFF)
endif()

option(SSR_MACOS_SOURCE_DEPS "Build SDL2 and libpng for the selected macOS target" ${APPLE})
if(SSR_SWITCH)
    # SDL2 from devkitPro's portlibs (the switch-sdl2 package), linked statically with what it
    # needs (EGL and Mesa for OpenGL ES, libnx), which only its pkg-config file lists.
    if(NOT PKG_CONFIG_EXECUTABLE)
        find_program(PKG_CONFIG_EXECUTABLE NAMES aarch64-none-elf-pkg-config
            HINTS "${DEVKITPRO}/portlibs/switch/bin" NO_CMAKE_FIND_ROOT_PATH)
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
elseif(APPLE AND NOT ANDROID AND SSR_MACOS_SOURCE_DEPS)
    include(cmake/MacDependencies.cmake)
else()
    find_package(SDL2 REQUIRED)
    # PNG screenshots (--shot-dir, the test harness): not needed to play, so a player's build
    # can leave libpng out (SSR_PNG=OFF, as the PC all-games download does).
    option(SSR_PNG "Write screenshots as PNG files (libpng)" ON)
    if(NOT ANDROID AND SSR_PNG)
        find_package(PNG REQUIRED)
    endif()
endif()

enable_language(ASM)

# Recompiled game code, produced by tools/recomp.py.
include("${SSR_GENERATED_DIR}/sources.cmake")
add_library(gamecode OBJECT ${RC_GENERATED_SOURCES})
target_include_directories(gamecode PRIVATE melonDS/src runtime "${SSR_GENERATED_DIR}")
# Generated code is huge and uniform; -O1 keeps build times reasonable.
target_compile_options(gamecode PRIVATE -O1 -g0 -fno-exceptions -fno-rtti "$<$<COMPILE_LANGUAGE:CXX>:-fno-stack-protector>"
    "$<$<COMPILE_LANGUAGE:ASM>:-Wa,-I${SSR_GENERATED_DIR}>")

# RetroAchievements (frontend/achievements.cpp): RetroAchievements' own rcheevos library, as it
# comes (frontend/thirdparty/rcheevos, MIT).
file(GLOB_RECURSE SSR_RCHEEVOS_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/frontend/thirdparty/rcheevos/src/*.c")
add_library(rcheevos STATIC ${SSR_RCHEEVOS_SOURCES})
target_include_directories(rcheevos PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/frontend/thirdparty/rcheevos/include")

set(SSRECOMP_SOURCES
    frontend/main.cpp
    frontend/achievements.cpp
    frontend/achievements_http.cpp
    frontend/platform.cpp
    runtime/recomp_rt.cpp
    runtime/recomp_mem.cpp
    "${SSR_GENERATED_DIR}/apps_table.cpp"
    $<TARGET_OBJECTS:gamecode>)

# The app's version (Android: for its check for a newer release; Switch: shown in the
# Homebrew Menu): the newest one in CHANGELOG.md.
set(SSR_VERSION "")
if(ANDROID OR SSR_SWITCH)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS CHANGELOG.md)
        file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md" SSR_RELEASES REGEX "^## v[0-9]")
        if(SSR_RELEASES)
            list(GET SSR_RELEASES 0 SSR_VERSION)
            string(REGEX REPLACE "^## v([0-9.]+).*" "\\1" SSR_VERSION "${SSR_VERSION}")
        endif()
    endif()
endif()

if(ANDROID)
    # On Android the game is the library libmain.so that SDL's Java activity loads
    # (tools/android/apk.py packs it into an APK). SDL2 is one built for Android
    # (CMAKE_PREFIX_PATH); no libpng: screenshots are a desktop test aid.
    add_library(ssrecomp SHARED ${SSRECOMP_SOURCES})
    set_target_properties(ssrecomp PROPERTIES OUTPUT_NAME main)
    target_include_directories(ssrecomp PRIVATE melonDS/src frontend runtime "${SSR_GENERATED_DIR}")
    target_compile_definitions(ssrecomp PRIVATE SSR_NO_PNG SSR_VERSION="${SSR_VERSION}")
    target_link_libraries(ssrecomp PRIVATE core SDL2::SDL2 android log)
    # The all-games app (tools/android/build_all.sh): no game inside, the game list from the
    # PC program (frontend/pc_launcher.cpp) with the ROMs in a folder the player chooses.
    option(SSR_PC_ALL_GAMES "The all-games program with its game list" OFF)
    if(SSR_PC_ALL_GAMES)
        target_sources(ssrecomp PRIVATE frontend/pc_launcher.cpp)
        target_compile_definitions(ssrecomp PRIVATE SSR_PC_LAUNCHER)
    endif()
elseif(SSR_SWITCH)
    # On the Switch the game is an .nro, a program for the Homebrew Menu, made from the
    # executable below. Like the Android app it holds the player's own game, so it is built
    # on the player's PC and never shared. No libpng: screenshots are a desktop test aid.
    add_executable(ssrecomp ${SSRECOMP_SOURCES} frontend/switch_platform.cpp)
    # main.cpp includes <SDL2/SDL.h>, so the folder above SDL2's own is needed too.
    pkg_get_variable(SDL2_INCLUDEDIR sdl2 includedir)
    target_include_directories(ssrecomp PRIVATE melonDS/src frontend runtime "${SSR_GENERATED_DIR}" "${SDL2_INCLUDEDIR}" ${SDL2_STATIC_INCLUDE_DIRS})
    target_compile_options(ssrecomp PRIVATE ${SDL2_STATIC_CFLAGS_OTHER})
    target_compile_definitions(ssrecomp PRIVATE SSR_NO_PNG SSR_VERSION="${SSR_VERSION}")
    target_link_directories(ssrecomp PRIVATE ${SDL2_STATIC_LIBRARY_DIRS})
    target_link_libraries(ssrecomp PRIVATE core ${SDL2_STATIC_LIBRARIES} nx m)
    if(SSR_GPU3D)
        # (glad's loader includes <dlfcn.h>, which the Switch does not have; it is not used)
        target_include_directories(core BEFORE PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/frontend/switch_stub")
        target_include_directories(core PRIVATE ${SDL2_STATIC_INCLUDE_DIRS})
        # EGL and Mesa (SDL's OpenGL ES drawing uses them too; named here in case its
        # pkg-config file does not pass them on).
        foreach(lib EGL glapi drm_nouveau)
            find_library(SSR_SWITCH_${lib} ${lib} HINTS "${DEVKITPRO}/portlibs/switch/lib")
            if(SSR_SWITCH_${lib})
                target_link_libraries(ssrecomp PRIVATE "${SSR_SWITCH_${lib}}")
            endif()
        endforeach()
        target_link_libraries(ssrecomp PRIVATE nx)
    endif()

    # deko3D, the Switch's own graphics API (devkitPro's switch-dev has the library and uam,
    # which compiles its shaders, written in GLSL, at build time; they are then built into the
    # program as arrays): the widescreen world on the graphics chip (melonDS/src/GPU3D_WideGpu.h)
    # and a test of deko3D (frontend/dekoprobe.cpp).
    find_library(SSR_DEKO3D_LIB deko3d HINTS "${DEVKITPRO}/libnx/lib")
    find_program(SSR_UAM uam HINTS "${DEVKITPRO}/tools/bin" NO_CMAKE_FIND_ROOT_PATH)
    if(SSR_DEKO3D_LIB AND SSR_UAM)
        set(SSR_DEKO_OUT "${CMAKE_CURRENT_BINARY_DIR}/deko_shaders")
        file(MAKE_DIRECTORY "${SSR_DEKO_OUT}")
        set(SSR_DEKO_HEADERS)
        # folder:shader:stage:header[:DEFINE[,DEFINE]] (the header deko_shaders/<header>.h holds the array
        # <header>; with a DEFINE, the shader is compiled with it defined, as a variant: uam has
        # no option for that, so a copy with the #define after its first line is compiled)
        foreach(shader frontend/deko:probe_vsh:vert:probe_vsh frontend/deko:probe_fsh:frag:probe_fsh
                melonDS/src/wide_gpu:wide_vsh:vert:wide_vsh_dksh melonDS/src/wide_gpu:wide_fsh:frag:wide_fsh_dksh
                melonDS/src/wide_gpu:wide_fsh:frag:wide_fsh_texbuf_dksh:WIDE_TEXBUF
                melonDS/src/wide_gpu:wide_fsh:frag:wide_fsh_early_dksh:WIDE_EARLY
                melonDS/src/wide_gpu:wide_fsh:frag:wide_fsh_texbuf_early_dksh:WIDE_TEXBUF,WIDE_EARLY
                melonDS/src/wide_gpu:final_vsh:vert:final_vsh_dksh melonDS/src/wide_gpu:final_fsh:frag:final_fsh_dksh)
            string(REPLACE ":" ";" parts "${shader}")
            list(GET parts 0 folder)
            list(GET parts 1 name)
            list(GET parts 2 stage)
            list(GET parts 3 header)
            list(LENGTH parts nparts)
            set(src "${CMAKE_CURRENT_SOURCE_DIR}/${folder}/${name}.glsl")
            set(compiled "${src}")
            set(make_variant)
            set(compile COMMAND "${SSR_UAM}" -o "${SSR_DEKO_OUT}/${header}.dksh" -s ${stage} "${src}")
            if(nparts GREATER 4)
                list(GET parts 4 define)
                set(compiled "${SSR_DEKO_OUT}/${header}.glsl")
                set(make_variant COMMAND "${CMAKE_COMMAND}" "-DIN=${src}" "-DOUT=${compiled}" "-DDEFINE=${define}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch/glsl_define.cmake")
                # (A variant that does not compile leaves an empty shader, which the program
                # finds it cannot load, and does without, rather than stopping the build.)
                set(compile COMMAND sh -c "\"$0\" -o \"$1\" -s $2 \"$3\" || : > \"$1\""
                    "${SSR_UAM}" "${SSR_DEKO_OUT}/${header}.dksh" ${stage} "${compiled}")
            endif()
            add_custom_command(OUTPUT "${SSR_DEKO_OUT}/${header}.h"
                ${make_variant}
                ${compile}
                COMMAND "${CMAKE_COMMAND}" "-DIN=${SSR_DEKO_OUT}/${header}.dksh" "-DOUT=${SSR_DEKO_OUT}/${header}.h" "-DNAME=${header}"
                    -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch/embed.cmake"
                DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch/embed.cmake"
                    "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch/glsl_define.cmake"
                COMMENT "Compiling the shader ${header}"
                VERBATIM)
            list(APPEND SSR_DEKO_HEADERS "${SSR_DEKO_OUT}/${header}.h")
        endforeach()
        # (The core library is made in another folder, which cannot see that these headers are
        # generated here: both wait for a target that makes them instead.)
        add_custom_target(ssr_deko_shaders DEPENDS ${SSR_DEKO_HEADERS})
        target_sources(ssrecomp PRIVATE frontend/dekoprobe.cpp)
        add_dependencies(ssrecomp ssr_deko_shaders)
        target_include_directories(ssrecomp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_compile_definitions(ssrecomp PRIVATE SSR_DEKO3D)
        target_sources(core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/melonDS/src/GPU3D_WideGpu_Deko.cpp")
        add_dependencies(core ssr_deko_shaders)
        target_include_directories(core PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
        target_compile_definitions(core PRIVATE SSR_DEKO3D)
        target_link_libraries(ssrecomp PRIVATE "${SSR_DEKO3D_LIB}" nx)
        message(STATUS "deko3D: ${SSR_DEKO3D_LIB}, shaders compiled by ${SSR_UAM}")
    else()
        message(STATUS "deko3D or uam not found (both come with switch-dev): built without the deko3D test")
    endif()

    find_program(SSR_NACPTOOL nacptool HINTS "${DEVKITPRO}/tools/bin" NO_CMAKE_FIND_ROOT_PATH)
    find_program(SSR_ELF2NRO elf2nro HINTS "${DEVKITPRO}/tools/bin" NO_CMAKE_FIND_ROOT_PATH)
    if(NOT SSR_NACPTOOL OR NOT SSR_ELF2NRO)
        message(FATAL_ERROR "nacptool and elf2nro were not found in ${DEVKITPRO}/tools/bin: install switch-dev with dkp-pacman")
    endif()
    set(SSR_NRO_VERSION "${SSR_VERSION}")
    if(NOT SSR_NRO_VERSION)
        set(SSR_NRO_VERSION "dev")
    endif()
    # The .nro is packed on every build (a few seconds), so that changing only the icon needs
    # no rebuild. The icon is icon.jpg in the build folder when there is one (your own, a
    # 256x256 JPEG; kept out of the source), otherwise libnx's default.
    set(SSR_NRO_TITLE "ssrecomp" CACHE STRING "Title shown in the Homebrew Menu")
    # Files put in the .nro's romfs (for the Black games' shared build, their function tables
    # made without a ROM: tools/switch/build_game.sh bw). Empty: none.
    set(SSR_SWITCH_ROMFS "" CACHE PATH "Folder whose files go in the .nro's romfs")
    # The all-games program (tools/switch/build_game.sh all): its game list also shows the games
    # in the other folders of /switch (the one-game programs' folders). The one-game programs
    # only look in their own folder.
    option(SSR_ALL_GAMES "Switch: the all-games program" OFF)
    if(SSR_ALL_GAMES)
        target_compile_definitions(ssrecomp PRIVATE SSR_ALL_GAMES)
        # Its HOME MENU ICONS (the settings page of its list).
        target_sources(ssrecomp PRIVATE frontend/switch_forwarder.cpp)
    endif()
    add_custom_target(ssrecomp_nro ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DNACPTOOL=${SSR_NACPTOOL}" "-DELF2NRO=${SSR_ELF2NRO}"
            "-DELF=$<TARGET_FILE:ssrecomp>" "-DOUT=${CMAKE_CURRENT_BINARY_DIR}/ssrecomp.nro"
            "-DNACP=${CMAKE_CURRENT_BINARY_DIR}/ssrecomp.nacp"
            "-DTITLE=${SSR_NRO_TITLE}" "-DVERSION=${SSR_NRO_VERSION}"
            "-DICON=${CMAKE_CURRENT_BINARY_DIR}/icon.jpg" "-DDEFAULT_ICON=${DEVKITPRO}/libnx/default_icon.jpg"
            "-DROMFS=${SSR_SWITCH_ROMFS}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tools/switch/make_nro.cmake"
        DEPENDS ssrecomp
        COMMENT "Making ssrecomp.nro"
        VERBATIM)
else()
    add_executable(ssrecomp ${SSRECOMP_SOURCES})
    target_include_directories(ssrecomp PRIVATE melonDS/src frontend runtime "${SSR_GENERATED_DIR}" ${SDL2_INCLUDE_DIRS})
    find_package(Threads REQUIRED)
    if(TARGET SDL2::SDL2)
        if(TARGET SDL2::SDL2main)
            target_link_libraries(ssrecomp PRIVATE SDL2::SDL2main)
        endif()
        target_link_libraries(ssrecomp PRIVATE SDL2::SDL2)
    else()
        target_link_libraries(ssrecomp PRIVATE ${SDL2_LIBRARIES})
    endif()
    target_link_libraries(ssrecomp PRIVATE core Threads::Threads)
    if(SSR_PNG)
        target_link_libraries(ssrecomp PRIVATE PNG::PNG)
    else()
        target_compile_definitions(ssrecomp PRIVATE SSR_NO_PNG)
    endif()
    if(SSR_GPU3D AND WIN32)
        # (Windows: the context made with WGL, frontend/glcontext_wgl.cpp.)
        target_link_libraries(ssrecomp PRIVATE opengl32 gdi32)
    elseif(SSR_GPU3D)
        target_include_directories(ssrecomp PRIVATE "${SSR_EGL_INCLUDE}")
        target_link_libraries(ssrecomp PRIVATE "${SSR_EGL_LIBRARY}")
    endif()
    # The PC all-games program (tools/pc/build_all.sh): the game list when started without
    # arguments (frontend/pc_launcher.cpp), with the ROMs in the roms folder beside it.
    option(SSR_PC_ALL_GAMES "The PC all-games program with its game list" OFF)
    if(SSR_PC_ALL_GAMES)
        target_sources(ssrecomp PRIVATE frontend/pc_launcher.cpp)
        target_compile_definitions(ssrecomp PRIVATE SSR_PC_LAUNCHER)
    endif()
    if(UNIX AND NOT APPLE AND SSR_PC_ALL_GAMES)
        # The Linux download carries its own SDL2 beside the program (tools/pc/build_all.sh),
        # found there first: $ORIGIN, the program's folder.
        set_target_properties(ssrecomp PROPERTIES BUILD_RPATH "$ORIGIN")
    endif()
    if(WIN32 AND SSR_PC_ALL_GAMES)
        # A window program (no console window behind it; the log goes to game.log), with a
        # manifest that makes the C library's file names UTF-8 (folders named in any letters), and the C++
        # and thread libraries inside it so that only SDL2.dll goes beside it.
        enable_language(RC)
        target_sources(ssrecomp PRIVATE frontend/windows/ssrecomp.rc)
        # The program's icon (Explorer, the taskbar and its windows): SSR_PC_ICON, an .ico of the
        # builder's own (tools/pc/build_all.sh), as resource 1; without one, Windows' plain icon.
        set(SSR_PC_ICON "" CACHE FILEPATH "an .ico for the PC all-games program")
        if(SSR_PC_ICON AND EXISTS "${SSR_PC_ICON}")
            file(TO_CMAKE_PATH "${SSR_PC_ICON}" _ssr_icon)
            file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/icon.rc" "1 ICON \"${_ssr_icon}\"\n")
            target_sources(ssrecomp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/icon.rc")
        endif()
        set_target_properties(ssrecomp PROPERTIES WIN32_EXECUTABLE ON)
        target_link_options(ssrecomp PRIVATE -static)
        # (The game list's desktop shortcuts: frontend/shortcuts.h.)
        target_link_libraries(ssrecomp PRIVATE ole32 shell32 uuid)
    endif()
endif()
# The all-games program's game list: pictures of the games built into it (SSR_PC_ART, a folder of
# the builder's own with IREO.jpg, IRDO.jpg ... named as for the list's icons/ folder; none come
# with the source), shown for a game the player has no picture of.
set(SSR_PC_ART "" CACHE PATH "a folder of pictures for the game list, built into the program")
if((SSR_PC_ALL_GAMES OR SSR_ALL_GAMES) AND SSR_PC_ART AND IS_DIRECTORY "${SSR_PC_ART}")
    set(_art_body "")
    set(_art_table "")
    foreach(code IRBO IRAO IREO IRDO IPKE IPGE CPUE ECRA)
        foreach(ext jpg png)
            if(EXISTS "${SSR_PC_ART}/${code}.${ext}")
                set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${SSR_PC_ART}/${code}.${ext}")
                file(READ "${SSR_PC_ART}/${code}.${ext}" _hex HEX)
                string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
                string(APPEND _art_body "static const unsigned char art_${code}[] = {${_bytes}};\n")
                string(APPEND _art_table "        { \"${code}\", art_${code}, sizeof art_${code} },\n")
                break()
            endif()
        endforeach()
    endforeach()
    # (Written only when it changes, so that configuring again does not rebuild it.)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/pc_art.cpp.new"
        "// The game list's pictures, from ${SSR_PC_ART} (CMakeLists.txt, SSR_PC_ART).\n"
        "#include <cstddef>\n#include <cstring>\n${_art_body}"
        "namespace PcLauncher\n{\nconst unsigned char* BuiltInArt(const char* code, size_t& size)\n{\n"
        "    static const struct { const char* code; const unsigned char* data; size_t size; } art[] = {\n"
        "${_art_table}        { nullptr, nullptr, 0 },\n    };\n"
        "    for (const auto& a : art)\n"
        "        if (a.code && !strcmp(a.code, code))\n        {\n            size = a.size;\n            return a.data;\n        }\n"
        "    return nullptr;\n}\n}\n")
    configure_file("${CMAKE_CURRENT_BINARY_DIR}/pc_art.cpp.new" "${CMAKE_CURRENT_BINARY_DIR}/pc_art.cpp" COPYONLY)
    target_sources(ssrecomp PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/pc_art.cpp")
    target_compile_definitions(ssrecomp PRIVATE SSR_PC_ART)
endif()
# GBA games too, in the all-games program: Pokemon GBA Widescreen built in (mGBA with its wide
# renderer, and the widescreen patches for Emerald, FireRed, LeafGreen and Ultra Violet).
# SSR_GBA_DIR is that project's folder, after its setup.sh has made what it needs (in
# ~/pokemon-gba-wide, or GBAW_WORK); the list gets a GBA section and plays them
# (frontend/gba_games.h, its frontend/embed.h). Empty: DS games only.
set(SSR_GBA_DIR "" CACHE PATH "Pokemon GBA Widescreen's folder, to play GBA games too")
if(SSR_GBA_DIR AND (SSR_PC_ALL_GAMES OR SSR_ALL_GAMES))
    set(GBAW_EMBEDDED ON CACHE BOOL "" FORCE)
    add_subdirectory("${SSR_GBA_DIR}" gbaw EXCLUDE_FROM_ALL)
    target_link_libraries(ssrecomp PRIVATE gbawide)
    target_compile_definitions(ssrecomp PRIVATE SSR_GBA)
endif()
# RetroAchievements: the library, and each platform's way to the internet (achievements_http.cpp);
# the program's version, which the server is told with its name, from CHANGELOG.md's newest release.
target_link_libraries(ssrecomp PRIVATE rcheevos)
if(WIN32)
    target_link_libraries(ssrecomp PRIVATE winhttp)
elseif(APPLE)
    # (NSURLSession, from Objective-C++: built as C++ told it is Objective-C++.)
    target_sources(ssrecomp PRIVATE frontend/achievements_http_apple.mm)
    set_source_files_properties(frontend/achievements_http_apple.mm PROPERTIES LANGUAGE CXX COMPILE_FLAGS "-x objective-c++")
    target_link_libraries(ssrecomp PRIVATE "-framework Foundation")
elseif(SSR_SWITCH)
    # (devkitPro's libcurl, with its TLS library, from the portlibs.)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SSR_SWITCH_CURL REQUIRED IMPORTED_TARGET libcurl)
    target_link_libraries(ssrecomp PRIVATE PkgConfig::SSR_SWITCH_CURL)
elseif(UNIX AND NOT ANDROID)
    target_link_libraries(ssrecomp PRIVATE ${CMAKE_DL_LIBS})
endif()
set(SSR_RA_VERSION "0.0.0")
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md")
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/CHANGELOG.md" _ssr_releases REGEX "^## v[0-9]")
    if(_ssr_releases)
        list(GET _ssr_releases 0 SSR_RA_VERSION)
        string(REGEX REPLACE "^## v([0-9.]+).*" "\\1" SSR_RA_VERSION "${SSR_RA_VERSION}")
    endif()
endif()
target_compile_definitions(ssrecomp PRIVATE SSR_RA_VERSION="${SSR_RA_VERSION}")

if(SSR_GPU3D)
    if(WIN32)
        target_sources(ssrecomp PRIVATE frontend/glcontext_wgl.cpp)
    else()
        target_sources(ssrecomp PRIVATE frontend/glcontext.cpp)
    endif()
    target_compile_definitions(ssrecomp PRIVATE SSR_GPU3D)
endif()
