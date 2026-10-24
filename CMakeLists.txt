cmake_minimum_required(VERSION 3.20)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment version" FORCE)
endif()

# Set the Metal libraries' deployment target to the app's minimum supported
# macOS version. The compiler otherwise uses the active SDK's target, and a
# library built for a newer version will not load on older supported systems.
# scripts/package_macos_dmg.sh validates the target with
# check_metallib_target.py.
if(APPLE)
    set(LIU_METAL_TARGET_FLAG -mmacosx-version-min=${CMAKE_OSX_DEPLOYMENT_TARGET})
endif()

project(liu C)

# Objective-C is only used by the macOS (Cocoa/Metal) sources; enabling it
# unconditionally makes non-Apple toolchains fail to configure.
#
# Hand-written ASM: ARM64 NEON (src/asm/*_arm64.S, Apple Silicon, Mach-O
# leading-underscore symbols) and x86-64 SSE2 (src/asm/*_x86_64.S, System V
# ABI). Both are GAS/AT&T syntax, so they need clang/gcc's integrated
# assembler; non-Apple ARM has no hand-written path and falls back to the
# portable C. CMAKE_SYSTEM_PROCESSOR is populated by project() above.
if(APPLE)
    enable_language(OBJC)
endif()
if((APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64") OR
   (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64"))
    enable_language(ASM)
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# =============================================================================
# Build profiles
#
#   profile          optimisation   symbols   macros    LTO   hardening
#   Debug            -O0            -g        DEBUG     no    none
#   Release          -O3            none      NDEBUG    yes   FORTIFY=2 + stack protector
#   RelWithDebInfo   -O3            -g        NDEBUG    yes   FORTIFY=2 + stack protector
#   MinSizeRel       -Os            none      NDEBUG    no    FORTIFY=2 + stack protector
#
# An unset CMAKE_BUILD_TYPE defaults to Release for single-config generators.
# Multi-config generators select a configuration at build time and use the
# corresponding generator-expression flags shown here. CMake defines NDEBUG
# for non-Debug profiles and DEBUG only for Debug.
# =============================================================================
get_property(LIU_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT LIU_MULTI_CONFIG)
    if(NOT CMAKE_BUILD_TYPE)
        set(CMAKE_BUILD_TYPE Release CACHE STRING
            "Build profile: Debug, Release, RelWithDebInfo or MinSizeRel" FORCE)
        message(STATUS "CMAKE_BUILD_TYPE not set - building Release")
    endif()
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS
                 Debug Release RelWithDebInfo MinSizeRel)
    if(NOT CMAKE_BUILD_TYPE MATCHES "^(Debug|Release|RelWithDebInfo|MinSizeRel)$")
        message(FATAL_ERROR
            "CMAKE_BUILD_TYPE='${CMAKE_BUILD_TYPE}' is not a profile this project "
            "defines flags for. Use Debug, Release, RelWithDebInfo or MinSizeRel "
            "(the names are case-sensitive).")
    endif()
endif()

# C compiler capabilities, asked of the compiler rather than inferred from its
# name (so a compiler that is neither Clang nor GCC gets the same options when
# it takes them):
#   LIU_C_GNU_OPTIONS    GNU-style driver options (-W..., -fvisibility=...)
#   LIU_C_NEON_HEADER    the target's intrinsic header; a compiler without it
#                        has the units that need it built by the platform
#                        compiler, see LIU_C_INTRINSIC_UNITS
include(CheckCCompilerFlag)
check_c_compiler_flag(-fvisibility=hidden LIU_C_GNU_OPTIONS)
set(LIU_C_INTRINSIC_UNITS
    src/voice/whisper_cpu.c)
if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    include(CheckIncludeFile)
    check_include_file(arm_neon.h LIU_C_NEON_HEADER)
    if(NOT LIU_C_NEON_HEADER)
        set_source_files_properties(${LIU_C_INTRINSIC_UNITS} PROPERTIES LANGUAGE OBJC)
    endif()
endif()

# App version shown in Settings > About. Read the canonical version from the
# top-level VERSION file. LIU_BUILD includes the short git hash to identify
# builds; it is empty outside a git checkout.
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" LIU_VERSION LIMIT_COUNT 1)
# Track VERSION as a configure dependency so changes rerun CMake and update the
# version compiled into each binary.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/VERSION")
string(STRIP "${LIU_VERSION}" LIU_VERSION)
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE LIU_BUILD_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
add_compile_definitions(LIU_VERSION="${LIU_VERSION}" LIU_BUILD="${LIU_BUILD_HASH}")

# The auto-update feed defaults to GitHub Releases, where appcast.json
# is attached to each release. Override the URL at configure time with
# -DLIU_UPDATE_FEED_URL=...
if(NOT DEFINED LIU_UPDATE_FEED_URL)
    set(LIU_UPDATE_FEED_URL "https://github.com/calculusteam/liu/releases/latest/download/appcast.json")
endif()
add_compile_definitions(LIU_UPDATE_FEED_URL="${LIU_UPDATE_FEED_URL}")

# Local Translate model download. The ~440 MB GGUF is NOT bundled; it is
# fetched on demand from Settings. The URL is host-agnostic (curl -L follows
# redirects, so a Hugging Face resolve/main URL works); the SHA-256 + size
# gate the finished file. Default is Tencent AngelSlim's official Sherry
# (STQ1_0 / PR #22836) GGUF, in the canonical packing the engine decodes.
# Override at configure time, e.g.
#   -DLIU_MODEL_URL=https://huggingface.co/<user>/<repo>/resolve/main/<file>.gguf
if(NOT DEFINED LIU_MODEL_URL)
    set(LIU_MODEL_URL "https://huggingface.co/AngelSlim/Hy-MT1.5-1.8B-1.25bit-GGUF/resolve/main/Hy-MT1.5-1.8B-1.25bit.gguf")
endif()
if(NOT DEFINED LIU_MODEL_SHA256)
    set(LIU_MODEL_SHA256 "93e025c93cc082e73a3f142b757623a8b9cf541c020a8013ca4ee669556860ab")
endif()
if(NOT DEFINED LIU_MODEL_SIZE)
    set(LIU_MODEL_SIZE "461860704")
endif()
add_compile_definitions(
    LIU_MODEL_URL="${LIU_MODEL_URL}"
    LIU_MODEL_SHA256="${LIU_MODEL_SHA256}"
    LIU_MODEL_SIZE=${LIU_MODEL_SIZE}ULL)

# =============================================================================
# Build options
# =============================================================================
option(USE_ASM "Enable hand-written assembly optimizations" ON)
option(LIU_NATIVE_ARCH "Tune the Release build for the host CPU (-march=native). OFF = a portable, distributable baseline." OFF)
option(USE_VENDORED_SSH_DEPS "Prefer vendored libssh2/OpenSSL build when available" ON)
option(AUTO_DOWNLOAD_SSH_DEPS "Automatically download/build vendored SSH dependencies when missing" ON)
option(LIU_SANITIZE "Build with Address+UB Sanitizer" OFF)
option(LIU_RENDER_REPLAY_PROBE
       "Compile the static-terminal replay and staging measurement probe" OFF)
if(LIU_RENDER_REPLAY_PROBE)
    add_compile_definitions(LIU_RENDER_REPLAY_PROBE=1)
endif()
option(LIU_SLEEP_SNAPSHOT_PROBE
       "Compile the real sleep-snapshot latency matrix driver" OFF)
if(LIU_SLEEP_SNAPSHOT_PROBE)
    add_compile_definitions(LIU_SLEEP_SNAPSHOT_PROBE=1)
endif()
option(LIU_E2E_PROBE "Build the isolated PTY echo-to-presentation driver" OFF)

# Developer hooks include the E2E driver, demo runs, frame and settings dumps,
# LIU_NO_ACTIVATE, and timing probes accessed through liu_dev_env(). Release
# builds omit them because they can send input to panes or write screen
# contents to disk. Debug builds enable them by default; enable them explicitly
# for a Release development build. Release packaging disables and verifies this
# option.
if(CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(_liu_dev_hooks_default ON)
else()
    set(_liu_dev_hooks_default OFF)
endif()
option(LIU_DEV_HOOKS "Compile in the test driver, the demo runs and the dump/probe hooks" ${_liu_dev_hooks_default})
if(LIU_DEV_HOOKS)
    add_compile_definitions(LIU_DEV_HOOKS=1)
endif()
if(LIU_E2E_PROBE)
    if(NOT APPLE)
        message(FATAL_ERROR "LIU_E2E_PROBE requires macOS Metal presentation callbacks")
    endif()
    add_compile_definitions(LIU_E2E_PROBE=1)
endif()

# =============================================================================
# Compiler flags
# =============================================================================
if(LIU_C_GNU_OPTIONS)
    include(CheckCCompilerFlag)

    add_compile_options(
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration
        -fno-exceptions
        -fvisibility=hidden
    )
    if(APPLE)
        add_compile_options(-mmacosx-version-min=11.0)
        add_link_options(-mmacosx-version-min=11.0)
    endif()

    # Apply these flags after CMake's per-configuration CMAKE_C_FLAGS_<CONFIG>
    # so the selected profile determines the effective optimization level.
    # RelWithDebInfo uses the Release optimization level with debug symbols.
    set(LIU_CFG_OPTIMIZED "$<CONFIG:Release,RelWithDebInfo>")
    set(LIU_CFG_HARDENED  "$<CONFIG:Release,RelWithDebInfo,MinSizeRel>")

    add_compile_options(
        "$<$<CONFIG:Debug>:-O0;-g>"
        "$<${LIU_CFG_OPTIMIZED}:-O3;-flto>"
        "$<$<CONFIG:RelWithDebInfo>:-g>"
        "$<$<CONFIG:MinSizeRel>:-Os>")
    add_compile_definitions("$<$<CONFIG:Debug>:DEBUG>")
    add_link_options("$<${LIU_CFG_OPTIMIZED}:-flto>")
    if(APPLE)
        # With LTO the DWARF lives in an object the linker makes and deletes;
        # keeping it is what lets dsymutil / lldb / Instruments find the
        # symbols RelWithDebInfo exists for. One object PER TARGET: given a
        # directory the linker writes `lto.o` into it, and every executable of
        # the build would overwrite the one before.
        add_link_options(
            "$<$<CONFIG:RelWithDebInfo>:-Wl,-object_path_lto,${CMAKE_BINARY_DIR}/lto-objects/$<TARGET_PROPERTY:NAME>.o>")
        file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/lto-objects")
    endif()

    # Hardening (Clang/GCC only — this project targets macOS/Linux).
    # stack-protector-strong is safe at any opt level; FORTIFY_SOURCE needs -O1+,
    # which every profile but Debug has.
    check_c_compiler_flag(-fstack-protector-strong HAS_STACK_PROTECTOR_STRONG)
    if(HAS_STACK_PROTECTOR_STRONG)
        add_compile_options("$<${LIU_CFG_HARDENED}:-fstack-protector-strong>")
    endif()
    # Undef first so Apple SDKs / prior flags that pre-define it don't warn.
    add_compile_options("$<${LIU_CFG_HARDENED}:-U_FORTIFY_SOURCE;-D_FORTIFY_SOURCE=2>")

    # Architecture baseline of the optimised profiles.
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        add_compile_options("$<${LIU_CFG_OPTIMIZED}:-mcpu=apple-m1>")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        add_compile_options("$<${LIU_CFG_OPTIMIZED}:-march=armv8-a+simd>")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        # Distributable default: tune generically but keep the baseline
        # x86-64 ISA so the binary runs on any x86-64 CPU (no host-specific
        # AVX-512/etc. baked in). Self-builders opt into host tuning with
        # -DLIU_NATIVE_ARCH=ON.
        if(LIU_NATIVE_ARCH)
            add_compile_options("$<${LIU_CFG_OPTIMIZED}:-march=native>")
        else()
            add_compile_options("$<${LIU_CFG_OPTIMIZED}:-mtune=generic>")
        endif()
    endif()

    # Optional ASan + UBSan (Debug-friendly; keep OFF for default builds).
    if(LIU_SANITIZE)
        add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,undefined)
        message(STATUS "LIU_SANITIZE=ON: Address+UB Sanitizer enabled")
    endif()
endif()

# =============================================================================
# Platform detection
# =============================================================================
set(PLATFORM_SOURCES "")
set(PLATFORM_LIBS "")

if(APPLE)
    set(PLATFORM_SOURCES
        src/platform/platform_macos.m
        src/platform/clipboard_secret_macos.m
        src/platform/watch_hold.c
        src/platform/option_layer_macos.m)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(CORE_VIDEO_FRAMEWORK CoreVideo REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    find_library(CORETEXT_FRAMEWORK CoreText REQUIRED)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
    find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)  # in-process notify TTS
    find_library(CARBON_FRAMEWORK Carbon REQUIRED)  # UCKeyTranslate: Option-layer probe
    # CoreAudio enumerates input devices and AudioToolbox carries
    # AudioUnitSetProperty, which is how the capture engine is pinned to one.
    find_library(COREAUDIO_FRAMEWORK CoreAudio REQUIRED)
    find_library(AUDIOTOOLBOX_FRAMEWORK AudioToolbox REQUIRED)
    set(PLATFORM_LIBS
        ${COCOA_FRAMEWORK}
        ${OPENGL_FRAMEWORK}
        ${IOKIT_FRAMEWORK}
        ${CORE_VIDEO_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${IOSURFACE_FRAMEWORK}
        ${SECURITY_FRAMEWORK}
        ${CORETEXT_FRAMEWORK}
        ${COREGRAPHICS_FRAMEWORK}
        ${USERNOTIFICATIONS_FRAMEWORK}
        ${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}
        ${AVFOUNDATION_FRAMEWORK}
        ${CARBON_FRAMEWORK}
        ${COREAUDIO_FRAMEWORK}
        ${AUDIOTOOLBOX_FRAMEWORK}
    )
    add_compile_definitions(PLATFORM_MACOS=1 GL_SILENCE_DEPRECATION=1)
elseif(UNIX)
    set(PLATFORM_SOURCES
        src/platform/platform_linux.c
        src/platform/atspi_bridge.c
        src/platform/watch_hold.c)
    find_package(X11 REQUIRED)
    find_package(OpenGL REQUIRED)
    # vault crypto + the updater's signature check call OpenSSL EVP directly, so
    # libcrypto must be on the link line (libssh2's transitive dep isn't enough).
    find_package(OpenSSL REQUIRED)
    set(PLATFORM_LIBS
        ${X11_LIBRARIES}
        OpenGL::GL
        OpenSSL::SSL
        OpenSSL::Crypto
        m
    )
    # XRandR is how a client learns the panel's real refresh rate; without it
    # the vsync-off present pace is stuck at the 60 Hz floor even on a 144 Hz
    # monitor. Optional: find_package(X11) reports it per-component, and the
    # backend falls back to 60 when it is missing.
    if(X11_Xrandr_FOUND)
        list(APPEND PLATFORM_LIBS ${X11_Xrandr_LIB})
        add_compile_definitions(LIU_HAVE_XRANDR=1)
        message(STATUS "XRandR found: per-monitor refresh rate enabled")
    else()
        message(STATUS "XRandR not found: refresh rate pinned to 60 Hz")
    endif()
    # XInput2 carries the scroll *valuators* a touchpad reports. Core X11 only
    # has buttons 4-7, i.e. whole wheel notches, so without it two-finger
    # scrolling arrives quantised into notches instead of pixels. Optional: the
    # backend keeps the core button path as the fallback.
    if(X11_Xi_FOUND)
        list(APPEND PLATFORM_LIBS ${X11_Xi_LIB})
        add_compile_definitions(LIU_HAVE_XINPUT2=1)
        message(STATUS "XInput2 found: smooth/trackpad scrolling enabled")
    else()
        message(STATUS "XInput2 not found: scrolling limited to wheel notches")
    endif()
    # Fontconfig answers "which installed font covers this codepoint" — the
    # thing CoreText does for the macOS build. Without it the fallback chain is
    # the fixed basename list in font.c, so any script outside that list renders
    # as tofu even when the user has a font for it. Optional: the basename scan
    # stays as the fallback path.
    # IMPORTED_TARGET, not the bare *_LIBRARIES lists: the target carries the
    # include dirs and the link line together, and it only exists when
    # pkg-config really resolved the module. Keying the compile definition off
    # the plain FONTCONFIG_FOUND variable would let a half-successful probe
    # define LIU_HAVE_FONTCONFIG while contributing no library to link against.
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FONTCONFIG QUIET IMPORTED_TARGET fontconfig)
    endif()
    if(TARGET PkgConfig::FONTCONFIG)
        list(APPEND PLATFORM_LIBS PkgConfig::FONTCONFIG)
        add_compile_definitions(LIU_HAVE_FONTCONFIG=1)
        message(STATUS "Fontconfig found: per-codepoint font discovery enabled")
    else()
        message(STATUS "Fontconfig not found: fallback fonts limited to the built-in list")
    endif()
    # FreeType rasterises the glyphs. stb_truetype (the fallback) has no
    # hinting at all: at a 16 px body size a vertical stem lands wherever the
    # outline puts it, so it comes out as two half-lit columns instead of one
    # solid one, and the whole face reads soft. FreeType is also where the
    # user's fontconfig hint/antialias settings take effect. Optional in the
    # same sense as the rest of this block: without it the stb path still
    # renders every glyph, just less sharply.
    if(PkgConfig_FOUND)
        pkg_check_modules(FREETYPE QUIET IMPORTED_TARGET freetype2)
    endif()
    if(TARGET PkgConfig::FREETYPE)
        list(APPEND PLATFORM_LIBS PkgConfig::FREETYPE)
        add_compile_definitions(LIU_HAVE_FREETYPE=1)
        message(STATUS "FreeType found: hinted glyph rasterisation enabled")
        option(LIU_HARFBUZZ "Use system HarfBuzz for programming ligatures" ON)
        if(LIU_HARFBUZZ AND PkgConfig_FOUND)
            pkg_check_modules(HARFBUZZ QUIET IMPORTED_TARGET harfbuzz)
        endif()
        if(LIU_HARFBUZZ AND TARGET PkgConfig::HARFBUZZ)
            list(APPEND PLATFORM_LIBS PkgConfig::HARFBUZZ)
            add_compile_definitions(LIU_HAVE_HARFBUZZ=1)
            message(STATUS "HarfBuzz found: programming ligature shaping enabled")
        endif()
    else()
        message(STATUS "FreeType not found: falling back to unhinted stb_truetype raster")
    endif()
    # The taskbar count badge (macOS: the dock badge) is a session-bus signal,
    # com.canonical.Unity.LauncherEntry — understood by Plasma's task manager
    # and by GNOME with Dash-to-Dock. Optional: without it the badge is simply
    # never shown, exactly as before. The same library carries the AT-SPI2
    # bridge (src/platform/atspi_bridge.c), which lets a screen reader read the
    # focused pane; without D-Bus there is no accessibility bus to join.
    if(PkgConfig_FOUND)
        pkg_check_modules(DBUS QUIET IMPORTED_TARGET dbus-1)
    endif()
    if(TARGET PkgConfig::DBUS)
        list(APPEND PLATFORM_LIBS PkgConfig::DBUS)
        add_compile_definitions(LIU_HAVE_DBUS=1)
        message(STATUS "D-Bus found: taskbar badge and screen-reader bridge enabled")
    else()
        message(STATUS "D-Bus not found: taskbar badge and screen-reader bridge disabled")
    endif()
    add_compile_definitions(PLATFORM_LINUX=1)
    # glibc gates POSIX/GNU extensions (struct ucred, posix_openpt/grantpt/
    # ptsname, strdup, Xutf8*, …) behind _GNU_SOURCE. Define it build-wide so
    # every TU sees them before the first system header is included.
    add_compile_definitions(_GNU_SOURCE)
endif()

# =============================================================================
# Metal renderer (macOS, default ON)
# =============================================================================
option(USE_METAL "Use Metal renderer instead of OpenGL (macOS only)" ON)
if(LIU_E2E_PROBE AND NOT USE_METAL)
    message(FATAL_ERROR "LIU_E2E_PROBE requires Metal presentation callbacks")
endif()

# In-app browser (WKWebView) — macOS default ON
option(LIU_BROWSER "Enable in-app WKWebView browser (macOS)" ON)

# Animation pacing probe — a diagnostic build that drives a modal open/closed on
# a timer with no user input and prints every present-to-present interval while
# an animation is in flight. Off by default; the code is compiled out entirely.
#   cmake -B build -DLIU_ANIM_PROBE=ON && cmake --build build
#   LIU_ANIM_PROBE=1 ./build/Liu
option(LIU_ANIM_PROBE "Build the animation frame-pacing probe" OFF)
if(LIU_ANIM_PROBE)
    add_compile_definitions(LIU_ANIM_PROBE=1)
endif()

if(USE_METAL AND APPLE)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    add_compile_definitions(USE_METAL=1)

    # Recent Xcode versions install the Metal compiler as a separate component.
    # A device/framework alone is not a working shader toolchain. Diagnose it
    # up front without downloading components or silently changing renderers;
    # independent CPU test targets must still be configurable on a headless host.
    execute_process(COMMAND xcrun -sdk macosx metal --version
        RESULT_VARIABLE LIU_METAL_TOOL_STATUS
        OUTPUT_QUIET ERROR_VARIABLE LIU_METAL_TOOL_DIAGNOSTIC TIMEOUT 20)
    execute_process(COMMAND xcrun -sdk macosx --find metallib
        RESULT_VARIABLE LIU_METALLIB_TOOL_STATUS
        OUTPUT_QUIET ERROR_VARIABLE LIU_METALLIB_TOOL_DIAGNOSTIC TIMEOUT 20)
    if(NOT LIU_METAL_TOOL_STATUS STREQUAL "0" OR NOT LIU_METALLIB_TOOL_STATUS STREQUAL "0")
        message(WARNING "Metal shader toolchain is unavailable:\n"
            "${LIU_METAL_TOOL_DIAGNOSTIC}\n${LIU_METALLIB_TOOL_DIAGNOSTIC}\n"
            "Install the selected Xcode's component with:\n"
            "  xcodebuild -downloadComponent MetalToolchain\n"
            "Metal application/shader builds cannot pass until it is installed. "
            "For CPU-only test work, explicitly configure a separate tree with -DUSE_METAL=OFF; "
            "that does not validate the Metal renderer.")
    endif()

    # Compile Metal shaders to metallib
    set(METAL_SHADER ${CMAKE_SOURCE_DIR}/src/swift/LiuShaders.metal)
    set(METAL_AIR ${CMAKE_BINARY_DIR}/LiuShaders.air)
    set(METALLIB ${CMAKE_BINARY_DIR}/LiuShaders.metallib)
    add_custom_command(
        OUTPUT ${METALLIB}
        COMMAND xcrun -sdk macosx metal ${LIU_METAL_TARGET_FLAG} -fmodules-cache-path=${CMAKE_BINARY_DIR}/metal-module-cache -c ${METAL_SHADER} -o ${METAL_AIR}
        COMMAND xcrun -sdk macosx metallib ${METAL_AIR} -o ${METALLIB}
        DEPENDS ${METAL_SHADER}
        COMMENT "Compiling Metal shaders"
    )
    add_custom_target(metal_shaders DEPENDS ${METALLIB})

    # Add Metal frameworks, remove OpenGL
    list(APPEND PLATFORM_LIBS ${METAL_FRAMEWORK} ${QUARTZCORE_FRAMEWORK})
    list(REMOVE_ITEM PLATFORM_LIBS ${OPENGL_FRAMEWORK})
endif()

# =============================================================================
# Third-party: SQLite3 (amalgamation)
# =============================================================================
add_library(sqlite3 STATIC third_party/sqlite3/sqlite3.c)
target_include_directories(sqlite3 PUBLIC third_party/sqlite3)
# Trimmed SQLite build: retain the history scanner's progress callback so
# cancellation can interrupt a long query, as well as the vault's features.
# Saves ~250 KB of compiled code + a few KB of static data segments, and cuts
# first-open bytecode processing slightly. Leave core WAL/FTS-free features.
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_DQS=0
    SQLITE_THREADSAFE=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_LIKE_DOESNT_MATCH_BLOBS
    SQLITE_OMIT_DEPRECATED
    SQLITE_OMIT_SHARED_CACHE
    SQLITE_OMIT_LOAD_EXTENSION
    SQLITE_OMIT_AUTOINIT
    SQLITE_OMIT_TRACE
    SQLITE_OMIT_DECLTYPE
    SQLITE_OMIT_AUTHORIZATION
    SQLITE_OMIT_COMPLETE
    SQLITE_OMIT_GET_TABLE
    SQLITE_OMIT_TCL_VARIABLE
    SQLITE_OMIT_UTF16
    SQLITE_USE_ALLOCA
)
if(LIU_C_GNU_OPTIONS)
    target_compile_options(sqlite3 PRIVATE -w)
endif()

# =============================================================================
# Third-party: libssh2
# =============================================================================
find_package(PkgConfig QUIET)
set(VENDORED_SSH_ROOT ${CMAKE_SOURCE_DIR}/third_party/macos-deps/install)
set(LIBSSH2_BOOTSTRAP_TARGET "")
# An install directory is only trusted when it was built by THIS recipe: the
# script stamps it with sha256(sha256(script) || sha256(patch list)) (see
# scripts/build_macos_compat_deps.sh for why). A tree built before the libssh2
# security patches existed has no stamp and must not be linked into a release.
set(VENDORED_SSH_CURRENT FALSE)
if(APPLE AND USE_VENDORED_SSH_DEPS AND
   EXISTS "${VENDORED_SSH_ROOT}/lib/libssh2.a" AND
   EXISTS "${VENDORED_SSH_ROOT}/lib/libssl.a" AND
   EXISTS "${VENDORED_SSH_ROOT}/lib/libcrypto.a" AND
   EXISTS "${VENDORED_SSH_ROOT}/.liu-deps-stamp")
    file(SHA256 "${CMAKE_SOURCE_DIR}/scripts/build_macos_compat_deps.sh" _ssh_deps_script_hash)
    file(SHA256 "${CMAKE_SOURCE_DIR}/scripts/patches/libssh2/SHA256SUMS" _ssh_deps_patch_hash)
    string(SHA256 _ssh_deps_expected "${_ssh_deps_script_hash}${_ssh_deps_patch_hash}")
    file(READ "${VENDORED_SSH_ROOT}/.liu-deps-stamp" _ssh_deps_actual)
    string(STRIP "${_ssh_deps_actual}" _ssh_deps_actual)
    if(_ssh_deps_actual STREQUAL _ssh_deps_expected)
        set(VENDORED_SSH_CURRENT TRUE)
    else()
        message(STATUS "Vendored libssh2/OpenSSL in ${VENDORED_SSH_ROOT} was built by an older recipe (stamp mismatch)")
    endif()
elseif(APPLE AND USE_VENDORED_SSH_DEPS AND EXISTS "${VENDORED_SSH_ROOT}/lib/libssh2.a")
    message(STATUS "Vendored libssh2/OpenSSL in ${VENDORED_SSH_ROOT} has no .liu-deps-stamp (built before the libssh2 security patches) - it will be rebuilt")
endif()
if(VENDORED_SSH_CURRENT)
    set(LIBSSH2_FOUND TRUE)
    set(LIBSSH2_INCLUDE_DIRS "${VENDORED_SSH_ROOT}/include")
    set(LIBSSH2_LIBRARIES
        "${VENDORED_SSH_ROOT}/lib/libssh2.a"
        "${VENDORED_SSH_ROOT}/lib/libssl.a"
        "${VENDORED_SSH_ROOT}/lib/libcrypto.a"
        z
    )
elseif(APPLE AND USE_VENDORED_SSH_DEPS AND AUTO_DOWNLOAD_SSH_DEPS)
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(SSH_DEPS_ARCH "arm64")
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        set(SSH_DEPS_ARCH "x86_64")
    else()
        set(SSH_DEPS_ARCH "${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    set(LIBSSH2_BOOTSTRAP_STAMP ${CMAKE_BINARY_DIR}/libssh2-bootstrap.stamp)
    add_custom_command(
        OUTPUT ${LIBSSH2_BOOTSTRAP_STAMP}
        COMMAND ${CMAKE_COMMAND} -E env
            ARCH=${SSH_DEPS_ARCH}
            MACOS_MIN=${CMAKE_OSX_DEPLOYMENT_TARGET}
            "${CMAKE_SOURCE_DIR}/scripts/build_macos_compat_deps.sh"
        COMMAND ${CMAKE_COMMAND} -E touch ${LIBSSH2_BOOTSTRAP_STAMP}
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
        DEPENDS ${CMAKE_SOURCE_DIR}/scripts/build_macos_compat_deps.sh
                ${CMAKE_SOURCE_DIR}/scripts/patches/libssh2/SHA256SUMS
        COMMENT "Downloading and building vendored libssh2/OpenSSL"
        VERBATIM
    )
    add_custom_target(libssh2_bootstrap DEPENDS ${LIBSSH2_BOOTSTRAP_STAMP})
    set(LIBSSH2_BOOTSTRAP_TARGET libssh2_bootstrap)

    set(LIBSSH2_FOUND TRUE)
    set(LIBSSH2_INCLUDE_DIRS "${VENDORED_SSH_ROOT}/include")
    set(LIBSSH2_LIBRARIES
        "${VENDORED_SSH_ROOT}/lib/libssh2.a"
        "${VENDORED_SSH_ROOT}/lib/libssl.a"
        "${VENDORED_SSH_ROOT}/lib/libcrypto.a"
        z
    )
elseif(APPLE AND USE_VENDORED_SSH_DEPS)
    message(FATAL_ERROR
        "Vendored libssh2/OpenSSL deps are missing or were built by an older recipe (no matching .liu-deps-stamp). Enable AUTO_DOWNLOAD_SSH_DEPS or run scripts/build_macos_compat_deps.sh")
else()
    # ---------------------------------------------------------------------
    # If libssh2 is installed as the CMake config package's imported target
    # `Libssh2::libssh2` (it carries its own include dirs + transitive
    # crypto/zlib link deps), use it directly and skip the raw-library
    # plumbing below.
    # ---------------------------------------------------------------------
    if(TARGET Libssh2::libssh2)
        set(LIBSSH2_FOUND TRUE)
        set(LIBSSH2_LIBRARIES Libssh2::libssh2)
        # The imported target supplies INTERFACE_INCLUDE_DIRECTORIES, so leave
        # LIBSSH2_INCLUDE_DIRS empty (target_link_libraries propagates it).
        set(LIBSSH2_INCLUDE_DIRS "")
    else()
        if(PKG_CONFIG_FOUND)
            pkg_check_modules(LIBSSH2 libssh2)
        endif()
        if(NOT LIBSSH2_FOUND)
            find_path(LIBSSH2_INCLUDE_DIRS libssh2.h)
            find_library(LIBSSH2_LIBRARIES ssh2 NAMES ssh2 libssh2)
        endif()

        if(NOT LIBSSH2_LIBRARIES)
            message(FATAL_ERROR "libssh2 not found. Install vendored deps with scripts/build_macos_compat_deps.sh, or install system libssh2")
        endif()

        # Ensure linker can find libssh2
        if(LIBSSH2_LIBRARY_DIRS)
            link_directories(${LIBSSH2_LIBRARY_DIRS})
        endif()
    endif()
endif()

# =============================================================================
# ASM sources — hand-written SIMD for the hot paths:
#   ARM64 NEON  (*_arm64.S)   on Apple Silicon (Mach-O symbol conventions)
#   x86-64 SSE2 (*_x86_64.S)  on any x86_64 target with a GAS-capable compiler
#                             (clang/gcc; System V ABI)
# Non-Apple ARM has no hand-written path, so it builds the portable C fallbacks.
# =============================================================================
set(ASM_SOURCES "")
if(USE_ASM)
    if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
        set(ASM_SOURCES
            src/asm/memops_arm64.S
            src/asm/utf8_arm64.S
            src/asm/terminal_arm64.S
            src/asm/render_arm64.S
        )
    elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
        set(ASM_SOURCES
            src/asm/memops_x86_64.S
            src/asm/utf8_x86_64.S
            src/asm/terminal_x86_64.S
        )
    else()
        message(STATUS "USE_ASM=ON but no hand-written ASM for this target (non-Apple ARM): using C implementations")
    endif()
    if(ASM_SOURCES)
        add_compile_definitions(USE_ASM=1)
    endif()
endif()

# =============================================================================
# Main executable
# =============================================================================
# =============================================================================
# Agent-icon embedding — turn PNG logos into compiled-in byte arrays so the
# palette can show real Claude / Codex / Copilot artwork without runtime file
# lookups. Missing PNGs are silently omitted (module falls back to a letter
# badge for any tool whose icon isn't shipped).
# =============================================================================
# ---------------------------------------------------------------------------
# SVG icon sets — every assets/icons/<dir>/*.svg is embedded into one
# generated header as NUL-terminated source text plus a name->text lookup.
# Two sets: Lucide (the chrome's icons, ui/lucide.c) and Seti UI (the file
# browser's file icons, ui/seti_icons.c).
#
# Globbed on purpose: adding an icon means dropping in the .svg and naming it
# in icon.c's table. There is no third place to forget (the PNG pipeline below
# needs a line here AND a line in the .h.in, which is exactly the kind of
# coupling that goes stale). CONFIGURE_DEPENDS re-runs the glob when the
# directory changes, so a new or edited SVG can't build as stale bytes.
# ---------------------------------------------------------------------------
function(_generate_svg_header out_header dir prefix type)
    file(GLOB _svgs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/assets/icons/${dir}/*.svg")
    list(SORT _svgs)
    set(_defs "")
    set(_rows "")
    foreach(_svg IN LISTS _svgs)
        get_filename_component(_name "${_svg}" NAME_WE)
        string(MAKE_C_IDENTIFIER "${_name}" _ident)
        string(TOUPPER "${_ident}" _ident)
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_svg}")
        file(READ "${_svg}" _hex HEX)
        string(LENGTH "${_hex}" _hexlen)
        math(EXPR _bytelen "${_hexlen} / 2")
        string(REGEX REPLACE "(..)" "0x\\1," _bytes "${_hex}")
        # Trailing 0x00: nanosvg parses a C string, and nsvgParse() mutates its
        # input, so icon.c copies this before handing it over.
        string(APPEND _defs
            "static const char ${prefix}_SVG_${_ident}[] = { ${_bytes} 0x00 };\n")
        string(APPEND _rows
            "    { \"${_name}\", ${prefix}_SVG_${_ident}, ${_bytelen} },\n")
    endforeach()
    list(LENGTH _svgs _count)
    file(WRITE "${out_header}"
"/* Auto-generated by CMake from assets/icons/${dir}/. Do not edit. */
#ifndef UI_${prefix}_DATA_H
#define UI_${prefix}_DATA_H

#include \"core/types.h\"

${_defs}
typedef struct { const char *name; const char *svg; i32 size; } ${type};

static const ${type} ${prefix}_SVGS[] = {
${_rows}};
#define ${prefix}_SVG_COUNT ${_count}

#endif /* UI_${prefix}_DATA_H */
")
    message(STATUS "SVG icons: embedded ${_count} from assets/icons/${dir}")
endfunction()

_generate_svg_header("${CMAKE_BINARY_DIR}/generated/ui/lucide_data.h" lucide LUCIDE LucideSvg)
_generate_svg_header("${CMAKE_BINARY_DIR}/generated/ui/seti_data.h"   seti   SETI   SetiSvg)

# Embed a text asset as a NUL-terminated byte array. Same configure-time hexing
# as _embed_png_array, plus the trailing 0x00 that a C string parser (nanosvg)
# requires — without it nsvgParse walks off the end of the array.
function(_embed_text_asset var_prefix path)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
    if(EXISTS "${path}")
        file(READ "${path}" _hex HEX)
        string(LENGTH "${_hex}" _hexlen)
        math(EXPR _bytelen "${_hexlen} / 2")
        string(REGEX REPLACE "(..)" "0x\\1," _hex "${_hex}")
        set(${var_prefix}_DEF
            "static const char ${var_prefix}_data[] = { ${_hex} 0x00 };\nstatic const i32 ${var_prefix}_size = ${_bytelen};"
            PARENT_SCOPE)
    else()
        set(${var_prefix}_DEF
            "static const char *const ${var_prefix}_data = 0;\nstatic const i32 ${var_prefix}_size = 0;"
            PARENT_SCOPE)
    endif()
endfunction()

function(_embed_png_array var_prefix pngpath)
    # Icon bytes are embedded at configure time. Add the PNG as a configure
    # dependency so artwork changes trigger reconfiguration and update the
    # embedded data.
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${pngpath}")
    if(EXISTS "${pngpath}")
        file(READ "${pngpath}" _hex HEX)
        string(LENGTH "${_hex}" _hexlen)
        math(EXPR _bytelen "${_hexlen} / 2")
        string(REGEX REPLACE "(..)" "0x\\1," _hex "${_hex}")
        set(${var_prefix}_DEF
            "static const u8 ${var_prefix}_data[] = { ${_hex} };\nstatic const i32 ${var_prefix}_size = ${_bytelen};"
            PARENT_SCOPE)
    else()
        set(${var_prefix}_DEF
            "static const u8 *const ${var_prefix}_data = 0;\nstatic const i32 ${var_prefix}_size = 0;"
            PARENT_SCOPE)
    endif()
endfunction()

_embed_png_array(AGENT_ICON_CLAUDE      "${CMAKE_SOURCE_DIR}/assets/agents/claude.png")
_embed_png_array(AGENT_ICON_CODEX       "${CMAKE_SOURCE_DIR}/assets/agents/codex.png")
# Codex, Droid and xAI ship a BLACK badge, which is the light-surface cut —
# the dark one is derived. scripts/gen_agent_onlight_icons.py.
_embed_png_array(AGENT_ICON_CODEX_ONDARK "${CMAKE_SOURCE_DIR}/assets/agents/codex-on-dark.png")
_embed_png_array(AGENT_ICON_COPILOT     "${CMAKE_SOURCE_DIR}/assets/agents/copilot.png")
_embed_png_array(AGENT_ICON_CURSOR      "${CMAKE_SOURCE_DIR}/assets/agents/cursor.png")
_embed_png_array(AGENT_ICON_AMP         "${CMAKE_SOURCE_DIR}/assets/agents/amp.png")
_embed_png_array(AGENT_ICON_CLINE       "${CMAKE_SOURCE_DIR}/assets/agents/cline.png")
_embed_png_array(AGENT_ICON_ROO         "${CMAKE_SOURCE_DIR}/assets/agents/roo.png")
_embed_png_array(AGENT_ICON_KILO        "${CMAKE_SOURCE_DIR}/assets/agents/kilo.png")
_embed_png_array(AGENT_ICON_KIRO        "${CMAKE_SOURCE_DIR}/assets/agents/kiro.png")
_embed_png_array(AGENT_ICON_CRUSH       "${CMAKE_SOURCE_DIR}/assets/agents/crush.png")
_embed_png_array(AGENT_ICON_OPENCODE    "${CMAKE_SOURCE_DIR}/assets/agents/opencode.png")
# Second artwork for the same tool: OpenCode's mark is monochrome, so one file
# cannot survive both a near-white icon tile and the dark tab strip. `-on-light`
# is the dark-ink cut, picked at draw time by background luminance.
_embed_png_array(AGENT_ICON_OPENCODE_ONLIGHT "${CMAKE_SOURCE_DIR}/assets/agents/opencode-on-light.png")
_embed_png_array(AGENT_ICON_DROID       "${CMAKE_SOURCE_DIR}/assets/agents/droid.png")
_embed_png_array(AGENT_ICON_DROID_ONDARK "${CMAKE_SOURCE_DIR}/assets/agents/droid-on-dark.png")
_embed_png_array(AGENT_ICON_ANTIGRAVITY "${CMAKE_SOURCE_DIR}/assets/agents/antigravity.png")
_embed_png_array(AGENT_ICON_KIMI        "${CMAKE_SOURCE_DIR}/assets/agents/kimi.png")
# Kimi and Continue publish one cut each and it is the wrong one for a light
# panel, so theirs are derived — scripts/gen_agent_onlight_icons.py.
_embed_png_array(AGENT_ICON_KIMI_ONLIGHT "${CMAKE_SOURCE_DIR}/assets/agents/kimi-on-light.png")
_embed_png_array(AGENT_ICON_QWEN        "${CMAKE_SOURCE_DIR}/assets/agents/qwen.png")
_embed_png_array(AGENT_ICON_AIDER       "${CMAKE_SOURCE_DIR}/assets/agents/aider.png")
_embed_png_array(AGENT_ICON_AMAZONQ     "${CMAKE_SOURCE_DIR}/assets/agents/amazon-q.png")
_embed_png_array(AGENT_ICON_CONTINUE    "${CMAKE_SOURCE_DIR}/assets/agents/continue.png")
_embed_png_array(AGENT_ICON_CONTINUE_ONLIGHT "${CMAKE_SOURCE_DIR}/assets/agents/continue-on-light.png")
_embed_png_array(AGENT_ICON_WINDSURF    "${CMAKE_SOURCE_DIR}/assets/agents/windsurf.png")
_embed_png_array(AGENT_ICON_COMMANDCODE "${CMAKE_SOURCE_DIR}/assets/agents/commandcode.png")
# Command Code ships a monochrome mark, so it needs the same two cuts OpenCode
# does: the white one for dark surfaces, the dark one for a light tile.
_embed_png_array(AGENT_ICON_COMMANDCODE_ONLIGHT "${CMAKE_SOURCE_DIR}/assets/agents/commandcode-on-light.png")
_embed_png_array(AGENT_ICON_XAI         "${CMAKE_SOURCE_DIR}/assets/agents/xai.png")
_embed_png_array(AGENT_ICON_XAI_ONDARK  "${CMAKE_SOURCE_DIR}/assets/agents/xai-on-dark.png")
_embed_png_array(AGENT_ICON_PI          "${CMAKE_SOURCE_DIR}/assets/agents/pi.png")
_embed_png_array(AGENT_ICON_PI_ONDARK   "${CMAKE_SOURCE_DIR}/assets/agents/pi-on-dark.png")
_embed_png_array(AGENT_ICON_OMP         "${CMAKE_SOURCE_DIR}/assets/agents/omp.png")
_embed_png_array(AGENT_ICON_OMP_ONDARK  "${CMAKE_SOURCE_DIR}/assets/agents/omp-on-dark.png")
_embed_png_array(AGENT_ICON_OPENCLAW    "${CMAKE_SOURCE_DIR}/assets/agents/openclaw.png")
_embed_png_array(AGENT_ICON_HERMES      "${CMAKE_SOURCE_DIR}/assets/agents/hermes.png")
_embed_png_array(AGENT_ICON_HERMES_ONDARK "${CMAKE_SOURCE_DIR}/assets/agents/hermes-on-dark.png")
_embed_png_array(LIU_SLEEP_ICON         "${CMAKE_SOURCE_DIR}/assets/liu-sleep.png")
# Sleeping-tab mascot as SVG SOURCE, not a raster: agent_icons.c rewrites the
# three fill colours to the active theme before rasterising, so the card's
# artwork follows the palette instead of staying purple on every theme. The
# .png above is the fallback for a build whose SVG is missing.
_embed_text_asset(LIU_SLEEP_SVG         "${CMAKE_SOURCE_DIR}/assets/liu-sleep.svg")
# The app mark itself. Compiled in rather than read from Contents/assets so a
# dev run straight out of build/ shows it too.
#
# This is a 256² DERIVED copy of assets/appicon.png (the 1024² master the Dock
# icon in packaging/macos/ is compiled from) — regenerate with:
#     scripts/gen_app_icon.py
# The master is 191 KB of hex in the binary for a mark drawn at ~104 px; 256²
# costs 23 KB and is still 2.5x the largest size it is asked for, which is why
# this is a separate, smaller file. liu_app_icon_rgba falls back to drawing the
# embedded bitmap upscaled if a display asks for more than it holds.
_embed_png_array(LIU_APP_ICON           "${CMAKE_SOURCE_DIR}/assets/appicon-256.png")
_embed_png_array(FB_ICON_FOLDER         "${CMAKE_SOURCE_DIR}/assets/filebrowser/folder.png")
_embed_png_array(FB_ICON_FILE           "${CMAKE_SOURCE_DIR}/assets/filebrowser/file.png")
_embed_png_array(FB_ICON_CODE           "${CMAKE_SOURCE_DIR}/assets/filebrowser/code.png")
_embed_png_array(FB_ICON_TEXT           "${CMAKE_SOURCE_DIR}/assets/filebrowser/text.png")
_embed_png_array(FB_ICON_IMAGE          "${CMAKE_SOURCE_DIR}/assets/filebrowser/image.png")
_embed_png_array(FB_ICON_VIDEO          "${CMAKE_SOURCE_DIR}/assets/filebrowser/video.png")
_embed_png_array(FB_ICON_AUDIO          "${CMAKE_SOURCE_DIR}/assets/filebrowser/audio.png")
_embed_png_array(FB_ICON_ARCHIVE        "${CMAKE_SOURCE_DIR}/assets/filebrowser/archive.png")
_embed_png_array(FB_ICON_PDF            "${CMAKE_SOURCE_DIR}/assets/filebrowser/pdf.png")
_embed_png_array(FB_ICON_TERMINAL       "${CMAKE_SOURCE_DIR}/assets/filebrowser/terminal.png")
_embed_png_array(FB_ICON_GEAR           "${CMAKE_SOURCE_DIR}/assets/filebrowser/gear.png")
_embed_png_array(FB_ICON_GIT            "${CMAKE_SOURCE_DIR}/assets/filebrowser/git.png")
_embed_png_array(FB_ICON_LOCK           "${CMAKE_SOURCE_DIR}/assets/filebrowser/lock.png")
_embed_png_array(FB_ICON_UP             "${CMAKE_SOURCE_DIR}/assets/filebrowser/up.png")
_embed_png_array(FB_ICON_REFRESH        "${CMAKE_SOURCE_DIR}/assets/filebrowser/refresh.png")
_embed_png_array(FB_ICON_CLOSE          "${CMAKE_SOURCE_DIR}/assets/filebrowser/close.png")
_embed_png_array(FB_ICON_LANG_PYTHON     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/python.png")
_embed_png_array(FB_ICON_LANG_ERLANG     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/erlang.png")
_embed_png_array(FB_ICON_LANG_FORTRAN    "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/fortran.png")
_embed_png_array(FB_ICON_LANG_BUN        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/bun.png")
_embed_png_array(FB_ICON_LANG_VB         "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/vb.png")
_embed_png_array(FB_ICON_LANG_DELPHI     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/delphi.png")
_embed_png_array(FB_ICON_LANG_MATLAB     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/matlab.png")
_embed_png_array(FB_ICON_LANG_ASTRO      "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/astro.png")
_embed_png_array(FB_ICON_LANG_NIX        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/nix.png")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/ui/agent_icons_data.h.in"
    "${CMAKE_BINARY_DIR}/generated/ui/agent_icons_data.h"
    @ONLY)
configure_file(
    "${CMAKE_SOURCE_DIR}/src/ui/fb_asset_icons_data.h.in"
    "${CMAKE_BINARY_DIR}/generated/ui/fb_asset_icons_data.h"
    @ONLY)

# Embed the theming guide so the Create-Theme agent always has it, regardless
# of install layout (the on-disk THEMING.md is only a dev-time override). The
# byte-array macro is content-agnostic; the reader NUL-terminates a copy.
_embed_png_array(THEMING_GUIDE "${CMAKE_SOURCE_DIR}/THEMING.md")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/theming_guide_data.h.in"
    "${CMAKE_BINARY_DIR}/generated/theming_guide_data.h"
    @ONLY)

# First-run intro (macOS only, src/intro/): the mark's three coverage masks,
# the wordmark, and the shader's SOURCE — it is compiled at run time, so the
# intro adds no metallib step and no dependency on the Metal toolchain. The two
# PNGs are derived (scripts/gen_intro_assets.py, --check re-verifies them).
_embed_png_array(INTRO_BUNNY_LAYERS "${CMAKE_SOURCE_DIR}/assets/intro/bunny-layers.png")
# One caption atlas per intro language. The list is the generated header's
# (INTRO_LANG_CODES), so the table below is in the order intro_lang.c
# indexes it by.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/src/intro/intro_geom.h")
file(STRINGS "${CMAKE_SOURCE_DIR}/src/intro/intro_geom.h" _intro_lang_line
    REGEX "#define INTRO_LANG_CODES")
string(REGEX MATCHALL "\"[a-z]+\"" _intro_lang_quoted "${_intro_lang_line}")
set(INTRO_CAPTIONS_ALL_DEF "")
set(INTRO_CAPTIONS_TABLE "")
foreach(_quoted ${_intro_lang_quoted})
    string(REPLACE "\"" "" _lang "${_quoted}")
    string(TOUPPER "${_lang}" _LANG)
    _embed_png_array(INTRO_CAPTIONS_${_LANG}
        "${CMAKE_SOURCE_DIR}/assets/intro/captions-${_lang}.png")
    string(APPEND INTRO_CAPTIONS_ALL_DEF "${INTRO_CAPTIONS_${_LANG}_DEF}\n")
    string(APPEND INTRO_CAPTIONS_TABLE
        "    { INTRO_CAPTIONS_${_LANG}_data, INTRO_CAPTIONS_${_LANG}_size },\n")
endforeach()
_embed_text_asset(INTRO_SHADER      "${CMAKE_SOURCE_DIR}/src/intro/intro_shader.metal")
configure_file(
    "${CMAKE_SOURCE_DIR}/src/intro/intro_assets.h.in"
    "${CMAKE_BINARY_DIR}/generated/intro/intro_assets.h"
    @ONLY)

set(FB_ICON_SOURCE "")
if(EXISTS "${CMAKE_SOURCE_DIR}/src/ui/fb_icon.c" AND
   EXISTS "${CMAKE_SOURCE_DIR}/src/ui/fb_icon.h")
    set(FB_ICON_SOURCE src/ui/fb_icon.c)
else()
    message(STATUS "File-browser icon source missing: building without Nerd Font icons")
endif()

# =============================================================================
# Cross-agent session continuation (src/history/bridge)
#
# Shared verbatim by Liu and liu-history, so the CLI can run a migration
# headlessly and the tests can link the same objects the app does. Adding an
# agent means one source parser and/or one target adapter here — never a
# change in bridge.c, and never one in the UI.
# =============================================================================
set(BRIDGE_SOURCES
    # Foundation
    src/history/bridge/ir.c
    src/history/bridge/tool_map.c
    src/history/bridge/jsonw.c
    src/history/bridge/compatibility.c
    src/history/bridge/lineage.c
    src/history/bridge/handoff.c
    src/history/bridge/bridge.c
    src/history/bridge/sources.c
    # Source parsers: agent store -> BridgeHistory
    src/history/bridge/sources/src_jsonl.c
    src/history/bridge/sources/src_claude.c
    src/history/bridge/sources/src_codex.c
    src/history/bridge/sources/src_grok.c
    src/history/bridge/sources/src_opencode.c
    src/history/bridge/sources/src_kimi.c
    src/history/bridge/sources/src_cursor.c
    src/history/bridge/sources/src_qwen.c
    src/history/bridge/sources/src_generic.c
    # Native session writers (version-gated)
    src/history/bridge/writers/writer_common.c
    src/history/bridge/writers/writer_claude.c
    src/history/bridge/writers/writer_commandcode.c
    src/history/bridge/writers/writer_qwen.c
    src/history/bridge/writers/writer_grok.c
    src/history/bridge/writers/writer_kimi.c
    src/history/bridge/writers/writer_cline.c
    # Target adapters
    src/history/bridge/targets/target_codex.c
    src/history/bridge/targets/target_opencode.c
    src/history/bridge/targets/target_writers.c
    src/history/bridge/targets/target_handoff.c
)

set(CORE_SOURCES
    src/main.c
    src/core/atomic_file.c
    src/core/shell_integration.c
    src/core/memory.c
    src/core/string_utils.c
    src/core/utf8.c
    src/core/config.c
    src/core/theme_import.c
    src/core/agent_detect.c
    src/core/net.c
    src/core/child_reaper.c
    src/core/keybind.c
    src/core/cmd_history.c
    src/core/cmd_suggest.c
    src/core/folder_history.c
    src/core/project_dir.c
    src/core/sites.c
    src/core/pty_reader.c
    src/core/crashlog.c
    src/core/stb_image_impl.c
    src/core/imgproc.c
    src/core/mem_budget.c
    src/core/soak_probe.c
    src/core/vm_regions.c
    src/renderer/renderer.c
    src/renderer/gl_loader.c
    src/renderer/font.c
    src/renderer/emoji_bitmap.c
    src/terminal/terminal.c
    src/terminal/vt_parser.c
    src/terminal/buffer.c
    src/terminal/mouse.c
    src/terminal/search.c
    src/terminal/selection.c
    src/terminal/cmdline.c
    src/terminal/url.c
    src/terminal/img_link.c
    src/terminal/agent_activity.c
    src/terminal/prompt_capture.c
    src/terminal/paste_log.c
    src/terminal/bidi.c
    src/ssh/ssh_session.c
    src/ssh/ssh_config.c
    src/ssh/sftp.c
    src/ssh/port_forward.c
    src/ssh/telnet.c
    src/ssh/mosh.c
    src/ssh/serial.c
    src/ssh/keygen.c
    src/ssh/known_hosts.c
    src/vault/vault.c
    src/vault/crypto.c
    src/ui/ui.c
    src/ui/sleep_flush.c
    src/ui/sleep_snap.c
    src/ui/sleep_io.c
    src/ui/anim.c
    src/ui/textedit.c
    src/ui/tabs.c
    src/ui/tab_trash.c
    src/ui/app_soak.c
    src/ui/tab_popup.c
    src/ui/split.c
    src/ui/sidebar.c
    src/ui/lucide.c
    src/ui/nanosvg_impl.c
    src/ui/svg_raster.c
    src/ui/palette_row.c
    src/ui/settings_layout.c
    src/ui/input.c
    src/ui/palette.c
    src/ui/hittest.c
    src/ui/a11y_text.c
    src/ui/a11y_bridge.c
    src/ui/sidebar_sections.c
    src/ui/sidebar_customize.c
    src/ui/lightbox.c
    src/ui/sketch.c
    src/ui/sketch_layout.c
    src/ui/sketch_job.c
    src/ui/sketch_model.c
    src/ui/sketch_export.c
    src/ui/prompt_thumb.c
    src/ui/chrome_palette.c
    src/ui/settings.c
    src/ui/settings_state.c
    src/ui/diagnostics.c
    src/ui/vault_ui.c
    src/ui/git_status.c
    src/ui/syntax.c
    src/ui/git_diff.c
    src/ui/effective_settings.c
    src/ui/keycap.c
    src/ui/browser_ui.c
    src/ui/sftp_tab.c
    src/browser/browser_nav.c
    src/browser/browser_viewport.c
    src/browser/browser_format.c
    src/ui/filebrowser.c
    src/ui/bg_image.c
    src/ui/fb_find.c
    src/ui/sites_ui.c
    ${FB_ICON_SOURCE}
    src/ui/fb_asset_icons.c
    src/ui/seti_icons.c
    src/ui/fileops.c
    src/ui/agent_icons.c
    src/ui/icon.c
    src/ui/markdown/md_parse.c
    src/ui/markdown/md_embed.c
    src/ui/markdown/md_render.c
    src/ui/markdown/md_image.c
    src/ui/markdown/md_graph.c
    src/ui/markdown/md_math.c
    src/ui/markdown/md_diagram.c
    # Agent history (used by Cmd+K palette to list Claude/Codex/Copilot sessions
    # and the in-app transcript viewer modal).
    src/history/scanner.c
    src/history/event.c
    src/history/util.c
    src/history/live_title.c
    src/history/live_title_async.c
    src/history/parser_claude.c
    src/history/parser_codex.c
    src/history/parser_copilot.c
    src/history/parser_antigravity.c
    src/history/parser_grok.c
    src/history/parser_commandcode.c src/history/parser_pi.c
    src/history/parser_cline.c
    src/history/parser_cline_cli.c
    src/history/parser_run.c
    # Cross-agent session continuation ("Continue with…"): parse a session
    # from any supported agent into one universal IR, then hand it to the
    # target agent's own import surface. One integration per agent instead of
    # one per PAIR — see src/history/bridge/ir.h.
    ${BRIDGE_SOURCES}
    # Translate-on-Tab (Tab×3 in an AI-agent CLI rewrites the selection
    # via translate_agent_spawn). The local-LLM backend lives behind
    # USE_LOCAL_LLM and only gets added below when that's on.
    src/translate/translate.c
    src/translate/translate_agent.c
    src/translate/translate_api.c
    src/translate/translate_segment.c
    src/translate/translate_draft.c
    src/translate/translate_history_cleanup.c
    src/translate/model_catalog.c
    src/translate/model_download.c
    # Notifications config (Settings tab Browse/Save flow) — Liu reads and
    # writes the same notify.conf the in-process server owns.
    src/notify/config.c
    # Claude Code hook installer (Settings → Notifications → Install button)
    src/notify/claude_hooks.c
    # Codex/Grok/OpenCode/Antigravity notify-hook installers (same Settings buttons)
    src/notify/agent_hooks.c
    src/notify/queue.c
    src/notify/protocol.c
    src/update/updater.c
    third_party/cjson/cJSON.c
)

# In-process notification server (notify_server_start/stop): hosts the daemon's
# poll loop on a thread so there is no separate persistent background process.
# Pulls in the AF_UNIX transport.
list(APPEND CORE_SOURCES
    src/notify/daemon.c
    src/notify/socket.c
    # Agent lifecycle signals lifted off the hook frames (server thread -> UI).
    src/notify/agent_signal.c
    # "Is Liu frontmost?" for the suppression gate, published by the app instead
    # of queried from the window server (which cost a screen-recording prompt).
    src/notify/focus_state.c
)

# Platform TTS / desktop-notification backend for the in-process server.
if(APPLE)
    list(APPEND CORE_SOURCES src/notify/platform_notify_macos.m)
elseif(UNIX)
    list(APPEND CORE_SOURCES src/notify/platform_notify_linux.c)
endif()

# Auto-updater platform shim (NSURLSession + install/relaunch on macOS, curl +
# tarball swap on Linux, no-op everywhere else). Named in a variable so the
# updater test targets below link the same one the app does.
if(APPLE)
    set(LIU_UPDATER_SHIM src/update/updater_macos.m)
    set(LIU_UPDATER_TEST_LIBS ${COCOA_FRAMEWORK})
elseif(UNIX)
    set(LIU_UPDATER_SHIM src/update/updater_linux.c)
    # updater.c verifies Ed25519/SHA-256 through libcrypto directly; on macOS
    # it comes in transitively, here it has to be named.
    set(LIU_UPDATER_TEST_LIBS pthread OpenSSL::Crypto)
else()
    set(LIU_UPDATER_SHIM src/update/updater_stub.c)
    set(LIU_UPDATER_TEST_LIBS "")
endif()
list(APPEND CORE_SOURCES ${LIU_UPDATER_SHIM})

option(USE_LOCAL_LLM "Build local Hy-MT inference engine for Translate-on-Tab" ON)
option(LIU_LLM_METAL "Metal-accelerated local LLM backend (Apple Silicon)" ON)
set(LIU_LLM_METAL_ACTIVE OFF)
if(USE_LOCAL_LLM)
    list(APPEND CORE_SOURCES
        src/llm/llm_gguf.c
        src/llm/llm_quant_stq1_0.c
        src/llm/llm_quant_q6k.c
        src/llm/llm_stq1_0_kernel.c
        src/llm/llm_tokenizer.c
        src/llm/llm_pretokenizer.c
        src/llm/llm_model.c
        src/llm/llm_sampler.c
        src/llm/llm_engine.c
        src/translate/translate_local.c)

    # Metal GPU backend. Needs USE_METAL (shared device infra) + Apple, so it
    # is the macOS configuration and nothing else has one: off Apple the CPU
    # engine above is not a fallback, it is the whole engine. (It does not fall
    # back within a Metal build either — llm_engine.c treats a missing device
    # or an unsupported model as a hard error rather than quietly dropping to
    # the CPU path, which would turn a GPU regression into a mystery slowdown.)
    #
    # There is no Vulkan backend: a Linux GPU path would mean the compute
    # kernels in llm_metal_kernels.metal rewritten as SPIR-V, llm_metal.m
    # rewritten against descriptor sets and command buffers, explicit
    # host<->device staging that Metal's unified memory lets the Apple path
    # skip, and glslang in the build.
    if(LIU_LLM_METAL AND USE_METAL AND APPLE)
        set(LIU_LLM_METAL_ACTIVE ON)
        list(APPEND CORE_SOURCES src/llm/llm_metal.m)
        # Compile the Objective-C glue as a normal object. Release builds
        # add a global -flto; LTO'ing this mixed C/ObjC translation unit
        # makes the linker drop its symbols into an undefined state in
        # lto.o. It is thin glue (the GPU kernels carry the work), so a
        # non-LTO object costs nothing and links cleanly. Applies to
        # every target that compiles this source.
        set_source_files_properties(src/llm/llm_metal.m PROPERTIES
            COMPILE_OPTIONS "-fno-lto")
        # LLM compute kernels -> sibling metallib (loaded next to the binary).
        set(LLM_METAL_SHADER ${CMAKE_SOURCE_DIR}/src/llm/llm_metal_kernels.metal)
        set(LLM_METAL_AIR ${CMAKE_BINARY_DIR}/LiuLLM.air)
        set(LLM_METALLIB ${CMAKE_BINARY_DIR}/LiuLLM.metallib)
        add_custom_command(
            OUTPUT ${LLM_METALLIB}
            COMMAND xcrun -sdk macosx metal ${LIU_METAL_TARGET_FLAG} -fmodules-cache-path=${CMAKE_BINARY_DIR}/metal-module-cache -I${CMAKE_SOURCE_DIR}/src/llm
                    -c ${LLM_METAL_SHADER} -o ${LLM_METAL_AIR}
            COMMAND xcrun -sdk macosx metallib ${LLM_METAL_AIR} -o ${LLM_METALLIB}
            DEPENDS ${LLM_METAL_SHADER} ${CMAKE_SOURCE_DIR}/src/llm/llm_metal_params.h
                    ${CMAKE_SOURCE_DIR}/src/llm/llm_stq1_0_codebook.h
            COMMENT "Compiling LLM Metal kernels"
        )
        add_custom_target(llm_metal_shaders DEPENDS ${LLM_METALLIB})
    endif()
endif()

# Voice input (local Whisper speech-to-text).
#
# Unconditional, unlike USE_LOCAL_LLM: the portable C path is the reference
# implementation AND the non-Apple backend, and voice.c degrades to "model not
# downloaded" on its own, so there is nothing to gate. The Metal backend is
# what makes it usable in practice and follows USE_METAL.
option(LIU_WHISPER_METAL "Metal-accelerated Whisper backend (Apple Silicon)" ON)
set(LIU_WHISPER_METAL_ACTIVE OFF)
list(APPEND CORE_SOURCES
    src/voice/whisper_ggml.c
    src/voice/whisper_quant.c
    src/voice/whisper_mel.c
    src/voice/whisper_vocab.c
    src/voice/whisper_model.c
    src/voice/whisper_cpu.c
    src/voice/whisper_engine.c
    src/voice/voice_api.c
    src/voice/voice_lang.c
    src/voice/voice_sound.c
    src/voice/voice.c)

if(LIU_WHISPER_METAL AND USE_METAL AND APPLE)
    set(LIU_WHISPER_METAL_ACTIVE ON)
    list(APPEND CORE_SOURCES src/voice/whisper_metal.m)
    # Same -fno-lto reasoning as llm_metal.m: a global -flto over this mixed
    # C/ObjC translation unit leaves its symbols undefined in lto.o.
    set_source_files_properties(src/voice/whisper_metal.m PROPERTIES
        COMPILE_OPTIONS "-fno-lto")
    set(WHISPER_METAL_SHADER ${CMAKE_SOURCE_DIR}/src/voice/whisper_kernels.metal)
    set(WHISPER_METAL_AIR ${CMAKE_BINARY_DIR}/LiuWhisper.air)
    set(WHISPER_METALLIB ${CMAKE_BINARY_DIR}/LiuWhisper.metallib)
    add_custom_command(
        OUTPUT ${WHISPER_METALLIB}
        COMMAND xcrun -sdk macosx metal ${LIU_METAL_TARGET_FLAG} -fmodules-cache-path=${CMAKE_BINARY_DIR}/metal-module-cache -I${CMAKE_SOURCE_DIR}/src/voice
                -c ${WHISPER_METAL_SHADER} -o ${WHISPER_METAL_AIR}
        COMMAND xcrun -sdk macosx metallib ${WHISPER_METAL_AIR} -o ${WHISPER_METALLIB}
        DEPENDS ${WHISPER_METAL_SHADER}
                ${CMAKE_SOURCE_DIR}/src/voice/whisper_metal_params.h
        COMMENT "Compiling Whisper Metal kernels"
    )
    add_custom_target(whisper_metal_shaders DEPENDS ${WHISPER_METALLIB})
endif()

# Vulkan compute backend for Whisper on Linux and Windows systems without
# Metal. The option is experimental and off by default.
#
# Only Vulkan headers are required at build time; the binary loads libvulkan
# dynamically at runtime. Builds can run when the Vulkan loader is
# unavailable. The checked-in SPIR-V is precompiled, so glslc is not required.
option(LIU_WHISPER_VULKAN "Vulkan compute backend for Whisper (experimental)" OFF)
set(LIU_WHISPER_VULKAN_ACTIVE OFF)
if(LIU_WHISPER_VULKAN)
    find_path(VULKAN_INCLUDE_DIR vulkan/vulkan.h
        HINTS ENV VULKAN_SDK PATH_SUFFIXES include
        PATHS /opt/homebrew/include /usr/local/include /usr/include)
    if(VULKAN_INCLUDE_DIR)
        set(LIU_WHISPER_VULKAN_ACTIVE ON)
        list(APPEND CORE_SOURCES src/voice/whisper_vulkan.c)
    else()
        message(WARNING "LIU_WHISPER_VULKAN=ON but vulkan/vulkan.h was not found "
                        "(brew install vulkan-headers / apt install libvulkan-dev) "
                        "— building without the Vulkan backend")
    endif()
endif()

if(USE_METAL AND APPLE)
    # Add Metal renderer source
    list(APPEND CORE_SOURCES src/renderer/renderer_metal.m src/renderer/renderer_metal_sync.m)
    # font.c needs Objective-C for Metal texture API
    set_source_files_properties(src/renderer/font.c PROPERTIES LANGUAGE OBJC)
endif()

# First-run intro. The policy and the three-call seam compile everywhere (off
# macOS they answer "no" and do nothing), and so does the timeline the tour's
# steps are timed by (arithmetic only): intro_tour.c and main.c call it, and a
# build without LTO (Debug) keeps those calls; the picture, its sound and the
# overlay window need Metal and AppKit.
# The tour's timing reads the timeline, so the timeline (plain arithmetic) is
# built everywhere too: a Release link dropped the tour's unused code and hid
# that, a Debug link off macOS failed on it.
list(APPEND CORE_SOURCES src/intro/intro.c src/intro/intro_policy.c src/intro/intro_tour.c
    src/intro/intro_lang.c src/intro/intro_timeline.c)
if(USE_METAL AND APPLE)
    list(APPEND CORE_SOURCES
        src/intro/intro_sound.c
        src/intro/intro_render_macos.m
        src/intro/intro_macos.m)
endif()

# In-app browser: WKWebView on macOS when LIU_BROWSER=ON; Linux stub otherwise.
if(LIU_BROWSER AND APPLE)
    find_library(WEBKIT_FRAMEWORK WebKit REQUIRED)
    list(APPEND PLATFORM_LIBS ${WEBKIT_FRAMEWORK})
    list(APPEND CORE_SOURCES src/browser/browser_macos.m)
    add_compile_definitions(LIU_BROWSER=1)
elseif(NOT APPLE)
    list(APPEND CORE_SOURCES src/browser/browser_linux.c)
else()
    list(APPEND CORE_SOURCES src/browser/browser_linux.c)
endif()

add_executable(Liu
    ${CORE_SOURCES}
    ${PLATFORM_SOURCES}
    ${ASM_SOURCES}
)

# Liu's own terminfo entry (liu.terminfo → <build>/terminfo/…/xterm-liu),
# compiled beside the binary so a pane whose config asks for TERM=xterm-liu
# finds it through TERMINFO. Packaging copies the folder into the bundle's
# Resources. Without tic the app simply has no entry and panes keep
# xterm-256color.
find_program(LIU_TIC_EXECUTABLE tic)
if(LIU_TIC_EXECUTABLE)
    add_custom_command(
        OUTPUT ${CMAKE_BINARY_DIR}/terminfo/.stamp
        COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/terminfo
        COMMAND ${LIU_TIC_EXECUTABLE} -x -o ${CMAKE_BINARY_DIR}/terminfo ${CMAKE_SOURCE_DIR}/liu.terminfo
        COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_BINARY_DIR}/terminfo/.stamp
        DEPENDS ${CMAKE_SOURCE_DIR}/liu.terminfo
        COMMENT "Compiling liu.terminfo"
        VERBATIM)
    add_custom_target(liu_terminfo ALL DEPENDS ${CMAKE_BINARY_DIR}/terminfo/.stamp)
    add_dependencies(Liu liu_terminfo)
endif()

if(FB_ICON_SOURCE)
    target_compile_definitions(Liu PRIVATE HAVE_FB_ICON=1)
endif()

# Lets settings.c resolve assets/sounds/ in dev builds (running the binary
# straight out of build/). The .app bundle path takes precedence at runtime —
# this fallback only kicks in when _NSGetExecutablePath doesn't land in a
# Contents/MacOS layout. Quoted so the string survives the compiler.
#
# DEV BUILDS ONLY. This bakes the build machine's absolute source path into the
# binary, so a packaged build would carry the packager's home directory and
# folder names. A packaged app never needs the fallback (it always resolves
# assets through its own bundle), so the packaged configurations omit the
# define and ship no build path at all.
#
# Per configuration: Debug and RelWithDebInfo are run straight from the build
# directory and need it; Release and MinSizeRel are the two that get packaged.
target_compile_definitions(Liu PRIVATE
    "$<$<CONFIG:Debug,RelWithDebInfo>:LIU_ASSETS_DIR=\"${CMAKE_SOURCE_DIR}/assets\">")

if(USE_LOCAL_LLM)
    target_compile_definitions(Liu PRIVATE LIU_HAVE_LOCAL_LLM=1)
endif()

if(LIU_LLM_METAL_ACTIVE)
    target_compile_definitions(Liu PRIVATE LIU_LLM_METAL=1)
    add_dependencies(Liu llm_metal_shaders)
endif()

if(LIU_WHISPER_METAL_ACTIVE)
    target_compile_definitions(Liu PRIVATE LIU_WHISPER_METAL=1)
    add_dependencies(Liu whisper_metal_shaders)
endif()

if(LIU_WHISPER_VULKAN_ACTIVE)
    target_compile_definitions(Liu PRIVATE LIU_WHISPER_VULKAN=1)
    target_include_directories(Liu PRIVATE ${VULKAN_INCLUDE_DIR})
endif()

# ${CMAKE_BINARY_DIR}/generated contains the auto-generated agent_icons_data.h
target_include_directories(Liu PRIVATE
    src
    third_party
    third_party/stb
    third_party/sqlite3
    third_party/cjson
    ${CMAKE_BINARY_DIR}/generated
    ${LIBSSH2_INCLUDE_DIRS}
)

# Upstream cJSON ships with its own warnings — silence them for the main target
# the same way liu-history does.
set_source_files_properties(third_party/cjson/cJSON.c PROPERTIES COMPILE_FLAGS "-w")

target_link_libraries(Liu PRIVATE
    sqlite3
    ${LIBSSH2_LIBRARIES}
    ${PLATFORM_LIBS}
)

if(APPLE)
    # A probe runs beside the installed app. Native UI clients select by
    # bundle identity, so sharing production's identity can address its window.
    if(LIU_E2E_PROBE)
        set(LIU_MACOS_BUNDLE_ID "com.github.calculusteam.Liu.e2e")
    elseif(LIU_DEV_HOOKS)
        # Use a separate bundle identifier for builds with developer hooks so
        # automation and injected libraries use permissions associated with the
        # development app, not the installed app.
        set(LIU_MACOS_BUNDLE_ID "com.github.calculusteam.Liu.dev")
    else()
        set(LIU_MACOS_BUNDLE_ID "com.github.calculusteam.Liu")
    endif()
    # Embed an Info.plist into the bare Mach-O so dev builds (running
    # ./build/Liu directly, with no .app bundle) still carry the microphone
    # usage description macOS TCC requires before AVAudioRecorder can show its
    # permission prompt. The packaged .app uses its own Contents/Info.plist
    # (scripts/package_macos_dmg.sh) and ignores this embedded section.
    #
    # The other usage strings cover every TCC service Liu can reach: local
    # network (SSH/Mosh/Telnet to a LAN host), external and network volumes (a
    # repo or vault kept off the boot disk), and the three per-user folders the
    # file browser can be navigated into. Without a string macOS still prompts,
    # just with no reason attached. This list matches package_macos_dmg.sh and
    # run_dev_macos.sh; all three describe the same app.
    #
    # NSAllowsArbitraryLoadsInWebContent: App Transport Security lets a web view
    # fetch plain http only from an IP address, a one-label name or .local, so a
    # dev server on a tailnet (MagicDNS) or LAN host NAME fails with -1022
    # before a byte leaves the machine. Which pages the browser opens is Liu's
    # policy (browser/browser_nav.c), not ATS's; Liu's own connections keep ATS.
    set(LIU_EMBED_PLIST "${CMAKE_BINARY_DIR}/Liu-embedded-Info.plist")
    file(WRITE "${LIU_EMBED_PLIST}"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
    <key>CFBundleIdentifier</key>
    <string>${LIU_MACOS_BUNDLE_ID}</string>
    <key>CFBundleName</key>
    <string>Liu</string>
    <key>NSMicrophoneUsageDescription</key>
    <string>Liu uses the microphone when you dictate a prompt with Voice Input, and when you record a custom notification sound. Speech is transcribed on this Mac by default; if you choose a hosted speech service in the Voice settings, your audio is sent to that service.</string>
    <key>NSLocalNetworkUsageDescription</key>
    <string>Liu connects to hosts on your local network when you open an SSH, Mosh or Telnet session to one, or a page on one in its browser.</string>
    <key>NSAppTransportSecurity</key>
    <dict>
        <key>NSAllowsArbitraryLoadsInWebContent</key>
        <true/>
    </dict>
    <key>NSRemovableVolumesUsageDescription</key>
    <string>Liu opens terminals, browses files and runs git in folders you pick, including ones on external drives.</string>
    <key>NSNetworkVolumesUsageDescription</key>
    <string>Liu opens terminals, browses files and runs git in folders you pick, including ones on network shares.</string>
    <key>NSDesktopFolderUsageDescription</key>
    <string>Liu's file browser only reads the folders you navigate to — your Desktop among them.</string>
    <key>NSDocumentsFolderUsageDescription</key>
    <string>Liu's file browser only reads the folders you navigate to — your Documents among them.</string>
    <key>NSDownloadsFolderUsageDescription</key>
    <string>Liu's file browser only reads the folders you navigate to — your Downloads among them.</string>
    <key>NSServices</key>
    <array>
        <dict>
            <key>NSMenuItem</key>
            <dict>
                <key>default</key>
                <string>New Liu Window Here</string>
            </dict>
            <key>NSMessage</key>
            <string>openLiuWindowHere</string>
            <key>NSPortName</key>
            <string>Liu</string>
            <key>NSRequiredContext</key>
            <dict>
                <key>NSTextContent</key>
                <string>FilePath</string>
            </dict>
            <key>NSSendFileTypes</key>
            <array>
                <string>public.folder</string>
                <string>public.item</string>
            </array>
        </dict>
        <dict>
            <key>NSMenuItem</key>
            <dict>
                <key>default</key>
                <string>New Liu Tab Here</string>
            </dict>
            <key>NSMessage</key>
            <string>openLiuTabHere</string>
            <key>NSPortName</key>
            <string>Liu</string>
            <key>NSRequiredContext</key>
            <dict>
                <key>NSTextContent</key>
                <string>FilePath</string>
            </dict>
            <key>NSSendFileTypes</key>
            <array>
                <string>public.folder</string>
                <string>public.item</string>
            </array>
        </dict>
    </array>
</dict>
</plist>
")
    target_link_options(Liu PRIVATE
        "SHELL:-sectcreate __TEXT __info_plist ${LIU_EMBED_PLIST}")
    # Relink when the embedded plist content changes (it's only a linker
    # argument, not an input object, so CMake wouldn't otherwise notice).
    set_property(TARGET Liu APPEND PROPERTY LINK_DEPENDS "${LIU_EMBED_PLIST}")

    # Use Developer ID signing for development builds when an identity is
    # available. TCC binds grants to the bundle identifier and code requirement;
    # ad-hoc signatures are tied to a cdhash that changes when the binary
    # changes.
    #
    # Builds with LIU_DEV_HOOKS use the .dev bundle identifier and omit the
    # hardened runtime so debugging and injection tools use the development app's
    # permissions. Builds without these hooks use the release identifier,
    # hardened runtime, and production entitlement.
    #
    # Set LIU_DEV_CODESIGN=OFF to disable signing, or set LIU_DEV_SIGN_IDENTITY
    # to select an identity.
    option(LIU_DEV_CODESIGN "Sign the dev binary with a Developer ID when one exists" ON)
    if(LIU_DEV_CODESIGN)
        if(NOT DEFINED LIU_DEV_SIGN_IDENTITY)
            execute_process(COMMAND security find-identity -v -p codesigning
                            OUTPUT_VARIABLE _liu_signing_ids ERROR_QUIET)
            string(REGEX MATCH "Developer ID Application: [^\"]+" _liu_dev_id
                   "${_liu_signing_ids}")
            set(LIU_DEV_SIGN_IDENTITY "${_liu_dev_id}")
        endif()
        if(LIU_DEV_SIGN_IDENTITY)
            # `|| true`: a locked keychain or expired cert must not fail a build
            # whose only loss is having to re-grant permissions.
            set(_liu_dev_sign_opts "")
            if(NOT LIU_DEV_HOOKS)
                file(WRITE "${CMAKE_BINARY_DIR}/liu_entitlements.plist"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\"><dict>
<key>com.apple.security.device.audio-input</key><true/>
</dict></plist>
")
                set(_liu_dev_sign_opts "--options runtime --entitlements '${CMAKE_BINARY_DIR}/liu_entitlements.plist'")
            endif()
            add_custom_command(TARGET Liu POST_BUILD
                COMMAND sh -c
                    "codesign --force --sign '${LIU_DEV_SIGN_IDENTITY}' ${_liu_dev_sign_opts} --identifier '${LIU_MACOS_BUNDLE_ID}' '$<TARGET_FILE:Liu>' >/dev/null 2>&1 || echo 'warning: dev codesign failed; macOS will re-ask for permissions after rebuilds' >&2"
                VERBATIM)
        endif()
    endif()
endif()

if(USE_METAL AND APPLE)
    add_dependencies(Liu metal_shaders)
endif()

if(LIBSSH2_BOOTSTRAP_TARGET)
    add_dependencies(Liu ${LIBSSH2_BOOTSTRAP_TARGET})
endif()

# =============================================================================
# liu-notify: standalone voice-notification daemon + CLI (no sqlite/libssh2/UI)
# =============================================================================
set(NOTIFY_SOURCES
    src/notify/cli.c
    src/notify/config.c
    src/notify/daemon.c
    src/notify/agent_signal.c
    src/notify/focus_state.c
    src/notify/protocol.c
    src/notify/queue.c
    src/notify/socket.c
    src/core/memory.c
    src/core/utf8.c
    src/core/string_utils.c
    src/core/net.c
)

set(NOTIFY_PLATFORM_LIBS "")
if(APPLE)
    list(APPEND NOTIFY_SOURCES src/notify/platform_notify_macos.m)
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)
    set(NOTIFY_PLATFORM_LIBS
        ${AVFOUNDATION_FRAMEWORK}
        ${COCOA_FRAMEWORK}
        ${CORETEXT_FRAMEWORK}
        ${COREGRAPHICS_FRAMEWORK}
    )
elseif(UNIX)
    list(APPEND NOTIFY_SOURCES src/notify/platform_notify_linux.c)
    set(NOTIFY_PLATFORM_LIBS m)
endif()

# utf8_validate is ASM-only when USE_ASM=1 is set globally, so liu-notify must
# link the utf8 asm for this arch (utf8.c omits its C fallback). memops is also
# linked because its C fallbacks gate on the same define; unused fast_* symbols
# get dead-stripped by the linker. Keep this arch/compiler test in lockstep with
# the main ASM_SOURCES block above.
set(NOTIFY_ASM_SOURCES "")
if(USE_ASM AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(NOTIFY_ASM_SOURCES
        src/asm/memops_arm64.S
        src/asm/utf8_arm64.S
    )
elseif(USE_ASM AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(NOTIFY_ASM_SOURCES
        src/asm/memops_x86_64.S
        src/asm/utf8_x86_64.S
    )
endif()

# liu-notify and liu-history link the platform's threads library.
find_package(Threads REQUIRED)

add_executable(liu-notify ${NOTIFY_SOURCES} ${NOTIFY_ASM_SOURCES})

target_include_directories(liu-notify PRIVATE
    src
    third_party
)

# utf8.c interns grapheme clusters under a mutex (the table is paged and grows).
target_link_libraries(liu-notify PRIVATE ${NOTIFY_PLATFORM_LIBS} Threads::Threads)

# =============================================================================
# liu-history: read agent chat history (Claude / Codex / Copilot) from disk
# =============================================================================
set(HISTORY_SOURCES
    src/history/cli.c
    src/history/event.c
    src/history/formatter.c
    src/history/parser_claude.c
    src/history/parser_codex.c
    src/history/parser_copilot.c
    src/history/parser_antigravity.c
    src/history/parser_grok.c
    src/history/parser_commandcode.c src/history/parser_pi.c
    src/history/parser_cline.c
    src/history/parser_cline_cli.c
    src/history/parser_run.c
    src/history/scanner.c
    src/history/util.c
    ${BRIDGE_SOURCES}
    src/core/atomic_file.c
    src/core/memory.c
    # scanner.c charges MEM_HISTORY for its scan arena, so every target that
    # links it needs the ledger.
    src/core/mem_budget.c
    src/core/utf8.c
    src/core/string_utils.c
    third_party/cjson/cJSON.c
)

set(BUILD_LIU_HISTORY ON)
set(LIU_HISTORY_MISSING "")
foreach(src IN LISTS HISTORY_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${src}")
        set(BUILD_LIU_HISTORY OFF)
        list(APPEND LIU_HISTORY_MISSING "${src}")
    endif()
endforeach()

if(BUILD_LIU_HISTORY)
    add_executable(liu-history ${HISTORY_SOURCES} ${NOTIFY_ASM_SOURCES})

    target_include_directories(liu-history PRIVATE
        src
        third_party
        third_party/cjson
    )

    # liu-history needs sqlite3 because the OpenCode scanner reads sessions
    # straight out of opencode.db (the only place the per-session cwd lives).
    target_link_libraries(liu-history PRIVATE sqlite3 Threads::Threads)

    set_source_files_properties(third_party/cjson/cJSON.c PROPERTIES
        COMPILE_FLAGS "-w")  # upstream lib; suppress its warnings

    # Post-build symlink: build/agenthistory → liu-history.
    # Lets `agenthistory` be typed as a command in liu's local shells without
    # needing an install (liu prepends its own dir to PATH on session spawn).
    add_custom_command(TARGET liu-history POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E create_symlink liu-history
            $<TARGET_FILE_DIR:liu-history>/agenthistory
        COMMENT "Creating agenthistory → liu-history symlink"
        VERBATIM)
else()
    message(STATUS "liu-history target disabled; missing sources: ${LIU_HISTORY_MISSING}")
endif()

# =============================================================================
# Install
# =============================================================================
install(TARGETS Liu RUNTIME DESTINATION bin)
install(TARGETS liu-notify RUNTIME DESTINATION bin)
install(CODE "execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink \
liu-notify \${CMAKE_INSTALL_PREFIX}/bin/liu-notifyd)")
if(BUILD_LIU_HISTORY)
    install(TARGETS liu-history RUNTIME DESTINATION bin)
    install(CODE "execute_process(COMMAND \${CMAKE_COMMAND} -E create_symlink \
liu-history \${CMAKE_INSTALL_PREFIX}/bin/agenthistory)")
endif()

# =============================================================================
# The test targets are defined in tests/tests.cmake, included only when it
# exists. They are EXCLUDE_FROM_ALL: building the product does not run or
# build the test suite.
# =============================================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/tests.cmake")
    include("${CMAKE_CURRENT_SOURCE_DIR}/tests/tests.cmake")
endif()

# Give test runners the configured executable paths instead of scraping a
# generator-specific `--target help` or guessing which conditional targets exist.
# Includes generated/foreach targets and works with Make, Ninja and multi-config.
function(liu_collect_tests directory)
    get_property(targets DIRECTORY "${directory}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(target IN LISTS targets)
        get_target_property(kind "${target}" TYPE)
        if(target MATCHES "^test_" AND kind STREQUAL "EXECUTABLE")
            set_property(GLOBAL APPEND_STRING PROPERTY LIU_TEST_MANIFEST
                "${target}\t$<TARGET_FILE:${target}>\n")
        endif()
    endforeach()
    get_property(children DIRECTORY "${directory}" PROPERTY SUBDIRECTORIES)
    foreach(child IN LISTS children)
        liu_collect_tests("${child}")
    endforeach()
endfunction()
liu_collect_tests("${CMAKE_CURRENT_SOURCE_DIR}")
get_property(LIU_TEST_MANIFEST GLOBAL PROPERTY LIU_TEST_MANIFEST)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/liu-tests/$<CONFIG>/targets.tsv"
    CONTENT "${LIU_TEST_MANIFEST}")
