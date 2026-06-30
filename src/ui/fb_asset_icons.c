/*
 * Liu — decode and cache compiled-in filebrowser PNG icons.
 * Build input: ${CMAKE_BINARY_DIR}/generated/ui/fb_asset_icons_data.h
 */
#include "ui/fb_asset_icons.h"
#include "stb_image.h"
#include "ui/fb_asset_icons_data.h"

typedef struct {
    const u8 *png_data;
    i32       png_size;
    const u8 *rgba;
    i32       w, h;
    bool      tried;
} FbAssetIconSlot;

static FbAssetIconSlot g_fb_asset_icons[FB_ASSET_ICON_COUNT] = {
    [FB_ASSET_ICON_NONE]     = { NULL, 0, NULL, 0, 0, false },
    [FB_ASSET_ICON_FOLDER]   = { FB_ICON_FOLDER_data,   FB_ICON_FOLDER_size,   NULL, 0, 0, false },
    [FB_ASSET_ICON_FILE]     = { FB_ICON_FILE_data,     FB_ICON_FILE_size,     NULL, 0, 0, false },
    [FB_ASSET_ICON_CODE]     = { FB_ICON_CODE_data,     FB_ICON_CODE_size,     NULL, 0, 0, false },
    [FB_ASSET_ICON_TEXT]     = { FB_ICON_TEXT_data,     FB_ICON_TEXT_size,     NULL, 0, 0, false },
    [FB_ASSET_ICON_IMAGE]    = { FB_ICON_IMAGE_data,    FB_ICON_IMAGE_size,    NULL, 0, 0, false },
    [FB_ASSET_ICON_VIDEO]    = { FB_ICON_VIDEO_data,    FB_ICON_VIDEO_size,    NULL, 0, 0, false },
    [FB_ASSET_ICON_AUDIO]    = { FB_ICON_AUDIO_data,    FB_ICON_AUDIO_size,    NULL, 0, 0, false },
    [FB_ASSET_ICON_ARCHIVE]  = { FB_ICON_ARCHIVE_data,  FB_ICON_ARCHIVE_size,  NULL, 0, 0, false },
    [FB_ASSET_ICON_PDF]      = { FB_ICON_PDF_data,      FB_ICON_PDF_size,      NULL, 0, 0, false },
    [FB_ASSET_ICON_TERMINAL] = { FB_ICON_TERMINAL_data, FB_ICON_TERMINAL_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_GEAR]     = { FB_ICON_GEAR_data,     FB_ICON_GEAR_size,     NULL, 0, 0, false },
    [FB_ASSET_ICON_GIT]      = { FB_ICON_GIT_data,      FB_ICON_GIT_size,      NULL, 0, 0, false },
    [FB_ASSET_ICON_LOCK]     = { FB_ICON_LOCK_data,     FB_ICON_LOCK_size,     NULL, 0, 0, false },
    [FB_ASSET_ICON_UP]       = { FB_ICON_UP_data,       FB_ICON_UP_size,       NULL, 0, 0, false },
    [FB_ASSET_ICON_REFRESH]  = { FB_ICON_REFRESH_data,  FB_ICON_REFRESH_size,  NULL, 0, 0, false },
    [FB_ASSET_ICON_CLOSE]    = { FB_ICON_CLOSE_data,    FB_ICON_CLOSE_size,    NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_C         ] = { FB_ICON_LANG_C_data, FB_ICON_LANG_C_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_CPP       ] = { FB_ICON_LANG_CPP_data, FB_ICON_LANG_CPP_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_CSHARP    ] = { FB_ICON_LANG_CSHARP_data, FB_ICON_LANG_CSHARP_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_PYTHON    ] = { FB_ICON_LANG_PYTHON_data, FB_ICON_LANG_PYTHON_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_RUST      ] = { FB_ICON_LANG_RUST_data, FB_ICON_LANG_RUST_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_GO        ] = { FB_ICON_LANG_GO_data, FB_ICON_LANG_GO_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_JS        ] = { FB_ICON_LANG_JS_data, FB_ICON_LANG_JS_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_TS        ] = { FB_ICON_LANG_TS_data, FB_ICON_LANG_TS_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_JAVA      ] = { FB_ICON_LANG_JAVA_data, FB_ICON_LANG_JAVA_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_RUBY      ] = { FB_ICON_LANG_RUBY_data, FB_ICON_LANG_RUBY_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_PHP       ] = { FB_ICON_LANG_PHP_data, FB_ICON_LANG_PHP_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_SWIFT     ] = { FB_ICON_LANG_SWIFT_data, FB_ICON_LANG_SWIFT_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_KOTLIN    ] = { FB_ICON_LANG_KOTLIN_data, FB_ICON_LANG_KOTLIN_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_SCALA     ] = { FB_ICON_LANG_SCALA_data, FB_ICON_LANG_SCALA_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_DART      ] = { FB_ICON_LANG_DART_data, FB_ICON_LANG_DART_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_LUA       ] = { FB_ICON_LANG_LUA_data, FB_ICON_LANG_LUA_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_PERL      ] = { FB_ICON_LANG_PERL_data, FB_ICON_LANG_PERL_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_R         ] = { FB_ICON_LANG_R_data, FB_ICON_LANG_R_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_JULIA     ] = { FB_ICON_LANG_JULIA_data, FB_ICON_LANG_JULIA_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_HASKELL   ] = { FB_ICON_LANG_HASKELL_data, FB_ICON_LANG_HASKELL_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_ELIXIR    ] = { FB_ICON_LANG_ELIXIR_data, FB_ICON_LANG_ELIXIR_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_ERLANG    ] = { FB_ICON_LANG_ERLANG_data, FB_ICON_LANG_ERLANG_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_CLOJURE   ] = { FB_ICON_LANG_CLOJURE_data, FB_ICON_LANG_CLOJURE_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_OCAML     ] = { FB_ICON_LANG_OCAML_data, FB_ICON_LANG_OCAML_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_FSHARP    ] = { FB_ICON_LANG_FSHARP_data, FB_ICON_LANG_FSHARP_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_OBJC      ] = { FB_ICON_LANG_OBJC_data, FB_ICON_LANG_OBJC_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_GROOVY    ] = { FB_ICON_LANG_GROOVY_data, FB_ICON_LANG_GROOVY_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_NIM       ] = { FB_ICON_LANG_NIM_data, FB_ICON_LANG_NIM_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_CRYSTAL   ] = { FB_ICON_LANG_CRYSTAL_data, FB_ICON_LANG_CRYSTAL_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_SOLIDITY  ] = { FB_ICON_LANG_SOLIDITY_data, FB_ICON_LANG_SOLIDITY_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_FORTRAN   ] = { FB_ICON_LANG_FORTRAN_data, FB_ICON_LANG_FORTRAN_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_HAXE      ] = { FB_ICON_LANG_HAXE_data, FB_ICON_LANG_HAXE_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_ZIG       ] = { FB_ICON_LANG_ZIG_data, FB_ICON_LANG_ZIG_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_POWERSHELL] = { FB_ICON_LANG_POWERSHELL_data, FB_ICON_LANG_POWERSHELL_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_SHELL     ] = { FB_ICON_LANG_SHELL_data, FB_ICON_LANG_SHELL_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_HTML      ] = { FB_ICON_LANG_HTML_data, FB_ICON_LANG_HTML_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_CSS       ] = { FB_ICON_LANG_CSS_data, FB_ICON_LANG_CSS_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_SASS      ] = { FB_ICON_LANG_SASS_data, FB_ICON_LANG_SASS_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_STYLUS    ] = { FB_ICON_LANG_STYLUS_data, FB_ICON_LANG_STYLUS_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_REACT     ] = { FB_ICON_LANG_REACT_data, FB_ICON_LANG_REACT_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_VUE       ] = { FB_ICON_LANG_VUE_data, FB_ICON_LANG_VUE_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_SVELTE    ] = { FB_ICON_LANG_SVELTE_data, FB_ICON_LANG_SVELTE_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_JSON      ] = { FB_ICON_LANG_JSON_data, FB_ICON_LANG_JSON_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_YAML      ] = { FB_ICON_LANG_YAML_data, FB_ICON_LANG_YAML_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_MARKDOWN  ] = { FB_ICON_LANG_MARKDOWN_data, FB_ICON_LANG_MARKDOWN_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_LATEX     ] = { FB_ICON_LANG_LATEX_data, FB_ICON_LANG_LATEX_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_GRAPHQL   ] = { FB_ICON_LANG_GRAPHQL_data, FB_ICON_LANG_GRAPHQL_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_DOCKER    ] = { FB_ICON_LANG_DOCKER_data, FB_ICON_LANG_DOCKER_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_CMAKE     ] = { FB_ICON_LANG_CMAKE_data, FB_ICON_LANG_CMAKE_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_GIT       ] = { FB_ICON_LANG_GIT_data, FB_ICON_LANG_GIT_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_NPM       ] = { FB_ICON_LANG_NPM_data, FB_ICON_LANG_NPM_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_YARN      ] = { FB_ICON_LANG_YARN_data, FB_ICON_LANG_YARN_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_GRADLE    ] = { FB_ICON_LANG_GRADLE_data, FB_ICON_LANG_GRADLE_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_TERRAFORM ] = { FB_ICON_LANG_TERRAFORM_data, FB_ICON_LANG_TERRAFORM_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_BUN       ] = { FB_ICON_LANG_BUN_data, FB_ICON_LANG_BUN_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_DENO      ] = { FB_ICON_LANG_DENO_data, FB_ICON_LANG_DENO_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_VB        ] = { FB_ICON_LANG_VB_data, FB_ICON_LANG_VB_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_DELPHI    ] = { FB_ICON_LANG_DELPHI_data, FB_ICON_LANG_DELPHI_size, NULL, 0, 0, false },
    [FB_ASSET_ICON_LANG_MATLAB    ] = { FB_ICON_LANG_MATLAB_data, FB_ICON_LANG_MATLAB_size, NULL, 0, 0, false },
};

const u8 *fb_asset_icon_rgba(FbAssetIconKind kind, i32 *w, i32 *h) {
    if (kind <= FB_ASSET_ICON_NONE || kind >= FB_ASSET_ICON_COUNT) return NULL;
    FbAssetIconSlot *s = &g_fb_asset_icons[kind];
    if (!s->png_data || s->png_size <= 0) return NULL;

    if (!s->tried) {
        s->tried = true;
        int iw = 0, ih = 0, ch = 0;
        u8 *px = stbi_load_from_memory(s->png_data, s->png_size, &iw, &ih, &ch, 4);
        if (px) {
            s->rgba = px;
            s->w = iw;
            s->h = ih;
        }
    }

    if (!s->rgba) return NULL;
    if (w) *w = s->w;
    if (h) *h = s->h;
    return s->rgba;
}

const void *fb_asset_icon_cache_key(FbAssetIconKind kind) {
    if (kind <= FB_ASSET_ICON_NONE || kind >= FB_ASSET_ICON_COUNT) return NULL;
    return &g_fb_asset_icons[kind];
}
