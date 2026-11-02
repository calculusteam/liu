/*
 * Liu — what each Settings text field accepts.
 *
 * Printable ASCII is the rule for an API key, a URL, a model id and a host,
 * which are ASCII by construction; it is not the rule for a master password,
 * which is the user's own word and is typed UTF-8 into the unlock dialog next to
 * it. A field that takes only ASCII must also refuse what an input method
 * commits, or a composed 你好 would be drawn while it is composed and dropped
 * when it is committed.
 *
 * Every field has a kind, and a kind has a policy that says which it is:
 *
 *   BROWSER_RULE    ASCII: the allowed-sites matcher takes a host in its
 *                   canonical, punycode form ([a-z0-9.-]); a Unicode host
 *                   could never match, so it is refused where it is typed.
 *   MODEL_FILTER    ASCII: it narrows a list of model ids.
 *   API_KEY         ASCII, no quote or backslash, secret: an HTTP header value.
 *   API_URL         ASCII, no quote or backslash: a URL carries no raw Unicode.
 *   API_MODEL       ASCII, no quote or backslash: an id the provider names.
 *                   (The quote and backslash are the curl config's: both end or
 *                   escape its double-quoted values; voice_api.c and
 *                   translate_api.c drop them again before they are used.)
 *   VAULT_PASSWORD  UTF-8, secret: the unlock dialog takes any character.
 *
 * An ASCII field takes no input method at all: it never draws a composition
 * and the window's input context stands down while it has the keyboard, so the
 * letters go in as typed. A UTF-8 field is composed in, committed to and pasted
 * into like any other text field.
 *
 * The policy becomes TextEdit flags, so a typed character, an input method's
 * commit and a paste are judged by the same rule (textedit_cp_allowed).
 *
 * Pure: the table needs no display.
 */
#ifndef UI_SETTINGS_FIELD_H
#define UI_SETTINGS_FIELD_H

#include "core/types.h"
#include "ui/textedit.h"

typedef enum {
    SETTINGS_FIELD_NONE = 0,       /* not a Settings field: no policy */
    SETTINGS_FIELD_BROWSER_RULE,   /* Browser > Allowed sites */
    SETTINGS_FIELD_MODEL_FILTER,   /* Translate > the Model menu's search */
    SETTINGS_FIELD_API_KEY,        /* Translate and Voice > API key */
    SETTINGS_FIELD_API_URL,        /* ... base URL */
    SETTINGS_FIELD_API_MODEL,      /* ... model id */
    SETTINGS_FIELD_VAULT_PASSWORD, /* Vault > current / new / confirm master password */
    SETTINGS_FIELD_COUNT
} SettingsFieldKind;

typedef struct {
    const char *name;
    bool utf8;        /* a character beyond ASCII is the user's to type, compose and paste */
    bool secret;      /* drawn as bullets; Cmd+C / Cmd+X refused, no undo ring */
    bool no_quotes;   /* '"' and '\\' refused: spliced into a double-quoted curl config */
} SettingsFieldPolicy;

static inline const SettingsFieldPolicy *settings_field_policy(SettingsFieldKind k) {
    static const SettingsFieldPolicy table[SETTINGS_FIELD_COUNT] = {
        [SETTINGS_FIELD_NONE]           = { "none",           true,  false, false },
        [SETTINGS_FIELD_BROWSER_RULE]   = { "browser rule",   false, false, false },
        [SETTINGS_FIELD_MODEL_FILTER]   = { "model filter",   false, false, false },
        [SETTINGS_FIELD_API_KEY]        = { "API key",        false, true,  true  },
        [SETTINGS_FIELD_API_URL]        = { "API base URL",   false, false, true  },
        [SETTINGS_FIELD_API_MODEL]      = { "API model id",   false, false, true  },
        [SETTINGS_FIELD_VAULT_PASSWORD] = { "vault password", true,  true,  false },
    };
    return &table[(unsigned)k < (unsigned)SETTINGS_FIELD_COUNT ? k : SETTINGS_FIELD_NONE];
}

/* The flags app_edit_ex takes for a field of this kind. */
static inline u32 settings_field_edit_flags(SettingsFieldKind k) {
    const SettingsFieldPolicy *p = settings_field_policy(k);
    u32 f = 0;
    if (p->secret)    f |= TEXTEDIT_SECRET;
    if (!p->utf8)     f |= TEXTEDIT_ASCII;
    if (p->no_quotes) f |= TEXTEDIT_NO_QUOTES;
    return f;
}

/* Does a field of this kind take an input method? An ASCII one does not: it
 * draws no composition and the window's input context stands down. */
static inline bool settings_field_takes_ime(SettingsFieldKind k) {
    return settings_field_policy(k)->utf8;
}

/* May `cp` be typed, committed or pasted into a field of this kind? */
static inline bool settings_field_accepts(SettingsFieldKind k, u32 cp) {
    return textedit_cp_allowed(settings_field_edit_flags(k), cp);
}

#endif /* UI_SETTINGS_FIELD_H */
