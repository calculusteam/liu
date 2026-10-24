#include "intro/intro_lang.h"

static const char *const g_codes[INTRO_LANG_COUNT] = INTRO_LANG_CODES;

const char *intro_lang_code(i32 index) {
    return index >= 0 && index < INTRO_LANG_COUNT ? g_codes[index] : g_codes[0];
}

i32 intro_lang_for_tag(const char *tag) {
    if (!tag) return 0;
    /* The primary subtag: the letters before the first '-', '_', '.' or '@'. */
    char primary[9];
    usize n = 0;
    for (; tag[n]; n++) {
        char c = tag[n];
        if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
        if (c < 'a' || c > 'z') break;
        if (n >= sizeof primary - 1) return 0;
        primary[n] = c;
    }
    primary[n] = '\0';
    /* Letters, then nothing or a separator: "trx" is not Turkish, and neither
     * is "tr9". */
    if (n < 2 || (tag[n] && tag[n] != '-' && tag[n] != '_' && tag[n] != '.' && tag[n] != '@')) return 0;
    for (i32 i = 0; i < INTRO_LANG_COUNT; i++) {
        const char *code = g_codes[i];
        usize k = 0;
        while (code[k] && code[k] == primary[k]) k++;
        if (!code[k] && !primary[k]) return i;
    }
    return 0;
}
