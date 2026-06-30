cmake_minimum_required(VERSION 3.20)

if(APPLE AND NOT CMAKE_OSX_DEPLOYMENT_TARGET)
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.0" CACHE STRING "Minimum macOS deployment version" FORCE)
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

# App version shown in Settings > About. The canonical version lives in the
# top-level VERSION file (single source of truth — also read by the packaging
# script, the website, and the update feed). LIU_BUILD carries the short git
# hash for precise bug reports (empty outside a git checkout).
file(STRINGS "${CMAKE_SOURCE_DIR}/VERSION" LIU_VERSION LIMIT_COUNT 1)
string(STRIP "${LIU_VERSION}" LIU_VERSION)
execute_process(
    COMMAND git rev-parse --short HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE LIU_BUILD_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
add_compile_definitions(LIU_VERSION="${LIU_VERSION}" LIU_BUILD="${LIU_BUILD_HASH}")

# Auto-update feed URL. Canonical host is liu.software (S3/R2-backed); the
# release pipeline publishes appcast.json + the per-arch downloads there.
# Overridable at configure time with -DLIU_UPDATE_FEED_URL=...
if(NOT DEFINED LIU_UPDATE_FEED_URL)
    set(LIU_UPDATE_FEED_URL "https://liu.software/appcast.json")
endif()
add_compile_definitions(LIU_UPDATE_FEED_URL="${LIU_UPDATE_FEED_URL}")

# Local Translate model download. The ~440 MB GGUF is NOT bundled; it is
# fetched on demand from Settings. The URL is host-agnostic (curl -L follows
# redirects, so a Hugging Face resolve/main URL works); the SHA-256 + size
# gate the finished file. Default is Tencent AngelSlim's official Sherry
# (STQ1_0 / PR #22836) GGUF — the engine now decodes that canonical packing.
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

# =============================================================================
# Compiler flags
# =============================================================================
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -Wall -Wextra -Wpedantic -Werror=implicit-function-declaration
        -fno-exceptions
        -fvisibility=hidden
    )
    if(APPLE)
        add_compile_options(-mmacosx-version-min=11.0)
        add_link_options(-mmacosx-version-min=11.0)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O3 -flto)
        if(APPLE AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            add_compile_options(-mcpu=apple-m1)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
            add_compile_options(-march=armv8-a+simd)
        elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
            # Distributable default: tune generically but keep the baseline
            # x86-64 ISA so the binary runs on any x86-64 CPU (no host-specific
            # AVX-512/etc. baked in). Self-builders opt into host tuning with
            # -DLIU_NATIVE_ARCH=ON.
            if(LIU_NATIVE_ARCH)
                add_compile_options(-march=native)
            else()
                add_compile_options(-mtune=generic)
            endif()
        endif()
        add_link_options(-flto)
    else()
        add_compile_options(-O0 -g -DDEBUG)
    endif()
endif()

# =============================================================================
# Platform detection
# =============================================================================
set(PLATFORM_SOURCES "")
set(PLATFORM_LIBS "")

if(APPLE)
    set(PLATFORM_SOURCES src/platform/platform_macos.m)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(OPENGL_FRAMEWORK OpenGL REQUIRED)
    find_library(IOKIT_FRAMEWORK IOKit REQUIRED)
    find_library(CORE_VIDEO_FRAMEWORK CoreVideo REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(SECURITY_FRAMEWORK Security REQUIRED)
    find_library(CORETEXT_FRAMEWORK CoreText REQUIRED)
    find_library(COREGRAPHICS_FRAMEWORK CoreGraphics REQUIRED)
    find_library(USERNOTIFICATIONS_FRAMEWORK UserNotifications REQUIRED)
    find_library(UNIFORMTYPEIDENTIFIERS_FRAMEWORK UniformTypeIdentifiers REQUIRED)
    find_library(AVFOUNDATION_FRAMEWORK AVFoundation REQUIRED)  # in-process notify TTS
    set(PLATFORM_LIBS
        ${COCOA_FRAMEWORK}
        ${OPENGL_FRAMEWORK}
        ${IOKIT_FRAMEWORK}
        ${CORE_VIDEO_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${SECURITY_FRAMEWORK}
        ${CORETEXT_FRAMEWORK}
        ${COREGRAPHICS_FRAMEWORK}
        ${USERNOTIFICATIONS_FRAMEWORK}
        ${UNIFORMTYPEIDENTIFIERS_FRAMEWORK}
        ${AVFOUNDATION_FRAMEWORK}
    )
    add_compile_definitions(PLATFORM_MACOS=1 GL_SILENCE_DEPRECATION=1)
elseif(UNIX)
    set(PLATFORM_SOURCES src/platform/platform_linux.c)
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

if(USE_METAL AND APPLE)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    add_compile_definitions(USE_METAL=1)

    # Compile Metal shaders to metallib
    set(METAL_SHADER ${CMAKE_SOURCE_DIR}/src/swift/LiuShaders.metal)
    set(METAL_AIR ${CMAKE_BINARY_DIR}/LiuShaders.air)
    set(METALLIB ${CMAKE_BINARY_DIR}/LiuShaders.metallib)
    add_custom_command(
        OUTPUT ${METALLIB}
        COMMAND xcrun -sdk macosx metal -c ${METAL_SHADER} -o ${METAL_AIR}
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
# Trimmed SQLite build: disable every feature we don't exercise from vault.c.
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
    SQLITE_OMIT_PROGRESS_CALLBACK
    SQLITE_OMIT_TRACE
    SQLITE_OMIT_DECLTYPE
    SQLITE_OMIT_AUTHORIZATION
    SQLITE_OMIT_COMPLETE
    SQLITE_OMIT_GET_TABLE
    SQLITE_OMIT_TCL_VARIABLE
    SQLITE_OMIT_UTF16
    SQLITE_USE_ALLOCA
)
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(sqlite3 PRIVATE -w)
endif()

# =============================================================================
# Third-party: libssh2
# =============================================================================
find_package(PkgConfig QUIET)
set(VENDORED_SSH_ROOT ${CMAKE_SOURCE_DIR}/third_party/macos-deps/install)
set(LIBSSH2_BOOTSTRAP_TARGET "")
if(APPLE AND USE_VENDORED_SSH_DEPS AND
   EXISTS "${VENDORED_SSH_ROOT}/lib/libssh2.a" AND
   EXISTS "${VENDORED_SSH_ROOT}/lib/libssl.a" AND
   EXISTS "${VENDORED_SSH_ROOT}/lib/libcrypto.a")
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
        "Vendored libssh2/OpenSSL deps are missing. Enable AUTO_DOWNLOAD_SSH_DEPS or run scripts/build_macos_compat_deps.sh")
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
function(_embed_png_array var_prefix pngpath)
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
_embed_png_array(AGENT_ICON_COPILOT     "${CMAKE_SOURCE_DIR}/assets/agents/copilot.png")
_embed_png_array(AGENT_ICON_CURSOR      "${CMAKE_SOURCE_DIR}/assets/agents/cursor.png")
_embed_png_array(AGENT_ICON_AMP         "${CMAKE_SOURCE_DIR}/assets/agents/amp.png")
_embed_png_array(AGENT_ICON_CLINE       "${CMAKE_SOURCE_DIR}/assets/agents/cline.png")
_embed_png_array(AGENT_ICON_ROO         "${CMAKE_SOURCE_DIR}/assets/agents/roo.png")
_embed_png_array(AGENT_ICON_KILO        "${CMAKE_SOURCE_DIR}/assets/agents/kilo.png")
_embed_png_array(AGENT_ICON_KIRO        "${CMAKE_SOURCE_DIR}/assets/agents/kiro.png")
_embed_png_array(AGENT_ICON_CRUSH       "${CMAKE_SOURCE_DIR}/assets/agents/crush.png")
_embed_png_array(AGENT_ICON_OPENCODE    "${CMAKE_SOURCE_DIR}/assets/agents/opencode.png")
_embed_png_array(AGENT_ICON_DROID       "${CMAKE_SOURCE_DIR}/assets/agents/droid.png")
_embed_png_array(AGENT_ICON_ANTIGRAVITY "${CMAKE_SOURCE_DIR}/assets/agents/antigravity.png")
_embed_png_array(AGENT_ICON_KIMI        "${CMAKE_SOURCE_DIR}/assets/agents/kimi.png")
_embed_png_array(AGENT_ICON_QWEN        "${CMAKE_SOURCE_DIR}/assets/agents/qwen.png")
_embed_png_array(AGENT_ICON_AIDER       "${CMAKE_SOURCE_DIR}/assets/agents/aider.png")
_embed_png_array(AGENT_ICON_AMAZONQ     "${CMAKE_SOURCE_DIR}/assets/agents/amazon-q.png")
_embed_png_array(AGENT_ICON_CONTINUE    "${CMAKE_SOURCE_DIR}/assets/agents/continue.png")
_embed_png_array(AGENT_ICON_WINDSURF    "${CMAKE_SOURCE_DIR}/assets/agents/windsurf.png")
_embed_png_array(AGENT_ICON_ZED         "${CMAKE_SOURCE_DIR}/assets/agents/zed.png")
_embed_png_array(AGENT_ICON_COMMANDCODE "${CMAKE_SOURCE_DIR}/assets/agents/commandcode.png")
_embed_png_array(AGENT_ICON_XAI         "${CMAKE_SOURCE_DIR}/assets/agents/xai.png")
_embed_png_array(LIU_SLEEP_ICON         "${CMAKE_SOURCE_DIR}/assets/liu-sleep.png")
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
_embed_png_array(FB_ICON_LANG_C          "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/c.png")
_embed_png_array(FB_ICON_LANG_CPP        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/cpp.png")
_embed_png_array(FB_ICON_LANG_CSHARP     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/csharp.png")
_embed_png_array(FB_ICON_LANG_PYTHON     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/python.png")
_embed_png_array(FB_ICON_LANG_RUST       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/rust.png")
_embed_png_array(FB_ICON_LANG_GO         "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/go.png")
_embed_png_array(FB_ICON_LANG_JS         "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/js.png")
_embed_png_array(FB_ICON_LANG_TS         "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/ts.png")
_embed_png_array(FB_ICON_LANG_JAVA       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/java.png")
_embed_png_array(FB_ICON_LANG_RUBY       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/ruby.png")
_embed_png_array(FB_ICON_LANG_PHP        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/php.png")
_embed_png_array(FB_ICON_LANG_SWIFT      "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/swift.png")
_embed_png_array(FB_ICON_LANG_KOTLIN     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/kotlin.png")
_embed_png_array(FB_ICON_LANG_SCALA      "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/scala.png")
_embed_png_array(FB_ICON_LANG_DART       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/dart.png")
_embed_png_array(FB_ICON_LANG_LUA        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/lua.png")
_embed_png_array(FB_ICON_LANG_PERL       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/perl.png")
_embed_png_array(FB_ICON_LANG_R          "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/r.png")
_embed_png_array(FB_ICON_LANG_JULIA      "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/julia.png")
_embed_png_array(FB_ICON_LANG_HASKELL    "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/haskell.png")
_embed_png_array(FB_ICON_LANG_ELIXIR     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/elixir.png")
_embed_png_array(FB_ICON_LANG_ERLANG     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/erlang.png")
_embed_png_array(FB_ICON_LANG_CLOJURE    "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/clojure.png")
_embed_png_array(FB_ICON_LANG_OCAML      "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/ocaml.png")
_embed_png_array(FB_ICON_LANG_FSHARP     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/fsharp.png")
_embed_png_array(FB_ICON_LANG_OBJC       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/objc.png")
_embed_png_array(FB_ICON_LANG_GROOVY     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/groovy.png")
_embed_png_array(FB_ICON_LANG_NIM        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/nim.png")
_embed_png_array(FB_ICON_LANG_CRYSTAL    "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/crystal.png")
_embed_png_array(FB_ICON_LANG_SOLIDITY   "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/solidity.png")
_embed_png_array(FB_ICON_LANG_FORTRAN    "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/fortran.png")
_embed_png_array(FB_ICON_LANG_HAXE       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/haxe.png")
_embed_png_array(FB_ICON_LANG_ZIG        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/zig.png")
_embed_png_array(FB_ICON_LANG_POWERSHELL "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/powershell.png")
_embed_png_array(FB_ICON_LANG_SHELL      "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/shell.png")
_embed_png_array(FB_ICON_LANG_HTML       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/html.png")
_embed_png_array(FB_ICON_LANG_CSS        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/css.png")
_embed_png_array(FB_ICON_LANG_SASS       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/sass.png")
_embed_png_array(FB_ICON_LANG_STYLUS     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/stylus.png")
_embed_png_array(FB_ICON_LANG_REACT      "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/react.png")
_embed_png_array(FB_ICON_LANG_VUE        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/vue.png")
_embed_png_array(FB_ICON_LANG_SVELTE     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/svelte.png")
_embed_png_array(FB_ICON_LANG_JSON       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/json.png")
_embed_png_array(FB_ICON_LANG_YAML       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/yaml.png")
_embed_png_array(FB_ICON_LANG_MARKDOWN   "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/markdown.png")
_embed_png_array(FB_ICON_LANG_LATEX      "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/latex.png")
_embed_png_array(FB_ICON_LANG_GRAPHQL    "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/graphql.png")
_embed_png_array(FB_ICON_LANG_DOCKER     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/docker.png")
_embed_png_array(FB_ICON_LANG_CMAKE      "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/cmake.png")
_embed_png_array(FB_ICON_LANG_GIT        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/git.png")
_embed_png_array(FB_ICON_LANG_NPM        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/npm.png")
_embed_png_array(FB_ICON_LANG_YARN       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/yarn.png")
_embed_png_array(FB_ICON_LANG_GRADLE     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/gradle.png")
_embed_png_array(FB_ICON_LANG_TERRAFORM  "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/terraform.png")
_embed_png_array(FB_ICON_LANG_BUN        "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/bun.png")
_embed_png_array(FB_ICON_LANG_DENO       "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/deno.png")
_embed_png_array(FB_ICON_LANG_VB         "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/vb.png")
_embed_png_array(FB_ICON_LANG_DELPHI     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/delphi.png")
_embed_png_array(FB_ICON_LANG_MATLAB     "${CMAKE_SOURCE_DIR}/assets/filebrowser/lang/matlab.png")
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

set(FB_ICON_SOURCE "")
if(EXISTS "${CMAKE_SOURCE_DIR}/src/ui/fb_icon.c" AND
   EXISTS "${CMAKE_SOURCE_DIR}/src/ui/fb_icon.h")
    set(FB_ICON_SOURCE src/ui/fb_icon.c)
else()
    message(STATUS "File-browser icon source missing: building without Nerd Font icons")
endif()

set(CORE_SOURCES
    src/main.c
    src/core/memory.c
    src/core/string_utils.c
    src/core/utf8.c
    src/core/config.c
    src/core/theme_import.c
    src/core/agent_detect.c
    src/core/net.c
    src/core/keybind.c
    src/core/cmd_history.c
    src/core/cmd_suggest.c
    src/core/project_dir.c
    src/core/sites.c
    src/core/crashlog.c
    src/core/stb_image_impl.c
    src/renderer/renderer.c
    src/renderer/gl_loader.c
    src/renderer/font.c
    src/terminal/terminal.c
    src/terminal/vt_parser.c
    src/terminal/buffer.c
    src/terminal/mouse.c
    src/terminal/search.c
    src/terminal/selection.c
    src/terminal/url.c
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
    src/ui/anim.c
    src/ui/tabs.c
    src/ui/tab_trash.c
    src/ui/split.c
    src/ui/sidebar.c
    src/ui/input.c
    src/ui/palette.c
    src/ui/hittest.c
    src/ui/chrome_palette.c
    src/ui/settings.c
    src/ui/vault_ui.c
    src/ui/git_status.c
    src/ui/filebrowser.c
    src/ui/fb_find.c
    src/ui/sites_ui.c
    ${FB_ICON_SOURCE}
    src/ui/fb_asset_icons.c
    src/ui/fileops.c
    src/ui/agent_icons.c
    src/ui/icon.c
    src/ui/markdown/md_parse.c
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
    src/history/parser_claude.c
    src/history/parser_codex.c
    src/history/parser_copilot.c
    src/history/parser_antigravity.c
    src/history/parser_grok.c
    src/history/parser_commandcode.c
    src/history/parser_cline.c
    src/history/parser_run.c
    # Translate-on-Tab (Tab×3 in an AI-agent CLI rewrites the selection
    # via translate_agent_spawn). The local-LLM backend lives behind
    # USE_LOCAL_LLM and only gets added below when that's on.
    src/translate/translate.c
    src/translate/translate_agent.c
    src/translate/translate_api.c
    src/translate/translate_segment.c
    src/translate/translate_history_cleanup.c
    src/translate/model_catalog.c
    src/translate/model_download.c
    # Notifications config (Settings tab Browse/Save flow) — Liu reads and
    # writes the same notify.conf the in-process server owns.
    src/notify/config.c
    # Claude Code hook installer (Settings → Notifications → Install button)
    src/notify/claude_hooks.c
    # Codex/Grok/OpenCode notify-hook installers (same Settings buttons)
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
)

# Platform TTS / desktop-notification backend for the in-process server.
if(APPLE)
    list(APPEND CORE_SOURCES src/notify/platform_notify_macos.m)
elseif(UNIX)
    list(APPEND CORE_SOURCES src/notify/platform_notify_linux.c)
endif()

# Auto-updater platform shim (NSURLSession + install/relaunch on macOS).
if(APPLE)
    list(APPEND CORE_SOURCES src/update/updater_macos.m)
else()
    list(APPEND CORE_SOURCES src/update/updater_stub.c)
endif()

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
        src/llm/llm_model.c
        src/llm/llm_sampler.c
        src/llm/llm_engine.c
        src/translate/translate_local.c)

    # Metal GPU backend — opt-in, falls back to the CPU engine when off
    # or unavailable. Needs USE_METAL (shared device infra) + Apple.
    if(LIU_LLM_METAL AND USE_METAL AND APPLE)
        set(LIU_LLM_METAL_ACTIVE ON)
        list(APPEND CORE_SOURCES src/llm/llm_metal.m)
        # Compile the Objective-C glue as a normal object. Release builds
        # add a global -flto; LTO'ing this mixed C/ObjC translation unit
        # makes the linker drop its symbols into an undefined state in
        # lto.o. It is thin glue (the GPU kernels carry the work), so a
        # non-LTO object costs nothing and links cleanly. Applies to
        # every target that compiles this source (Liu + test_llm_smoke).
        set_source_files_properties(src/llm/llm_metal.m PROPERTIES
            COMPILE_OPTIONS "-fno-lto")
        # LLM compute kernels -> sibling metallib (loaded next to the binary).
        set(LLM_METAL_SHADER ${CMAKE_SOURCE_DIR}/src/llm/llm_metal_kernels.metal)
        set(LLM_METAL_AIR ${CMAKE_BINARY_DIR}/LiuLLM.air)
        set(LLM_METALLIB ${CMAKE_BINARY_DIR}/LiuLLM.metallib)
        add_custom_command(
            OUTPUT ${LLM_METALLIB}
            COMMAND xcrun -sdk macosx metal -I${CMAKE_SOURCE_DIR}/src/llm
                    -c ${LLM_METAL_SHADER} -o ${LLM_METAL_AIR}
            COMMAND xcrun -sdk macosx metallib ${LLM_METAL_AIR} -o ${LLM_METALLIB}
            DEPENDS ${LLM_METAL_SHADER} ${CMAKE_SOURCE_DIR}/src/llm/llm_metal_params.h
                    ${CMAKE_SOURCE_DIR}/src/llm/llm_stq1_0_codebook.h
            COMMENT "Compiling LLM Metal kernels"
        )
        add_custom_target(llm_metal_shaders DEPENDS ${LLM_METALLIB})
    endif()
endif()

if(USE_METAL AND APPLE)
    # Add Metal renderer source
    list(APPEND CORE_SOURCES src/renderer/renderer_metal.m)
    # font.c needs Objective-C for Metal texture API
    set_source_files_properties(src/renderer/font.c PROPERTIES LANGUAGE OBJC)
endif()

add_executable(Liu
    ${CORE_SOURCES}
    ${PLATFORM_SOURCES}
    ${ASM_SOURCES}
)

if(FB_ICON_SOURCE)
    target_compile_definitions(Liu PRIVATE HAVE_FB_ICON=1)
endif()

# Lets settings.c resolve assets/sounds/ in dev builds (running the binary
# straight out of build/). The .app bundle path takes precedence at runtime
# — this fallback only kicks in when _NSGetExecutablePath doesn't land in a
# Contents/MacOS layout. Quoted so the string survives the compiler. */
target_compile_definitions(Liu PRIVATE
    LIU_ASSETS_DIR="${CMAKE_SOURCE_DIR}/assets")

if(USE_LOCAL_LLM)
    target_compile_definitions(Liu PRIVATE LIU_HAVE_LOCAL_LLM=1)
endif()

if(LIU_LLM_METAL_ACTIVE)
    target_compile_definitions(Liu PRIVATE LIU_LLM_METAL=1)
    add_dependencies(Liu llm_metal_shaders)
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
    # Embed an Info.plist into the bare Mach-O so dev builds (running
    # ./build/Liu directly, with no .app bundle) still carry the microphone
    # usage description macOS TCC requires before AVAudioRecorder can show
    # its permission prompt. The packaged .app uses its own
    # Contents/Info.plist (scripts/package_macos_dmg.sh) and ignores this
    # embedded section.
    set(LIU_EMBED_PLIST "${CMAKE_BINARY_DIR}/Liu-embedded-Info.plist")
    file(WRITE "${LIU_EMBED_PLIST}"
"<?xml version=\"1.0\" encoding=\"UTF-8\"?>
<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">
<plist version=\"1.0\">
<dict>
    <key>CFBundleIdentifier</key>
    <string>com.github.calculusteam.Liu</string>
    <key>CFBundleName</key>
    <string>Liu</string>
    <key>NSMicrophoneUsageDescription</key>
    <string>Liu records short audio clips you choose as custom notification sounds.</string>
</dict>
</plist>
")
    target_link_options(Liu PRIVATE
        "SHELL:-sectcreate __TEXT __info_plist ${LIU_EMBED_PLIST}")
    # Relink when the embedded plist content changes (it's only a linker
    # argument, not an input object, so CMake wouldn't otherwise notice).
    set_property(TARGET Liu APPEND PROPERTY LINK_DEPENDS "${LIU_EMBED_PLIST}")
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

add_executable(liu-notify ${NOTIFY_SOURCES} ${NOTIFY_ASM_SOURCES})

target_include_directories(liu-notify PRIVATE
    src
    third_party
)

target_link_libraries(liu-notify PRIVATE ${NOTIFY_PLATFORM_LIBS})

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
    src/history/parser_commandcode.c
    src/history/parser_cline.c
    src/history/parser_run.c
    src/history/scanner.c
    src/history/util.c
    src/core/memory.c
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
    target_link_libraries(liu-history PRIVATE sqlite3)

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
# Dev test + fuzz harnesses (local-only — the tests/ tree is NOT published to the
# public repo). Guard the whole section on tests/ existing so a clone WITHOUT it
# configures cleanly, while a local checkout WITH tests/ builds them as before.
# =============================================================================
if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests")

# =============================================================================
# notify_validate tests
# =============================================================================

# Stand-alone corpus tester (no sanitizer runtime required). Builds with any
# C11 compiler.
option(BUILD_NOTIFY_TESTS "Build notify test binaries (stand-alone)" OFF)
if(BUILD_NOTIFY_TESTS)
    add_executable(test_notify_validate
        tests/test_notify_validate.c
        src/notify/protocol.c
        src/core/utf8.c
        src/core/memory.c
        src/core/string_utils.c
        ${NOTIFY_ASM_SOURCES}
    )
    target_include_directories(test_notify_validate PRIVATE src third_party)
endif()

# Smart Vault tests — crypto + vault primitives. macOS-only (other platforms
# have no real crypto backend wired up yet, so tests would fail loud).
# Reuses the vendored OpenSSL already built for libssh2.
option(BUILD_VAULT_TESTS "Build Smart Vault unit tests" OFF)
if(BUILD_VAULT_TESTS AND APPLE)
    add_executable(test_vault_smart
        tests/test_vault_smart.c
        src/vault/crypto.c
        src/vault/vault.c
    )
    target_include_directories(test_vault_smart PRIVATE
        src
        third_party
        third_party/sqlite3
        ${LIBSSH2_INCLUDE_DIRS}
    )
    target_link_libraries(test_vault_smart PRIVATE
        sqlite3
        ${LIBSSH2_LIBRARIES}
        ${SECURITY_FRAMEWORK}
    )
    target_compile_definitions(test_vault_smart PRIVATE PLATFORM_MACOS=1)
    if(LIBSSH2_BOOTSTRAP_TARGET)
        add_dependencies(test_vault_smart ${LIBSSH2_BOOTSTRAP_TARGET})
    endif()
endif()

# Auto-updater semver unit test (pure logic; uses the stub platform shim so it
# needs no Cocoa). Built on demand: cmake --build build --target test_semver
add_executable(test_semver EXCLUDE_FROM_ALL
    tests/test_semver.c
    src/update/updater.c
    src/update/updater_stub.c
    third_party/cjson/cJSON.c
)
target_include_directories(test_semver PRIVATE
    src third_party third_party/cjson ${LIBSSH2_INCLUDE_DIRS})
target_link_libraries(test_semver PRIVATE ${LIBSSH2_LIBRARIES})
set_source_files_properties(third_party/cjson/cJSON.c PROPERTIES COMPILE_FLAGS "-w")
if(LIBSSH2_BOOTSTRAP_TARGET)
    add_dependencies(test_semver ${LIBSSH2_BOOTSTRAP_TARGET})
endif()

# Translate prompt segmenter unit test (pure logic; no deps). Asserts paths,
# URLs, @refs and [Image #N] chips are kept verbatim while text segments are
# split out for translation. Build: cmake --build build --target test_translate_segment
add_executable(test_translate_segment EXCLUDE_FROM_ALL
    tests/test_translate_segment.c
    src/translate/translate_segment.c
)
target_include_directories(test_translate_segment PRIVATE src)

# Model downloader test (real curl + streaming SHA-256). Point it at a local
# `python3 -m http.server` serving the GGUF for a hermetic end-to-end run.
# Build: cmake --build build --target test_model_download
add_executable(test_model_download EXCLUDE_FROM_ALL
    tests/test_model_download.c
    src/translate/model_download.c
)
target_include_directories(test_model_download PRIVATE src third_party ${LIBSSH2_INCLUDE_DIRS})
target_link_libraries(test_model_download PRIVATE ${LIBSSH2_LIBRARIES})
if(NOT APPLE)
    target_link_libraries(test_model_download PRIVATE pthread)
endif()

# Knowledge-graph force-layout test (pure logic; no Cocoa/renderer). Asserts the
# layout settles and nodes don't interpenetrate on a synthetic 150-note vault.
# Built on demand: cmake --build build --target test_graph_layout
# utf8.c/memory.c gate their C fallbacks behind USE_ASM (set globally), so the
# per-arch asm is linked the same way liu-notify does it; unused fast_* symbols
# get dead-stripped.
set(GRAPH_TEST_ASM_SOURCES "")
if(USE_ASM AND CMAKE_SYSTEM_PROCESSOR MATCHES "arm64|aarch64")
    set(GRAPH_TEST_ASM_SOURCES
        src/asm/memops_arm64.S
        src/asm/utf8_arm64.S
    )
elseif(USE_ASM AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|amd64|AMD64")
    set(GRAPH_TEST_ASM_SOURCES
        src/asm/memops_x86_64.S
        src/asm/utf8_x86_64.S
    )
endif()
add_executable(test_graph_layout EXCLUDE_FROM_ALL
    tests/test_graph_layout.c
    src/ui/markdown/md_graph.c
    src/ui/markdown/md_parse.c
    src/core/memory.c
    src/core/utf8.c
    ${GRAPH_TEST_ASM_SOURCES}
)
target_include_directories(test_graph_layout PRIVATE src)

# Auto-updater functional end-to-end harness: drives the REAL state machine
# (feed parse + SHA-256/Ed25519 verify + ditto extract + atomic install +
# relaunch) against signed artifacts, no network. macOS only (needs the Cocoa
# shim + libcrypto). Build: cmake --build build --target test_update_e2e
if(APPLE)
    add_executable(test_update_e2e EXCLUDE_FROM_ALL
        tests/test_update_e2e.c
        src/update/updater.c
        src/update/updater_macos.m
        third_party/cjson/cJSON.c
    )
    target_include_directories(test_update_e2e PRIVATE
        src third_party third_party/cjson ${LIBSSH2_INCLUDE_DIRS})
    target_link_libraries(test_update_e2e PRIVATE
        ${LIBSSH2_LIBRARIES} ${COCOA_FRAMEWORK})
    target_compile_definitions(test_update_e2e PRIVATE PLATFORM_MACOS=1)
    if(LIBSSH2_BOOTSTRAP_TARGET)
        add_dependencies(test_update_e2e ${LIBSSH2_BOOTSTRAP_TARGET})
    endif()

    # Feed-parser gate tests (https-pinning, yank fallback, arch/min-os, malformed)
    # driven through the real state machine with no network and no fixtures.
    # Build: cmake --build build --target test_update_feed
    add_executable(test_update_feed EXCLUDE_FROM_ALL
        tests/test_update_feed.c
        src/update/updater.c
        src/update/updater_macos.m
        third_party/cjson/cJSON.c
    )
    target_include_directories(test_update_feed PRIVATE
        src third_party third_party/cjson ${LIBSSH2_INCLUDE_DIRS})
    target_link_libraries(test_update_feed PRIVATE
        ${LIBSSH2_LIBRARIES} ${COCOA_FRAMEWORK})
    target_compile_definitions(test_update_feed PRIVATE PLATFORM_MACOS=1)
    if(LIBSSH2_BOOTSTRAP_TARGET)
        add_dependencies(test_update_feed ${LIBSSH2_BOOTSTRAP_TARGET})
    endif()
endif()

# Local LLM tests — STQ1_0 dequant unit test (standalone, fast) and the
# engine smoke harness (needs a real GGUF model on disk). Only meaningful
# alongside USE_LOCAL_LLM since they pull in src/llm/*.
option(BUILD_LLM_TESTS "Build local LLM test harnesses" OFF)
if(BUILD_LLM_TESTS AND USE_LOCAL_LLM)
    add_executable(test_llm_quant
        tests/test_llm_quant.c
        src/llm/llm_quant_stq1_0.c
    )
    target_include_directories(test_llm_quant PRIVATE src third_party)

    set(LLM_SMOKE_SOURCES
        tests/test_llm_smoke.c
        src/llm/llm_gguf.c
        src/llm/llm_quant_stq1_0.c
        src/llm/llm_quant_q6k.c
        src/llm/llm_stq1_0_kernel.c
        src/llm/llm_tokenizer.c
        src/llm/llm_model.c
        src/llm/llm_sampler.c
        src/llm/llm_engine.c
        src/core/utf8.c
        src/core/memory.c
        ${NOTIFY_ASM_SOURCES}
    )
    if(LIU_LLM_METAL_ACTIVE)
        list(APPEND LLM_SMOKE_SOURCES src/llm/llm_metal.m)
    endif()
    add_executable(test_llm_smoke ${LLM_SMOKE_SOURCES})
    target_include_directories(test_llm_smoke PRIVATE src third_party)
    if(NOT APPLE)
        # libm + pthread for the CPU pool (Apple folds both into libSystem).
        target_link_libraries(test_llm_smoke PRIVATE m pthread)
    endif()
    if(LIU_LLM_METAL_ACTIVE)
        target_compile_definitions(test_llm_smoke PRIVATE LIU_LLM_METAL=1)
        # Foundation pulls the Objective-C runtime + NSString/NSURL/NSError;
        # Metal provides MTLDevice/etc. (The main Liu target gets these via
        # the Cocoa framework in PLATFORM_LIBS.)
        find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
        target_link_libraries(test_llm_smoke PRIVATE
            ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
        add_dependencies(test_llm_smoke llm_metal_shaders)
    endif()

    # Lifecycle test for the local-LLM Translate backend: verifies the model
    # is loaded per-translation and freed the instant it finishes (no
    # residency between runs). Pulls in the translate module on top of the
    # same engine sources as the smoke test.
    set(LLM_LIFECYCLE_SOURCES
        tests/test_translate_local_lifecycle.c
        src/translate/translate_local.c
        src/translate/translate.c
        src/llm/llm_gguf.c
        src/llm/llm_quant_stq1_0.c
        src/llm/llm_quant_q6k.c
        src/llm/llm_stq1_0_kernel.c
        src/llm/llm_tokenizer.c
        src/llm/llm_model.c
        src/llm/llm_sampler.c
        src/llm/llm_engine.c
        src/core/utf8.c
        src/core/memory.c
        ${NOTIFY_ASM_SOURCES}
    )
    if(LIU_LLM_METAL_ACTIVE)
        list(APPEND LLM_LIFECYCLE_SOURCES src/llm/llm_metal.m)
    endif()
    add_executable(test_translate_local_lifecycle ${LLM_LIFECYCLE_SOURCES})
    target_include_directories(test_translate_local_lifecycle PRIVATE src third_party)
    if(NOT APPLE)
        target_link_libraries(test_translate_local_lifecycle PRIVATE m pthread)
    endif()
    if(LIU_LLM_METAL_ACTIVE)
        target_compile_definitions(test_translate_local_lifecycle PRIVATE LIU_LLM_METAL=1)
        find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
        target_link_libraries(test_translate_local_lifecycle PRIVATE
            ${METAL_FRAMEWORK} ${FOUNDATION_FRAMEWORK})
        add_dependencies(test_translate_local_lifecycle llm_metal_shaders)
    endif()
elseif(BUILD_LLM_TESTS AND NOT USE_LOCAL_LLM)
    message(STATUS "BUILD_LLM_TESTS=ON ignored — needs USE_LOCAL_LLM=ON")
endif()

# libFuzzer harness (opt-in). Requires a clang toolchain with the fuzzer
# runtime: stock Apple Clang on macOS does NOT ship it — use Homebrew LLVM:
#     brew install llvm
#     cmake -B build-fuzz -DUSE_LIBFUZZER=ON \
#           -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang
option(USE_LIBFUZZER "Build the notify_validate libFuzzer harness (needs full clang+fuzzer)" OFF)
if(USE_LIBFUZZER)
    if(NOT CMAKE_C_COMPILER_ID STREQUAL "Clang" AND NOT CMAKE_C_COMPILER_ID STREQUAL "AppleClang")
        message(FATAL_ERROR "USE_LIBFUZZER requires a clang toolchain.")
    endif()
    add_executable(fuzz_notify_validate
        tests/fuzz_notify_validate.c
        src/notify/protocol.c
        src/core/utf8.c
        src/core/memory.c
        src/core/string_utils.c
    )
    target_include_directories(fuzz_notify_validate PRIVATE src third_party)
    target_compile_options(fuzz_notify_validate PRIVATE
        -fsanitize=fuzzer,address,undefined -g -O1)
    target_link_options(fuzz_notify_validate PRIVATE
        -fsanitize=fuzzer,address,undefined)
endif()

endif()  # tests/ present (dev-only tree, not published to the public repo)
