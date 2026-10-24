/*
 * Liu — line-oriented syntax scanner.
 *
 * One pass over one line produces a run of typed spans. It is deliberately
 * lexical and not a parser: a diff hands over fragments of files, out of order
 * and with no guarantee the enclosing construct is even present, so anything
 * that needs real context would be wrong more often than it was right.
 *
 * Continuation across lines (an unterminated block comment, a triple-quoted
 * string, a JS template literal and the `${ }` expressions inside it, a
 * heredoc, a quoted string in a language whose strings may hold a line break)
 * travels in an opaque `state` word. A caller that has the whole file threads
 * it line by line; a caller that starts mid-file — the diff dock, at every
 * hunk boundary — asks syntax_guess_state() for a best effort instead.
 *
 * What is carried is decided per language, in the row's flags: a string that
 * the language ends at the line break (C's "…", Swift's, Go's, a shell's `…`)
 * ends with its line here too and leaves no state, so a missing quote
 * mis-paints one line and never the rest of the file.
 *
 * This is the ONE highlighter in the tree. Three surfaces share it so a
 * language reads the same everywhere it appears:
 *   - the code/text editor           (src/ui/filebrowser.c)
 *   - the git diff dock              (src/ui/git_diff.c)
 *   - Markdown fenced code blocks    (src/ui/markdown/md_render.c)
 * Adding a language here lights it up in all three at once; adding one to a
 * private lexer in any of them is what this file exists to prevent.
 *
 * Component files are several languages at once: a Vue/Svelte/Astro file, an
 * HTML page or a Jinja/ERB template is markup carrying <script> (JavaScript),
 * <style> (CSS), a `---` frontmatter, `{ }` / `{{ }}` expressions or `<% %>`
 * code. The scanner switches language at each of those boundaries and the
 * region it is inside travels in the same `state` word, so the sub-language
 * is always one of the ordinary rows below — there is no "JS as seen from a
 * <script> tag" variant to drift from the real one.
 */
#ifndef UI_SYNTAX_H
#define UI_SYNTAX_H

#include "core/types.h"
#include "core/config.h"
#include "renderer/renderer.h"

typedef enum {
    SYN_TEXT = 0,
    SYN_KEYWORD,     /* control flow, declarations */
    SYN_TYPE,        /* built-in and user type names, shell/PHP variables */
    SYN_STRING,      /* "…", '…', `…`, """…""" */
    SYN_COMMENT,
    SYN_NUMBER,
    SYN_PREPROC,     /* #include, #define, and shebangs */
    SYN_FUNC,        /* identifier immediately followed by '(' */
    SYN_TAG,         /* markup element names, CSS selectors, [toml.sections] */
    SYN_ATTR,        /* attribute names, mapping keys, @annotations, #[attrs] */
    SYN_INSERT,      /* a unified diff's added line   (SYN_LANG_DIFF only) */
    SYN_DELETE,      /* a unified diff's removed line (SYN_LANG_DIFF only) */
    SYN_KIND_COUNT,
} SynKind;

/* Recognized languages. Values 0..4 are pinned because the diff dock stores
 * them as a `u8` alongside cached hunks. `SYN_LANG_C`, `_SCRIPT`, `_MAKE`, and
 * `_MARKUP` are broad family values refined by the entries below. */
typedef enum {
    SYN_LANG_NONE = 0,
    SYN_LANG_C,        /* C / C++ / Objective-C — block + line comments, cpp */
    SYN_LANG_SCRIPT,   /* generic '#'-comment config (ini, conf, env, perl) */
    SYN_LANG_MAKE,     /* Makefile / CMake */
    SYN_LANG_MARKUP,   /* XML-ish tag markup (svg, plist, xaml) — embeds <script>/<style> */
    SYN_LANG_PYTHON,
    SYN_LANG_JS,       /* JavaScript / TypeScript / JSX / TSX */
    SYN_LANG_GO,
    SYN_LANG_RUST,
    SYN_LANG_SWIFT,
    SYN_LANG_JVM,      /* Java / Kotlin / Scala / C# / Dart / Groovy */
    SYN_LANG_SHELL,    /* sh / bash / zsh / fish */
    SYN_LANG_RUBY,
    SYN_LANG_LUA,
    SYN_LANG_SQL,
    SYN_LANG_HTML,     /* HTML — embeds <script>/<style>, {{ }} and {% %} */
    SYN_LANG_CSS,      /* CSS / SCSS / LESS */
    SYN_LANG_JSON,     /* JSON / JSONC */
    SYN_LANG_YAML,
    SYN_LANG_TOML,     /* TOML / INI */
    SYN_LANG_ASM,      /* GAS / NASM */
    SYN_LANG_PHP,
    /* Component / template hosts: markup rows that switch language inside
     * <script>, <style>, `---`, `{ }`, `{{ }}`, `{% %}` and `<% %>` regions. */
    SYN_LANG_VUE,      /* Vue SFC: {{ }} mustaches, :prop/@event/v-* directives */
    SYN_LANG_SVELTE,   /* Svelte: { } expressions, {#if}/{:else}/{/if} blocks */
    SYN_LANG_ASTRO,    /* Astro: --- TypeScript frontmatter, JSX-style { } */
    SYN_LANG_TEMPLATE, /* Jinja / Twig / Nunjucks / Liquid / Handlebars over HTML */
    SYN_LANG_JINJA,    /* the {% %} / {{ }} statement language on its own */
    SYN_LANG_ERB,      /* <% %> Ruby over HTML */
    SYN_LANG_EJS,      /* <% %> JavaScript over HTML */
    SYN_LANG_HEEX,     /* <% %> Elixir over HTML (EEx / HEEx, plus {@assigns}) */
    SYN_LANG_JSP,      /* <% %> Java / C# over HTML (JSP, ASP.NET Web Forms) */
    /* Plain rows. */
    SYN_LANG_ZIG,
    SYN_LANG_ELIXIR,
    SYN_LANG_ERLANG,
    SYN_LANG_HASKELL,  /* Haskell / Elm / PureScript / Idris */
    SYN_LANG_OCAML,    /* OCaml / F# / Reason */
    SYN_LANG_LISP,     /* Clojure / Common Lisp / Scheme / Racket / Emacs Lisp */
    SYN_LANG_R,
    SYN_LANG_JULIA,
    SYN_LANG_NIM,
    SYN_LANG_HCL,      /* Terraform / HCL / Nomad / Packer */
    SYN_LANG_NIX,
    SYN_LANG_DOCKER,   /* Dockerfile / Containerfile */
    SYN_LANG_POWERSHELL,
    SYN_LANG_BATCH,    /* .bat / .cmd */
    SYN_LANG_GRAPHQL,
    SYN_LANG_PROTO,    /* Protocol Buffers / Thrift / FlatBuffers */
    SYN_LANG_PRISMA,
    SYN_LANG_SOLIDITY,
    SYN_LANG_LATEX,    /* LaTeX / BibTeX */
    SYN_LANG_DIFF,     /* unified diff / patch — whole-line SYN_INSERT/SYN_DELETE */
    SYN_LANG_WASM,     /* WebAssembly text format */
    SYN_LANG_JSONNET,
    SYN_LANG_GOMOD,    /* go.mod / go.work */
    SYN_LANG_COUNT,
} SynLang;

typedef struct {
    u32 off;   /* byte offset into the scanned line */
    u32 len;
    u8  kind;  /* SynKind */
} SynSpan;

/* A comfortable per-line span budget for callers that don't want to think
 * about it. A 200-column line of dense code lands well under this. */
#define SYN_MAX_SPANS 192

/* Language for a path, by extension and then by basename (Makefile, Dockerfile,
 * CMakeLists.txt). `len` is a byte count — the path need not be NUL-terminated,
 * which is what lets a diff pass a slice of its own buffer. */
SynLang syntax_lang_for_path(const char *path, usize len);

/* Language for a Markdown fence info string (```python, ```rs, ```bash). Same
 * non-NUL-terminated contract. Accepts both the spelled-out name and the file
 * extension, and ignores anything after the first space or '{' so
 * "```js title=x" still resolves. */
SynLang syntax_lang_for_tag(const char *tag, usize len);

/* Display name of the scanner ROW ("C", "Python", "YAML"); "" for
 * SYN_LANG_NONE. Rows are buckets — one JS lexer serves .ts — so a status bar
 * wants syntax_lang_label() instead. */
const char *syntax_lang_name(SynLang lang);

/* The token that starts a line comment in the row ("//", "#", "--"), or NULL
 * when the row has none (a language with block comments only, plain text).
 * Where a row lists several, the first. What a "toggle comment" key writes. */
const char *syntax_line_comment(SynLang lang);

/* Display name for the FILE the path names ("TypeScript", "Astro",
 * "Dockerfile", "Objective-C++"), resolved through the same extension and
 * basename table syntax_lang_for_path() uses so the two can never disagree
 * about what a file is. NULL when the table has nothing to say — the caller
 * decides how to spell an unknown extension. Same non-NUL-terminated contract
 * as syntax_lang_for_path(). */
const char *syntax_lang_label(const char *path, usize len);

/* Scan one line. `*state` carries continuation in and out; zero it at the start
 * of a file. Returns the number of spans written (spans tile the line in order
 * and never overlap). Text the scanner has no opinion about is left out
 * entirely, so the caller draws it in its default colour. */
i32 syntax_scan(SynLang lang, const char *line, u32 len, u32 *state,
                SynSpan *out, i32 cap);

/* Same lexing, spans discarded — for walking the lines above the viewport to
 * recover the continuation state without paying for span bookkeeping. */
void syntax_advance_state(SynLang lang, const char *line, u32 len, u32 *state);

/* Best-effort continuation state for a line whose predecessors are unknown.
 * Recognises the shapes a mid-file fragment gives away — most obviously a
 * line whose first non-space character is the star of a C block comment. */
u32 syntax_guess_state(SynLang lang, const char *line, u32 len);

/* Theme colour for a token kind. SYN_TEXT returns the theme foreground. */
Color syntax_color(const Theme *t, SynKind kind);

#endif /* UI_SYNTAX_H */
