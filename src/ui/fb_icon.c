/*
 * liu filebrowser — extension → icon mapping.
 *
 * The table covers 110+ programming languages, build systems, data formats,
 * media types, archives and special filenames (Dockerfile, Makefile, …).
 * Codepoints come from the Nerd Font 3.x cheat sheet (Private-Use Area) and
 * remain as a fallback when compiled PNG assets are unavailable.
 * Brand colours are the widely-recognised language/product identities.
 */
#include "ui/fb_icon.h"

#include <ctype.h>
#include <string.h>

#define RGB(r, g, b) ((Color){ (r)/255.0f, (g)/255.0f, (b)/255.0f, 1.0f })

/* Commonly reused colours — must be macros (not static const) because C11
   does not consider const-qualified structs as compile-time constants in
   aggregate initialisers. */
#define C_FOLDER   ((Color){ 0.32f, 0.63f, 0.93f, 1.0f })  /* #519FED */
#define C_ACCENT   ((Color){ 0.82f, 0.82f, 0.85f, 1.0f })
#define C_MUTED    ((Color){ 0.58f, 0.60f, 0.64f, 1.0f })

/* Brand-ish palette */
#define C_C          RGB( 92, 107, 192)   /* #5C6BC0 */
#define C_CPP        RGB(  0,  89, 156)   /* #00599C */
#define C_CSHARP     RGB(104,  33, 122)   /* #68217A */
#define C_PY         RGB(255, 212,  59)   /* #FFD43B */
#define C_RUST       RGB(222, 165, 132)   /* #DEA584 */
#define C_GO         RGB(  0, 173, 216)   /* #00ADD8 */
#define C_JS         RGB(247, 223,  30)   /* #F7DF1E */
#define C_TS         RGB( 49, 120, 198)   /* #3178C6 */
#define C_JAVA       RGB(237, 139,   0)   /* #ED8B00 */
#define C_RUBY       RGB(204,  52,  45)   /* #CC342D */
#define C_PHP        RGB(119, 123, 180)   /* #777BB4 */
#define C_SWIFT      RGB(240,  81,  56)   /* #F05138 */
#define C_KOTLIN     RGB(169, 123, 255)   /* #A97BFF */
#define C_ZIG        RGB(247, 164,  29)   /* #F7A41D */
#define C_LUA        RGB(  0,   0, 128)   /* #000080 */
#define C_HTML       RGB(227,  79,  38)   /* #E34F26 */
#define C_CSS        RGB( 21, 114, 182)   /* #1572B6 */
#define C_VUE        RGB( 65, 184, 131)   /* #41B883 */
#define C_REACT      RGB( 97, 218, 251)   /* #61DAFB */
#define C_SVELTE     RGB(255,  62,   0)   /* #FF3E00 */
#define C_JSON       RGB(203, 166, 109)   /* tan */
#define C_YAML       RGB(203,  23,  30)   /* #CB171E */
#define C_MARKDOWN   RGB(  8,  63, 161)   /* #083FA1 */
#define C_SHELL      RGB( 78, 170,  37)   /* #4EAA25 */
#define C_DOCKER     RGB( 13, 183, 237)   /* #0DB7ED */
#define C_GIT        RGB(240,  80,  50)   /* #F05032 */
#define C_IMAGE      RGB(226, 119, 153)   /* pink */
#define C_VIDEO      RGB(179, 123, 232)   /* purple */
#define C_AUDIO      RGB(112, 180, 215)   /* sky */
#define C_ARCHIVE    RGB(245, 193,  87)   /* amber */
#define C_PDF        RGB(220,  80,  68)   /* red */
#define C_CONFIG     RGB(150, 160, 180)   /* slate */
#define C_BUILD      RGB(197, 156,  96)   /* sepia */
#define C_LOCK       RGB(140, 140, 160)   /* grey */

/* Nerd Font codepoints (Nerd Fonts 3.x cheat-sheet positions). */
#define NF_FOLDER         0xF07Bu
#define NF_FOLDER_OPEN    0xF07Cu
#define NF_FILE           0xF15Bu
#define NF_FILE_CODE      0xF1C9u
#define NF_FILE_TEXT      0xF0F6u
#define NF_FILE_IMAGE     0xF1C5u
#define NF_FILE_VIDEO     0xF1C8u
#define NF_FILE_AUDIO     0xF1C7u
#define NF_FILE_ARCHIVE   0xF1C6u
#define NF_FILE_PDF       0xF1C1u
#define NF_TERMINAL       0xEA85u
#define NF_GEAR           0xF013u

#define NF_C              0xE61Eu
#define NF_CPP            0xE61Du
#define NF_CSHARP         0xE648u
#define NF_PYTHON         0xE606u
#define NF_RUST           0xE7A8u
#define NF_GO             0xE627u
#define NF_JS             0xE781u
#define NF_TS             0xE628u
#define NF_JAVA           0xE738u
#define NF_RUBY           0xE739u
#define NF_PHP            0xE73Du
#define NF_SWIFT          0xE755u
#define NF_KOTLIN         0xE634u
#define NF_HTML           0xE736u
#define NF_CSS            0xE749u
#define NF_SASS           0xE603u
#define NF_REACT          0xE7BAu
#define NF_VUE            0xE6A0u
#define NF_SVELTE         0xE697u
#define NF_NODE           0xE718u
#define NF_NPM            0xE71Eu
#define NF_JSON           0xE60Bu
#define NF_YAML           0xE6A8u
#define NF_TOML           0xE615u
#define NF_XML            0xE619u
#define NF_SQL            0xE706u
#define NF_MARKDOWN       0xE609u
#define NF_SHELL          0xE795u
#define NF_DOCKER         0xE650u
#define NF_GIT            0xE702u
#define NF_LOCK           0xF023u
#define NF_CMAKE          0xE7B3u
#define NF_ZIG            0xE6A9u
#define NF_LUA            0xE620u
#define NF_SCALA          0xE737u

/* Lowercase `s` into `dst[cap]`, returning length (excl. NUL). ASCII-only.  */
static size_t lc_copy(char *dst, size_t cap, const char *s) {
    size_t i;
    for (i = 0; i + 1 < cap && s[i]; i++) {
        unsigned char c = (unsigned char)s[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c + 32) : (char)c;
    }
    dst[i] = '\0';
    return i;
}

static bool eq_ci(const char *a, const char *b) {
    while (*a && *b) {
        unsigned char ca = (unsigned char)*a++, cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca += 32;
        if (cb >= 'A' && cb <= 'Z') cb += 32;
        if (ca != cb) return false;
    }
    return *a == 0 && *b == 0;
}

typedef struct { const char *ext; u32 cp; Color color; } ExtMap;

/* Matched case-insensitively; `.` stripped before lookup. First match wins. */
static const ExtMap g_ext_map[] = {
    /* systems / C family */
    { "c",        NF_C,       C_C },
    { "h",        NF_C,       C_C },
    { "cc",       NF_CPP,     C_CPP },
    { "cpp",      NF_CPP,     C_CPP },
    { "cxx",      NF_CPP,     C_CPP },
    { "c++",      NF_CPP,     C_CPP },
    { "hpp",      NF_CPP,     C_CPP },
    { "hxx",      NF_CPP,     C_CPP },
    { "hh",       NF_CPP,     C_CPP },
    { "cs",       NF_CSHARP,  C_CSHARP },
    { "fs",       NF_CSHARP,  C_CSHARP },   /* F# close enough */
    { "m",        NF_C,       C_C },        /* Objective-C */
    { "mm",       NF_CPP,     C_CPP },      /* Objective-C++ */

    /* scripting */
    { "py",       NF_PYTHON,  C_PY },
    { "pyc",      NF_PYTHON,  C_PY },
    { "pyi",      NF_PYTHON,  C_PY },
    { "ipynb",    NF_PYTHON,  C_PY },
    { "rb",       NF_RUBY,    C_RUBY },
    { "erb",      NF_RUBY,    C_RUBY },
    { "php",      NF_PHP,     C_PHP },
    { "lua",      NF_LUA,     C_LUA },
    { "pl",       NF_FILE_CODE, C_MUTED },  /* Perl */
    { "pm",       NF_FILE_CODE, C_MUTED },
    { "r",        NF_FILE_CODE, C_MUTED },
    { "jl",       NF_FILE_CODE, C_MUTED },  /* Julia */

    /* modern compiled */
    { "rs",       NF_RUST,    C_RUST },
    { "go",       NF_GO,      C_GO },
    { "zig",      NF_ZIG,     C_ZIG },
    { "swift",    NF_SWIFT,   C_SWIFT },
    { "kt",       NF_KOTLIN,  C_KOTLIN },
    { "kts",      NF_KOTLIN,  C_KOTLIN },
    { "scala",    NF_SCALA,   C_CSHARP },
    { "sbt",      NF_SCALA,   C_CSHARP },
    { "dart",     NF_FILE_CODE, C_TS },

    /* JVM */
    { "java",     NF_JAVA,    C_JAVA },
    { "class",    NF_JAVA,    C_JAVA },
    { "jar",      NF_JAVA,    C_JAVA },
    { "groovy",   NF_JAVA,    C_JAVA },
    { "gradle",   NF_JAVA,    C_JAVA },

    /* JS/TS ecosystem */
    { "js",       NF_JS,      C_JS },
    { "mjs",      NF_JS,      C_JS },
    { "cjs",      NF_JS,      C_JS },
    { "jsx",      NF_REACT,   C_REACT },
    { "ts",       NF_TS,      C_TS },
    { "tsx",      NF_REACT,   C_TS },
    { "vue",      NF_VUE,     C_VUE },
    { "svelte",   NF_SVELTE,  C_SVELTE },

    /* web */
    { "html",     NF_HTML,    C_HTML },
    { "htm",      NF_HTML,    C_HTML },
    { "xhtml",    NF_HTML,    C_HTML },
    { "css",      NF_CSS,     C_CSS },
    { "scss",     NF_SASS,    C_HTML },
    { "sass",     NF_SASS,    C_HTML },
    { "less",     NF_SASS,    C_HTML },
    { "styl",     NF_SASS,    C_HTML },

    /* data */
    { "json",     NF_JSON,    C_JSON },
    { "jsonc",    NF_JSON,    C_JSON },
    { "json5",    NF_JSON,    C_JSON },
    { "yaml",     NF_YAML,    C_YAML },
    { "yml",      NF_YAML,    C_YAML },
    { "toml",     NF_TOML,    C_CONFIG },
    { "xml",      NF_XML,     C_HTML },
    { "csv",      NF_FILE_TEXT, C_MUTED },
    { "tsv",      NF_FILE_TEXT, C_MUTED },
    { "sql",      NF_SQL,     C_TS },
    { "proto",    NF_FILE_CODE, C_MUTED },

    /* docs / markup */
    { "md",       NF_MARKDOWN, C_MARKDOWN },
    { "markdown", NF_MARKDOWN, C_MARKDOWN },
    { "mdx",      NF_MARKDOWN, C_MARKDOWN },
    { "rst",      NF_FILE_TEXT, C_MARKDOWN },
    { "adoc",     NF_FILE_TEXT, C_MARKDOWN },
    { "txt",      NF_FILE_TEXT, C_MUTED },
    { "log",      NF_FILE_TEXT, C_MUTED },
    { "tex",      NF_FILE_TEXT, C_C },

    /* PDF / office */
    { "pdf",      NF_FILE_PDF, C_PDF },
    { "doc",      NF_FILE_TEXT, C_CPP },
    { "docx",     NF_FILE_TEXT, C_CPP },
    { "odt",      NF_FILE_TEXT, C_CPP },
    { "xls",      NF_FILE_TEXT, C_GO },
    { "xlsx",     NF_FILE_TEXT, C_GO },
    { "ppt",      NF_FILE_TEXT, C_SWIFT },
    { "pptx",     NF_FILE_TEXT, C_SWIFT },

    /* shell */
    { "sh",       NF_SHELL,   C_SHELL },
    { "bash",     NF_SHELL,   C_SHELL },
    { "zsh",      NF_SHELL,   C_SHELL },
    { "fish",     NF_SHELL,   C_SHELL },
    { "ksh",      NF_SHELL,   C_SHELL },
    { "ps1",      NF_SHELL,   C_TS },
    { "bat",      NF_TERMINAL, C_TS },
    { "cmd",      NF_TERMINAL, C_TS },

    /* images */
    { "png",      NF_FILE_IMAGE, C_IMAGE },
    { "jpg",      NF_FILE_IMAGE, C_IMAGE },
    { "jpeg",     NF_FILE_IMAGE, C_IMAGE },
    { "gif",      NF_FILE_IMAGE, C_IMAGE },
    { "webp",     NF_FILE_IMAGE, C_IMAGE },
    { "svg",      NF_FILE_IMAGE, C_HTML },
    { "ico",      NF_FILE_IMAGE, C_IMAGE },
    { "bmp",      NF_FILE_IMAGE, C_IMAGE },
    { "tif",      NF_FILE_IMAGE, C_IMAGE },
    { "tiff",     NF_FILE_IMAGE, C_IMAGE },
    { "heic",     NF_FILE_IMAGE, C_IMAGE },
    { "raw",      NF_FILE_IMAGE, C_IMAGE },
    { "psd",      NF_FILE_IMAGE, C_JS },   /* yellow */

    /* video */
    { "mp4",      NF_FILE_VIDEO, C_VIDEO },
    { "mov",      NF_FILE_VIDEO, C_VIDEO },
    { "mkv",      NF_FILE_VIDEO, C_VIDEO },
    { "avi",      NF_FILE_VIDEO, C_VIDEO },
    { "webm",     NF_FILE_VIDEO, C_VIDEO },
    { "wmv",      NF_FILE_VIDEO, C_VIDEO },
    { "flv",      NF_FILE_VIDEO, C_VIDEO },
    { "m4v",      NF_FILE_VIDEO, C_VIDEO },

    /* audio */
    { "mp3",      NF_FILE_AUDIO, C_AUDIO },
    { "wav",      NF_FILE_AUDIO, C_AUDIO },
    { "flac",     NF_FILE_AUDIO, C_AUDIO },
    { "ogg",      NF_FILE_AUDIO, C_AUDIO },
    { "m4a",      NF_FILE_AUDIO, C_AUDIO },
    { "aac",      NF_FILE_AUDIO, C_AUDIO },
    { "opus",     NF_FILE_AUDIO, C_AUDIO },

    /* archives */
    { "zip",      NF_FILE_ARCHIVE, C_ARCHIVE },
    { "tar",      NF_FILE_ARCHIVE, C_ARCHIVE },
    { "gz",       NF_FILE_ARCHIVE, C_ARCHIVE },
    { "tgz",      NF_FILE_ARCHIVE, C_ARCHIVE },
    { "bz2",      NF_FILE_ARCHIVE, C_ARCHIVE },
    { "xz",       NF_FILE_ARCHIVE, C_ARCHIVE },
    { "7z",       NF_FILE_ARCHIVE, C_ARCHIVE },
    { "rar",      NF_FILE_ARCHIVE, C_ARCHIVE },
    { "dmg",      NF_FILE_ARCHIVE, C_ARCHIVE },
    { "iso",      NF_FILE_ARCHIVE, C_ARCHIVE },

    /* misc / exec */
    { "exe",      NF_TERMINAL, C_CPP },
    { "dll",      NF_TERMINAL, C_CPP },
    { "so",       NF_TERMINAL, C_CPP },
    { "dylib",    NF_TERMINAL, C_CPP },
    { "a",        NF_TERMINAL, C_CPP },
    { "o",        NF_TERMINAL, C_CPP },

    /* build / package */
    { "mk",       NF_FILE_CODE, C_BUILD },

    /* asm */
    { "asm",      NF_FILE_CODE, C_C },
    { "s",        NF_FILE_CODE, C_C },

    /* sentinel */
    { NULL, 0, {0, 0, 0, 0} }
};

typedef struct { const char *name; u32 cp; Color color; } NameMap;

/* Exact (case-insensitive) basename matches — override extension lookup. */
static const NameMap g_name_map[] = {
    { "Dockerfile",          NF_DOCKER,   C_DOCKER },
    { ".dockerignore",       NF_DOCKER,   C_DOCKER },
    { "Makefile",            NF_FILE_CODE,C_BUILD },
    { "makefile",            NF_FILE_CODE,C_BUILD },
    { "GNUmakefile",         NF_FILE_CODE,C_BUILD },
    { "CMakeLists.txt",      NF_CMAKE,    C_BUILD },
    { "Cargo.toml",          NF_RUST,     C_RUST },
    { "Cargo.lock",          NF_LOCK,     C_LOCK },
    { "go.mod",              NF_GO,       C_GO },
    { "go.sum",              NF_GO,       C_GO },
    { "package.json",        NF_NPM,      C_HTML },
    { "package-lock.json",   NF_LOCK,     C_LOCK },
    { "yarn.lock",           NF_LOCK,     C_LOCK },
    { "pnpm-lock.yaml",      NF_LOCK,     C_LOCK },
    { "tsconfig.json",       NF_TS,       C_TS },
    { "jsconfig.json",       NF_JS,       C_JS },
    { ".babelrc",            NF_JSON,     C_JS },
    { ".eslintrc",           NF_JSON,     C_TS },
    { ".prettierrc",         NF_JSON,     C_MUTED },
    { ".gitignore",          NF_GIT,      C_GIT },
    { ".gitattributes",      NF_GIT,      C_GIT },
    { ".gitmodules",         NF_GIT,      C_GIT },
    { ".gitconfig",          NF_GIT,      C_GIT },
    { ".git",                NF_GIT,      C_GIT },   /* dir too */
    { ".env",                NF_GEAR,     C_JS },
    { ".env.local",          NF_GEAR,     C_JS },
    { ".env.example",        NF_GEAR,     C_MUTED },
    { "README.md",           NF_MARKDOWN, C_MARKDOWN },
    { "README",              NF_FILE_TEXT,C_MARKDOWN },
    { "LICENSE",             NF_FILE_TEXT,C_PY },
    { "LICENSE.md",          NF_FILE_TEXT,C_PY },
    { "LICENSE.txt",         NF_FILE_TEXT,C_PY },
    { "COPYING",             NF_FILE_TEXT,C_PY },
    { "CHANGELOG.md",        NF_MARKDOWN, C_MARKDOWN },
    { "CONTRIBUTING.md",     NF_MARKDOWN, C_MARKDOWN },
    { "Gemfile",             NF_RUBY,     C_RUBY },
    { "Gemfile.lock",        NF_LOCK,     C_LOCK },
    { "Rakefile",            NF_RUBY,     C_RUBY },
    { "requirements.txt",    NF_PYTHON,   C_PY },
    { "setup.py",            NF_PYTHON,   C_PY },
    { "pyproject.toml",      NF_PYTHON,   C_PY },
    { "Pipfile",             NF_PYTHON,   C_PY },
    { "Pipfile.lock",        NF_LOCK,     C_LOCK },
    { ".bashrc",             NF_SHELL,    C_SHELL },
    { ".zshrc",              NF_SHELL,    C_SHELL },
    { ".profile",            NF_SHELL,    C_SHELL },
    { ".vimrc",              NF_FILE_CODE,C_SHELL },
    { "Brewfile",            NF_FILE_CODE,C_SHELL },
    { NULL, 0, {0,0,0,0} }
};

static FbAssetIconKind fb_asset_for_cp(u32 cp) {
    switch (cp) {
    case NF_FOLDER:
    case NF_FOLDER_OPEN:
        return FB_ASSET_ICON_FOLDER;
    case NF_FILE:
        return FB_ASSET_ICON_FILE;
    case NF_FILE_TEXT:
    case NF_MARKDOWN:
    case NF_JSON:
    case NF_YAML:
    case NF_TOML:
    case NF_XML:
    case NF_SQL:
        return FB_ASSET_ICON_TEXT;
    case NF_FILE_IMAGE:
        return FB_ASSET_ICON_IMAGE;
    case NF_FILE_VIDEO:
        return FB_ASSET_ICON_VIDEO;
    case NF_FILE_AUDIO:
        return FB_ASSET_ICON_AUDIO;
    case NF_FILE_ARCHIVE:
        return FB_ASSET_ICON_ARCHIVE;
    case NF_FILE_PDF:
        return FB_ASSET_ICON_PDF;
    case NF_TERMINAL:
        return FB_ASSET_ICON_TERMINAL;
    case NF_GEAR:
        return FB_ASSET_ICON_GEAR;
    case NF_GIT:
        return FB_ASSET_ICON_GIT;
    case NF_LOCK:
        return FB_ASSET_ICON_LOCK;
    default:
        return FB_ASSET_ICON_CODE;
    }
}

/* Per-language logo asset lookup (devicon PNGs, MIT). Kept separate from the
 * cp/colour tables above so the logo resolves even for extensions the legacy
 * Nerd-Font table never listed (Haskell, Elixir, Crystal, …). The Nerd-Font
 * cp/colour still serve as the glyph fallback when a PNG fails to decode. */
typedef struct { const char *key; FbAssetIconKind asset; } LangAsset;

static const LangAsset g_lang_ext[] = {
    { "c",         FB_ASSET_ICON_LANG_C },
    { "h",         FB_ASSET_ICON_LANG_C },
    { "cpp",       FB_ASSET_ICON_LANG_CPP },
    { "cc",        FB_ASSET_ICON_LANG_CPP },
    { "cxx",       FB_ASSET_ICON_LANG_CPP },
    { "c++",       FB_ASSET_ICON_LANG_CPP },
    { "hpp",       FB_ASSET_ICON_LANG_CPP },
    { "hxx",       FB_ASSET_ICON_LANG_CPP },
    { "hh",        FB_ASSET_ICON_LANG_CPP },
    { "ipp",       FB_ASSET_ICON_LANG_CPP },
    { "tpp",       FB_ASSET_ICON_LANG_CPP },
    { "cs",        FB_ASSET_ICON_LANG_CSHARP },
    { "csx",       FB_ASSET_ICON_LANG_CSHARP },
    { "py",        FB_ASSET_ICON_LANG_PYTHON },
    { "pyc",       FB_ASSET_ICON_LANG_PYTHON },
    { "pyi",       FB_ASSET_ICON_LANG_PYTHON },
    { "pyw",       FB_ASSET_ICON_LANG_PYTHON },
    { "pyx",       FB_ASSET_ICON_LANG_PYTHON },
    { "ipynb",     FB_ASSET_ICON_LANG_PYTHON },
    { "rs",        FB_ASSET_ICON_LANG_RUST },
    { "go",        FB_ASSET_ICON_LANG_GO },
    { "js",        FB_ASSET_ICON_LANG_JS },
    { "mjs",       FB_ASSET_ICON_LANG_JS },
    { "cjs",       FB_ASSET_ICON_LANG_JS },
    { "ts",        FB_ASSET_ICON_LANG_TS },
    { "java",      FB_ASSET_ICON_LANG_JAVA },
    { "class",     FB_ASSET_ICON_LANG_JAVA },
    { "jar",       FB_ASSET_ICON_LANG_JAVA },
    { "rb",        FB_ASSET_ICON_LANG_RUBY },
    { "erb",       FB_ASSET_ICON_LANG_RUBY },
    { "gemspec",   FB_ASSET_ICON_LANG_RUBY },
    { "php",       FB_ASSET_ICON_LANG_PHP },
    { "phtml",     FB_ASSET_ICON_LANG_PHP },
    { "swift",     FB_ASSET_ICON_LANG_SWIFT },
    { "kt",        FB_ASSET_ICON_LANG_KOTLIN },
    { "kts",       FB_ASSET_ICON_LANG_KOTLIN },
    { "scala",     FB_ASSET_ICON_LANG_SCALA },
    { "sbt",       FB_ASSET_ICON_LANG_SCALA },
    { "dart",      FB_ASSET_ICON_LANG_DART },
    { "lua",       FB_ASSET_ICON_LANG_LUA },
    { "pl",        FB_ASSET_ICON_LANG_PERL },
    { "pm",        FB_ASSET_ICON_LANG_PERL },
    { "pod",       FB_ASSET_ICON_LANG_PERL },
    { "r",         FB_ASSET_ICON_LANG_R },
    { "jl",        FB_ASSET_ICON_LANG_JULIA },
    { "hs",        FB_ASSET_ICON_LANG_HASKELL },
    { "lhs",       FB_ASSET_ICON_LANG_HASKELL },
    { "ex",        FB_ASSET_ICON_LANG_ELIXIR },
    { "exs",       FB_ASSET_ICON_LANG_ELIXIR },
    { "erl",       FB_ASSET_ICON_LANG_ERLANG },
    { "hrl",       FB_ASSET_ICON_LANG_ERLANG },
    { "clj",       FB_ASSET_ICON_LANG_CLOJURE },
    { "cljs",      FB_ASSET_ICON_LANG_CLOJURE },
    { "cljc",      FB_ASSET_ICON_LANG_CLOJURE },
    { "edn",       FB_ASSET_ICON_LANG_CLOJURE },
    { "ml",        FB_ASSET_ICON_LANG_OCAML },
    { "mli",       FB_ASSET_ICON_LANG_OCAML },
    { "fs",        FB_ASSET_ICON_LANG_FSHARP },
    { "fsx",       FB_ASSET_ICON_LANG_FSHARP },
    { "fsi",       FB_ASSET_ICON_LANG_FSHARP },
    { "m",         FB_ASSET_ICON_LANG_OBJC },
    { "mm",        FB_ASSET_ICON_LANG_OBJC },
    { "groovy",    FB_ASSET_ICON_LANG_GROOVY },
    { "nim",       FB_ASSET_ICON_LANG_NIM },
    { "nims",      FB_ASSET_ICON_LANG_NIM },
    { "cr",        FB_ASSET_ICON_LANG_CRYSTAL },
    { "sol",       FB_ASSET_ICON_LANG_SOLIDITY },
    { "f",         FB_ASSET_ICON_LANG_FORTRAN },
    { "f90",       FB_ASSET_ICON_LANG_FORTRAN },
    { "f95",       FB_ASSET_ICON_LANG_FORTRAN },
    { "f03",       FB_ASSET_ICON_LANG_FORTRAN },
    { "for",       FB_ASSET_ICON_LANG_FORTRAN },
    { "hx",        FB_ASSET_ICON_LANG_HAXE },
    { "zig",       FB_ASSET_ICON_LANG_ZIG },
    { "ps1",       FB_ASSET_ICON_LANG_POWERSHELL },
    { "psm1",      FB_ASSET_ICON_LANG_POWERSHELL },
    { "psd1",      FB_ASSET_ICON_LANG_POWERSHELL },
    { "sh",        FB_ASSET_ICON_LANG_SHELL },
    { "bash",      FB_ASSET_ICON_LANG_SHELL },
    { "zsh",       FB_ASSET_ICON_LANG_SHELL },
    { "fish",      FB_ASSET_ICON_LANG_SHELL },
    { "ksh",       FB_ASSET_ICON_LANG_SHELL },
    { "html",      FB_ASSET_ICON_LANG_HTML },
    { "htm",       FB_ASSET_ICON_LANG_HTML },
    { "xhtml",     FB_ASSET_ICON_LANG_HTML },
    { "css",       FB_ASSET_ICON_LANG_CSS },
    { "scss",      FB_ASSET_ICON_LANG_SASS },
    { "sass",      FB_ASSET_ICON_LANG_SASS },
    { "styl",      FB_ASSET_ICON_LANG_STYLUS },
    { "jsx",       FB_ASSET_ICON_LANG_REACT },
    { "tsx",       FB_ASSET_ICON_LANG_REACT },
    { "vue",       FB_ASSET_ICON_LANG_VUE },
    { "svelte",    FB_ASSET_ICON_LANG_SVELTE },
    { "json",      FB_ASSET_ICON_LANG_JSON },
    { "jsonc",     FB_ASSET_ICON_LANG_JSON },
    { "json5",     FB_ASSET_ICON_LANG_JSON },
    { "yaml",      FB_ASSET_ICON_LANG_YAML },
    { "yml",       FB_ASSET_ICON_LANG_YAML },
    { "md",        FB_ASSET_ICON_LANG_MARKDOWN },
    { "markdown",  FB_ASSET_ICON_LANG_MARKDOWN },
    { "mdx",       FB_ASSET_ICON_LANG_MARKDOWN },
    { "tex",       FB_ASSET_ICON_LANG_LATEX },
    { "sty",       FB_ASSET_ICON_LANG_LATEX },
    { "cls",       FB_ASSET_ICON_LANG_LATEX },
    { "graphql",   FB_ASSET_ICON_LANG_GRAPHQL },
    { "gql",       FB_ASSET_ICON_LANG_GRAPHQL },
    { "cmake",     FB_ASSET_ICON_LANG_CMAKE },
    { "gradle",    FB_ASSET_ICON_LANG_GRADLE },
    { "tf",        FB_ASSET_ICON_LANG_TERRAFORM },
    { "tfvars",    FB_ASSET_ICON_LANG_TERRAFORM },
    { "vb",        FB_ASSET_ICON_LANG_VB },
    { "vbs",       FB_ASSET_ICON_LANG_VB },
    { "pas",       FB_ASSET_ICON_LANG_DELPHI },
    { "dpr",       FB_ASSET_ICON_LANG_DELPHI },
    { "dfm",       FB_ASSET_ICON_LANG_DELPHI },
    { "mat",       FB_ASSET_ICON_LANG_MATLAB },
    { NULL, FB_ASSET_ICON_NONE }
};

static const LangAsset g_lang_name[] = {
    { "requirements.txt",      FB_ASSET_ICON_LANG_PYTHON },
    { "setup.py",              FB_ASSET_ICON_LANG_PYTHON },
    { "pyproject.toml",        FB_ASSET_ICON_LANG_PYTHON },
    { "Pipfile",               FB_ASSET_ICON_LANG_PYTHON },
    { "Cargo.toml",            FB_ASSET_ICON_LANG_RUST },
    { "go.mod",                FB_ASSET_ICON_LANG_GO },
    { "go.sum",                FB_ASSET_ICON_LANG_GO },
    { "jsconfig.json",         FB_ASSET_ICON_LANG_JS },
    { "tsconfig.json",         FB_ASSET_ICON_LANG_TS },
    { "Gemfile",               FB_ASSET_ICON_LANG_RUBY },
    { "Rakefile",              FB_ASSET_ICON_LANG_RUBY },
    { ".bashrc",               FB_ASSET_ICON_LANG_SHELL },
    { ".zshrc",                FB_ASSET_ICON_LANG_SHELL },
    { ".profile",              FB_ASSET_ICON_LANG_SHELL },
    { ".bash_profile",         FB_ASSET_ICON_LANG_SHELL },
    { "README.md",             FB_ASSET_ICON_LANG_MARKDOWN },
    { "CHANGELOG.md",          FB_ASSET_ICON_LANG_MARKDOWN },
    { "CONTRIBUTING.md",       FB_ASSET_ICON_LANG_MARKDOWN },
    { "Dockerfile",            FB_ASSET_ICON_LANG_DOCKER },
    { ".dockerignore",         FB_ASSET_ICON_LANG_DOCKER },
    { "docker-compose.yml",    FB_ASSET_ICON_LANG_DOCKER },
    { "docker-compose.yaml",   FB_ASSET_ICON_LANG_DOCKER },
    { "CMakeLists.txt",        FB_ASSET_ICON_LANG_CMAKE },
    { ".gitignore",            FB_ASSET_ICON_LANG_GIT },
    { ".gitattributes",        FB_ASSET_ICON_LANG_GIT },
    { ".gitmodules",           FB_ASSET_ICON_LANG_GIT },
    { ".gitconfig",            FB_ASSET_ICON_LANG_GIT },
    { "package.json",          FB_ASSET_ICON_LANG_NPM },
    { "package-lock.json",     FB_ASSET_ICON_LANG_NPM },
    { "yarn.lock",             FB_ASSET_ICON_LANG_YARN },
    { "build.gradle",          FB_ASSET_ICON_LANG_GRADLE },
    { "settings.gradle",       FB_ASSET_ICON_LANG_GRADLE },
    { "bun.lockb",             FB_ASSET_ICON_LANG_BUN },
    { "deno.json",             FB_ASSET_ICON_LANG_DENO },
    { "deno.jsonc",            FB_ASSET_ICON_LANG_DENO },
    { NULL, FB_ASSET_ICON_NONE }
};

static FbAssetIconKind lang_asset_ext(const char *ext_lc) {
    for (const LangAsset *m = g_lang_ext; m->key; m++)
        if (strcmp(ext_lc, m->key) == 0) return m->asset;
    return FB_ASSET_ICON_NONE;
}

static FbAssetIconKind lang_asset_name(const char *name) {
    for (const LangAsset *m = g_lang_name; m->key; m++)
        if (eq_ci(name, m->key)) return m->asset;
    return FB_ASSET_ICON_NONE;
}

FbIcon fb_icon_for(const char *name, bool is_dir) {
    FbIcon out = { 0, C_ACCENT, FB_ASSET_ICON_NONE };
    if (!name || !*name) return out;

    if (is_dir) {
        /* .git is a file OR a dir but the glyph stays the same */
        if (eq_ci(name, ".git")) {
            out.codepoint = NF_GIT;
            out.color = C_GIT;
            out.asset = FB_ASSET_ICON_GIT;
            return out;
        }
        out.codepoint = NF_FOLDER;
        out.color     = C_FOLDER;
        out.asset     = FB_ASSET_ICON_FOLDER;
        return out;
    }

    /* Glyph + colour fallback from the legacy tables: exact name first, then
     * extension. The PNG asset resolved below is what normally renders. */
    bool matched = false;
    for (const NameMap *m = g_name_map; m->name; m++) {
        if (eq_ci(name, m->name)) {
            out.codepoint = m->cp;
            out.color     = m->color;
            matched = true;
            break;
        }
    }
    char ext_lc[32];
    ext_lc[0] = '\0';
    const char *dot = strrchr(name, '.');
    if (dot && dot != name) {
        lc_copy(ext_lc, sizeof ext_lc, dot + 1);
        if (!matched) {
            for (const ExtMap *m = g_ext_map; m->ext; m++) {
                if (strcmp(ext_lc, m->ext) == 0) {
                    out.codepoint = m->cp;
                    out.color     = m->color;
                    matched = true;
                    break;
                }
            }
        }
    }

    /* Prefer a per-language logo PNG: exact filename, then extension. */
    FbAssetIconKind la = lang_asset_name(name);
    if (la == FB_ASSET_ICON_NONE && ext_lc[0]) la = lang_asset_ext(ext_lc);
    if (la != FB_ASSET_ICON_NONE) {
        out.asset = la;
        if (!matched) out.codepoint = NF_FILE_CODE;   /* sensible glyph fallback */
        return out;
    }

    if (matched) {
        out.asset = fb_asset_for_cp(out.codepoint);
        return out;
    }

    /* Generic dotfile (no extension) — subtle gear icon */
    if (name[0] == '.') {
        out.codepoint = NF_GEAR;
        out.color = C_MUTED;
        out.asset = FB_ASSET_ICON_GEAR;
        return out;
    }

    out.codepoint = NF_FILE;
    out.color     = C_ACCENT;
    out.asset     = FB_ASSET_ICON_FILE;
    return out;
}

FbIcon fb_icon_default_file(Color dim_fg) {
    return (FbIcon){ NF_FILE, dim_fg, FB_ASSET_ICON_FILE };
}
