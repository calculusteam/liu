/*
 * Liu - Whisper's language set, for display (see voice_lang.h).
 */
#include "voice/voice_lang.h"

#include <string.h>

typedef struct { const char *code, *name; } VoiceLang;

/* Every language large-v3 was trained on, sorted by name.
 *
 * A few codes are Whisper's own rather than ISO 639-1 — "haw", "yue" and "jw" —
 * because that is what the model's tokenizer expects. They are passed through
 * to the hosted APIs unchanged as well; a service that does not know one
 * answers with its own error, rather than this table silently rewriting the
 * chosen language into a different one. */
static const VoiceLang g_langs[] = {
    { "af",  "Afrikaans" },        { "sq",  "Albanian" },
    { "am",  "Amharic" },          { "ar",  "Arabic" },
    { "hy",  "Armenian" },         { "as",  "Assamese" },
    { "az",  "Azerbaijani" },      { "ba",  "Bashkir" },
    { "eu",  "Basque" },           { "be",  "Belarusian" },
    { "bn",  "Bengali" },          { "bs",  "Bosnian" },
    { "br",  "Breton" },           { "bg",  "Bulgarian" },
    { "yue", "Cantonese" },        { "ca",  "Catalan" },
    { "zh",  "Chinese" },          { "hr",  "Croatian" },
    { "cs",  "Czech" },            { "da",  "Danish" },
    { "nl",  "Dutch" },            { "en",  "English" },
    { "et",  "Estonian" },         { "fo",  "Faroese" },
    { "fi",  "Finnish" },          { "fr",  "French" },
    { "gl",  "Galician" },         { "ka",  "Georgian" },
    { "de",  "German" },           { "el",  "Greek" },
    { "gu",  "Gujarati" },         { "ht",  "Haitian Creole" },
    { "ha",  "Hausa" },            { "haw", "Hawaiian" },
    { "he",  "Hebrew" },           { "hi",  "Hindi" },
    { "hu",  "Hungarian" },        { "is",  "Icelandic" },
    { "id",  "Indonesian" },       { "it",  "Italian" },
    { "ja",  "Japanese" },         { "jw",  "Javanese" },
    { "kn",  "Kannada" },          { "kk",  "Kazakh" },
    { "km",  "Khmer" },            { "ko",  "Korean" },
    { "lo",  "Lao" },              { "la",  "Latin" },
    { "lv",  "Latvian" },          { "ln",  "Lingala" },
    { "lt",  "Lithuanian" },       { "lb",  "Luxembourgish" },
    { "mk",  "Macedonian" },       { "mg",  "Malagasy" },
    { "ms",  "Malay" },            { "ml",  "Malayalam" },
    { "mt",  "Maltese" },          { "mi",  "Maori" },
    { "mr",  "Marathi" },          { "mn",  "Mongolian" },
    { "my",  "Myanmar" },          { "ne",  "Nepali" },
    { "no",  "Norwegian" },        { "nn",  "Nynorsk" },
    { "oc",  "Occitan" },          { "ps",  "Pashto" },
    { "fa",  "Persian" },          { "pl",  "Polish" },
    { "pt",  "Portuguese" },       { "pa",  "Punjabi" },
    { "ro",  "Romanian" },         { "ru",  "Russian" },
    { "sa",  "Sanskrit" },         { "sr",  "Serbian" },
    { "sn",  "Shona" },            { "sd",  "Sindhi" },
    { "si",  "Sinhala" },          { "sk",  "Slovak" },
    { "sl",  "Slovenian" },        { "so",  "Somali" },
    { "es",  "Spanish" },          { "su",  "Sundanese" },
    { "sw",  "Swahili" },          { "sv",  "Swedish" },
    { "tl",  "Tagalog" },          { "tg",  "Tajik" },
    { "ta",  "Tamil" },            { "tt",  "Tatar" },
    { "te",  "Telugu" },           { "th",  "Thai" },
    { "bo",  "Tibetan" },          { "tr",  "Turkish" },
    { "tk",  "Turkmen" },          { "uk",  "Ukrainian" },
    { "ur",  "Urdu" },             { "uz",  "Uzbek" },
    { "vi",  "Vietnamese" },       { "cy",  "Welsh" },
    { "yi",  "Yiddish" },          { "yo",  "Yoruba" },
};

#define VOICE_LANG_N ((i32)(sizeof g_langs / sizeof g_langs[0]))

i32 voice_lang_count(void) { return VOICE_LANG_N; }

const char *voice_lang_code(i32 index) {
    if (index < 0 || index >= VOICE_LANG_N) return "";
    return g_langs[index].code;
}

const char *voice_lang_name(i32 index) {
    if (index < 0 || index >= VOICE_LANG_N) return "";
    return g_langs[index].name;
}

i32 voice_lang_index_for_code(const char *code) {
    if (!code || !code[0]) return -1;
    for (i32 i = 0; i < VOICE_LANG_N; i++)
        if (strcmp(g_langs[i].code, code) == 0) return i;
    return -1;
}

const char *voice_lang_name_for_code(const char *code) {
    i32 i = voice_lang_index_for_code(code);
    return i < 0 ? NULL : g_langs[i].name;
}
