/*
 * Liu - Voice input: hosted speech-to-text backend (see voice_api.h).
 */
#include "voice/voice_api.h"
#include "core/curl_path.h"

#include "core/checked.h"
#include "core/api_url.h"
#include "platform/platform.h"
#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#ifndef PLATFORM_WIN32
#include <sys/wait.h>
#endif

extern char **environ;

/* Bound the buffered server response. Reject oversized responses so an
 * unbounded or invalid reply cannot keep the voice worker waiting
 * indefinitely. */
#define VAPI_RESP_CAP (256 * 1024)

/* ----------------------------------------------------------------- *
 *  Provider table                                                    *
 * ----------------------------------------------------------------- */

typedef enum {
    VAPI_OPENAI = 0,
    VAPI_GROQ,
    VAPI_ELEVENLABS,
    VAPI_CUSTOM,
    VAPI_UNKNOWN,
} VApiProvider;

static VApiProvider provider_id(const char *p) {
    if (!p || !p[0])                   return VAPI_OPENAI;   /* default */
    if (strcmp(p, "openai") == 0)      return VAPI_OPENAI;
    if (strcmp(p, "groq") == 0)        return VAPI_GROQ;
    if (strcmp(p, "elevenlabs") == 0)  return VAPI_ELEVENLABS;
    if (strcmp(p, "custom") == 0)      return VAPI_CUSTOM;
    return VAPI_UNKNOWN;
}

const char *voice_api_provider_display(const char *provider) {
    switch (provider_id(provider)) {
    case VAPI_OPENAI:     return "OpenAI";
    case VAPI_GROQ:       return "Groq";
    case VAPI_ELEVENLABS: return "ElevenLabs";
    case VAPI_CUSTOM:     return "Custom API";
    default:              return "API";
    }
}

const char *voice_api_default_model(const char *provider) {
    switch (provider_id(provider)) {
    /* Default OpenAI transcription model. */
    case VAPI_OPENAI:     return "gpt-4o-mini-transcribe";
    case VAPI_GROQ:       return "whisper-large-v3-turbo";
    case VAPI_ELEVENLABS: return "scribe_v1";
    default:              return "";   /* custom: requires a model */
    }
}

const char *voice_api_effective_key(const VoiceConfig *cfg,
                                    char *buf, usize cap) {
    if (!buf || cap == 0) return "";
    buf[0] = '\0';
    if (!cfg) return buf;
    if (cfg->api_key[0]) {
        snprintf(buf, cap, "%s", cfg->api_key);
        return buf;
    }
    const char *env = NULL;
    switch (provider_id(cfg->api_provider)) {
    case VAPI_OPENAI:     env = getenv("OPENAI_API_KEY");     break;
    case VAPI_GROQ:       env = getenv("GROQ_API_KEY");       break;
    case VAPI_ELEVENLABS: env = getenv("ELEVENLABS_API_KEY"); break;
    default: break;   /* custom: config-only, never env */
    }
    if (env && env[0]) snprintf(buf, cap, "%s", env);
    return buf;
}

bool voice_api_ready(const VoiceConfig *cfg, char *why, usize why_cap) {
    if (why && why_cap) why[0] = '\0';
    if (!cfg) return false;
    VApiProvider prov = provider_id(cfg->api_provider);
    if (prov == VAPI_UNKNOWN) {
        if (why) snprintf(why, why_cap, "unknown speech API provider");
        return false;
    }
    if (prov == VAPI_CUSTOM) {
        if (!cfg->api_base_url[0]) {
            if (why) snprintf(why, why_cap, "set the API base URL in Settings");
            return false;
        }
        if (!cfg->api_model[0]) {
            if (why) snprintf(why, why_cap, "pick an API model in Settings");
            return false;
        }
        return true;   /* a local server may be unauthenticated */
    }
    char key[256];
    voice_api_effective_key(cfg, key, sizeof key);
    if (!key[0]) {
        if (why) snprintf(why, why_cap, "set the speech API key in Settings");
        return false;
    }
    return true;
}

/* ----------------------------------------------------------------- *
 *  Small helpers shared with the Translate API backend's shape       *
 * ----------------------------------------------------------------- */

static bool write_all_fd(int fd, const char *data, usize len) {
    usize off = 0;
    while (off < len) {
        ssize_t n = write(fd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        off += (usize)n;
    }
    return true;
}

/* Keep spawn source fds away from stdin/stdout/stderr — Liu may have been
 * launched with one of those closed. */
static int fd_above_stdio(int fd) {
    if (fd < 0) return fd;
    if (fd < 3) {
        int moved = fcntl(fd, F_DUPFD_CLOEXEC, 3);
        close(fd);
        return moved;
    }
    int flags = fcntl(fd, F_GETFD, 0);
    if (flags < 0 || fcntl(fd, F_SETFD, flags | FD_CLOEXEC) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Copy `s` dropping anything that could break out of a double-quoted
 * curl-config value: a newline starts a new config line (option injection),
 * and a quote or backslash ends or escapes the value. Keys, URLs, model ids
 * and language codes are plain ASCII tokens, so dropping is safe. The Settings
 * editors already filter these, but config.json can be hand-edited and env
 * vars are arbitrary. */
static void curlcfg_sanitize(const char *s, char *out, usize cap) {
    if (!out || cap == 0) return;
    usize o = 0;
    if (!s) { out[0] = '\0'; return; }
    for (const char *p = s; *p && o < cap - 1u; p++) {
        u8 c = (u8)*p;
        if (c < 32 || c == '"' || c == '\\') continue;
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

static void copy_trimmed(const char *s, char *out, usize out_cap) {
    if (!out || out_cap == 0) return;
    if (!s) { out[0] = '\0'; return; }
    usize start = 0, end = strlen(s);
    while (start < end && (u8)s[start] <= ' ') start++;
    while (end > start && (u8)s[end - 1] <= ' ') end--;
    usize n = end - start;
    if (n >= out_cap) n = out_cap - 1u;
    if (n) memmove(out, s + start, n);
    out[n] = '\0';
}

/* "<base>/audio/transcriptions" (or /translations) with trailing slashes
 * trimmed off the base. */
static void custom_audio_url(const char *base, const char *leaf,
                             char *out, usize cap) {
    usize blen = strlen(base);
    while (blen > 0 && base[blen - 1] == '/') blen--;
    snprintf(out, cap, "%.*s/audio/%s", (int)blen, base, leaf);
}

/* ----------------------------------------------------------------- *
 *  WAV container                                                     *
 * ----------------------------------------------------------------- */

static void put_u32le(u8 *p, u32 v) {
    p[0] = (u8)(v & 0xFF);  p[1] = (u8)((v >> 8) & 0xFF);
    p[2] = (u8)((v >> 16) & 0xFF); p[3] = (u8)((v >> 24) & 0xFF);
}
static void put_u16le(u8 *p, u16 v) {
    p[0] = (u8)(v & 0xFF);  p[1] = (u8)((v >> 8) & 0xFF);
}

/* Write the utterance as 16-bit mono PCM.
 *
 * 16-bit rather than the float32 already held: every one of these services
 * accepts it, it halves the upload, and the quantisation floor is ~30 dB below
 * anything a speech model resolves. Written in blocks so a two-minute
 * recording (the config ceiling) does not need a 3.8 MB staging buffer. */
static bool write_wav(int fd, const f32 *pcm, i32 n) {
    const u32 rate = (u32)PLATFORM_CAPTURE_RATE;
    const u32 data_bytes = (u32)n * 2u;

    u8 hdr[44];
    memcpy(hdr, "RIFF", 4);
    put_u32le(hdr + 4, 36u + data_bytes);
    memcpy(hdr + 8, "WAVEfmt ", 8);
    put_u32le(hdr + 16, 16);            /* PCM fmt chunk size   */
    put_u16le(hdr + 20, 1);             /* format = PCM         */
    put_u16le(hdr + 22, 1);             /* channels             */
    put_u32le(hdr + 24, rate);
    put_u32le(hdr + 28, rate * 2u);     /* byte rate            */
    put_u16le(hdr + 32, 2);             /* block align          */
    put_u16le(hdr + 34, 16);            /* bits per sample      */
    memcpy(hdr + 36, "data", 4);
    put_u32le(hdr + 40, data_bytes);
    if (!write_all_fd(fd, (const char *)hdr, sizeof hdr)) return false;

    u8 block[4096];
    i32 i = 0;
    while (i < n) {
        usize o = 0;
        while (i < n && o + 2u <= sizeof block) {
            f32 s = pcm[i++];
            if (s > 1.0f) s = 1.0f;
            if (s < -1.0f) s = -1.0f;
            i32 q = (i32)(s * 32767.0f + (s >= 0.0f ? 0.5f : -0.5f));
            if (q > 32767) q = 32767;
            if (q < -32768) q = -32768;
            put_u16le(block + o, (u16)(i16)q);
            o += 2u;
        }
        if (!write_all_fd(fd, (const char *)block, o)) return false;
    }
    return true;
}

/* ----------------------------------------------------------------- *
 *  Request                                                           *
 * ----------------------------------------------------------------- */

/* Assemble the curl config. Returns a malloc'd string or NULL. */
static char *build_curl_config(const VoiceConfig *cfg, VApiProvider prov,
                               const char *audio_path) {
    char key_raw[256], key[256];
    voice_api_effective_key(cfg, key_raw, sizeof key_raw);
    curlcfg_sanitize(key_raw, key, sizeof key);

    char model[64];
    curlcfg_sanitize(cfg->api_model[0] ? cfg->api_model
                                       : voice_api_default_model(cfg->api_provider),
                     model, sizeof model);
    if (!model[0]) return NULL;

    char lang[8];
    curlcfg_sanitize(cfg->lang, lang, sizeof lang);

    /* ElevenLabs has no translation endpoint, so `translate` is a transcribe
     * there; the other three use OpenAI's /audio/translations, which always
     * emits English and rejects a `language` field. */
    const bool translate = cfg->translate && prov != VAPI_ELEVENLABS;
    const char *leaf = translate ? "translations" : "transcriptions";

    char url[512];
    char auth[320];
    switch (prov) {
    case VAPI_OPENAI:
        snprintf(url, sizeof url, "https://api.openai.com/v1/audio/%s", leaf);
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
        break;
    case VAPI_GROQ:
        snprintf(url, sizeof url,
                 "https://api.groq.com/openai/v1/audio/%s", leaf);
        snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
        break;
    case VAPI_ELEVENLABS:
        snprintf(url, sizeof url, "https://api.elevenlabs.io/v1/speech-to-text");
        snprintf(auth, sizeof auth, "xi-api-key: %s", key);
        break;
    case VAPI_CUSTOM:
    default: {
        char base[256];
        curlcfg_sanitize(cfg->api_base_url, base, sizeof base);
        if (!liu_api_base_url_ok(base)) return NULL;    /* http(s) only */
        custom_audio_url(base, leaf, url, sizeof url);
        if (key[0]) snprintf(auth, sizeof auth, "Authorization: Bearer %s", key);
        else auth[0] = '\0';
        break;
    }
    }

    usize cap = 2048;
    char *text = (char *)malloc(cap);
    if (!text) return NULL;
    usize off = 0;
    i32 w;

    /* max-time is generous because the whole utterance uploads first: a
     * two-minute recording is ~3.8 MB and the model runs after that lands. */
    w = snprintf(text + off, cap - off,
                 "url = \"%s\"\n"
                 LIU_API_CURL_PROTO_LINES
                 "silent\n"
                 "show-error\n"
                 "max-time = 180\n",
                 url);
    if (w > 0) off += (usize)w;
    if (auth[0] && off < cap) {
        w = snprintf(text + off, cap - off, "header = \"%s\"\n", auth);
        if (w > 0) off += (usize)w;
    }
    /* The service reads the format from the part's filename, so the temp
     * file's mkstemp name has to be overridden with a real one. */
    if (off < cap) {
        w = snprintf(text + off, cap - off,
                     "form = \"file=@%s;filename=audio.wav;type=audio/wav\"\n",
                     audio_path);
        if (w > 0) off += (usize)w;
    }
    if (off < cap) {
        w = snprintf(text + off, cap - off, "form = \"%s=%s\"\n",
                     prov == VAPI_ELEVENLABS ? "model_id" : "model", model);
        if (w > 0) off += (usize)w;
    }
    if (prov != VAPI_ELEVENLABS && off < cap) {
        w = snprintf(text + off, cap - off, "form = \"response_format=json\"\n");
        if (w > 0) off += (usize)w;
    }
    if (lang[0] && !translate && off < cap) {
        w = snprintf(text + off, cap - off, "form = \"%s=%s\"\n",
                     prov == VAPI_ELEVENLABS ? "language_code" : "language",
                     lang);
        if (w > 0) off += (usize)w;
    }

    if (off >= cap) { free(text); return NULL; }   /* truncated — refuse */
    return text;
}

/* Drain the child's stdout until EOF, polling `keep_going` while it runs.
 * Returns the byte count, or -1 when the caller cancelled. */
static i32 drain_child(int fd, pid_t pid, char *buf, i32 cap,
                       bool (*keep_going)(void *user), void *user) {
    i32 len = 0;
    for (;;) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, 200);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (keep_going && !keep_going(user)) {
            (void)kill(pid, SIGKILL);
            return -1;
        }
        if (pr == 0) continue;                   /* nothing yet; poll again */

        ssize_t n = read(fd, buf + len, (usize)(cap - 1 - len));
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN) continue;
            break;
        }
        if (n == 0) break;                       /* EOF: the child is done  */
        len += (i32)n;
        if (len >= cap - 1) break;
    }
    buf[len] = '\0';
    return len;
}

/* ----------------------------------------------------------------- *
 *  Response                                                          *
 * ----------------------------------------------------------------- */

static bool parse_response(const char *log, i32 log_len,
                           char *out, i32 out_cap,
                           char *lang_out, usize lang_cap,
                           char *err, usize err_cap) {
    /* curl's stderr is merged into the same pipe, so noise may precede the
     * JSON — parse from the first '{'. cJSON ignores trailing bytes. */
    const char *start = (const char *)memchr(log, '{', (usize)log_len);
    cJSON *root = NULL;
    if (start) {
        usize jlen = (usize)(log + log_len - start);
        usize scratch_size;
        char *scratch = checked_add_usize(jlen, 1u, &scratch_size)
            ? (char *)malloc(scratch_size) : NULL;
        if (scratch) {
            memcpy(scratch, start, jlen);
            scratch[jlen] = '\0';
            root = cJSON_Parse(scratch);
            free(scratch);
        }
    }
    if (!root) {
        if (err && err_cap) {
            char tail[200];
            i32 tn = log_len < (i32)sizeof(tail) - 1 ? log_len
                                                     : (i32)sizeof(tail) - 1;
            memcpy(tail, log, (usize)tn);
            tail[tn] = '\0';
            copy_trimmed(tail, err, err_cap);
            if (!err[0]) snprintf(err, err_cap, "no response from the API");
        }
        return false;
    }

    /* Error envelope first. Hosted providers use {"error":{"message": …}};
     * several self-hosted OpenAI-compatible servers return {"error":"…"} as a
     * plain string, and ElevenLabs uses {"detail":{"message": …}}. */
    cJSON *error = cJSON_GetObjectItemCaseSensitive(root, "error");
    if (cJSON_IsObject(error)) {
        cJSON *m = cJSON_GetObjectItemCaseSensitive(error, "message");
        if (err && err_cap && cJSON_IsString(m)) copy_trimmed(m->valuestring, err, err_cap);
        cJSON_Delete(root);
        return false;
    }
    if (cJSON_IsString(error)) {
        if (err && err_cap) copy_trimmed(error->valuestring, err, err_cap);
        cJSON_Delete(root);
        return false;
    }

    cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    bool got = false;
    if (cJSON_IsString(text) && text->valuestring) {
        copy_trimmed(text->valuestring, out, (usize)out_cap);
        got = out[0] != '\0';
    }

    if (got && lang_out && lang_cap) {
        cJSON *lg = cJSON_GetObjectItemCaseSensitive(root, "language_code");
        if (!cJSON_IsString(lg))
            lg = cJSON_GetObjectItemCaseSensitive(root, "language");
        if (cJSON_IsString(lg) && lg->valuestring)
            snprintf(lang_out, lang_cap, "%s", lg->valuestring);
    }

    if (!got && err && err_cap && !err[0]) {
        cJSON *detail = cJSON_GetObjectItemCaseSensitive(root, "detail");
        if (cJSON_IsObject(detail)) {
            cJSON *m = cJSON_GetObjectItemCaseSensitive(detail, "message");
            if (cJSON_IsString(m)) copy_trimmed(m->valuestring, err, err_cap);
        } else if (cJSON_IsString(detail)) {
            copy_trimmed(detail->valuestring, err, err_cap);
        } else {
            cJSON *m = cJSON_GetObjectItemCaseSensitive(root, "message");
            if (cJSON_IsString(m)) copy_trimmed(m->valuestring, err, err_cap);
        }
    }
    cJSON_Delete(root);
    if (!got && err && err_cap && !err[0])
        snprintf(err, err_cap, "the API returned no transcript");
    return got;
}

/* ----------------------------------------------------------------- *
 *  Entry point                                                       *
 * ----------------------------------------------------------------- */

i32 voice_api_transcribe(const VoiceConfig *cfg,
                         const f32 *pcm, i32 n_samples,
                         char *out, i32 out_cap,
                         char *lang_out, usize lang_cap,
                         char *err, usize err_cap,
                         bool (*keep_going)(void *user), void *user) {
    if (out && out_cap > 0) out[0] = '\0';
    if (err && err_cap) err[0] = '\0';
    if (!cfg || !pcm || n_samples <= 0 || !out || out_cap <= 1) return -1;

    VApiProvider prov = provider_id(cfg->api_provider);
    if (prov == VAPI_UNKNOWN) {
        if (err) snprintf(err, err_cap, "unknown speech API provider");
        return -1;
    }
    if (prov != VAPI_CUSTOM) {
        char key[256];
        voice_api_effective_key(cfg, key, sizeof key);
        if (!key[0]) {
            if (err) snprintf(err, err_cap, "no API key");
            return -1;
        }
    }
    if (lang_out && lang_cap) snprintf(lang_out, lang_cap, "%s", cfg->lang);

    /* Create the audio file with mode 0600 and unlink it after curl exits so
     * the recording is not retained beyond the request. */
    char audio_path[] = "/tmp/liu-voice-XXXXXX";
    int audio_fd = mkstemp(audio_path);
    if (audio_fd < 0) {
        if (err) snprintf(err, err_cap, "cannot stage the audio");
        return -1;
    }
    bool wrote = write_wav(audio_fd, pcm, n_samples);
    close(audio_fd);
    if (!wrote) {
        (void)unlink(audio_path);
        if (err) snprintf(err, err_cap, "cannot stage the audio");
        return -1;
    }

    char *cfg_text = build_curl_config(cfg, prov, audio_path);
    if (!cfg_text) {
        (void)unlink(audio_path);
        if (err) snprintf(err, err_cap, "pick an API model in Settings");
        return -1;
    }

    int pipefd[2];
    if (pipe(pipefd) != 0) {
        free(cfg_text); (void)unlink(audio_path);
        if (err) snprintf(err, err_cap, "cannot start the request");
        return -1;
    }
    pipefd[0] = fd_above_stdio(pipefd[0]);
    pipefd[1] = fd_above_stdio(pipefd[1]);
    int cfgfd = -1;
    bool ok = pipefd[0] >= 0 && pipefd[1] >= 0;
    if (ok) {
        char tmp[] = "/tmp/liu-voice-config-XXXXXX";
        cfgfd = mkstemp(tmp);
        if (cfgfd < 0) ok = false;
        else {
            (void)unlink(tmp);
            cfgfd = fd_above_stdio(cfgfd);
            ok = cfgfd >= 0 &&
                 write_all_fd(cfgfd, cfg_text, strlen(cfg_text)) &&
                 lseek(cfgfd, 0, SEEK_SET) == 0;
        }
    }
    free(cfg_text);
    if (!ok) {
        if (cfgfd >= 0) close(cfgfd);
        if (pipefd[0] >= 0) close(pipefd[0]);
        if (pipefd[1] >= 0) close(pipefd[1]);
        (void)unlink(audio_path);
        if (err) snprintf(err, err_cap, "cannot start the request");
        return -1;
    }

    posix_spawn_file_actions_t fa;
    int rc = posix_spawn_file_actions_init(&fa);
    bool fa_inited = rc == 0;
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, cfgfd, STDIN_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDOUT_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_adddup2(&fa, pipefd[1], STDERR_FILENO);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&fa, cfgfd);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&fa, pipefd[0]);
    if (rc == 0) rc = posix_spawn_file_actions_addclose(&fa, pipefd[1]);

    pid_t pid = 0;
    char *argv[] = { (char *)"curl", (char *)"-K", (char *)"-", NULL };
    if (rc == 0) rc = posix_spawnp(&pid, liu_curl_exe(), &fa, NULL, argv, environ);
    if (fa_inited) posix_spawn_file_actions_destroy(&fa);
    close(cfgfd);
    close(pipefd[1]);
    if (rc != 0 || pid <= 0) {
        close(pipefd[0]);
        if (rc == 0 && pid > 0) {
            (void)kill(pid, SIGKILL);
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        }
        (void)unlink(audio_path);
        if (err) snprintf(err, err_cap, "cannot run curl");
        return -1;
    }

    int flags = fcntl(pipefd[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(pipefd[0], F_SETFL, flags | O_NONBLOCK);

    char *resp = (char *)malloc(VAPI_RESP_CAP);
    i32 len = -1;
    if (resp) len = drain_child(pipefd[0], pid, resp, VAPI_RESP_CAP,
                                keep_going, user);
    close(pipefd[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    (void)unlink(audio_path);

    if (!resp) {
        if (err) snprintf(err, err_cap, "out of memory");
        return -1;
    }
    if (len < 0) {                       /* cancelled */
        free(resp);
        if (err) snprintf(err, err_cap, "cancelled");
        return -1;
    }

    bool got = parse_response(resp, len, out, out_cap, lang_out, lang_cap,
                              err, err_cap);
    free(resp);
    return got ? (i32)strlen(out) : -1;
}
