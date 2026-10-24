/*
 * Liu — line-oriented syntax scanner (see syntax.h).
 *
 * Table-driven: every language is one `LangDef` row of keyword tables, comment
 * tokens and behaviour flags, and one scanner walks them all. That is what
 * keeps "add a language" a data change instead of another hand-rolled lexer.
 *
 * Embedded regions. A markup row can declare that parts of its file are
 * another language: <script> (JS) and <style> (CSS) via F_EMBED, an Astro
 * `---` frontmatter via F_FRONT, `{ }` expressions via F_BRACE, `{{ }}` /
 * `{% %}` / `{# #}` via F_TMPL and `<% %>` via F_ERB. `scan_line` runs the host
 * row until one of those opens, then runs the SUB-language's row over the
 * region and the host again after it closes. The region kind is carried in
 * the state word (bits 12..15) next to the sub-language's own continuation
 * bits, so a <script> block or a multi-line `{ }` expression resumes correctly
 * on the next line. Sub-languages are the ordinary rows: the JavaScript inside
 * a Vue <script> is lexed by exactly the code that lexes a .js file.
 */
#include "ui/syntax.h"

#include <string.h>
#include <strings.h>

/* ---------------------------------------------------------------- state -- */

/* Continuation bits carried between lines. ST_BLOCK is 0x1; the diff dock
 * caches this word per row.
 *
 * While an embedded region is open, bits 0..3, the depth nibble and bits
 * 20..31 belong to the SUB-language (an open JS block comment or template
 * literal inside <script>), and ST_TAG is the HOST's — a `{ }` expression
 * opened inside a tag leaves it set so the host resumes inside that tag when
 * the expression closes. */
#define ST_BLOCK       0x0001u   /* inside a block comment */
#define ST_TRIPLE_D    0x0002u   /* inside a """ string   */
#define ST_TRIPLE_S    0x0004u   /* inside a ''' string   */
#define ST_BACKTICK    0x0008u   /* inside a `…` template literal / raw string */
#define ST_TAG         0x0010u   /* inside an unclosed <tag …            */
#define ST_TAG_SCRIPT  0x0020u   /* … and that open tag is <script       */
#define ST_TAG_STYLE   0x0040u   /* … or <style                          */
#define ST_BODY        0x0080u   /* Astro: past the top, `---` is text now */
#define ST_DEPTH_SHIFT 8         /* bits 8..11: block-comment nesting depth */
#define ST_DEPTH_MASK  0x0F00u
#define ST_EMBED_SHIFT 12        /* bits 12..15: EMB_* region kind */
#define ST_EMBED_MASK  0xF000u
#define ST_BRACE_SHIFT 16        /* bits 16..19: `{ }` expression depth */
#define ST_BRACE_MASK  0x000F0000u
#define ST_STR_SHIFT   20        /* bits 20..22: STR_* — the spanning string that is open */
#define ST_STR_MASK    0x00700000u
#define ST_AUX_SHIFT   23        /* bits 23..31: `${ }` frames, or a heredoc's marker */
#define ST_AUX_MASK    0xFF800000u
/* Everything a sub-language may leave behind, cleared when its region ends. */
#define ST_SUB_MASK    (ST_BLOCK | ST_TRIPLE_D | ST_TRIPLE_S | ST_BACKTICK | \
                        ST_DEPTH_MASK | ST_STR_MASK | ST_AUX_MASK)

/* A string that runs across line breaks in a row whose language lets it. Only one
 * can be open at a time, so it is a number and not a bit each; `…` has its own
 * bit (ST_BACKTICK) beside it. A quote in a row WITHOUT the matching F_*_SPAN
 * flag never gets here: it ends with its line and leaves no state.
 *
 * Two kinds borrow the depth nibble, which is free while no block comment is
 * open: STR_RAW keeps its count of '#' there, STR_HEREDOC its marker's length. */
enum {
    STR_NONE = 0,
    STR_DQ,         /* "…"                                                    */
    STR_SQ,         /* '…'                                                    */
    STR_QQ,         /* ''…''  (Nix indented string)                           */
    STR_RAW,        /* r"…" / r#"…"#  (Rust)                                  */
    STR_HEREDOC,    /* the body of <<WORD, up to the line that is WORD        */
    STR_TEXT,       /* PHP: outside <? … ?> — text until the next <?          */
};

/* The aux bits hold one of two things:
 *
 *   Expression frames — two of four bits, the outer one in bits 23..26. A
 *   `${` in a template literal (a `$(` in a shell's "…") opens an expression
 *   that is code again, and that code can open another string, so "which
 *   string resumes at the brace that closes this expression" is a stack. A
 *   frame is 0 when free; otherwise its low three bits are the expression's
 *   count of open braces plus one, and FRAME_DQ says the string to resume is
 *   "…" rather than the row's `…` / ''…''. A third level is not tracked: its
 *   opener reads as text.
 *
 *   While STR_HEREDOC is open, the heredoc's marker instead: bit 23 says it
 *   may be indented (`<<-`) and bits 26..31 are a hash of it. The marker
 *   itself cannot ride in a word; its length and a hash tell the closing line
 *   from the body well enough for a highlighter. Bits 24..25 count the frames
 *   that were open at the opener (`x="$(cat <<EOF`), so the quoted string
 *   they belong to is open again after the marker. */
#define ST_HD_INDENT   (1u << ST_AUX_SHIFT)
#define ST_HD_FRAMES_SHIFT (ST_AUX_SHIFT + 1)
#define ST_HD_HASH_SHIFT   (ST_AUX_SHIFT + 3)
#define ST_HD_HASH_MASK    0x3Fu
#define FRAME_DQ       0x8u
#define FRAME_DEPTH    0x7u

enum {
    EMB_NONE = 0,
    EMB_SCRIPT,     /* <script> … </script>          : JavaScript          */
    EMB_STYLE,      /* <style> … </style>            : CSS                 */
    EMB_FRONT,      /* --- … ---  (Astro frontmatter): JavaScript          */
    EMB_BRACE,      /* { … }  (Svelte/Astro/HEEx)    : expr row, by depth  */
    EMB_MUSTACHE,   /* {{ … }}                       : expr row            */
    EMB_STMT,       /* {% … %}                       : Jinja               */
    EMB_HASHCMT,    /* {# … #}                       : comment             */
    EMB_DASHCMT,    /* {{!-- … --}}                  : comment             */
    EMB_BANGCMT,    /* {{! … }}                      : comment             */
    EMB_CODE,       /* <% … %>  (ERB/EJS/EEx/JSP)    : expr row            */
    EMB_CODECMT,    /* <%# … %>                      : comment             */
};

static inline u32 st_depth(u32 st)          { return (st & ST_DEPTH_MASK) >> ST_DEPTH_SHIFT; }
static inline u32 st_with_depth(u32 st, u32 d) {
    if (d > 15) d = 15;
    return (st & ~ST_DEPTH_MASK) | (d << ST_DEPTH_SHIFT);
}
static inline u32 st_embed(u32 st)          { return (st & ST_EMBED_MASK) >> ST_EMBED_SHIFT; }
static inline u32 st_with_embed(u32 st, u32 e) {
    return (st & ~ST_EMBED_MASK) | ((e & 15u) << ST_EMBED_SHIFT);
}
static inline u32 st_brace(u32 st)          { return (st & ST_BRACE_MASK) >> ST_BRACE_SHIFT; }
static inline u32 st_with_brace(u32 st, u32 d) {
    if (d > 15) d = 15;
    return (st & ~ST_BRACE_MASK) | (d << ST_BRACE_SHIFT);
}
static inline u32 st_str(u32 st)            { return (st & ST_STR_MASK) >> ST_STR_SHIFT; }
static inline u32 st_with_str(u32 st, u32 k) {
    return (st & ~ST_STR_MASK) | ((k & 7u) << ST_STR_SHIFT);
}
static inline u32 st_frame(u32 st, u32 n)   { return (st >> (ST_AUX_SHIFT + 4 * n)) & 15u; }
static inline u32 st_with_frame(u32 st, u32 n, u32 f) {
    u32 sh = ST_AUX_SHIFT + 4 * n;
    return (st & ~(15u << sh)) | ((f & 15u) << sh);
}
/* The innermost `${ }` expression that is open, or -1 when none is. */
static inline i32 st_frame_top(u32 st) {
    return st_frame(st, 1) ? 1 : st_frame(st, 0) ? 0 : -1;
}
/* One more open brace inside that expression. Past what three bits count it
 * goes uncounted, and the expression ends one brace early. */
static inline u32 st_frame_nest(u32 st) {
    i32 top = st_frame_top(st);
    if (top < 0) return st;
    u32 f = st_frame(st, (u32)top);
    return (f & FRAME_DEPTH) < FRAME_DEPTH ? st_with_frame(st, (u32)top, f + 1) : st;
}

/* ---------------------------------------------------------------- flags -- */

#define F_PREPROC     0x00000001ull  /* '#' directive at line start (C/C++/ObjC) */
#define F_CHARLIT     0x00000002ull  /* '…' is a char literal, not a string      */
#define F_LIFETIME    0x00000004ull  /* 'a is a lifetime/label/type var, not a char */
#define F_BACKTICK    0x00000008ull  /* `…` string; one line unless F_BT_SPAN    */
#define F_TRIPLE      0x00000010ull  /* """…""" / '''…''' strings                */
#define F_TAGS        0x00000020ull  /* <tag attr="v"> markup                    */
#define F_DOLLARVAR   0x00000040ull  /* $var / ${var} / $(cmd)                   */
#define F_ATTR_AT     0x00000080ull  /* @annotation / @decorator / @media        */
#define F_ATTR_HASH   0x00000100ull  /* #[attr] / #![attr]  (Rust)               */
#define F_NESTBLK     0x00000200ull  /* block comments nest (Rust/Swift/Zig)     */
#define F_CI_KW       0x00000400ull  /* keywords match case-insensitively (SQL)  */
#define F_KEY_YAML    0x00000800ull  /* leading `ident:` is a mapping key        */
#define F_KEY_JSON    0x00001000ull  /* `"str":` is an object key                */
#define F_KEY_EQ      0x00002000ull  /* leading `ident =` is a key (TOML/INI)    */
#define F_SECTION     0x00004000ull  /* leading `[section]` header               */
#define F_SEMI_CMT    0x00008000ull  /* ';' starts a line comment                */
#define F_HASH_BOL    0x00010000ull  /* '#' comments only at line start (asm)    */
#define F_CAPS_TYPE   0x00020000ull  /* CamelCase identifier reads as a type     */
#define F_CSS         0x00040000ull  /* CSS selector / property / colour rules   */
#define F_SIGIL_RB    0x00080000ull  /* @ivar, @@cvar, :symbol  (Ruby/Elixir)    */
#define F_ASM         0x00100000ull  /* .directive, %macro, `label:`             */
#define F_TARGET      0x00200000ull  /* leading `name:` is a rule target (make)  */
/* Embedded regions a markup HOST row can open (see the file comment). */
#define F_EMBED       0x00400000ull  /* <script> → JS, <style> → CSS             */
#define F_TMPL        0x00800000ull  /* {{ expr }}, {% stmt %}, {# comment #}    */
#define F_BRACE       0x01000000ull  /* { expr } in text and attribute values    */
#define F_FRONT       0x02000000ull  /* `---` frontmatter at the top is JS       */
#define F_ERB         0x04000000ull  /* <% code %>, <%= %>, <%# comment %>       */
/* Lexical extras for the plain rows. */
#define F_DASH_IDENT  0x08000000ull  /* identifiers carry - ? ! * + < > = /      */
#define F_PRIME_IDENT 0x10000000ull  /* x' is an identifier, not a char (Haskell)*/
#define F_NO_SQUOTE   0x20000000ull  /* ' never opens a string (Lisp quote)      */
#define F_BS_CMD      0x40000000ull  /* \command keywords and $math$ (TeX)       */
#define F_PCTVAR      0x80000000ull  /* %VAR%, %1, %~dp0 (Batch)                 */
#define F_DIFF       0x100000000ull  /* first column decides the whole line      */
#define F_KW_BOL     0x200000000ull  /* keywords only as the line's first word   */
#define F_DOT_IDENT  0x400000000ull  /* identifiers carry '.' (R: is.na)         */
/* Strings that run across line breaks. A row sets these only where its
 * language allows the line break: a string that cannot legally hold one ends
 * with its line and leaves no state, so a typo never repaints the file. */
#define F_DQ_SPAN    0x800000000ull  /* "…" runs across lines                    */
#define F_SQ_SPAN   0x1000000000ull  /* '…' runs across lines                    */
#define F_BT_SPAN   0x2000000000ull  /* `…` runs across lines                    */
#define F_BT_RAW    0x4000000000ull  /* `…` takes no backslash escapes (Go)      */
#define F_INTERP    0x8000000000ull  /* ${ } in `…`, ''…'' and a spanning "…" is code */
#define F_QQ_STR   0x10000000000ull  /* ''…'' indented strings (Nix)             */
#define F_RAW_HASH 0x20000000000ull  /* r"…" / r#"…"# raw strings (Rust)         */
#define F_SH_QUOTE 0x40000000000ull  /* shell quoting: nothing escapes in '…', a
                                      * backslash in code quotes the next byte,
                                      * $( ) holds commands — also inside "…"   */
#define F_HEREDOC  0x80000000000ull  /* <<WORD / <<-WORD … WORD (shell, HCL)     */
#define F_HEREDOC3 0x100000000000ull /* <<<WORD / <<<'WORD' … WORD; (PHP)        */
#define F_CODE_TAGS 0x200000000000ull /* code sits in <? … ?>; the rest is text  */
#define F_REGEX    0x400000000000ull /* /…/ where an operand can start (JS)      */

#define F_HOST (F_EMBED | F_TMPL | F_BRACE | F_FRONT | F_ERB)
/* The flags whose rules are tried on every token in code position. */
#define F_CODE_EXTRAS (F_REGEX | F_CODE_TAGS | F_SH_QUOTE | F_HEREDOC | F_HEREDOC3)

/* ------------------------------------------------------------- keywords -- */

/* One flat table per class, NUL-terminated. Lookup is a linear scan gated on
 * the first character, so a miss costs a byte compare per entry — cheaper to
 * maintain than a hash and only ever run once per identifier on a line that is
 * actually on screen. */

static const char *const KW_C[] = {
    "alignas","alignof","asm","auto","break","case","catch","class","const",
    "consteval","constexpr","constinit","continue","co_await","co_return",
    "co_yield","decltype","default","delete","do","else","enum","explicit",
    "export","extern","final","for","friend","goto","if","inline","mutable",
    "namespace","new","noexcept","nullptr","operator","override","private",
    "protected","public","register","restrict","return","sizeof","static",
    "static_assert","struct","switch","template","this","thread_local","throw",
    "try","typedef","typeid","typename","union","using","virtual","volatile",
    "while","_Atomic","_Generic","_Noreturn","_Static_assert",
    /* Objective-C */
    "@interface","@implementation","@property","@synthesize","@end","@selector",
    "@autoreleasepool","in","out","inout","bycopy","byref","oneway",
    NULL
};
static const char *const TY_C[] = {
    "void","int","char","float","double","long","short","unsigned","signed",
    "bool","size_t","ssize_t","uint8_t","uint16_t","uint32_t","uint64_t",
    "int8_t","int16_t","int32_t","int64_t","intptr_t","uintptr_t","ptrdiff_t",
    "wchar_t","char8_t","char16_t","char32_t","FILE","va_list",
    "u8","u16","u32","u64","i8","i16","i32","i64","f32","f64","usize","isize",
    "true","false","NULL","nullptr","id","instancetype","SEL","IMP","Class",
    "BOOL","YES","NO","nil","Nil","self","super","auto",
    NULL
};

static const char *const KW_PY[] = {
    "and","as","assert","async","await","break","class","continue","def","del",
    "elif","else","except","finally","for","from","global","if","import","in",
    "is","lambda","match","case","nonlocal","not","or","pass","raise","return",
    "try","while","with","yield",
    NULL
};
static const char *const TY_PY[] = {
    "True","False","None","self","cls","int","float","str","bytes","bool",
    "list","dict","set","tuple","frozenset","object","type","complex",
    "Any","Optional","Union","List","Dict","Set","Tuple","Callable","Iterable",
    "Iterator","Sequence","Mapping","NamedTuple","TypedDict","Literal",
    NULL
};

static const char *const KW_JS[] = {
    "abstract","as","async","await","break","case","catch","class","const",
    "constructor","continue","debugger","declare","default","delete","do",
    "else","enum","export","extends","finally","for","from","function","get",
    "if","implements","import","in","infer","instanceof","interface","is",
    "keyof","let","new","of","package","private","protected","public",
    "readonly","return","satisfies","set","static","super","switch","this",
    "throw","try","type","typeof","var","void","while","with","yield",
    NULL
};
static const char *const TY_JS[] = {
    "any","bigint","boolean","never","null","number","object","string","symbol",
    "undefined","unknown","true","false","Array","Boolean","Date","Error",
    "Function","JSON","Map","Math","Number","Object","Promise","Proxy","Reflect",
    "RegExp","Set","String","Symbol","WeakMap","WeakSet","Record","Partial",
    "Readonly","Pick","Omit","console","document","window","globalThis",
    NULL
};

static const char *const KW_GO[] = {
    "break","case","chan","const","continue","default","defer","else",
    "fallthrough","for","func","go","goto","if","import","interface","map",
    "package","range","return","select","struct","switch","type","var",
    NULL
};
static const char *const TY_GO[] = {
    "bool","byte","complex64","complex128","error","float32","float64","int",
    "int8","int16","int32","int64","rune","string","uint","uint8","uint16",
    "uint32","uint64","uintptr","any","true","false","nil","iota",
    "append","cap","close","copy","delete","len","make","new","panic","print",
    "println","recover",
    NULL
};

static const char *const KW_RUST[] = {
    "as","async","await","break","const","continue","crate","dyn","else","enum",
    "extern","fn","for","if","impl","in","let","loop","match","mod","move",
    "mut","pub","ref","return","static","struct","super","trait","type",
    "unsafe","use","where","while","yield","macro_rules","union",
    /* WGSL rides on this row: shader stages and storage classes. */
    "var","switch","case","default","discard","fallthrough","override",
    NULL
};
static const char *const TY_RUST[] = {
    "bool","char","f32","f64","i8","i16","i32","i64","i128","isize","str",
    "u8","u16","u32","u64","u128","usize","String","Vec","Box","Option","Result",
    "Some","None","Ok","Err","Self","self","true","false","Rc","Arc","RefCell",
    "Cell","HashMap","HashSet","BTreeMap","Cow","Iterator","Into","From",
    "vec2","vec3","vec4","mat4x4","f16","sampler","texture_2d",
    NULL
};

static const char *const KW_SWIFT[] = {
    "associatedtype","actor","any","as","async","await","break","case","catch",
    "class","continue","default","defer","deinit","do","else","enum","extension",
    "fallthrough","fileprivate","for","func","guard","if","import","in","indirect",
    "init","inout","internal","is","lazy","let","mutating","nonmutating","open",
    "operator","private","protocol","public","repeat","rethrows","return","some",
    "static","struct","subscript","switch","throw","throws","try","typealias",
    "var","weak","where","while","unowned","convenience","required","override",
    NULL
};
static const char *const TY_SWIFT[] = {
    "Any","AnyObject","Array","Bool","Character","Dictionary","Double","Float",
    "Int","Int8","Int16","Int32","Int64","Never","Optional","Set","String",
    "UInt","UInt8","UInt16","UInt32","UInt64","Void","Result","Error","Data",
    "self","Self","super","nil","true","false","CGFloat","CGRect","CGSize",
    NULL
};

static const char *const KW_JVM[] = {
    "abstract","actual","annotation","as","assert","async","base","break",
    "by","case","catch","checked","class","companion","const","constructor",
    "continue","crossinline","data","default","deferred","delegate","do",
    "dynamic","else","enum","event","expect","explicit","extends","extension",
    "external","factory","final","finally","fixed","for","foreach","fun",
    "get","goto","if","implements","implicit","import","in","infix","init",
    "inline","inner","instanceof","interface","internal","is","it","lateinit",
    "lazy","let","lock","match","namespace","native","new","noinline","object",
    "open","operator","out","override","package","param","partial","private",
    "protected","public","reified","required","return","sealed","set","stackalloc",
    "static","strictfp","super","suspend","switch","synchronized","tailrec",
    "this","throw","throws","trait","transient","try","typealias","typeof",
    "unchecked","unsafe","using","val","value","var","virtual","volatile",
    "when","where","while","with","yield","record","await",
    NULL
};
static const char *const TY_JVM[] = {
    "boolean","byte","char","decimal","double","float","int","long","object",
    "sbyte","short","string","uint","ulong","ushort","void","var","dynamic",
    "Any","Boolean","Byte","Char","Double","Float","Int","Integer","Long",
    "Nothing","Number","Object","Short","String","Unit","List","Map","Set",
    "Array","true","false","null","this","super","base","System","Exception",
    NULL
};

static const char *const KW_SH[] = {
    "if","then","elif","else","fi","for","while","until","do","done","case",
    "esac","in","function","select","time","coproc","return","break","continue",
    "local","declare","typeset","readonly","export","unset","shift","exit",
    "source","alias","unalias","eval","exec","trap","set","shopt","let",
    "and","or","not","end","begin","switch","test",
    NULL
};
static const char *const TY_SH[] = {
    "echo","printf","read","cd","pwd","pushd","popd","dirs","command","builtin",
    "getopts","hash","type","ulimit","umask","wait","jobs","kill","bg","fg",
    "true","false","mkdir","rm","cp","mv","ln","cat","grep","sed","awk","find",
    "xargs","curl","git","make","cmake","python","python3","node","npm","brew",
    NULL
};

static const char *const KW_RUBY[] = {
    "alias","and","begin","break","case","class","def","defined?","do","else",
    "elsif","end","ensure","for","if","in","module","next","not","or","redo",
    "rescue","retry","return","then","undef","unless","until","when","while",
    "yield","require","require_relative","include","extend","attr_accessor",
    "attr_reader","attr_writer","lambda","proc","raise","puts","print","loop",
    NULL
};
static const char *const TY_RUBY[] = {
    "true","false","nil","self","super","__FILE__","__LINE__","__method__",
    "String","Symbol","Integer","Float","Array","Hash","Range","Struct","Proc",
    "Module","Class","Object","Kernel","Comparable","Enumerable",
    NULL
};

static const char *const KW_LUA[] = {
    "and","break","do","else","elseif","end","for","function","goto","if","in",
    "local","not","or","repeat","return","then","until","while",
    NULL
};
static const char *const TY_LUA[] = {
    "nil","true","false","self","_G","_ENV","string","table","math","io","os",
    "coroutine","require","pairs","ipairs","type","tostring","tonumber","print",
    "pcall","xpcall","setmetatable","getmetatable","rawget","rawset","select",
    NULL
};

static const char *const KW_SQL[] = {
    "add","all","alter","and","any","as","asc","begin","between","by","case",
    "cast","check","column","commit","constraint","create","cross","database",
    "default","delete","desc","distinct","drop","else","end","exists","foreign",
    "from","full","group","having","if","in","index","inner","insert","into",
    "is","join","key","left","like","limit","not","null","offset","on","or",
    "order","outer","primary","references","replace","returning","right",
    "rollback","select","set","table","then","transaction","truncate","union",
    "unique","update","using","values","view","when","where","with",
    NULL
};
static const char *const TY_SQL[] = {
    "bigint","binary","bit","blob","boolean","char","date","datetime","decimal",
    "double","float","int","integer","json","numeric","real","serial","smallint",
    "text","time","timestamp","uuid","varchar","true","false","current_date",
    "current_time","current_timestamp",
    NULL
};

static const char *const KW_CSS[] = {
    "charset","import","media","namespace","supports","keyframes","font-face",
    "page","layer","container","property","scope","use","forward","mixin",
    "include","extend","if","else","each","for","while","function","return",
    "important","apply","tailwind","screen",
    NULL
};
static const char *const TY_CSS[] = {
    "absolute","auto","block","bold","border-box","center","column","contain",
    "cover","dashed","dotted","fixed","flex","grid","hidden","inherit","initial",
    "inline","inline-block","italic","left","none","normal","pointer","relative",
    "right","row","solid","static","sticky","transparent","unset","visible",
    "wrap","currentColor","inline-flex","space-between","space-around",
    NULL
};

static const char *const TY_JSON[] = { "true","false","null", NULL };
static const char *const TY_YAML[] = {
    "true","false","null","yes","no","on","off","~","True","False","Null",
    NULL
};
static const char *const KW_YAML[] = { NULL };

static const char *const KW_TOML[] = { NULL };
static const char *const TY_TOML[] = { "true","false","inf","nan", NULL };

static const char *const KW_MAKE[] = {
    "if","ifeq","ifneq","ifdef","ifndef","else","endif","include","define",
    "endef","export","unexport","override","vpath","set","list","string","file",
    "foreach","while","endwhile","endforeach","macro","endmacro","function",
    "endfunction","target_link_libraries","add_executable","add_library",
    "target_include_directories","project","cmake_minimum_required","option",
    "find_package","install","add_subdirectory","message","return","elseif",
    "target_compile_definitions","target_compile_options","add_custom_command",
    "add_custom_target","add_dependencies","source_group","configure_file",
    "rule","build","pool","subninja","default",
    NULL
};

static const char *const KW_SCRIPT[] = {
    "if","elif","else","fi","then","for","while","do","done","in","case","esac",
    "function","def","class","return","import","from","as","with","try","except",
    "finally","raise","pass","break","continue","lambda","yield","global",
    "nonlocal","assert","del","and","or","not","is","my","our","sub","use",
    "package","local","unless","elsif","last","next","foreach","proc","set",
    "puts","BEGIN","END","printf","print",
    NULL
};
static const char *const TY_SCRIPT[] = {
    "true","false","null","none","yes","no","on","off","True","False","None",
    NULL
};

static const char *const KW_ASM[] = {
    /* Structural mnemonics worth calling out; the rest read as plain text so
     * the ISA's full opcode list never has to live here. */
    "b","bl","blr","br","ret","cbz","cbnz","tbz","tbnz","beq","bne","blt","bgt",
    "ble","bge","call","jmp","je","jne","jz","jnz","jg","jl","ja","jb","leave",
    "push","pop","enter","syscall","svc","brk","nop","hlt","int",
    NULL
};
static const char *const TY_ASM[] = {
    "byte","word","dword","qword","ptr","offset","short","near","far",
    NULL
};

static const char *const KW_PHP[] = {
    "abstract","and","array","as","break","callable","case","catch","class",
    "clone","const","continue","declare","default","do","echo","else","elseif",
    "empty","enddeclare","endfor","endforeach","endif","endswitch","endwhile",
    "enum","extends","final","finally","fn","for","foreach","function","global",
    "goto","if","implements","include","include_once","instanceof","insteadof",
    "interface","isset","list","match","namespace","new","or","print","private",
    "protected","public","readonly","require","require_once","return","static",
    "switch","throw","trait","try","unset","use","var","while","xor","yield",
    NULL
};
static const char *const TY_PHP[] = {
    "bool","float","int","iterable","mixed","never","null","object","string",
    "void","true","false","self","parent","this","Closure","Generator",
    NULL
};

/* The statement language of Jinja, Twig, Nunjucks, Liquid, Django, Go and
 * Handlebars templates — one table because a `{% %}` region cannot know which
 * engine will run it, and the vocabularies barely collide. */
static const char *const KW_JINJA[] = {
    "if","elif","else","endif","for","endfor","in","not","and","or","is","as",
    "import","from","include","extends","block","endblock","set","endset",
    "macro","endmacro","call","endcall","filter","endfilter","with","endwith",
    "raw","endraw","trans","endtrans","autoescape","endautoescape","do",
    /* Twig */
    "apply","endapply","embed","endembed","verbatim","endverbatim","sandbox",
    "endsandbox","flush","spaceless","endspaceless","use","only","ignore",
    "missing","deprecated",
    /* Liquid */
    "assign","capture","endcapture","case","when","endcase","unless","endunless",
    "comment","endcomment","increment","decrement","cycle","tablerow",
    "endtablerow","render","layout","section","paginate","endpaginate","form",
    "endform","schema","endschema","liquid","echo","break","continue",
    /* Django */
    "load","url","static","csrf_token","now","firstof","ifchanged",
    "endifchanged","regroup","widthratio","templatetag","cache","endcache",
    "blocktrans","endblocktrans","blocktranslate","endblocktranslate","empty",
    "lorem","spaceless","debug","by","reversed",
    /* Go text/template and Handlebars */
    "range","end","define","template","each","this","lookup","log","partial",
    "nil","pipeline",
    NULL
};
static const char *const TY_JINJA[] = {
    "true","false","none","True","False","None","null","loop","super","self",
    "forloop","request","caller","varargs","kwargs","range","dict","lipsum",
    "cycler","joiner","namespace","site","page","content","user","perms",
    NULL
};

static const char *const KW_ZIG[] = {
    "addrspace","align","allowzero","and","anyframe","anytype","asm","async",
    "await","break","callconv","catch","comptime","const","continue","defer",
    "else","enum","errdefer","error","export","extern","fn","for","if","inline",
    "linksection","noalias","noinline","nosuspend","opaque","or","orelse",
    "packed","pub","resume","return","struct","suspend","switch","test",
    "threadlocal","try","union","unreachable","usingnamespace","var","volatile",
    "while",
    NULL
};
static const char *const TY_ZIG[] = {
    "bool","c_int","c_long","c_longdouble","c_longlong","c_short","c_uint",
    "c_ulong","c_ulonglong","c_ushort","comptime_float","comptime_int","f16",
    "f32","f64","f80","f128","false","i8","i16","i32","i64","i128","isize",
    "noreturn","null","true","type","u8","u16","u32","u64","u128","undefined",
    "usize","void","anyerror","anyopaque",
    NULL
};

static const char *const KW_ELIXIR[] = {
    "after","alias","and","case","catch","cond","def","defcallback",
    "defdelegate","defexception","defguard","defguardp","defimpl","defmacro",
    "defmacrop","defmodule","defoverridable","defp","defprotocol","defstruct",
    "do","else","end","fn","for","if","import","in","not","or","quote","raise",
    "receive","require","rescue","reraise","super","throw","try","unless",
    "unquote","unquote_splicing","use","when","with","yield",
    NULL
};
static const char *const TY_ELIXIR[] = {
    "true","false","nil","self","__MODULE__","__DIR__","__ENV__","__CALLER__",
    "Enum","Map","List","String","Keyword","Tuple","Atom","Integer","Float",
    "IO","Kernel","Process","Agent","GenServer","Supervisor","Task","Stream",
    "File","Path","Regex","Module","Code","Macro","Base","Access","Date",
    "DateTime","NaiveDateTime","Time","Phoenix","Ecto","Plug",
    NULL
};

static const char *const KW_ERLANG[] = {
    "after","and","andalso","band","begin","bnot","bor","bsl","bsr","bxor",
    "case","catch","cond","div","end","fun","if","let","not","of","or","orelse",
    "receive","rem","try","when","xor","export","import","module","record",
    "define","include","include_lib","ifdef","ifndef","else","endif",
    "behaviour","behavior","spec","type","callback","opaque","export_type",
    NULL
};
static const char *const TY_ERLANG[] = {
    "true","false","ok","error","undefined","atom","binary","integer","float",
    "list","tuple","pid","port","reference","term","boolean","string","any",
    "map","node","self","throw","exit",
    NULL
};

static const char *const KW_HASKELL[] = {
    "as","case","class","data","default","deriving","do","else","family",
    "forall","foreign","hiding","if","import","in","infix","infixl","infixr",
    "instance","let","mdo","module","newtype","of","proc","qualified","rec",
    "then","type","where","via","stock","anyclass",
    /* Elm / PureScript */
    "port","exposing","alias","effect","command","subscription","ado",
    NULL
};
static const char *const TY_HASKELL[] = {
    "Int","Integer","Float","Double","Char","String","Bool","Maybe","Either",
    "IO","Ordering","True","False","Nothing","Just","Left","Right","LT","EQ",
    "GT","Monad","Functor","Applicative","Show","Eq","Ord","Num","Enum","Read",
    "Text","ByteString","Map","Set","List","Unit","Effect","Html","Cmd","Sub",
    "Msg","Model",
    NULL
};

static const char *const KW_OCAML[] = {
    "and","as","assert","begin","class","constraint","do","done","downto",
    "else","end","exception","external","for","fun","function","functor","if",
    "in","include","inherit","initializer","lazy","let","match","method",
    "module","mutable","new","nonrec","object","of","open","or","private","rec",
    "sig","struct","then","to","try","type","val","virtual","when","while",
    "with",
    /* F# */
    "elif","yield","abstract","default","delegate","extern","finally","global",
    "inline","interface","internal","member","namespace","override","return",
    "static","upcast","downcast","use","base","null",
    NULL
};
static const char *const TY_OCAML[] = {
    "int","float","string","bool","char","unit","list","array","option","ref",
    "exn","bytes","int32","int64","nativeint","true","false","Some","None","Ok",
    "Error","List","Array","String","Printf","Hashtbl","Map","Set","Option",
    "Result","Lwt","Async","Seq","Buffer","Format",
    NULL
};

static const char *const KW_LISP[] = {
    "def","defn","defn-","defmacro","defmulti","defmethod","defprotocol",
    "defrecord","deftype","defstruct","defonce","defvar","defparameter",
    "defconstant","defun","defclass","defgeneric","define","define-syntax",
    "define-record-type","define-macro","lambda","fn","let","let*","letfn",
    "letrec","loop","recur","if","if-let","if-not","when","when-let","when-not",
    "unless","cond","condp","case","do","dotimes","doseq","for","while","try",
    "catch","finally","throw","ns","require","import","use","in-ns","quote",
    "quasiquote","unquote","set!","setq","setf","progn","prog1","begin","and",
    "or","not","eq","eql","equal","null","apply","funcall","->","->>","as->",
    "some->","comment","declare","defonce","defvar-local","defcustom",
    "defgroup","interactive","provide","use-package","export","load",
    NULL
};
static const char *const TY_LISP[] = {
    "nil","t","true","false","this","#t","#f","*ns*","*out*","*err*","*in*",
    "*standard-output*","*print-length*",
    NULL
};

static const char *const KW_R[] = {
    "if","else","repeat","while","function","for","in","next","break","return",
    "library","require","source","stop","warning","message","tryCatch",
    "invisible","switch","setClass","setGeneric","setMethod","local",
    NULL
};
static const char *const TY_R[] = {
    "TRUE","FALSE","NULL","NA","NA_integer_","NA_real_","NA_character_","Inf",
    "NaN","numeric","character","logical","integer","double","complex","list",
    "vector","matrix","factor","T","F",
    NULL
};

static const char *const KW_JULIA[] = {
    "abstract","baremodule","begin","break","catch","const","continue","do",
    "else","elseif","end","export","finally","for","function","global","if",
    "import","in","isa","let","local","macro","module","mutable","primitive",
    "quote","return","struct","try","type","using","where","while","public",
    NULL
};
static const char *const TY_JULIA[] = {
    "true","false","nothing","missing","Int","Int8","Int16","Int32","Int64",
    "Int128","UInt","UInt8","UInt16","UInt32","UInt64","Float16","Float32",
    "Float64","Bool","Char","String","Symbol","Any","Nothing","Missing","Array",
    "Vector","Matrix","Dict","Set","Tuple","NamedTuple","Function","Type",
    "Number","Real","Integer","AbstractString","AbstractArray","Union","Ref",
    NULL
};

static const char *const KW_NIM[] = {
    "addr","and","as","asm","bind","block","break","case","cast","concept",
    "const","continue","converter","defer","discard","distinct","div","do",
    "elif","else","end","enum","except","export","finally","for","from","func",
    "if","import","in","include","interface","is","isnot","iterator","let",
    "macro","method","mixin","mod","nil","not","notin","object","of","or","out",
    "proc","ptr","raise","ref","return","shl","shr","static","template","try",
    "tuple","type","using","var","when","while","xor","yield","echo",
    NULL
};
static const char *const TY_NIM[] = {
    "int","int8","int16","int32","int64","uint","uint8","uint16","uint32",
    "uint64","float","float32","float64","bool","char","string","cstring",
    "pointer","seq","array","openArray","set","range","true","false","void",
    "auto","any","typed","untyped","Natural","Positive","Table","Option",
    NULL
};

static const char *const KW_HCL[] = {
    "resource","data","variable","output","module","provider","locals",
    "terraform","backend","provisioner","connection","dynamic","for_each","for",
    "in","if","else","count","depends_on","lifecycle","required_providers",
    "required_version","import","moved","check","removed","job","group","task",
    "service","network","config","template","artifact","vault","meta",
    "constraint","affinity","spread","update","migrate","reschedule","restart",
    "ephemeral_disk","logs","env","source","packer","build","post-processor",
    "post-processors","validation","precondition","postcondition","content",
    NULL
};
static const char *const TY_HCL[] = {
    "true","false","null","string","number","bool","list","map","set","object",
    "tuple","any","var","local","each","self","path","terraform","module",
    "count","data",
    NULL
};

static const char *const KW_NIX[] = {
    "let","in","with","rec","inherit","if","then","else","assert","or","import",
    NULL
};
static const char *const TY_NIX[] = {
    "true","false","null","builtins","pkgs","lib","stdenv","self","super",
    "config","options","system","inputs","outputs",
    NULL
};

static const char *const KW_DOCKER[] = {
    "FROM","RUN","CMD","LABEL","EXPOSE","ENV","ADD","COPY","ENTRYPOINT","VOLUME",
    "USER","WORKDIR","ARG","ONBUILD","STOPSIGNAL","HEALTHCHECK","SHELL",
    "MAINTAINER","AS",
    NULL
};

static const char *const KW_PS[] = {
    "begin","break","catch","class","continue","data","define","do",
    "dynamicparam","else","elseif","end","enum","exit","filter","finally","for",
    "foreach","from","function","hidden","if","in","inlinescript","param",
    "process","return","static","switch","throw","trap","try","until","using",
    "var","while","workflow",
    /* the -operators, minus their dash */
    "eq","ne","gt","ge","lt","le","like","notlike","match","notmatch",
    "contains","notcontains","notin","replace","split","join","and","or","not",
    "xor","band","bor","bxor","is","isnot","as",
    NULL
};
static const char *const TY_PS[] = {
    "true","false","null","string","int","long","bool","char","byte","double",
    "decimal","float","array","hashtable","object","void","datetime","timespan",
    "guid","xml","regex","scriptblock","psobject","pscustomobject","switch",
    NULL
};

static const char *const KW_BATCH[] = {
    "echo","set","if","else","not","exist","defined","errorlevel","for","in",
    "do","goto","call","exit","rem","setlocal","endlocal",
    "enabledelayedexpansion","enableextensions","pause","shift","start",
    "title","cd","dir","copy","del","md","mkdir","rd","rmdir","move","ren",
    "type","cls","choice","timeout","findstr","find","sort","more","pushd",
    "popd","equ","neq","lss","leq","gtr","geq",
    NULL
};
static const char *const TY_BATCH[] = { "on","off","nul","con", NULL };

static const char *const KW_GRAPHQL[] = {
    "query","mutation","subscription","fragment","on","type","interface",
    "union","enum","input","scalar","schema","extend","directive","implements",
    "repeatable",
    NULL
};
static const char *const TY_GRAPHQL[] = {
    "Int","Float","String","Boolean","ID","true","false","null","QUERY",
    "MUTATION","SUBSCRIPTION","FIELD","FRAGMENT_DEFINITION","FRAGMENT_SPREAD",
    "INLINE_FRAGMENT","VARIABLE_DEFINITION",
    NULL
};

static const char *const KW_PROTO[] = {
    "syntax","package","import","option","message","enum","service","rpc",
    "returns","stream","repeated","optional","required","oneof","map",
    "reserved","extend","extensions","to","max","group","edition","public",
    "weak",
    /* Thrift / FlatBuffers */
    "struct","exception","typedef","namespace","include","const","throws",
    "oneway","union","senum","cpp_include","table","root_type","attribute",
    "file_identifier","file_extension",
    NULL
};
static const char *const TY_PROTO[] = {
    "double","float","int32","int64","uint32","uint64","sint32","sint64",
    "fixed32","fixed64","sfixed32","sfixed64","bool","string","bytes","true",
    "false","i8","i16","i32","i64","byte","binary","list","set","void","ubyte",
    "short","ushort","int","uint","long","ulong","ubyte",
    NULL
};

static const char *const KW_PRISMA[] = {
    "model","enum","datasource","generator","type","view",
    NULL
};
static const char *const TY_PRISMA[] = {
    "String","Boolean","Int","BigInt","Float","Decimal","DateTime","Json",
    "Bytes","Unsupported","true","false","null","env",
    NULL
};

static const char *const KW_SOL[] = {
    "pragma","solidity","import","contract","interface","library","abstract",
    "is","function","modifier","event","error","struct","enum","mapping",
    "public","private","internal","external","pure","view","payable","virtual",
    "override","returns","return","if","else","for","while","do","break",
    "continue","emit","require","revert","assert","new","delete","memory",
    "storage","calldata","constant","immutable","constructor","receive",
    "fallback","using","unchecked","try","catch","as","from","indexed",
    "anonymous","assembly","let",
    NULL
};
static const char *const TY_SOL[] = {
    "address","bool","string","bytes","byte","bytes1","bytes4","bytes32","uint",
    "uint8","uint16","uint32","uint64","uint128","uint256","int","int8","int16",
    "int32","int64","int128","int256","wei","gwei","ether","seconds","minutes",
    "hours","days","weeks","msg","block","tx","this","super","now","true",
    "false",
    NULL
};

static const char *const KW_WASM[] = {
    "module","func","param","result","local","global","export","import",
    "memory","table","type","elem","data","start","if","then","else","end",
    "loop","block","br","br_if","br_table","call","call_indirect","return",
    "drop","select","unreachable","nop","offset","align","mut","funcref",
    "externref","component","instance","core",
    NULL
};
static const char *const TY_WASM[] = {
    "i32","i64","f32","f64","v128","i8x16","i16x8","i32x4","i64x2","f32x4",
    "f64x2",
    NULL
};

static const char *const KW_JSONNET[] = {
    "local","function","import","importstr","importbin","if","then","else",
    "for","in","assert","error","tailstrict","super","self",
    NULL
};
static const char *const TY_JSONNET[] = { "null","true","false","std", NULL };

static const char *const KW_GOMOD[] = {
    "module","go","toolchain","godebug","require","replace","exclude","retract",
    "use","tool",
    NULL
};
static const char *const TY_GOMOD[] = { "indirect", NULL };

/* -------------------------------------------------------- language defs -- */

typedef struct {
    const char *name;
    const char *const *kw;
    const char *const *ty;
    const char *lc[3];      /* line-comment tokens, NULL-padded */
    const char *bo, *bc;    /* block comment open / close */
    u64 flags;
    /* Host rows only: the SynLang lexed inside `{ }`, `{{ }}` and `<% %>`
     * regions, and inside a directive attribute's quoted value. `{% %}` is
     * always SYN_LANG_JINJA. Zero for a plain row. */
    u8  expr;
} LangDef;

#define NOLC {NULL,NULL,NULL}

static const LangDef LANGS[SYN_LANG_COUNT] = {
    [SYN_LANG_NONE]   = { "", NULL, NULL, NOLC, NULL, NULL, 0, 0 },

    [SYN_LANG_C]      = { "C", KW_C, TY_C, {"//",NULL,NULL}, "/*", "*/",
                          F_PREPROC | F_CHARLIT | F_CAPS_TYPE, 0 },
    [SYN_LANG_SCRIPT] = { "Script", KW_SCRIPT, TY_SCRIPT, {"#",NULL,NULL}, NULL, NULL,
                          F_DOLLARVAR, 0 },
    [SYN_LANG_MAKE]   = { "Make", KW_MAKE, NULL, {"#",NULL,NULL}, NULL, NULL,
                          F_DOLLARVAR | F_TARGET, 0 },
    [SYN_LANG_MARKUP] = { "Markup", NULL, NULL, NOLC, "<!--", "-->",
                          F_TAGS | F_EMBED, 0 },

    [SYN_LANG_PYTHON] = { "Python", KW_PY, TY_PY, {"#",NULL,NULL}, NULL, NULL,
                          F_TRIPLE | F_ATTR_AT | F_CAPS_TYPE, 0 },
    [SYN_LANG_JS]     = { "JavaScript", KW_JS, TY_JS, {"//",NULL,NULL}, "/*", "*/",
                          F_BACKTICK | F_BT_SPAN | F_INTERP | F_REGEX |
                          F_ATTR_AT | F_CAPS_TYPE, 0 },
    [SYN_LANG_GO]     = { "Go", KW_GO, TY_GO, {"//",NULL,NULL}, "/*", "*/",
                          F_BACKTICK | F_BT_SPAN | F_BT_RAW | F_CHARLIT |
                          F_CAPS_TYPE, 0 },
    [SYN_LANG_RUST]   = { "Rust", KW_RUST, TY_RUST, {"//",NULL,NULL}, "/*", "*/",
                          F_NESTBLK | F_CHARLIT | F_LIFETIME | F_ATTR_HASH |
                          F_ATTR_AT | F_CAPS_TYPE | F_DQ_SPAN | F_RAW_HASH, 0 },
    /* Swift: only """…""" holds a line break; "…" is one line. */
    [SYN_LANG_SWIFT]  = { "Swift", KW_SWIFT, TY_SWIFT, {"//",NULL,NULL}, "/*", "*/",
                          F_NESTBLK | F_ATTR_AT | F_CAPS_TYPE | F_TRIPLE, 0 },
    [SYN_LANG_JVM]    = { "Java", KW_JVM, TY_JVM, {"//",NULL,NULL}, "/*", "*/",
                          F_CHARLIT | F_ATTR_AT | F_TRIPLE | F_CAPS_TYPE, 0 },
    [SYN_LANG_SHELL]  = { "Shell", KW_SH, TY_SH, {"#",NULL,NULL}, NULL, NULL,
                          F_DOLLARVAR | F_BACKTICK | F_DQ_SPAN | F_SQ_SPAN |
                          F_SH_QUOTE | F_HEREDOC, 0 },
    [SYN_LANG_RUBY]   = { "Ruby", KW_RUBY, TY_RUBY, {"#",NULL,NULL}, NULL, NULL,
                          F_SIGIL_RB | F_DOLLARVAR | F_CAPS_TYPE, 0 },
    [SYN_LANG_LUA]    = { "Lua", KW_LUA, TY_LUA, {"--",NULL,NULL}, "--[[", "]]", 0, 0 },
    [SYN_LANG_SQL]    = { "SQL", KW_SQL, TY_SQL, {"--","#",NULL}, "/*", "*/",
                          F_CI_KW, 0 },
    [SYN_LANG_HTML]   = { "HTML", NULL, NULL, NOLC, "<!--", "-->",
                          F_TAGS | F_EMBED | F_TMPL, SYN_LANG_JS },
    [SYN_LANG_CSS]    = { "CSS", KW_CSS, TY_CSS, {"//",NULL,NULL}, "/*", "*/",
                          F_CSS | F_ATTR_AT | F_DOLLARVAR, 0 },
    [SYN_LANG_JSON]   = { "JSON", NULL, TY_JSON, {"//",NULL,NULL}, "/*", "*/",
                          F_KEY_JSON, 0 },
    [SYN_LANG_YAML]   = { "YAML", KW_YAML, TY_YAML, {"#",NULL,NULL}, NULL, NULL,
                          F_KEY_YAML, 0 },
    [SYN_LANG_TOML]   = { "TOML", KW_TOML, TY_TOML, {"#",NULL,NULL}, NULL, NULL,
                          F_KEY_EQ | F_SECTION | F_SEMI_CMT, 0 },
    [SYN_LANG_ASM]    = { "Assembly", KW_ASM, TY_ASM, {"//",";","#"}, "/*", "*/",
                          F_ASM | F_HASH_BOL | F_CHARLIT, 0 },
    [SYN_LANG_PHP]    = { "PHP", KW_PHP, TY_PHP, {"//","#",NULL}, "/*", "*/",
                          F_DOLLARVAR | F_ATTR_HASH | F_CAPS_TYPE | F_DQ_SPAN |
                          F_SQ_SPAN | F_HEREDOC3 | F_CODE_TAGS, 0 },

    /* Component and template hosts. All markup; they differ only in which
     * regions open and which row lexes the expressions. */
    [SYN_LANG_VUE]      = { "Vue", NULL, NULL, NOLC, "<!--", "-->",
                            F_TAGS | F_EMBED | F_TMPL, SYN_LANG_JS },
    [SYN_LANG_SVELTE]   = { "Svelte", NULL, NULL, NOLC, "<!--", "-->",
                            F_TAGS | F_EMBED | F_BRACE, SYN_LANG_JS },
    [SYN_LANG_ASTRO]    = { "Astro", NULL, NULL, NOLC, "<!--", "-->",
                            F_TAGS | F_EMBED | F_BRACE | F_FRONT, SYN_LANG_JS },
    [SYN_LANG_TEMPLATE] = { "Template", NULL, NULL, NOLC, "<!--", "-->",
                            F_TAGS | F_EMBED | F_TMPL, SYN_LANG_JINJA },
    [SYN_LANG_JINJA]    = { "Jinja", KW_JINJA, TY_JINJA, NOLC, NULL, NULL, 0, 0 },
    [SYN_LANG_ERB]      = { "ERB", NULL, NULL, NOLC, "<!--", "-->",
                            F_TAGS | F_EMBED | F_ERB, SYN_LANG_RUBY },
    [SYN_LANG_EJS]      = { "EJS", NULL, NULL, NOLC, "<!--", "-->",
                            F_TAGS | F_EMBED | F_ERB, SYN_LANG_JS },
    [SYN_LANG_HEEX]     = { "HEEx", NULL, NULL, NOLC, "<!--", "-->",
                            F_TAGS | F_EMBED | F_ERB | F_BRACE, SYN_LANG_ELIXIR },
    [SYN_LANG_JSP]      = { "JSP", NULL, NULL, NOLC, "<!--", "-->",
                            F_TAGS | F_EMBED | F_ERB, SYN_LANG_JVM },

    [SYN_LANG_ZIG]      = { "Zig", KW_ZIG, TY_ZIG, {"//",NULL,NULL}, NULL, NULL,
                            F_ATTR_AT | F_CHARLIT | F_CAPS_TYPE, 0 },
    [SYN_LANG_ELIXIR]   = { "Elixir", KW_ELIXIR, TY_ELIXIR, {"#",NULL,NULL}, NULL, NULL,
                            F_TRIPLE | F_ATTR_AT | F_SIGIL_RB | F_CAPS_TYPE, 0 },
    [SYN_LANG_ERLANG]   = { "Erlang", KW_ERLANG, TY_ERLANG, {"%",NULL,NULL}, NULL, NULL,
                            0, 0 },
    [SYN_LANG_HASKELL]  = { "Haskell", KW_HASKELL, TY_HASKELL, {"--",NULL,NULL}, "{-", "-}",
                            F_NESTBLK | F_PRIME_IDENT | F_CAPS_TYPE, 0 },
    [SYN_LANG_OCAML]    = { "OCaml", KW_OCAML, TY_OCAML, {"//",NULL,NULL}, "(*", "*)",
                            F_NESTBLK | F_LIFETIME | F_PRIME_IDENT | F_CAPS_TYPE, 0 },
    [SYN_LANG_LISP]     = { "Lisp", KW_LISP, TY_LISP, {";",NULL,NULL}, "#|", "|#",
                            F_NESTBLK | F_DASH_IDENT | F_SIGIL_RB | F_NO_SQUOTE, 0 },
    [SYN_LANG_R]        = { "R", KW_R, TY_R, {"#",NULL,NULL}, NULL, NULL, F_DOT_IDENT, 0 },
    [SYN_LANG_JULIA]    = { "Julia", KW_JULIA, TY_JULIA, {"#",NULL,NULL}, "#=", "=#",
                            F_NESTBLK | F_ATTR_AT | F_TRIPLE | F_PRIME_IDENT |
                            F_CAPS_TYPE, 0 },
    [SYN_LANG_NIM]      = { "Nim", KW_NIM, TY_NIM, {"#",NULL,NULL}, "#[", "]#",
                            F_NESTBLK | F_TRIPLE | F_CAPS_TYPE, 0 },
    [SYN_LANG_HCL]      = { "HCL", KW_HCL, TY_HCL, {"#","//",NULL}, "/*", "*/",
                            F_DOLLARVAR | F_KEY_EQ | F_HEREDOC, 0 },
    /* Nix: a lone ' is the prime of an identifier (foldl'), never a string. */
    [SYN_LANG_NIX]      = { "Nix", KW_NIX, TY_NIX, {"#",NULL,NULL}, "/*", "*/",
                            F_DOLLARVAR | F_KEY_EQ | F_DQ_SPAN | F_QQ_STR |
                            F_INTERP | F_PRIME_IDENT | F_NO_SQUOTE, 0 },
    [SYN_LANG_DOCKER]   = { "Dockerfile", KW_DOCKER, TY_SH, {"#",NULL,NULL}, NULL, NULL,
                            F_CI_KW | F_KW_BOL | F_DOLLARVAR, 0 },
    [SYN_LANG_POWERSHELL] = { "PowerShell", KW_PS, TY_PS, {"#",NULL,NULL}, "<#", "#>",
                            F_CI_KW | F_DOLLARVAR | F_DASH_IDENT, 0 },
    [SYN_LANG_BATCH]    = { "Batch", KW_BATCH, TY_BATCH, {"::","rem ","REM "}, NULL, NULL,
                            F_CI_KW | F_PCTVAR, 0 },
    [SYN_LANG_GRAPHQL]  = { "GraphQL", KW_GRAPHQL, TY_GRAPHQL, {"#",NULL,NULL}, NULL, NULL,
                            F_TRIPLE | F_ATTR_AT | F_DOLLARVAR | F_CAPS_TYPE |
                            F_KEY_YAML, 0 },
    [SYN_LANG_PROTO]    = { "Protobuf", KW_PROTO, TY_PROTO, {"//","#",NULL}, "/*", "*/",
                            F_CAPS_TYPE, 0 },
    [SYN_LANG_PRISMA]   = { "Prisma", KW_PRISMA, TY_PRISMA, {"//",NULL,NULL}, NULL, NULL,
                            F_ATTR_AT | F_CAPS_TYPE, 0 },
    [SYN_LANG_SOLIDITY] = { "Solidity", KW_SOL, TY_SOL, {"//",NULL,NULL}, "/*", "*/",
                            F_CAPS_TYPE, 0 },
    [SYN_LANG_LATEX]    = { "LaTeX", NULL, NULL, {"%",NULL,NULL}, NULL, NULL,
                            F_BS_CMD | F_KEY_EQ | F_ATTR_AT, 0 },
    [SYN_LANG_DIFF]     = { "Diff", NULL, NULL, NOLC, NULL, NULL, F_DIFF, 0 },
    [SYN_LANG_WASM]     = { "WebAssembly", KW_WASM, TY_WASM, {";;",NULL,NULL}, "(;", ";)",
                            F_DOLLARVAR, 0 },
    [SYN_LANG_JSONNET]  = { "Jsonnet", KW_JSONNET, TY_JSONNET, {"//","#",NULL}, "/*", "*/",
                            F_KEY_YAML, 0 },
    [SYN_LANG_GOMOD]    = { "Go Module", KW_GOMOD, TY_GOMOD, {"//",NULL,NULL}, NULL, NULL,
                            0, 0 },
};

const char *syntax_lang_name(SynLang lang) {
    if (lang <= SYN_LANG_NONE || lang >= SYN_LANG_COUNT) return "";
    return LANGS[lang].name;
}

const char *syntax_line_comment(SynLang lang) {
    if (lang <= SYN_LANG_NONE || lang >= SYN_LANG_COUNT) return NULL;
    return LANGS[lang].lc[0];
}

/* -------------------------------------------------------- path → language - */

static bool word_is(const char *w, usize n, const char *lit) {
    return strlen(lit) == n && strncasecmp(w, lit, n) == 0;
}

/* One row per extension or fence word: the scanner row it selects and the
 * name the status bar shows for it. The label is the FILE's language, which
 * is finer than the row — .ts and .js share a lexer and must not share a
 * name. Rows with SYN_LANG_NONE exist only for their label ("Markdown",
 * "Text"), so the editor can still say what it has open. */
typedef struct { const char *tok; u8 lang; const char *label; } TokMap;

static const TokMap EXTS[] = {
    /* C family and the shader languages that borrow its shape */
    { "c", SYN_LANG_C, "C" },              { "h", SYN_LANG_C, "C" },
    { "cc", SYN_LANG_C, "C++" },           { "cpp", SYN_LANG_C, "C++" },
    { "cxx", SYN_LANG_C, "C++" },          { "c++", SYN_LANG_C, "C++" },
    { "hpp", SYN_LANG_C, "C++" },          { "hxx", SYN_LANG_C, "C++" },
    { "hh", SYN_LANG_C, "C++" },           { "ipp", SYN_LANG_C, "C++" },
    { "inl", SYN_LANG_C, "C++" },          { "tpp", SYN_LANG_C, "C++" },
    { "m", SYN_LANG_C, "Objective-C" },    { "mm", SYN_LANG_C, "Objective-C++" },
    { "objc", SYN_LANG_C, "Objective-C" }, { "objcpp", SYN_LANG_C, "Objective-C++" },
    { "objective-c", SYN_LANG_C, "Objective-C" },
    { "ino", SYN_LANG_C, "Arduino" },      { "pde", SYN_LANG_C, "Processing" },
    { "cu", SYN_LANG_C, "CUDA" },          { "cuh", SYN_LANG_C, "CUDA" },
    { "glsl", SYN_LANG_C, "GLSL" },        { "vert", SYN_LANG_C, "GLSL" },
    { "frag", SYN_LANG_C, "GLSL" },        { "comp", SYN_LANG_C, "GLSL" },
    { "geom", SYN_LANG_C, "GLSL" },        { "tesc", SYN_LANG_C, "GLSL" },
    { "tese", SYN_LANG_C, "GLSL" },        { "hlsl", SYN_LANG_C, "HLSL" },
    { "fx", SYN_LANG_C, "HLSL" },          { "metal", SYN_LANG_C, "Metal" },
    /* Python and the build languages that are Python */
    { "py", SYN_LANG_PYTHON, "Python" },   { "pyi", SYN_LANG_PYTHON, "Python" },
    { "pyw", SYN_LANG_PYTHON, "Python" },  { "python", SYN_LANG_PYTHON, "Python" },
    { "pyx", SYN_LANG_PYTHON, "Cython" },  { "pxd", SYN_LANG_PYTHON, "Cython" },
    { "pxi", SYN_LANG_PYTHON, "Cython" },  { "mojo", SYN_LANG_PYTHON, "Mojo" },
    { "bzl", SYN_LANG_PYTHON, "Starlark" },{ "bazel", SYN_LANG_PYTHON, "Starlark" },
    { "star", SYN_LANG_PYTHON, "Starlark" },{ "gyp", SYN_LANG_PYTHON, "GYP" },
    { "gypi", SYN_LANG_PYTHON, "GYP" },    { "gn", SYN_LANG_PYTHON, "GN" },
    { "gni", SYN_LANG_PYTHON, "GN" },      { "vy", SYN_LANG_PYTHON, "Vyper" },
    /* JavaScript / TypeScript */
    { "js", SYN_LANG_JS, "JavaScript" },   { "jsx", SYN_LANG_JS, "JavaScript" },
    { "mjs", SYN_LANG_JS, "JavaScript" },  { "cjs", SYN_LANG_JS, "JavaScript" },
    { "javascript", SYN_LANG_JS, "JavaScript" }, { "node", SYN_LANG_JS, "JavaScript" },
    { "ts", SYN_LANG_JS, "TypeScript" },   { "tsx", SYN_LANG_JS, "TypeScript" },
    { "mts", SYN_LANG_JS, "TypeScript" },  { "cts", SYN_LANG_JS, "TypeScript" },
    { "typescript", SYN_LANG_JS, "TypeScript" },
    { "res", SYN_LANG_JS, "ReScript" },    { "resi", SYN_LANG_JS, "ReScript" },
    /* Component and template files */
    { "vue", SYN_LANG_VUE, "Vue" },
    { "svelte", SYN_LANG_SVELTE, "Svelte" },
    { "astro", SYN_LANG_ASTRO, "Astro" },
    { "hbs", SYN_LANG_TEMPLATE, "Handlebars" },
    { "handlebars", SYN_LANG_TEMPLATE, "Handlebars" },
    { "mustache", SYN_LANG_TEMPLATE, "Mustache" },
    { "j2", SYN_LANG_TEMPLATE, "Jinja" },  { "jinja", SYN_LANG_TEMPLATE, "Jinja" },
    { "jinja2", SYN_LANG_TEMPLATE, "Jinja" }, { "django", SYN_LANG_TEMPLATE, "Django" },
    { "twig", SYN_LANG_TEMPLATE, "Twig" }, { "liquid", SYN_LANG_TEMPLATE, "Liquid" },
    { "njk", SYN_LANG_TEMPLATE, "Nunjucks" }, { "nunjucks", SYN_LANG_TEMPLATE, "Nunjucks" },
    { "tmpl", SYN_LANG_TEMPLATE, "Template" }, { "tpl", SYN_LANG_TEMPLATE, "Template" },
    { "gohtml", SYN_LANG_TEMPLATE, "Go Template" },
    { "gotmpl", SYN_LANG_TEMPLATE, "Go Template" },
    { "erb", SYN_LANG_ERB, "ERB" },        { "rhtml", SYN_LANG_ERB, "ERB" },
    { "ejs", SYN_LANG_EJS, "EJS" },
    { "heex", SYN_LANG_HEEX, "HEEx" },     { "eex", SYN_LANG_HEEX, "EEx" },
    { "leex", SYN_LANG_HEEX, "LiveEEx" },
    { "jsp", SYN_LANG_JSP, "JSP" },        { "jspx", SYN_LANG_JSP, "JSP" },
    { "asp", SYN_LANG_JSP, "ASP" },        { "aspx", SYN_LANG_JSP, "ASP.NET" },
    { "ascx", SYN_LANG_JSP, "ASP.NET" },
    /* Go / Rust / Zig / Swift */
    { "go", SYN_LANG_GO, "Go" },           { "golang", SYN_LANG_GO, "Go" },
    { "gomod", SYN_LANG_GOMOD, "Go Module" },
    { "rs", SYN_LANG_RUST, "Rust" },       { "rust", SYN_LANG_RUST, "Rust" },
    { "ron", SYN_LANG_RUST, "RON" },       { "wgsl", SYN_LANG_RUST, "WGSL" },
    { "zig", SYN_LANG_ZIG, "Zig" },        { "zon", SYN_LANG_ZIG, "Zig" },
    { "swift", SYN_LANG_SWIFT, "Swift" },
    /* JVM and its neighbours */
    { "java", SYN_LANG_JVM, "Java" },      { "kt", SYN_LANG_JVM, "Kotlin" },
    { "kts", SYN_LANG_JVM, "Kotlin" },     { "kotlin", SYN_LANG_JVM, "Kotlin" },
    { "scala", SYN_LANG_JVM, "Scala" },    { "sc", SYN_LANG_JVM, "Scala" },
    { "sbt", SYN_LANG_JVM, "Scala" },      { "cs", SYN_LANG_JVM, "C#" },
    { "csharp", SYN_LANG_JVM, "C#" },      { "csx", SYN_LANG_JVM, "C#" },
    { "dart", SYN_LANG_JVM, "Dart" },      { "groovy", SYN_LANG_JVM, "Groovy" },
    { "gvy", SYN_LANG_JVM, "Groovy" },     { "gradle", SYN_LANG_JVM, "Gradle" },
    /* Shells and their cousins */
    { "sh", SYN_LANG_SHELL, "Shell" },     { "bash", SYN_LANG_SHELL, "Bash" },
    { "zsh", SYN_LANG_SHELL, "Zsh" },      { "fish", SYN_LANG_SHELL, "Fish" },
    { "ksh", SYN_LANG_SHELL, "Shell" },    { "shell", SYN_LANG_SHELL, "Shell" },
    { "console", SYN_LANG_SHELL, "Shell" },{ "zshrc", SYN_LANG_SHELL, "Zsh" },
    { "bashrc", SYN_LANG_SHELL, "Bash" },  { "bats", SYN_LANG_SHELL, "Bash" },
    { "just", SYN_LANG_SHELL, "Just" },
    { "ps1", SYN_LANG_POWERSHELL, "PowerShell" },
    { "psm1", SYN_LANG_POWERSHELL, "PowerShell" },
    { "psd1", SYN_LANG_POWERSHELL, "PowerShell" },
    { "pwsh", SYN_LANG_POWERSHELL, "PowerShell" },
    { "powershell", SYN_LANG_POWERSHELL, "PowerShell" },
    { "bat", SYN_LANG_BATCH, "Batch" },    { "cmd", SYN_LANG_BATCH, "Batch" },
    { "batch", SYN_LANG_BATCH, "Batch" },
    { "dockerfile", SYN_LANG_DOCKER, "Dockerfile" },
    { "docker", SYN_LANG_DOCKER, "Dockerfile" },
    { "containerfile", SYN_LANG_DOCKER, "Dockerfile" },
    /* Ruby / Lua / PHP / Perl and friends */
    { "rb", SYN_LANG_RUBY, "Ruby" },       { "ruby", SYN_LANG_RUBY, "Ruby" },
    { "gemspec", SYN_LANG_RUBY, "Ruby" },  { "rake", SYN_LANG_RUBY, "Ruby" },
    { "ru", SYN_LANG_RUBY, "Ruby" },       { "podspec", SYN_LANG_RUBY, "Ruby" },
    { "cr", SYN_LANG_RUBY, "Crystal" },
    { "lua", SYN_LANG_LUA, "Lua" },        { "luau", SYN_LANG_LUA, "Luau" },
    { "rockspec", SYN_LANG_LUA, "Lua" },
    { "php", SYN_LANG_PHP, "PHP" },        { "phtml", SYN_LANG_PHP, "PHP" },
    { "pl", SYN_LANG_SCRIPT, "Perl" },     { "pm", SYN_LANG_SCRIPT, "Perl" },
    { "perl", SYN_LANG_SCRIPT, "Perl" },   { "tcl", SYN_LANG_SCRIPT, "Tcl" },
    { "awk", SYN_LANG_SCRIPT, "AWK" },
    /* Functional */
    { "ex", SYN_LANG_ELIXIR, "Elixir" },   { "exs", SYN_LANG_ELIXIR, "Elixir" },
    { "elixir", SYN_LANG_ELIXIR, "Elixir" },
    { "erl", SYN_LANG_ERLANG, "Erlang" },  { "hrl", SYN_LANG_ERLANG, "Erlang" },
    { "erlang", SYN_LANG_ERLANG, "Erlang" },
    { "hs", SYN_LANG_HASKELL, "Haskell" }, { "lhs", SYN_LANG_HASKELL, "Haskell" },
    { "haskell", SYN_LANG_HASKELL, "Haskell" }, { "elm", SYN_LANG_HASKELL, "Elm" },
    { "purs", SYN_LANG_HASKELL, "PureScript" }, { "idr", SYN_LANG_HASKELL, "Idris" },
    { "ml", SYN_LANG_OCAML, "OCaml" },     { "mli", SYN_LANG_OCAML, "OCaml" },
    { "ocaml", SYN_LANG_OCAML, "OCaml" },  { "fs", SYN_LANG_OCAML, "F#" },
    { "fsi", SYN_LANG_OCAML, "F#" },       { "fsx", SYN_LANG_OCAML, "F#" },
    { "fsharp", SYN_LANG_OCAML, "F#" },    { "re", SYN_LANG_OCAML, "Reason" },
    { "rei", SYN_LANG_OCAML, "Reason" },
    { "clj", SYN_LANG_LISP, "Clojure" },   { "cljs", SYN_LANG_LISP, "ClojureScript" },
    { "cljc", SYN_LANG_LISP, "Clojure" },  { "edn", SYN_LANG_LISP, "EDN" },
    { "clojure", SYN_LANG_LISP, "Clojure" }, { "bb", SYN_LANG_LISP, "Babashka" },
    { "lisp", SYN_LANG_LISP, "Lisp" },     { "lsp", SYN_LANG_LISP, "Lisp" },
    { "cl", SYN_LANG_LISP, "Common Lisp" },{ "el", SYN_LANG_LISP, "Emacs Lisp" },
    { "elisp", SYN_LANG_LISP, "Emacs Lisp" }, { "scm", SYN_LANG_LISP, "Scheme" },
    { "ss", SYN_LANG_LISP, "Scheme" },     { "scheme", SYN_LANG_LISP, "Scheme" },
    { "rkt", SYN_LANG_LISP, "Racket" },    { "racket", SYN_LANG_LISP, "Racket" },
    { "fnl", SYN_LANG_LISP, "Fennel" },
    /* Scientific */
    { "r", SYN_LANG_R, "R" },              { "jl", SYN_LANG_JULIA, "Julia" },
    { "julia", SYN_LANG_JULIA, "Julia" },  { "nim", SYN_LANG_NIM, "Nim" },
    { "nims", SYN_LANG_NIM, "Nim" },       { "nimble", SYN_LANG_NIM, "Nim" },
    /* Infrastructure and schemas */
    { "tf", SYN_LANG_HCL, "Terraform" },   { "tfvars", SYN_LANG_HCL, "Terraform" },
    { "terraform", SYN_LANG_HCL, "Terraform" }, { "hcl", SYN_LANG_HCL, "HCL" },
    { "nomad", SYN_LANG_HCL, "Nomad" },
    { "nix", SYN_LANG_NIX, "Nix" },
    { "sql", SYN_LANG_SQL, "SQL" },        { "psql", SYN_LANG_SQL, "SQL" },
    { "mysql", SYN_LANG_SQL, "SQL" },      { "pgsql", SYN_LANG_SQL, "SQL" },
    { "plsql", SYN_LANG_SQL, "PL/SQL" },   { "ddl", SYN_LANG_SQL, "SQL" },
    { "cql", SYN_LANG_SQL, "CQL" },        { "hql", SYN_LANG_SQL, "HiveQL" },
    { "prisma", SYN_LANG_PRISMA, "Prisma" },
    { "graphql", SYN_LANG_GRAPHQL, "GraphQL" }, { "gql", SYN_LANG_GRAPHQL, "GraphQL" },
    { "graphqls", SYN_LANG_GRAPHQL, "GraphQL" },
    { "proto", SYN_LANG_PROTO, "Protobuf" }, { "protobuf", SYN_LANG_PROTO, "Protobuf" },
    { "thrift", SYN_LANG_PROTO, "Thrift" }, { "fbs", SYN_LANG_PROTO, "FlatBuffers" },
    { "sol", SYN_LANG_SOLIDITY, "Solidity" }, { "solidity", SYN_LANG_SOLIDITY, "Solidity" },
    { "wat", SYN_LANG_WASM, "WebAssembly" }, { "wast", SYN_LANG_WASM, "WebAssembly" },
    { "wasm", SYN_LANG_WASM, "WebAssembly" },
    { "jsonnet", SYN_LANG_JSONNET, "Jsonnet" }, { "libsonnet", SYN_LANG_JSONNET, "Jsonnet" },
    /* Markup */
    { "html", SYN_LANG_HTML, "HTML" },     { "htm", SYN_LANG_HTML, "HTML" },
    { "xhtml", SYN_LANG_HTML, "XHTML" },   { "shtml", SYN_LANG_HTML, "HTML" },
    { "cshtml", SYN_LANG_HTML, "Razor" },  { "razor", SYN_LANG_HTML, "Razor" },
    { "xml", SYN_LANG_MARKUP, "XML" },     { "svg", SYN_LANG_MARKUP, "SVG" },
    { "plist", SYN_LANG_MARKUP, "Property List" },
    { "mobileconfig", SYN_LANG_MARKUP, "Property List" },
    { "entitlements", SYN_LANG_MARKUP, "Entitlements" },
    { "xcscheme", SYN_LANG_MARKUP, "Xcode Scheme" },
    { "xaml", SYN_LANG_MARKUP, "XAML" },   { "storyboard", SYN_LANG_MARKUP, "Storyboard" },
    { "xib", SYN_LANG_MARKUP, "XIB" },     { "xsd", SYN_LANG_MARKUP, "XML Schema" },
    { "xsl", SYN_LANG_MARKUP, "XSLT" },    { "xslt", SYN_LANG_MARKUP, "XSLT" },
    { "csproj", SYN_LANG_MARKUP, "MSBuild" }, { "vbproj", SYN_LANG_MARKUP, "MSBuild" },
    { "vcxproj", SYN_LANG_MARKUP, "MSBuild" }, { "fsproj", SYN_LANG_MARKUP, "MSBuild" },
    { "props", SYN_LANG_MARKUP, "MSBuild" }, { "targets", SYN_LANG_MARKUP, "MSBuild" },
    { "nuspec", SYN_LANG_MARKUP, "NuSpec" }, { "resx", SYN_LANG_MARKUP, "XML" },
    { "rss", SYN_LANG_MARKUP, "RSS" },     { "atom", SYN_LANG_MARKUP, "Atom" },
    { "opml", SYN_LANG_MARKUP, "OPML" },   { "kml", SYN_LANG_MARKUP, "KML" },
    { "gpx", SYN_LANG_MARKUP, "GPX" },     { "dae", SYN_LANG_MARKUP, "COLLADA" },
    { "ui", SYN_LANG_MARKUP, "Qt UI" },    { "qrc", SYN_LANG_MARKUP, "Qt Resource" },
    { "wsdl", SYN_LANG_MARKUP, "WSDL" },   { "xliff", SYN_LANG_MARKUP, "XLIFF" },
    { "xlf", SYN_LANG_MARKUP, "XLIFF" },
    /* Styles */
    { "css", SYN_LANG_CSS, "CSS" },        { "scss", SYN_LANG_CSS, "SCSS" },
    { "sass", SYN_LANG_CSS, "Sass" },      { "less", SYN_LANG_CSS, "Less" },
    { "styl", SYN_LANG_CSS, "Stylus" },    { "pcss", SYN_LANG_CSS, "PostCSS" },
    { "postcss", SYN_LANG_CSS, "PostCSS" },
    /* Data */
    { "json", SYN_LANG_JSON, "JSON" },     { "jsonc", SYN_LANG_JSON, "JSON" },
    { "json5", SYN_LANG_JSON, "JSON5" },   { "webmanifest", SYN_LANG_JSON, "JSON" },
    { "jsonl", SYN_LANG_JSON, "JSON Lines" }, { "ndjson", SYN_LANG_JSON, "JSON Lines" },
    { "geojson", SYN_LANG_JSON, "GeoJSON" }, { "topojson", SYN_LANG_JSON, "JSON" },
    { "har", SYN_LANG_JSON, "JSON" },      { "ipynb", SYN_LANG_JSON, "Jupyter" },
    { "avsc", SYN_LANG_JSON, "Avro" },     { "code-workspace", SYN_LANG_JSON, "JSON" },
    { "map", SYN_LANG_JSON, "Source Map" },
    { "yaml", SYN_LANG_YAML, "YAML" },     { "yml", SYN_LANG_YAML, "YAML" },
    { "raml", SYN_LANG_YAML, "RAML" },     { "cff", SYN_LANG_YAML, "YAML" },
    { "toml", SYN_LANG_TOML, "TOML" },     { "ini", SYN_LANG_TOML, "INI" },
    { "cfg", SYN_LANG_TOML, "Config" },    { "conf", SYN_LANG_TOML, "Config" },
    { "cnf", SYN_LANG_TOML, "Config" },    { "editorconfig", SYN_LANG_TOML, "EditorConfig" },
    { "properties", SYN_LANG_TOML, "Properties" },
    { "env", SYN_LANG_TOML, "Dotenv" },    { "gitconfig", SYN_LANG_TOML, "Git Config" },
    { "desktop", SYN_LANG_TOML, "Desktop Entry" },
    { "service", SYN_LANG_TOML, "systemd" }, { "timer", SYN_LANG_TOML, "systemd" },
    { "socket", SYN_LANG_TOML, "systemd" }, { "mount", SYN_LANG_TOML, "systemd" },
    { "target", SYN_LANG_TOML, "systemd" }, { "reg", SYN_LANG_TOML, "Registry" },
    { "inf", SYN_LANG_TOML, "INF" },
    { "gitignore", SYN_LANG_SCRIPT, "Git Ignore" },
    { "dockerignore", SYN_LANG_SCRIPT, "Docker Ignore" },
    { "npmignore", SYN_LANG_SCRIPT, "Ignore" },
    { "gitattributes", SYN_LANG_SCRIPT, "Git Attributes" },
    /* Assembly and build */
    { "s", SYN_LANG_ASM, "Assembly" },     { "asm", SYN_LANG_ASM, "Assembly" },
    { "nasm", SYN_LANG_ASM, "Assembly" },  { "assembly", SYN_LANG_ASM, "Assembly" },
    { "cmake", SYN_LANG_MAKE, "CMake" },   { "mk", SYN_LANG_MAKE, "Makefile" },
    { "make", SYN_LANG_MAKE, "Makefile" }, { "makefile", SYN_LANG_MAKE, "Makefile" },
    { "mak", SYN_LANG_MAKE, "Makefile" },  { "ninja", SYN_LANG_MAKE, "Ninja" },
    /* Documents */
    { "tex", SYN_LANG_LATEX, "LaTeX" },    { "latex", SYN_LANG_LATEX, "LaTeX" },
    { "sty", SYN_LANG_LATEX, "LaTeX" },    { "cls", SYN_LANG_LATEX, "LaTeX" },
    { "ltx", SYN_LANG_LATEX, "LaTeX" },    { "dtx", SYN_LANG_LATEX, "LaTeX" },
    { "bib", SYN_LANG_LATEX, "BibTeX" },   { "bibtex", SYN_LANG_LATEX, "BibTeX" },
    { "diff", SYN_LANG_DIFF, "Diff" },     { "patch", SYN_LANG_DIFF, "Patch" },
    { "rej", SYN_LANG_DIFF, "Patch" },
    /* Named for the status bar only; the scanner leaves them alone. */
    { "txt", SYN_LANG_NONE, "Text" },      { "text", SYN_LANG_NONE, "Text" },
    { "md", SYN_LANG_NONE, "Markdown" },   { "markdown", SYN_LANG_NONE, "Markdown" },
    { "mdx", SYN_LANG_NONE, "MDX" },       { "rst", SYN_LANG_NONE, "reStructuredText" },
    { "org", SYN_LANG_NONE, "Org" },       { "log", SYN_LANG_NONE, "Log" },
    { "csv", SYN_LANG_NONE, "CSV" },       { "tsv", SYN_LANG_NONE, "TSV" },
};

/* Extension-less files editors recognise by name. Matched case-insensitively
 * against the basename; a trailing ".suffix" is allowed for the ones that take
 * variants (Dockerfile.dev, Makefile.in, .env.production). */
static const TokMap BASENAMES[] = {
    { "Makefile", SYN_LANG_MAKE, "Makefile" },
    { "GNUmakefile", SYN_LANG_MAKE, "Makefile" },
    { "CMakeLists.txt", SYN_LANG_MAKE, "CMake" },
    { "Dockerfile", SYN_LANG_DOCKER, "Dockerfile" },
    { "Containerfile", SYN_LANG_DOCKER, "Dockerfile" },
    { "Justfile", SYN_LANG_SHELL, "Just" },
    { ".justfile", SYN_LANG_SHELL, "Just" },
    { "PKGBUILD", SYN_LANG_SHELL, "Bash" },
    { "APKBUILD", SYN_LANG_SHELL, "Shell" },
    { ".envrc", SYN_LANG_SHELL, "Shell" },
    { ".bashrc", SYN_LANG_SHELL, "Bash" },
    { ".bash_profile", SYN_LANG_SHELL, "Bash" },
    { ".bash_aliases", SYN_LANG_SHELL, "Bash" },
    { ".bash_logout", SYN_LANG_SHELL, "Bash" },
    { ".zshrc", SYN_LANG_SHELL, "Zsh" },
    { ".zshenv", SYN_LANG_SHELL, "Zsh" },
    { ".zprofile", SYN_LANG_SHELL, "Zsh" },
    { ".zlogin", SYN_LANG_SHELL, "Zsh" },
    { ".profile", SYN_LANG_SHELL, "Shell" },
    { "Rakefile", SYN_LANG_RUBY, "Ruby" },
    { "Gemfile", SYN_LANG_RUBY, "Ruby" },
    { "Podfile", SYN_LANG_RUBY, "Ruby" },
    { "Brewfile", SYN_LANG_RUBY, "Ruby" },
    { "Vagrantfile", SYN_LANG_RUBY, "Ruby" },
    { "Fastfile", SYN_LANG_RUBY, "Ruby" },
    { "Appfile", SYN_LANG_RUBY, "Ruby" },
    { "Guardfile", SYN_LANG_RUBY, "Ruby" },
    { "Capfile", SYN_LANG_RUBY, "Ruby" },
    { "Dangerfile", SYN_LANG_RUBY, "Ruby" },
    { ".irbrc", SYN_LANG_RUBY, "Ruby" },
    { ".pryrc", SYN_LANG_RUBY, "Ruby" },
    { "Jenkinsfile", SYN_LANG_JVM, "Groovy" },
    { "go.mod", SYN_LANG_GOMOD, "Go Module" },
    { "go.work", SYN_LANG_GOMOD, "Go Module" },
    { "BUILD", SYN_LANG_PYTHON, "Starlark" },
    { "BUILD.bazel", SYN_LANG_PYTHON, "Starlark" },
    { "WORKSPACE", SYN_LANG_PYTHON, "Starlark" },
    { "WORKSPACE.bazel", SYN_LANG_PYTHON, "Starlark" },
    { "MODULE.bazel", SYN_LANG_PYTHON, "Starlark" },
    { "Tiltfile", SYN_LANG_PYTHON, "Starlark" },
    { "SConstruct", SYN_LANG_PYTHON, "Python" },
    { "SConscript", SYN_LANG_PYTHON, "Python" },
    { "meson.build", SYN_LANG_PYTHON, "Meson" },
    { "meson_options.txt", SYN_LANG_PYTHON, "Meson" },
    { "wscript", SYN_LANG_PYTHON, "Python" },
    { ".gitconfig", SYN_LANG_TOML, "Git Config" },
    { ".gitmodules", SYN_LANG_TOML, "Git Config" },
    { ".npmrc", SYN_LANG_TOML, "npm Config" },
    { ".editorconfig", SYN_LANG_TOML, "EditorConfig" },
    { ".flake8", SYN_LANG_TOML, "INI" },
    { ".pylintrc", SYN_LANG_TOML, "INI" },
    { ".coveragerc", SYN_LANG_TOML, "INI" },
    { ".pypirc", SYN_LANG_TOML, "INI" },
    { "Pipfile", SYN_LANG_TOML, "TOML" },
    { ".env", SYN_LANG_TOML, "Dotenv" },
    { ".gitattributes", SYN_LANG_SCRIPT, "Git Attributes" },
    { ".gitignore", SYN_LANG_SCRIPT, "Git Ignore" },
    { ".dockerignore", SYN_LANG_SCRIPT, "Docker Ignore" },
    { ".yarnrc", SYN_LANG_SCRIPT, "Yarn Config" },
    { ".htaccess", SYN_LANG_SCRIPT, "Apache Config" },
    { ".inputrc", SYN_LANG_SCRIPT, "Readline" },
    { ".curlrc", SYN_LANG_SCRIPT, "Config" },
    { "CODEOWNERS", SYN_LANG_SCRIPT, "CODEOWNERS" },
    { "requirements.txt", SYN_LANG_SCRIPT, "Requirements" },
    { "constraints.txt", SYN_LANG_SCRIPT, "Requirements" },
    { ".clang-format", SYN_LANG_YAML, "YAML" },
    { ".clang-tidy", SYN_LANG_YAML, "YAML" },
    { ".gemrc", SYN_LANG_YAML, "YAML" },
    { "Procfile", SYN_LANG_YAML, "Procfile" },
    { ".prettierrc", SYN_LANG_JSON, "JSON" },
    { ".babelrc", SYN_LANG_JSON, "JSON" },
    { ".eslintrc", SYN_LANG_JSON, "JSON" },
    { ".swcrc", SYN_LANG_JSON, "JSON" },
    { "Package.resolved", SYN_LANG_JSON, "JSON" },
    { ".luacheckrc", SYN_LANG_LUA, "Lua" },
};

/* Resolve a bare token — a file extension or a fence info word — to its table
 * row. Shared by both public entry points so "```rs" and "main.rs" can never
 * drift. NULL when the token is unknown. */
static const TokMap *tok_lookup(const char *e, usize n) {
    if (n == 0) return NULL;
    for (usize i = 0; i < sizeof EXTS / sizeof EXTS[0]; i++)
        if (word_is(e, n, EXTS[i].tok)) return &EXTS[i];
    return NULL;
}

static SynLang lang_for_token(const char *e, usize n) {
    const TokMap *m = tok_lookup(e, n);
    return m ? (SynLang)m->lang : SYN_LANG_NONE;
}

/* The names that take a variant suffix: Dockerfile.dev, Makefile.in,
 * .env.production, Jenkinsfile.groovy. Everything else in BASENAMES matches
 * whole — "BUILD" must not claim build.bat, nor "Gemfile" Gemfile.lock. */
static const char *const BASE_VARIANTS[] = {
    "Dockerfile", "Containerfile", "Makefile", "GNUmakefile", ".env",
    "Jenkinsfile", "Vagrantfile", NULL
};

/* The basename row for `base` matched whole (case-insensitively), or NULL. */
static const TokMap *base_lookup(const char *base, usize blen) {
    for (usize i = 0; i < sizeof BASENAMES / sizeof BASENAMES[0]; i++)
        if (word_is(base, blen, BASENAMES[i].tok)) return &BASENAMES[i];
    return NULL;
}

/* The row for a variant of a BASE_VARIANTS name, or NULL. The suffix must
 * hang off a '.', '-' or '_' — "Dockerfiles.md" is not a Dockerfile. */
static const TokMap *base_variant_lookup(const char *base, usize blen) {
    for (i32 v = 0; BASE_VARIANTS[v]; v++) {
        usize n = strlen(BASE_VARIANTS[v]);
        if (blen <= n || strncasecmp(base, BASE_VARIANTS[v], n) != 0) continue;
        char c = base[n];
        if (c != '.' && c != '-' && c != '_') continue;
        return base_lookup(base, n);
    }
    return NULL;
}

/* Resolution order, shared by the language and the label so they agree:
 * a whole basename ("CMakeLists.txt" before its ".txt"), then a known
 * extension, then a basename variant ("Dockerfile.dev" has no known
 * extension, so the basename gets a second look). */
static const TokMap *path_lookup(const char *base, usize blen,
                                 const char *ext, usize elen) {
    const TokMap *m = base_lookup(base, blen);
    if (m) return m;
    if (ext) {
        m = tok_lookup(ext, elen);
        if (m && m->lang != SYN_LANG_NONE) return m;
    }
    const TokMap *v = base_variant_lookup(base, blen);
    if (v) return v;
    return m;   /* a labelled NONE row (".txt" → "Text"), or NULL */
}

/* Split a path into (basename, extension) the way both public path entry
 * points need them. `*ext_len` is 0 when there is no extension. */
static void path_parts(const char *path, usize len, const char **base,
                       usize *blen, const char **ext, usize *ext_len) {
    usize bstart = 0;
    for (usize i = 0; i < len; i++) if (path[i] == '/') bstart = i + 1;
    *base = path + bstart;
    *blen = len - bstart;
    const char *dot = NULL;
    for (usize i = 0; i < *blen; i++) if ((*base)[i] == '.') dot = *base + i;
    if (dot) { *ext = dot + 1; *ext_len = (usize)(*base + *blen - *ext); }
    else     { *ext = NULL;    *ext_len = 0; }
}

SynLang syntax_lang_for_path(const char *path, usize len) {
    if (!path || len == 0) return SYN_LANG_NONE;
    const char *base, *ext; usize blen, elen;
    path_parts(path, len, &base, &blen, &ext, &elen);
    /* Uppercase .S is GAS-with-cpp; the lookup is case-insensitive and lands
     * on assembly either way. */
    const TokMap *m = path_lookup(base, blen, ext, elen);
    return m ? (SynLang)m->lang : SYN_LANG_NONE;
}

const char *syntax_lang_label(const char *path, usize len) {
    if (!path || len == 0) return NULL;
    const char *base, *ext; usize blen, elen;
    path_parts(path, len, &base, &blen, &ext, &elen);
    const TokMap *m = path_lookup(base, blen, ext, elen);
    return m ? m->label : NULL;
}

SynLang syntax_lang_for_tag(const char *tag, usize len) {
    if (!tag || len == 0) return SYN_LANG_NONE;
    /* "```js title=foo" / "```rust,no_run" / "```c{1,3}" — the language is the
     * first word, everything after a delimiter is metadata. */
    usize n = 0;
    while (n < len && tag[n] != ' ' && tag[n] != '\t' && tag[n] != ',' &&
           tag[n] != '{' && tag[n] != ':' && tag[n] != ';')
        n++;
    while (n > 0 && tag[0] == '.') { tag++; n--; }   /* ".js" info strings */
    return lang_for_token(tag, n);
}

/* ---------------------------------------------------------- scan helpers - */

static bool in_table(const char *const *tab, const char *w, u32 n, bool ci) {
    if (!tab || n == 0) return false;
    char c0 = w[0];
    char c0l = (c0 >= 'A' && c0 <= 'Z') ? (char)(c0 + 32) : c0;
    for (i32 i = 0; tab[i]; i++) {
        const char *t = tab[i];
        if (ci) {
            char t0 = (t[0] >= 'A' && t[0] <= 'Z') ? (char)(t[0] + 32) : t[0];
            if (t0 != c0l) continue;
            if (strncasecmp(t, w, n) == 0 && t[n] == '\0') return true;
        } else {
            if (t[0] != c0) continue;
            if (strncmp(t, w, n) == 0 && t[n] == '\0') return true;
        }
    }
    return false;
}

static inline bool ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' ||
           (unsigned char)c >= 0x80;         /* keep UTF-8 identifiers whole */
}
static inline bool ident_start(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
           (unsigned char)c >= 0x80;
}
static inline bool ascii_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static inline bool digit(char c) { return c >= '0' && c <= '9'; }

/* Identifier continuation for a row: CSS and Lisp read '-' as part of a name,
 * Haskell and OCaml a trailing prime. */
static inline bool ident_more(const LangDef *d, char c) {
    if (ident_char(c)) return true;
    if ((d->flags & F_CSS) && c == '-') return true;
    if ((d->flags & F_PRIME_IDENT) && c == '\'') return true;
    if ((d->flags & F_DOT_IDENT) && c == '.') return true;
    if (d->flags & F_DASH_IDENT)
        return c == '-' || c == '?' || c == '!' || c == '*' || c == '+' ||
               c == '<' || c == '>' || c == '=' || c == '/';
    return false;
}

/* A name that reads as a type: capitalised AND carrying a lowercase letter, so
 * ALL_CAPS macros and SCREAMING constants stay out of it. */
static bool camel_case(const char *w, u32 n) {
    if (n < 2 || !(w[0] >= 'A' && w[0] <= 'Z')) return false;
    for (u32 i = 1; i < n; i++) if (w[i] >= 'a' && w[i] <= 'z') return true;
    return false;
}

static bool tok_at(const char *s, u32 len, u32 i, const char *t) {
    if (!t) return false;
    u32 n = (u32)strlen(t);
    return n > 0 && i + n <= len && memcmp(s + i, t, n) == 0;
}

/* Next non-blank byte at or after i, or len. */
static u32 skip_ws(const char *s, u32 len, u32 i) {
    while (i < len && (s[i] == ' ' || s[i] == '\t')) i++;
    return i;
}

/* Case-insensitive search for `needle` in s[from..len); len when absent. */
static u32 find_ci(const char *s, u32 len, u32 from, const char *needle) {
    u32 n = (u32)strlen(needle);
    if (n == 0) return len;
    for (u32 i = from; i + n <= len; i++)
        if (strncasecmp(s + i, needle, n) == 0) return i;
    return len;
}

/* `---` and nothing else on the line: an Astro frontmatter fence. */
static bool line_is_fence(const char *s, u32 len, u32 fns) {
    if (fns + 3 > len || memcmp(s + fns, "---", 3) != 0) return false;
    return skip_ws(s, len, fns + 3) >= len;
}

/* A line whose FIRST byte opens a tag. Inside <script>/<style>/frontmatter
 * this is how an unclosed region ends when the closer never comes — the
 * diff dock starts hunks mid-file, and a wrong guess there must not paint
 * the whole template as JavaScript. Real code in those regions never puts a
 * tag in column 0. */
static bool line_opens_tag(const char *s, u32 len) {
    if (len < 2 || s[0] != '<') return false;
    char c = s[1];
    if (c == '!') return true;
    if (c == '/') return len >= 3 && ascii_alpha(s[2]);
    return ascii_alpha(c);
}

/* An attribute whose quoted value is an EXPRESSION rather than text: Vue's
 * :prop / @event / #slot / v-*, Alpine's x-*, Angular's (event) / [prop] / *ngIf. */
static bool attr_is_directive(const char *a, u32 n) {
    if (n == 0) return false;
    char c = a[0];
    if (c == ':' || c == '@' || c == '#' || c == '*' || c == '(' || c == '[') return true;
    return n > 2 && (a[0] == 'v' || a[0] == 'x') && a[1] == '-';
}

typedef struct {
    SynSpan *out;
    i32      cap;
    i32      cnt;
} Emitter;

static void emit(Emitter *e, u32 off, u32 len, SynKind k) {
    if (len == 0) return;
    if (!e->out || e->cnt >= e->cap) { if (!e->out) e->cnt++; return; }
    e->out[e->cnt].off  = off;
    e->out[e->cnt].len  = len;
    e->out[e->cnt].kind = (u8)k;
    e->cnt++;
}

/* Walk to the end of a block comment that is already open. Returns the index
 * just past the closer, or `len` when the comment runs off the line; *depth is
 * decremented per closer (and incremented per nested opener). */
static u32 eat_block(const LangDef *d, const char *s, u32 len, u32 i, u32 *depth) {
    u32 ob = d->bo ? (u32)strlen(d->bo) : 0;
    u32 cb = d->bc ? (u32)strlen(d->bc) : 0;
    while (i < len) {
        if ((d->flags & F_NESTBLK) && tok_at(s, len, i, d->bo)) {
            (*depth)++; i += ob; continue;
        }
        if (tok_at(s, len, i, d->bc)) {
            i += cb;
            if (*depth > 0) (*depth)--;
            if (*depth == 0) return i;
            continue;
        }
        i++;
    }
    return len;
}

/* Walk to the end of a quoted run opened with `q`. `triple` scans for three
 * consecutive quotes instead of one. Returns the index just past the closer, or
 * `len` if the quote runs off the end of the line.
 *
 * `*closed` reports which of those two happened — the return value cannot,
 * because a string whose closing quote is the LAST byte of the line also ends
 * at `len`. Reading "ended at len" as "unterminated" would leave a
 * self-contained `"""one line"""` marking every following line as string. */
static u32 eat_quoted(const char *s, u32 len, u32 i, char q, bool triple,
                      bool *closed) {
    *closed = false;
    while (i < len) {
        if (s[i] == '\\' && i + 1 < len) { i += 2; continue; }
        if (s[i] == q) {
            if (!triple) { *closed = true; return i + 1; }
            if (i + 2 < len && s[i+1] == q && s[i+2] == q) {
                *closed = true;
                return i + 3;
            }
        }
        i++;
    }
    return len;
}

/* ---- heredocs ---- */

static u32 heredoc_hash(const char *w, u32 n) {
    u32 h = 2166136261u;                         /* FNV-1a, folded to six bits */
    for (u32 k = 0; k < n; k++) h = (h ^ (u8)w[k]) * 16777619u;
    return (h ^ (h >> 6) ^ (h >> 12) ^ (h >> 18) ^ (h >> 24)) & ST_HD_HASH_MASK;
}

/* The state a heredoc's body is scanned in: marker length (capped, in the
 * depth nibble), whether the marker may be indented, and the marker's hash. */
static u32 heredoc_state(const char *w, u32 n, bool indent) {
    u32 st = st_with_depth(st_with_str(0, STR_HEREDOC), n);
    if (indent) st |= ST_HD_INDENT;
    return st | (heredoc_hash(w, n) << ST_HD_HASH_SHIFT);
}

/* A heredoc opener at s[i] == '<': `<<WORD`, `<<-WORD`, `<< 'WORD'`, `<<\WORD`
 * in a shell row, `<<<WORD` / `<<<'WORD'` at the end of the line in PHP.
 * Returns the index past the marker and writes the state the NEXT line starts
 * in; 0 when this is something else — a here-string (`<<<` in a shell), or a
 * shift, whose right-hand side is a number, a `$variable` or a parenthesis
 * and so does not start like a marker. */
static u32 heredoc_open(const LangDef *d, const char *s, u32 end, u32 i, u32 *next) {
    bool php = (d->flags & F_HEREDOC3) != 0;
    bool indent = php;                 /* PHP's closing marker may be indented */
    u32 j = i + (php ? 3 : 2);
    if (php) {
        if (!(i + 2 < end && s[i+2] == '<')) return 0;       /* `<<` is a shift */
    } else {
        if (j < end && s[j] == '<') return 0;
        if (j < end && s[j] == '-') { indent = true; j++; }
    }
    j = skip_ws(s, end, j);
    if (j >= end) return 0;

    char q = (s[j] == '"' || s[j] == '\'') ? s[j] : '\0';
    if (q) j++;
    else if (!php && s[j] == '\\') j++;
    u32 w0 = j;
    if (q) {
        while (j < end && s[j] != q) j++;
        if (j >= end) return 0;
    } else {
        if (j >= end || !ident_start(s[j])) return 0;
        /* A shell word runs to the next blank or operator; PHP's is a name. */
        while (j < end && (php ? ident_char(s[j]) : !strchr(" \t\r;|&<>()`\"'", s[j])))
            j++;
    }
    u32 n = j - w0;
    if (n == 0) return 0;
    if (q) j++;
    if (php) {                          /* nothing may follow the opener */
        u32 e = skip_ws(s, end, j);
        if (e < end && s[e] != '\r') return 0;
    }
    *next = heredoc_state(s + w0, n, indent);
    return j;
}

/* Whether the body line s[beg..end) is the marker that `st` is waiting for.
 * *mark_end gets the index past the marker: the end of the line for a shell,
 * whose marker stands alone, and the end of the name for PHP, where `;` or
 * `)` may follow it. */
static bool heredoc_ends(const LangDef *d, const char *s, u32 beg, u32 end, u32 st,
                         u32 *mark_end) {
    u32 i = beg, e = end;
    if (e > i && s[e-1] == '\r') e--;            /* a CRLF file's marker */
    if (st & ST_HD_INDENT) i = skip_ws(s, e, i);
    if (d->flags & F_HEREDOC3) {
        u32 j = i;
        while (j < e && ident_char(s[j])) j++;
        e = j;
    }
    u32 n = e - i;
    if (n == 0 || (n > 15 ? 15 : n) != st_depth(st)) return false;
    if (heredoc_hash(s + i, n) != ((st >> ST_HD_HASH_SHIFT) & ST_HD_HASH_MASK)) return false;
    *mark_end = (d->flags & F_HEREDOC3) ? e : end;
    return true;
}

/* ---- regex literals ---- */

/* Whether a `/` at s[i] can begin a regex literal: only where an operand is
 * expected — at the start of the range, after an opening or separating
 * punctuator, or after a keyword that takes an expression. Anywhere else a
 * value has just ended and the slash divides. `<` is not in the set on
 * purpose: `</div>` is a JSX closer. */
static bool regex_can_start(const char *s, u32 beg, u32 i) {
    static const char *const before[] = {
        "return","typeof","case","throw","void","delete","yield","await",
        "instanceof","in","of","do","else", NULL
    };
    while (i > beg && (s[i-1] == ' ' || s[i-1] == '\t')) i--;
    if (i == beg) return true;
    char p = s[i-1];
    if (p != '\0' && strchr("(,=:[!&|?{};", p)) return true;
    if (!ident_char(p)) return false;
    u32 e = i;
    while (i > beg && e - i <= 10 && ident_char(s[i-1])) i--;   /* "instanceof" */
    return in_table(before, s + i, e - i, false);
}

/* The end of the regex literal that starts at s[i] == '/': the index past its
 * closing slash and flags, or 0 when the line holds no closing slash (it was
 * a division after all). A slash inside a [class] does not close. */
static u32 eat_regex(const char *s, u32 len, u32 i) {
    bool cls = false;
    for (u32 j = i + 1; j < len; j++) {
        char c = s[j];
        if (c == '\\') { j++; continue; }
        if (c == '[') cls = true;
        else if (c == ']') cls = false;
        else if (c == '/' && !cls) {
            j++;
            while (j < len && ascii_alpha(s[j])) j++;
            return j;
        }
    }
    return 0;
}

/* ---- spanning strings ---- */

/* Walk the string the state says is open, from `i`. `s0` is where its span
 * starts: `i` for a string carried in from the line above, the opener's
 * offset for one this line opened. Stops at the first of
 *   - the closer: the string's state is cleared and the index past it returned;
 *   - a `${` in a row with F_INTERP (a `$(` in a shell's "…"): an expression
 *     frame is pushed, the state is "code" again and the index past the
 *     opener is returned;
 *   - the end of the line: the state is left as it is, for the next line.
 * It always advances or clears the state, so the caller's loop cannot stall. */
static u32 scan_open_str(const LangDef *d, const char *s, u32 end, u32 s0, u32 i,
                         u32 *state, Emitter *em) {
    u32  st   = *state;
    bool bt   = (st & ST_BACKTICK) != 0;
    u32  kind = bt ? STR_NONE : st_str(st);

    if (kind == STR_HEREDOC) {
        u32 mark_end = end;
        if (heredoc_ends(d, s, i, end, st, &mark_end)) {
            /* Opened inside the `$( )` of a quoted string: that string is
             * open again, and so is the frame of one further out. */
            u32 frames = (st >> ST_HD_FRAMES_SHIFT) & 3u;
            st = st_with_depth(st & ~(ST_STR_MASK | ST_AUX_MASK), 0);
            if (frames >= 1) st = st_with_str(st, STR_DQ);
            if (frames >= 2) st = st_with_frame(st, 0, 1u | FRAME_DQ);
        }
        emit(em, s0, mark_end - s0, SYN_STRING);
        *state = st;
        return mark_end;
    }
    if (kind == STR_TEXT) {
        /* Markup between ?> and the next <?: left in the default colour. */
        u32 p = i;
        while (p + 1 < end && !(s[p] == '<' && s[p+1] == '?')) p++;
        if (p + 1 >= end) return end;
        u32 n = tok_at(s, end, p, "<?php") ? 5 : (p + 2 < end && s[p+2] == '=') ? 3 : 2;
        emit(em, p, n, SYN_PREPROC);
        *state = st & ~ST_STR_MASK;
        return p + n;
    }
    if (!bt && (kind < STR_DQ || kind > STR_RAW)) {      /* not a state we make */
        *state = st & ~ST_STR_MASK;
        return i;
    }

    char q      = bt ? '`' : (kind == STR_SQ || kind == STR_QQ) ? '\'' : '"';
    bool esc    = bt ? !(d->flags & F_BT_RAW)
                     : kind == STR_DQ || (kind == STR_SQ && !(d->flags & F_SH_QUOTE));
    bool interp = (d->flags & F_INTERP) && (bt || kind == STR_DQ || kind == STR_QQ);
    bool cmdsub = (d->flags & F_SH_QUOTE) && kind == STR_DQ;
    u32  hashes = kind == STR_RAW ? st_depth(st) : 0;

    while (i < end) {
        char c = s[i];
        if (esc && c == '\\') { i = (i + 2 <= end) ? i + 2 : end; continue; }
        /* Nix: `$$` is a dollar sign, and the `{` after it is text. */
        if (interp && c == '$' && (d->flags & F_QQ_STR) && i + 1 < end && s[i+1] == '$') {
            i += 2;
            continue;
        }
        /* `${` (a shell's `$(`, but not the arithmetic `$((`): code until the
         * brace that balances it, with strings of its own. */
        if (c == '$' && i + 1 < end &&
            ((interp && s[i+1] == '{') ||
             (cmdsub && s[i+1] == '(' && !(i + 2 < end && s[i+2] == '(')))) {
            i32 top = st_frame_top(st);
            if (top < 1) {
                emit(em, s0, i - s0, SYN_STRING);
                emit(em, i, 2, cmdsub ? SYN_TYPE : SYN_PREPROC);
                st = st_with_frame(st & ~(ST_BACKTICK | ST_STR_MASK), (u32)(top + 1),
                                   1u | (kind == STR_DQ ? FRAME_DQ : 0u));
                *state = st;
                return i + 2;
            }
            i += 2;                  /* deeper than the frames go: text */
            continue;
        }
        if (c != q) { i++; continue; }

        if (kind == STR_QQ) {
            /* Nix: the closer is two quotes, and ''' / ''$ / ''\x are how the
             * string spells a quote pair, a dollar and an escape. */
            if (!(i + 1 < end && s[i+1] == '\'')) { i++; continue; }
            if (i + 2 < end && (s[i+2] == '\'' || s[i+2] == '$')) { i += 3; continue; }
            if (i + 2 < end && s[i+2] == '\\') { i = (i + 4 <= end) ? i + 4 : end; continue; }
            i += 2;
        } else if (kind == STR_RAW) {
            u32 h = 0;
            while (h < hashes && i + 1 + h < end && s[i+1+h] == '#') h++;
            if (h < hashes) { i++; continue; }           /* a quote inside it */
            i += 1 + hashes;
            st = st_with_depth(st, 0);
        } else {
            i++;
        }
        emit(em, s0, i - s0, SYN_STRING);
        *state = st & ~(ST_BACKTICK | ST_STR_MASK);
        return i;
    }
    emit(em, s0, end - s0, SYN_STRING);
    return end;
}

/* ------------------------------------------------------------- the diff -- */

/* A unified diff is decided by its first column, whole line at a time. The
 * header shapes are tested before the '+'/'-' rule because "+++ b/x" and
 * "--- a/x" start with the same bytes as an added or removed line. */
static void scan_diff_line(const char *s, u32 len, Emitter *em) {
    static const char *const hdr[] = {
        "diff ", "index ", "new file", "deleted file", "old mode", "new mode",
        "similarity", "rename ", "copy ", "Binary files", "\\ No newline",
        "From ", "Date: ", "Subject: ", "Author: ", NULL
    };
    SynKind k = SYN_TEXT;
    if (len == 0) return;
    if (tok_at(s, len, 0, "+++ ") || tok_at(s, len, 0, "--- ") ||
        (len == 3 && memcmp(s, "---", 3) == 0))
        k = SYN_COMMENT;
    else if (tok_at(s, len, 0, "@@"))
        k = SYN_PREPROC;
    else if (s[0] == '+')
        k = SYN_INSERT;
    else if (s[0] == '-')
        k = SYN_DELETE;
    else {
        for (i32 i = 0; hdr[i]; i++)
            if (tok_at(s, len, 0, hdr[i])) { k = SYN_COMMENT; break; }
    }
    if (k != SYN_TEXT) emit(em, 0, len, k);
}

/* ------------------------------------------------------------------ scan -- */

/* Scan s[beg..end) as `lang`. Emits spans with absolute offsets and updates
 * *state. Returns where it stopped: `end`, or earlier when
 *   - a HOST row opened an embedded region (the state now names it and the
 *     caller switches rows), or
 *   - `brace` is non-NULL and the '}' that brings *brace to zero was reached
 *     (returns the index just past it).
 * `fns` is the line's first non-blank column, which the line-start rules
 * (C preprocessor, YAML keys, asm labels) compare against. */
static u32 scan_range(SynLang lang, const char *s, u32 beg, u32 end, u32 *state,
                      Emitter *em, u32 fns, i32 *brace) {
    const LangDef *d = &LANGS[lang];
    u32 st = *state;
    u32 i  = beg;
    bool ci = (d->flags & F_CI_KW) != 0;
    bool js_attr = false;        /* markup: last attribute was a directive */
    u32  heredoc = 0;            /* state for the NEXT line: a <<WORD was seen */
    bool arith   = false;        /* shell: inside (( )), where << is a shift */
    u32  re_miss = 0;            /* JS: slashes tried as a regex and found open */

    if (d->flags & F_DIFF) {
        scan_diff_line(s, end, em);
        return end;
    }

    /* ---- continuations opened by an earlier line ---- */
    if (st & ST_BLOCK) {
        u32 depth = st_depth(st);
        if (depth == 0) depth = 1;
        u32 e = eat_block(d, s, end, i, &depth);
        emit(em, i, e - i, SYN_COMMENT);
        if (e >= end && depth > 0) {
            *state = st_with_depth(st | ST_BLOCK, depth);
            return end;
        }
        st = st_with_depth(st & ~ST_BLOCK, 0);
        i = e;
    } else if (st & (ST_TRIPLE_D | ST_TRIPLE_S)) {
        char q = (st & ST_TRIPLE_D) ? '"' : '\'';
        bool closed = false;
        u32 e = eat_quoted(s, end, i, q, true, &closed);
        emit(em, i, e - i, SYN_STRING);
        if (!closed) { *state = st; return end; }
        st &= ~(ST_TRIPLE_D | ST_TRIPLE_S);
        i = e;
    } else if (st & (ST_BACKTICK | ST_STR_MASK)) {
        /* A string, a heredoc body or (PHP) the text outside the code. */
        i = scan_open_str(d, s, end, i, i, &st, em);
        if (i >= end) { *state = st; return end; }
    }

    /* Shebang: the one '#' worth calling out on its own. */
    if (i == 0 && end > 1 && s[0] == '#' && s[1] == '!') {
        emit(em, 0, end, SYN_PREPROC);
        *state = st;
        return end;
    }

    /* [section] headers (TOML / INI) — the whole bracket run is the name. */
    if ((d->flags & F_SECTION) && i <= fns && fns < end && s[fns] == '[') {
        u32 j = fns;
        while (j < end && s[j] != ']') j++;
        if (j < end) j++;
        emit(em, fns, j - fns, SYN_TAG);
        i = j;
    }

    bool css_value = false;      /* CSS: between ':' and ';' is a value */

    while (i < end) {
        char c = s[i];

        if (c == ' ' || c == '\t') { i++; continue; }

        /* Markup text content is prose, not code: an apostrophe in "don't"
         * is not a string opener and "see foo()" is not a call. Strings and
         * identifiers are only read inside a tag. */
        bool prose = (d->flags & F_TAGS) && !(st & ST_TAG);

        /* ---- markup: inside an open tag, or opening one ---- */
        if (d->flags & F_TAGS) {
            if (st & ST_TAG) {
                if (c == '>') {
                    emit(em, i, 1, SYN_TAG); i++;
                    u32 pend = st & (ST_TAG_SCRIPT | ST_TAG_STYLE);
                    st &= ~(ST_TAG | ST_TAG_SCRIPT | ST_TAG_STYLE);
                    /* The tag just closed was <script …> / <style …>: what
                     * follows is another language until its closer. */
                    if (pend & ST_TAG_SCRIPT) { st = st_with_embed(st, EMB_SCRIPT); goto out; }
                    if (pend & ST_TAG_STYLE)  { st = st_with_embed(st, EMB_STYLE);  goto out; }
                    continue;
                }
                if (c == '/' && i + 1 < end && s[i+1] == '>') {
                    emit(em, i, 2, SYN_TAG); i += 2;
                    st &= ~(ST_TAG | ST_TAG_SCRIPT | ST_TAG_STYLE);
                    continue;
                }
                if ((d->flags & F_BRACE) && c == '{') {
                    /* attr={expr} — ST_TAG stays set so the host resumes
                     * inside this tag once the expression closes. */
                    emit(em, i, 1, SYN_PREPROC); i++;
                    st = st_with_brace(st_with_embed(st, EMB_BRACE), 1);
                    goto out;
                }
                if (c == '"' || c == '\'') {
                    bool closed = false;
                    u32 e = eat_quoted(s, end, i + 1, c, false, &closed);
                    if (js_attr && d->expr && closed && e - i > 2) {
                        /* :prop="expr" — the value is code, in the row the
                         * host names for its expressions. Single-line only:
                         * the scratch state is thrown away. */
                        u32 tmp = 0;
                        emit(em, i, 1, SYN_STRING);
                        scan_range((SynLang)d->expr, s, i + 1, e - 1, &tmp, em, fns, NULL);
                        emit(em, e - 1, 1, SYN_STRING);
                    } else {
                        emit(em, i, e - i, SYN_STRING);
                    }
                    js_attr = false;
                    i = e; continue;
                }
                if (ident_start(c) || c == '-' || c == ':' || c == '@' || c == '#' ||
                    c == '*' || c == '(' || c == '[') {
                    u32 s0 = i; i++;
                    while (i < end && (ident_char(s[i]) || s[i] == '-' || s[i] == ':' ||
                           s[i] == '.' || s[i] == '|' || s[i] == '[' || s[i] == ']' ||
                           s[i] == ')'))
                        i++;
                    emit(em, s0, i - s0, SYN_ATTR);
                    js_attr = d->expr && attr_is_directive(s + s0, i - s0);
                    continue;
                }
                i++; continue;
            }
            if (c == '<' && !tok_at(s, end, i, d->bo)) {
                if ((d->flags & F_ERB) && i + 1 < end && s[i+1] == '%') {
                    /* <% code %>, <%= expr %>, <%- trimmed %>, <%# comment %>.
                     * "<%%" is a literal percent sign and stays text. */
                    u32 n = 2;
                    if (i + 2 < end && s[i+2] == '%') { i += 3; continue; }
                    bool cmt = (i + 2 < end && s[i+2] == '#');
                    if (i + 2 < end && (s[i+2] == '=' || s[i+2] == '-' || s[i+2] == '#' ||
                                        s[i+2] == '@'))
                        n = 3;
                    emit(em, i, n, cmt ? SYN_COMMENT : SYN_PREPROC);
                    i += n;
                    st = st_with_embed(st, cmt ? EMB_CODECMT : EMB_CODE);
                    goto out;
                }
                u32 s0 = i, j = i + 1;
                bool closing = false;
                if (j < end && (s[j] == '/' || s[j] == '!' || s[j] == '?')) {
                    closing = (s[j] == '/');
                    j++;
                }
                if (j < end && ident_start(s[j])) {
                    u32 n0 = j;
                    while (j < end && (ident_char(s[j]) || s[j] == '-' || s[j] == ':')) j++;
                    emit(em, s0, j - s0, SYN_TAG);
                    st |= ST_TAG;
                    if ((d->flags & F_EMBED) && !closing) {
                        u32 nl = j - n0;
                        if (nl == 6 && strncasecmp(s + n0, "script", 6) == 0) st |= ST_TAG_SCRIPT;
                        if (nl == 5 && strncasecmp(s + n0, "style", 5) == 0)  st |= ST_TAG_STYLE;
                    }
                    i = j; continue;
                }
                i++; continue;
            }
            if ((d->flags & F_TMPL) && c == '{' && i + 1 < end) {
                char n1 = s[i+1];
                if (n1 == '{') {
                    if (tok_at(s, end, i, "{{!--")) {
                        emit(em, i, 5, SYN_COMMENT); i += 5;
                        st = st_with_embed(st, EMB_DASHCMT); goto out;
                    }
                    if (tok_at(s, end, i, "{{!")) {
                        emit(em, i, 3, SYN_COMMENT); i += 3;
                        st = st_with_embed(st, EMB_BANGCMT); goto out;
                    }
                    emit(em, i, 2, SYN_PREPROC); i += 2;
                    /* Handlebars block sigils: {{#each}} {{/each}} {{^none}} {{> part}} */
                    if (i + 1 < end && (s[i] == '#' || s[i] == '/' || s[i] == '^' || s[i] == '>') &&
                        ident_start(s[i+1])) {
                        u32 s0 = i; i++;
                        while (i < end && (ident_char(s[i]) || s[i] == '-' || s[i] == '.' || s[i] == '/')) i++;
                        emit(em, s0, i - s0, SYN_KEYWORD);
                    }
                    st = st_with_embed(st, EMB_MUSTACHE); goto out;
                }
                if (n1 == '%') {
                    u32 n = (i + 2 < end && s[i+2] == '-') ? 3 : 2;
                    emit(em, i, n, SYN_PREPROC); i += n;
                    st = st_with_embed(st, EMB_STMT); goto out;
                }
                if (n1 == '#') {
                    emit(em, i, 2, SYN_COMMENT); i += 2;
                    st = st_with_embed(st, EMB_HASHCMT); goto out;
                }
            }
            if ((d->flags & F_BRACE) && c == '{') {
                emit(em, i, 1, SYN_PREPROC); i++;
                /* Svelte's block syntax: {#if x} {:else} {/if} {@html s}. Only
                 * there — HEEx's {@assign} is an Elixir expression. */
                if (lang == SYN_LANG_SVELTE && i + 1 < end &&
                    (s[i] == '#' || s[i] == ':' || s[i] == '/' || s[i] == '@') &&
                    ident_start(s[i+1])) {
                    u32 s0 = i; i++;
                    while (i < end && ident_char(s[i])) i++;
                    emit(em, s0, i - s0, SYN_KEYWORD);
                }
                st = st_with_brace(st_with_embed(st, EMB_BRACE), 1);
                goto out;
            }
        }

        /* ---- comments ---- */
        if (tok_at(s, end, i, d->bo)) {
            u32 s0 = i, depth = 1;
            i = eat_block(d, s, end, i + (u32)strlen(d->bo), &depth);
            emit(em, s0, i - s0, SYN_COMMENT);
            if (i >= end && depth > 0) st = st_with_depth(st | ST_BLOCK, depth);
            continue;
        }
        {
            bool linecmt = false;
            for (i32 k = 0; k < 3 && d->lc[k]; k++) {
                /* PHP/SQL '#' and asm ';' are only comments where they can't be
                 * anything else; F_HASH_BOL pins '#' to the line start so an
                 * `#immediate` operand in ARM asm keeps its colour. */
                if ((d->flags & F_HASH_BOL) && d->lc[k][0] == '#' && i != fns) continue;
                /* A shell's '#' comments only where a word starts: in `a#b`,
                 * a sed script and `http://x/#top` it is the word's own, and
                 * a comment there would hide the quote or paren that follows. */
                if ((d->flags & F_SH_QUOTE) && i > beg && !strchr(" \t;|&()<>", s[i-1])) continue;
                if (tok_at(s, end, i, d->lc[k])) { linecmt = true; break; }
            }
            if (!linecmt && (d->flags & F_SEMI_CMT) && c == ';') linecmt = true;
            /* CSS: the `//` of `https://` is a URL's, wherever it stands. */
            if (linecmt && (d->flags & F_CSS) && i > beg && s[i-1] == ':') linecmt = false;
            if (linecmt) {
                u32 e = end;
                if (d->flags & F_CODE_TAGS)          /* PHP: `// note ?>` */
                    for (u32 j = i; j + 1 < end; j++)
                        if (s[j] == '?' && s[j+1] == '>') { e = j; break; }
                emit(em, i, e - i, SYN_COMMENT);
                i = e;
                continue;
            }
        }

        /* Rows without code extras skip this path. */
        if (d->flags & F_CODE_EXTRAS) {
            /* JS regex literal: its body is not scanned for comment openers.
             * A try that finds no closing slash read the rest of the line
             * for nothing; real code has one or none of those on a line, and
             * a line built of them must not cost its length squared. */
            if (c == '/' && (d->flags & F_REGEX) && re_miss < 4 && regex_can_start(s, beg, i)) {
                u32 e = eat_regex(s, end, i);
                if (e) { emit(em, i, e - i, SYN_STRING); i = e; continue; }
                re_miss++;
            }

            /* PHP: where the code ends and the page's markup begins. */
            if (d->flags & F_CODE_TAGS) {
                if (c == '?' && i + 1 < end && s[i+1] == '>') {
                    emit(em, i, 2, SYN_PREPROC);
                    st = st_with_str(st, STR_TEXT);
                    i = scan_open_str(d, s, end, i + 2, i + 2, &st, em);
                    continue;
                }
                if (c == '<' && i + 1 < end && s[i+1] == '?') {
                    u32 n = tok_at(s, end, i, "<?php") ? 5 : (i + 2 < end && s[i+2] == '=') ? 3 : 2;
                    emit(em, i, n, SYN_PREPROC);
                    i += n;
                    continue;
                }
                /* A line that BEGINS with a tag is markup: a page before its
                 * first <?php, or a hunk that starts between two code islands.
                 * PHP code never puts `<name` first on a line. */
                if (c == '<' && i == fns && i + 1 < end &&
                    (ascii_alpha(s[i+1]) || s[i+1] == '!' ||
                     (s[i+1] == '/' && i + 2 < end && ascii_alpha(s[i+2])))) {
                    st = st_with_str(st, STR_TEXT);
                    i = scan_open_str(d, s, end, i, i, &st, em);
                    continue;
                }
            }

            /* Shell: a backslash in code quotes the next byte. */
            if (c == '\\' && (d->flags & F_SH_QUOTE)) {
                i = (i + 2 <= end) ? i + 2 : end;
                continue;
            }

            /* Heredoc opener: the body starts on the next line. */
            if (c == '<' && (d->flags & (F_HEREDOC | F_HEREDOC3)) &&
                i + 1 < end && s[i+1] == '<') {
                u32 e = (heredoc || arith) ? 0 : heredoc_open(d, s, end, i, &heredoc);
                if (e) {
                    u32 w = i + ((d->flags & F_HEREDOC3) ? 3 : 2);
                    if (w < e && s[w] == '-') w++;
                    w = skip_ws(s, e, w);
                    emit(em, w, e - w, SYN_STRING);          /* the marker */
                    i = e;
                } else {
                    while (i < end && s[i] == '<') i++;   /* a shift, a here-string */
                }
                continue;
            }
        }

        /* ---- preprocessor / attributes ---- */
        if (c == '#') {
            if ((d->flags & F_ATTR_HASH) && i + 1 < end &&
                (s[i+1] == '[' || s[i+1] == '!')) {
                /* To the bracket that balances `#[`. A string inside is
                 * stepped over whole — so `note = "use [`x`]"` does not end
                 * the attribute at its ']' and leave a lone quote behind —
                 * and one that runs off the line is carried like any other.
                 */
                u32 s0 = i;
                i32 nest = 0;
                while (i < end) {
                    char a = s[i];
                    if (a == '"') {
                        bool closed = false;
                        i = eat_quoted(s, end, i + 1, '"', false, &closed);
                        if (!closed && (d->flags & F_DQ_SPAN)) st = st_with_str(st, STR_DQ);
                        continue;
                    }
                    i++;
                    if (a == '[') nest++;
                    else if (a == ']' && --nest <= 0) break;
                }
                emit(em, s0, i - s0, SYN_ATTR);
                continue;
            }
            if ((d->flags & F_PREPROC) && i == fns) {
                u32 s0 = i; i++;
                i = skip_ws(s, end, i);
                while (i < end && ident_char(s[i])) i++;
                emit(em, s0, i - s0, SYN_PREPROC);
                continue;
            }
            if ((d->flags & F_CSS) && i + 1 < end && ident_char(s[i+1])) {
                /* '#fff' in a value is a colour; '#id' outside one is a selector. */
                u32 s0 = i; i++;
                while (i < end && ident_char(s[i])) i++;
                emit(em, s0, i - s0, css_value ? SYN_NUMBER : SYN_TAG);
                continue;
            }
            if (d->flags & F_ASM) {   /* '#1' immediates read as numbers */
                u32 s0 = i; i++;
                while (i < end && (ident_char(s[i]) || s[i] == '-')) i++;
                emit(em, s0, i - s0, SYN_NUMBER);
                continue;
            }
        }

        /* ---- TeX: \command, and $inline math$ ---- */
        if (d->flags & F_BS_CMD) {
            if (c == '\\' && i + 1 < end) {
                u32 s0 = i; i++;
                if (ascii_alpha(s[i])) {
                    while (i < end && (ascii_alpha(s[i]) || s[i] == '@')) i++;
                } else {
                    i++;                  /* \\ \% \{ — a one-byte control symbol */
                }
                emit(em, s0, i - s0, SYN_KEYWORD);
                continue;
            }
            if (c == '$') {
                u32 j = i + 1;
                while (j < end && s[j] != '$') { if (s[j] == '\\' && j + 1 < end) j++; j++; }
                if (j < end) {
                    emit(em, i, j + 1 - i, SYN_STRING);
                    i = j + 1; continue;
                }
                i++; continue;
            }
        }

        /* ---- annotations / decorators / at-rules ---- */
        if (c == '@' && (d->flags & F_ATTR_AT) && i + 1 < end && ident_start(s[i+1])) {
            u32 s0 = i; i++;
            while (i < end && (ident_char(s[i]) || s[i] == '-' || s[i] == '.')) i++;
            emit(em, s0, i - s0, (d->flags & F_CSS) ? SYN_KEYWORD : SYN_ATTR);
            continue;
        }
        /* Objective-C @keywords live in KW_C with their '@', so match there. */
        if (c == '@' && lang == SYN_LANG_C && i + 1 < end && ident_start(s[i+1])) {
            u32 s0 = i, j = i + 1;
            while (j < end && ident_char(s[j])) j++;
            if (in_table(d->kw, s + s0, j - s0, false)) {
                emit(em, s0, j - s0, SYN_KEYWORD);
                i = j; continue;
            }
        }
        /* Ruby @ivar / @@cvar and :symbol; Elixir and Clojure :atoms. */
        if ((d->flags & F_SIGIL_RB) && (c == '@' || c == ':') &&
            i + 1 < end && (ident_start(s[i+1]) || s[i+1] == '@')) {
            u32 s0 = i; i++;
            if (i < end && s[i] == '@') i++;
            while (i < end && ident_more(d, s[i])) i++;
            emit(em, s0, i - s0, c == '@' ? SYN_TYPE : SYN_STRING);
            continue;
        }

        /* ---- shell / make / PHP variables ---- */
        if (c == '$' && (d->flags & F_DOLLARVAR) && i + 1 < end) {
            char nx = s[i+1];
            if (nx == '(' && (d->flags & F_SH_QUOTE) && !(i + 2 < end && s[i+2] == '(')) {
                /* $( ) holds commands — code, with quotes and heredocs of its
                 * own — so only the opener takes the variable's colour. The
                 * arithmetic $(( )) below is still one token. */
                emit(em, i, 2, SYN_TYPE);
                st = st_frame_nest(st);
                i += 2;
                continue;
            }
            if (ident_start(nx) || nx == '{' || nx == '(' || nx == '@' ||
                nx == '*' || nx == '#' || nx == '?' || nx == '!' || digit(nx)) {
                u32 s0 = i; i += 2;
                if (nx == '{' || nx == '(') {
                    char close = (nx == '{') ? '}' : ')';
                    i32 depth = 1;
                    while (i < end && depth > 0) {
                        if (s[i] == nx) depth++;
                        else if (s[i] == close) depth--;
                        i++;
                    }
                } else if (ident_start(nx)) {
                    while (i < end && ident_char(s[i])) i++;
                }
                emit(em, s0, i - s0, SYN_TYPE);
                continue;
            }
        }
        /* ---- Batch %VAR%, %1, %~dp0, %%i ---- */
        if (c == '%' && (d->flags & F_PCTVAR) && i + 1 < end) {
            u32 s0 = i, j = i + 1;
            if (j < end && s[j] == '%') j++;
            if (j < end && s[j] == '~') { j++; while (j < end && ascii_alpha(s[j])) j++; }
            if (j < end && digit(s[j])) {
                j++;
            } else if (j < end && ident_start(s[j])) {
                while (j < end && ident_char(s[j])) j++;
                if (j < end && s[j] == '%') j++;
            } else if (j == i + 1) {
                i++; continue;
            }
            emit(em, s0, j - s0, SYN_TYPE);
            i = j; continue;
        }

        /* ---- assembly directives and labels ---- */
        if ((d->flags & F_ASM) && (c == '.' || c == '%') && i == fns &&
            i + 1 < end && ident_start(s[i+1])) {
            u32 s0 = i; i++;
            while (i < end && (ident_char(s[i]) || s[i] == '.')) i++;
            emit(em, s0, i - s0, SYN_PREPROC);
            continue;
        }

        /* ---- strings ---- */
        if ((d->flags & F_TRIPLE) && (c == '"' || c == '\'') &&
            i + 2 < end && s[i+1] == c && s[i+2] == c) {
            u32 s0 = i;
            bool closed = false;
            u32 e = eat_quoted(s, end, i + 3, c, true, &closed);
            emit(em, s0, e - s0, SYN_STRING);
            if (!closed) st |= (c == '"') ? ST_TRIPLE_D : ST_TRIPLE_S;
            i = e;
            continue;
        }
        if (c == '`' && (d->flags & F_BACKTICK)) {
            if (d->flags & F_BT_SPAN) {
                st |= ST_BACKTICK;
                i = scan_open_str(d, s, end, i, i + 1, &st, em);
                continue;
            }
            /* A shell's `…`: one line. A lone backtick in a heredoc or in
             * prose must not swallow the rest of the file. */
            u32 s0 = i;
            bool closed = false;
            u32 e = eat_quoted(s, end, i + 1, '`', false, &closed);
            emit(em, s0, e - s0, SYN_STRING);
            i = e;
            continue;
        }
        if (c == '\'' && (d->flags & F_QQ_STR) && i + 1 < end && s[i+1] == '\'') {
            /* `'';` / `'')` where code was expected is the END of an indented
             * string this scan never saw open — a diff hunk that starts inside
             * a build phase. An opener is never followed by a closing mark. */
            u32 nx = skip_ws(s, end, i + 2);
            if (nx < end && (s[nx] == ';' || s[nx] == ')' || s[nx] == ']' || s[nx] == ',') &&
                st_frame_top(st) < 0) {
                emit(em, i, 2, SYN_STRING);
                i += 2;
                continue;
            }
            st = st_with_str(st, STR_QQ);
            i = scan_open_str(d, s, end, i, i + 2, &st, em);
            continue;
        }
        if ((d->flags & F_RAW_HASH) && (c == 'r' || c == 'b' || c == 'c') &&
            (i == beg || !ident_char(s[i-1]))) {
            /* r"…", r#"…"#, br#"…"#, cr"…": no escapes, and only a quote
             * followed by as many '#' as opened it closes. `r#name` is a raw
             * identifier and falls through. */
            u32 j = i + 1;
            if (c != 'r' && j < end && s[j] == 'r') j++;
            u32 h = 0;
            while (j + h < end && s[j + h] == '#') h++;
            if ((c == 'r' || j > i + 1) && j + h < end && s[j + h] == '"') {
                st = st_with_depth(st_with_str(st, STR_RAW), h);
                i = scan_open_str(d, s, end, i, j + h + 1, &st, em);
                continue;
            }
        }
        if (c == '\'' && (d->flags & F_LIFETIME) &&
            i + 1 < end && ident_start(s[i+1]) &&
            !(i + 2 < end && s[i+2] == '\'')) {
            /* `'a` — a Rust lifetime, a label or an OCaml type variable, not
             * an unterminated char. */
            u32 s0 = i; i++;
            while (i < end && ident_char(s[i])) i++;
            emit(em, s0, i - s0, SYN_TYPE);
            continue;
        }
        if (c == '\'' && (d->flags & F_NO_SQUOTE)) { i++; continue; }   /* Lisp quote */
        if ((c == '"' || c == '\'') && !prose) {
            /* A row whose language lets this quote hold a line break carries
             * it. The shell's $'…' is its own form: one line, with escapes. */
            if ((d->flags & (c == '"' ? F_DQ_SPAN : F_SQ_SPAN)) &&
                !((d->flags & F_SH_QUOTE) && c == '\'' && i > beg && s[i-1] == '$')) {
                st = st_with_str(st, c == '"' ? STR_DQ : STR_SQ);
                i = scan_open_str(d, s, end, i, i + 1, &st, em);
                continue;
            }
            u32 s0 = i;
            bool closed = false;
            u32 e = eat_quoted(s, end, i + 1, c, false, &closed);
            /* A JSON/YAML key is the string that a ':' follows. */
            SynKind k = SYN_STRING;
            if ((d->flags & F_KEY_JSON) && c == '"') {
                u32 j = skip_ws(s, end, e);
                if (j < end && s[j] == ':') k = SYN_ATTR;
            }
            emit(em, s0, e - s0, k);
            i = e;
            continue;
        }

        /* ---- numbers ---- */
        if (digit(c) ||
            (c == '.' && i + 1 < end && digit(s[i+1]) &&
             (i == 0 || !ident_char(s[i-1])))) {
            /* Only at a token boundary: the '0' in `argv0` is not a number,
             * and neither is the '2' in `v1.2.3` or the '0' in `tuple.0`. */
            if (i > 0 && (ident_char(s[i-1]) ||
                          (s[i-1] == '.' && i > 1 && ident_char(s[i-2])))) {
                i++; continue;
            }
            u32 s0 = i;
            while (i < end && (ident_char(s[i]) || s[i] == '.' ||
                   ((s[i] == '+' || s[i] == '-') && i > s0 &&
                    (s[i-1] == 'e' || s[i-1] == 'E')))) i++;
            /* CSS units ride along with the number (12px, 1.5rem, 50%). */
            if ((d->flags & F_CSS) && i < end && s[i] == '%') i++;
            emit(em, s0, i - s0, SYN_NUMBER);
            continue;
        }

        /* ---- CSS: an unquoted url( ) is one token ---- */
        if ((d->flags & F_CSS) && (c == 'u' || c == 'U') && i + 4 <= end &&
            strncasecmp(s + i, "url(", 4) == 0 && (i == beg || !ident_more(d, s[i-1]))) {
            u32 j = skip_ws(s, end, i + 4);
            if (j < end && s[j] != '"' && s[j] != '\'' && s[j] != '$') {
                /* Its body is a URL, slashes and all: `//` in it starts no
                 * comment and `.png` is not a class selector. A quoted one is
                 * a string already, and `url($var)` is SCSS. */
                u32 e = j;
                while (e < end && s[e] != ')') e++;
                emit(em, i, 3, SYN_FUNC);
                emit(em, j, e - j, SYN_STRING);
                i = e;
                continue;
            }
        }

        /* ---- identifiers ---- */
        if (prose && ident_start(c)) {
            while (i < end && ident_char(s[i])) i++;
            continue;
        }
        if (ident_start(c) ||
            ((d->flags & F_CSS) && (c == '.' || c == '-') &&
             i + 1 < end && ident_start(s[i+1]))) {
            u32 s0 = i;
            bool css_sel = (d->flags & F_CSS) && (c == '.' || c == '-');
            if (css_sel) i++;
            while (i < end && ident_more(d, s[i])) i++;
            u32 n = i - s0;
            u32 nx = skip_ws(s, end, i);
            char nc = nx < end ? s[nx] : '\0';

            SynKind k = SYN_TEXT;
            if (css_sel) {
                k = SYN_TAG;
            } else if (in_table(d->ty, s + s0, n, ci)) {
                k = SYN_TYPE;
            } else if ((!(d->flags & F_KW_BOL) || s0 == fns) &&
                       in_table(d->kw, s + s0, n, ci)) {
                k = SYN_KEYWORD;   /* Dockerfile: `--from=` is not FROM */
            } else if ((d->flags & F_KEY_YAML) && s0 == fns && nc == ':') {
                k = SYN_ATTR;
            } else if ((d->flags & F_KEY_YAML) && fns + 2 <= end &&
                       s[fns] == '-' && s0 == fns + 2 && nc == ':') {
                k = SYN_ATTR;          /* "- key: value" list entries */
            } else if ((d->flags & F_KEY_EQ) && s0 == fns && nc == '=') {
                k = SYN_ATTR;
            } else if ((d->flags & F_CSS) && nc == ':') {
                k = SYN_ATTR;          /* declaration property */
            } else if ((d->flags & F_TARGET) && s0 == fns && nc == ':') {
                k = SYN_FUNC;          /* Makefile rule target */
            } else if ((d->flags & F_ASM) && s0 == fns && nc == ':') {
                k = SYN_FUNC;          /* assembly label */
            } else if (nc == '(') {
                /* An identifier that opens a call or a definition. Whitespace
                 * before the paren is allowed — `foo (x)` is still a call. */
                k = SYN_FUNC;
            } else if ((d->flags & F_CSS) && css_value) {
                k = SYN_TYPE;          /* bare keyword value: flex, none, auto */
            } else if ((d->flags & F_CAPS_TYPE) && camel_case(s + s0, n)) {
                k = SYN_TYPE;
            }
            if (k != SYN_TEXT) emit(em, s0, n, k);
            continue;
        }

        if (d->flags & F_CSS) {
            if (c == ':') css_value = true;
            else if (c == ';' || c == '{' || c == '}') css_value = false;
        }

        /* Inside a string's `${ }` (a shell's `$( )`) the braces are the
         * expression's: the one that balances the opener ends it and the
         * string it interrupted resumes. */
        if ((c == '{' || c == '}' || c == '(' || c == ')') &&
            (d->flags & (F_INTERP | F_SH_QUOTE)) && st_frame_top(st) >= 0) {
            bool sh = (d->flags & F_SH_QUOTE) != 0;
            if (c == (sh ? '(' : '{')) {
                st = st_frame_nest(st);
                i++;
                continue;
            }
            if (c == (sh ? ')' : '}')) {
                u32 top = (u32)st_frame_top(st);
                u32 f = st_frame(st, top);
                if ((f & FRAME_DEPTH) > 1) {
                    st = st_with_frame(st, top, f - 1);
                    i++;
                    continue;
                }
                if (!sh) emit(em, i, 1, SYN_PREPROC);
                st = st_with_frame(st, top, 0);
                if (f & FRAME_DQ)             st = st_with_str(st, STR_DQ);
                else if (d->flags & F_QQ_STR) st = st_with_str(st, STR_QQ);
                else                          st |= ST_BACKTICK;
                i = scan_open_str(d, s, end, i + 1, i + 1, &st, em);
                continue;
            }
        }
        if (d->flags & F_HEREDOC) {
            if (c == '(' && i + 1 < end && s[i+1] == '(') arith = true;
            else if (c == ')' && i + 1 < end && s[i+1] == ')') arith = false;
        }

        /* A `{ }` expression region ends at the brace that balances its
         * opener. Braces inside strings and comments never reach this line. */
        if (brace) {
            if (c == '{') (*brace)++;
            else if (c == '}' && --(*brace) <= 0) { i++; goto out; }
        }

        i++;   /* punctuation and anything else: default colour */
    }

out:
    /* A heredoc's body starts on the line after its opener, whatever else the
     * opener's line left open — except a block comment, which swallowed it. */
    if (heredoc && !(st & ST_BLOCK)) {
        u32 frames = (u32)(st_frame_top(st) + 1);
        st = (st & ~(ST_BACKTICK | ST_STR_MASK | ST_AUX_MASK | ST_DEPTH_MASK)) | heredoc |
             (frames << ST_HD_FRAMES_SHIFT);
    }
    *state = st;
    return i;
}

/* Heuristics for a hunk that opens mid-file in a host row, where nothing says
 * whether the line is template, script or style. Only the shapes that are
 * unambiguous in practice: the cost of a wrong "script" guess is a template
 * painted as JavaScript until the next column-0 tag. */
static bool guess_js_line(const char *s, u32 len, u32 i) {
    static const char *const words[] = {
        "import","export","const","let","var","function","return","if","else",
        "for","while","do","switch","case","default","break","continue","throw",
        "try","catch","finally","await","async","class","interface","type",
        "enum","new","delete","typeof","yield","this","super","static",
        "declare","console","document","window","props","emit","defineProps",
        "defineEmits","defineExpose","onMount","onDestroy","useState",
        "useEffect", NULL
    };
    if (i + 1 < len && s[i] == '/' && (s[i+1] == '/' || s[i+1] == '*')) return true;
    if (s[i] == '}' || s[i] == ')') return true;    /* closes a block or a call */
    /* Anything that carries a tag is template, whatever else it looks like:
     * a wrong "script" here paints every INDENTED template line of the hunk
     * as JavaScript (only a column-0 tag ends a region), so under-guessing
     * is the safe direction. */
    for (u32 j = i; j + 1 < len; j++)
        if (s[j] == '<' && (ascii_alpha(s[j+1]) || s[j+1] == '/')) return false;
    for (i32 k = 0; words[k]; k++) {
        u32 n = (u32)strlen(words[k]);
        if (i + n <= len && memcmp(s + i, words[k], n) == 0 &&
            (i + n == len || !ident_char(s[i + n])))
            return true;
    }
    /* `x = 1;` / `foo(bar);` / `a.b = c,` — an assignment or a call. */
    u32 e = len;
    while (e > i && (s[e-1] == ' ' || s[e-1] == '\t')) e--;
    if (e > i && ident_start(s[i]) &&
        (s[e-1] == ';' || s[e-1] == ',' || s[e-1] == '(' || s[e-1] == '{')) {
        bool has_eq = false, has_call = false, has_colon_space = false;
        for (u32 j = i; j + 1 < e; j++) {
            if (s[j] == '=' && s[j+1] != '=') has_eq = true;
            if (s[j] == '(') has_call = true;
            if (s[j] == ':' && s[j+1] == ' ') has_colon_space = true;
        }
        if ((has_eq || has_call) && !has_colon_space) return true;
    }
    return false;
}

static bool guess_css_line(const char *s, u32 len, u32 i) {
    u32 e = len;
    while (e > i && (s[e-1] == ' ' || s[e-1] == '\t')) e--;
    if (e <= i) return false;
    if (s[e-1] == '{') {
        char c = s[i];
        if (c == '.' || c == '#' || c == '@' || c == '&' || c == '*' || c == ':' ||
            c == '>' || c == '[')
            return true;
        if (!ascii_alpha(c)) return false;
        /* `div > p {`, `h1, h2 {`, `a:hover {` — but not `computed: {` or
         * `foo() {`, which is a method or an object member. */
        for (u32 j = i; j < e; j++) {
            if (s[j] == '(' || s[j] == '=') return false;
            if (s[j] == ':' && j + 1 < e && s[j+1] == ' ') return false;
        }
        return true;
    }
    /* `color: red;` — a dashed lowercase property, a colon, a value. */
    if (s[e-1] == ';' && s[i] >= 'a' && s[i] <= 'z') {
        u32 j = i;
        while (j < e && ((s[j] >= 'a' && s[j] <= 'z') || s[j] == '-' || digit(s[j]))) j++;
        if (j + 1 < e && s[j] == ':' && s[j+1] != ':') return true;
    }
    return false;
}

u32 syntax_guess_state(SynLang lang, const char *line, u32 len) {
    if (!line || lang <= SYN_LANG_NONE || lang >= SYN_LANG_COUNT) return 0;
    const LangDef *d = &LANGS[lang];
    u32 i = skip_ws(line, len, 0);

    if (d->flags & F_HOST) {
        if (i >= len) return 0;
        char c = line[i];
        /* A tag, an expression or a mustache: the host itself. */
        if (c == '<' || c == '{') return 0;
        /* The interior of a JS doc comment, which only a script region has. */
        if (c == '*' && (d->flags & F_EMBED) && !(i + 1 < len && line[i + 1] == '/')) {
            u32 st = st_with_embed(ST_BLOCK | (1u << ST_DEPTH_SHIFT), EMB_SCRIPT);
            return (d->flags & F_FRONT) ? (st | ST_BODY) : st;
        }
        if (d->flags & F_EMBED) {
            if (guess_css_line(line, len, i)) return st_with_embed(0, EMB_STYLE);
            if (guess_js_line(line, len, i)) {
                /* Astro keeps its script in the frontmatter, whose closer is a
                 * `---` line rather than </script>. */
                if (d->flags & F_FRONT) return st_with_embed(ST_BODY, EMB_FRONT);
                return st_with_embed(0, EMB_SCRIPT);
            }
        }
        return 0;
    }

    if (!d->bo || strcmp(d->bo, "/*") != 0) return 0;
    /* The two shapes a C block comment's interior lines take. A hunk that opens
     * on one of these is inside a comment no matter what came before it, and
     * without this every doc-comment body in a diff painted as live code. */
    if (i < len && line[i] == '*') {
        if (i + 1 < len && line[i + 1] == '/') return 0;   /* the closer itself */
        return ST_BLOCK | (1u << ST_DEPTH_SHIFT);
    }
    return 0;
}

/* One line, host row plus whatever regions it opens or is already inside. */
static i32 scan_line(SynLang lang, const char *s, u32 len, u32 *state,
                     SynSpan *out, i32 cap) {
    Emitter em = { out, cap, 0 };
    if (!s || lang <= SYN_LANG_NONE || lang >= SYN_LANG_COUNT) return 0;

    const LangDef *d = &LANGS[lang];
    u32 st  = state ? *state : 0;
    u32 fns = skip_ws(s, len, 0);

    if (!(d->flags & F_HOST)) {
        scan_range(lang, s, 0, len, &st, &em, fns, NULL);
        if (state) *state = st;
        return em.cnt;
    }

    u32 i = 0;
    /* Astro: a `---` line before anything else opens the frontmatter. The
     * first non-blank line sets ST_BODY either way, so a `---` further down
     * is text. */
    if ((d->flags & F_FRONT) && !(st & ST_BODY) && st_embed(st) == EMB_NONE && fns < len) {
        st |= ST_BODY;
        if (line_is_fence(s, len, fns)) {
            emit(&em, fns, 3, SYN_PREPROC);
            st = st_with_embed(st, EMB_FRONT);
            i = len;
        }
    }

    while (i < len) {
        u32 emb = st_embed(st);
        if (emb == EMB_NONE) {
            i = scan_range(lang, s, i, len, &st, &em, fns, NULL);
            continue;
        }

        SynLang     sub      = SYN_LANG_NONE;
        const char *closer   = NULL;
        bool        comment  = false;
        bool        tag_ends = false;   /* a column-0 tag ends the region */
        switch (emb) {
        case EMB_SCRIPT:   sub = SYN_LANG_JS;    closer = "</script"; tag_ends = true; break;
        case EMB_STYLE:    sub = SYN_LANG_CSS;   closer = "</style";  tag_ends = true; break;
        case EMB_FRONT:    sub = SYN_LANG_JS;                         tag_ends = true; break;
        case EMB_BRACE:    sub = (SynLang)d->expr; break;
        case EMB_MUSTACHE: sub = (SynLang)d->expr; closer = "}}"; break;
        case EMB_STMT:     sub = SYN_LANG_JINJA;   closer = "%}"; break;
        case EMB_HASHCMT:  comment = true;         closer = "#}"; break;
        case EMB_DASHCMT:  comment = true;         closer = "--}}"; break;
        case EMB_BANGCMT:  comment = true;         closer = "}}"; break;
        case EMB_CODE:     sub = (SynLang)d->expr; closer = "%>"; break;
        case EMB_CODECMT:  comment = true;         closer = "%>"; break;
        default:
            st = st_with_embed(st, EMB_NONE);
            continue;
        }
        if (sub <= SYN_LANG_NONE || sub >= SYN_LANG_COUNT) sub = SYN_LANG_JS;

        if (tag_ends && i == 0 && !(st & ST_SUB_MASK) && line_opens_tag(s, len)) {
            st = st_with_embed(st & ~ST_SUB_MASK, EMB_NONE);
            continue;
        }
        if (emb == EMB_FRONT && i == 0 && line_is_fence(s, len, fns)) {
            emit(&em, fns, 3, SYN_PREPROC);
            st = st_with_embed(st & ~ST_SUB_MASK, EMB_NONE);
            i = len;
            continue;
        }

        if (emb == EMB_BRACE) {
            i32 depth = (i32)st_brace(st);
            if (depth <= 0) depth = 1;
            u32 e = scan_range(sub, s, i, len, &st, &em, fns, &depth);
            if (depth <= 0) {
                emit(&em, e - 1, 1, SYN_PREPROC);
                st = st_with_brace(st_with_embed(st & ~ST_SUB_MASK, EMB_NONE), 0);
                i = e;
            } else {
                st = st_with_brace(st, (u32)depth);
                i = len;
            }
            continue;
        }

        u32 close_at = closer ? find_ci(s, len, i, closer) : len;
        u32 clen     = closer ? (u32)strlen(closer) : 0;
        if (comment) {
            emit(&em, i, (close_at < len ? close_at + clen : len) - i, SYN_COMMENT);
        } else if (close_at > i) {
            scan_range(sub, s, i, close_at, &st, &em, fns, NULL);
        }
        if (close_at >= len) { i = len; continue; }   /* still open next line */

        st = st_with_embed(st & ~ST_SUB_MASK, EMB_NONE);
        if (emb == EMB_SCRIPT || emb == EMB_STYLE) {
            /* "</script" is the start of a closing tag; the host takes it from
             * here and closes it at the '>'. */
            emit(&em, close_at, clen, SYN_TAG);
            st |= ST_TAG;
        } else if (!comment) {
            emit(&em, close_at, clen, SYN_PREPROC);
        }
        i = close_at + clen;
    }

    if (state) *state = st;
    return em.cnt;
}

i32 syntax_scan(SynLang lang, const char *line, u32 len, u32 *state,
                SynSpan *out, i32 cap) {
    if (!out || cap <= 0) return 0;
    return scan_line(lang, line, len, state, out, cap);
}

void syntax_advance_state(SynLang lang, const char *line, u32 len, u32 *state) {
    if (!state) return;
    scan_line(lang, line, len, state, NULL, 0);
}

Color syntax_color(const Theme *t, SynKind kind) {
    if (!t) return (Color){0.9f, 0.9f, 0.9f, 1.0f};
    Color c;
    switch (kind) {
    /* Drawn from the THEME's own ANSI slots so a user theme restyles the code
     * along with the terminal. Green is reserved for SYN_INSERT: in the diff
     * dock green already means "added line", and a green keyword inside a
     * removed line would contradict the row it sits in. SYN_INSERT and
     * SYN_DELETE only ever come out of SYN_LANG_DIFF — a .patch file open in
     * the editor — where added and removed ARE what the line means. */
    case SYN_KEYWORD: c = t->ansi[13]; break;  /* bright magenta/purple */
    case SYN_TYPE:    c = t->ansi[14]; break;  /* bright cyan */
    case SYN_STRING:  c = t->ansi[11]; break;  /* bright yellow */
    case SYN_NUMBER:  c = t->ansi[12]; break;  /* bright blue */
    case SYN_FUNC:    c = t->ansi[12]; break;
    case SYN_PREPROC: c = t->ansi[13]; break;
    case SYN_TAG:     c = t->ansi[13]; break;
    case SYN_ATTR:    c = t->ansi[14]; break;
    case SYN_INSERT:  c = t->ansi[10]; break;  /* bright green */
    case SYN_DELETE:  c = t->ansi[9];  break;  /* bright red */
    case SYN_COMMENT: c = t->fg; c.a *= 0.42f; return c;
    default:          return t->fg;
    }
    c.a = 1.0f;
    return c;
}
