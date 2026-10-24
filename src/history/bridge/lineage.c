/*
 * liu-history/bridge - migration lineage journal. See lineage.h.
 */
#include "history/bridge/lineage.h"
#include "history/bridge/jsonw.h"
#include "history/util.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Each migration record is small, so the journal scan has a fixed 4 MiB
 * limit and reads only its tail. This bounds the amount of history examined
 * when resolving a conversation's current destination. */
#define LINEAGE_MAX_BYTES   ((usize)MB(4))
#define LINEAGE_SCAN_TAIL   ((usize)KB(512))
#define LINEAGE_LINE_CAP    ((usize)KB(8))

bool bridge_lineage_path(char *out, usize cap) {
    if (!out || cap == 0) return false;
    /* Beside config.json, not under the state dir: this is durable user data
     * that survives a cache wipe, and it is the file a user would look at to
     * answer "where did that conversation go". */
    return hist_home_path(out, cap, ".config/Liu/agent-bridges.jsonl");
}

bool bridge_lineage_record(const BridgeLineage *rec) {
    if (!rec) return false;
    char path[ATOMIC_FILE_PATH_CAP];
    if (!bridge_lineage_path(path, sizeof path)) return false;
    if (!jsonw_path_safe(path)) return false;

    /* Cap the file rather than let a pathological caller fill the disk. The
     * check is before the append so the limit is a limit, not a suggestion. */
    struct stat st;
    if (lstat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) return false;      /* not ours to write */
        if ((usize)st.st_size >= LINEAGE_MAX_BYTES) return false;
    }

    /* Create the parent chain 0700 — a user who has never opened Settings may
     * have neither ~/.config nor ~/.config/Liu, and a single mkdir of the
     * direct parent fails on the first of those. */
    char dir[ATOMIC_FILE_PATH_CAP];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash && slash != dir) {
        *slash = '\0';
        if (!bridge_mkdir_p(dir)) return false;
    }

    /* O_APPEND|O_NOFOLLOW: appending is atomic for a write this small on
     * every platform Liu targets, so two windows migrating at once interleave
     * whole lines rather than corrupting each other. O_NOFOLLOW refuses a
     * symlink planted at the path. */
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    FILE *f = fdopen(fd, "a");
    if (!f) { close(fd); return false; }

    JsonW w;
    jsonw_open_stream(&w, f);
    jsonw_obj_begin(&w, NULL);
    jsonw_str(&w, "source_tool",       chat_tool_name(rec->source_tool));
    jsonw_str(&w, "source_session_id", rec->source_session_id);
    jsonw_str(&w, "target_tool",       chat_tool_name(rec->target_tool));
    jsonw_str(&w, "target_session_id", rec->target_session_id);
    jsonw_i64(&w, "created_at",        rec->created_at_ms);
    jsonw_str(&w, "mode",              bridge_import_mode_name(rec->mode));
    jsonw_str(&w, "fidelity",          bridge_fidelity_name(rec->fidelity));
    jsonw_obj_end(&w);
    bool ok = jsonw_ok(&w);
    fputc('\n', f);
    jsonw_close(&w, false);
    if (fclose(f) != 0) ok = false;
    return ok;
}

/* ------------------------------------------------------------------ scan -- */

static bool parse_line(const char *line, usize len, BridgeLineage *out) {
    cJSON *root = cJSON_ParseWithLength(line, len);
    if (!root) return false;
    memset(out, 0, sizeof *out);

    const char *st = hist_cjson_str(root, "source_tool");
    const char *tt = hist_cjson_str(root, "target_tool");
    const char *si = hist_cjson_str(root, "source_session_id");
    const char *ti = hist_cjson_str(root, "target_session_id");
    if (!st || !tt || !si || !ti) { cJSON_Delete(root); return false; }

    out->source_tool = chat_tool_from_agent_id(st);
    out->target_tool = chat_tool_from_agent_id(tt);
    hist_copy_bounded(out->source_session_id, sizeof out->source_session_id, si);
    hist_copy_bounded(out->target_session_id, sizeof out->target_session_id, ti);
    (void)hist_cjson_i64(root, "created_at", &out->created_at_ms);

    const char *mode = hist_cjson_str(root, "mode");
    out->mode = BRIDGE_IMPORT_UNSUPPORTED;
    if (mode) {
        for (i32 m = BRIDGE_IMPORT_UNSUPPORTED; m <= BRIDGE_IMPORT_CONTEXT_HANDOFF; m++) {
            if (strcmp(mode, bridge_import_mode_name((BridgeImportMode)m)) == 0) {
                out->mode = (BridgeImportMode)m;
                break;
            }
        }
    }
    const char *fid = hist_cjson_str(root, "fidelity");
    out->fidelity = BRIDGE_FIDELITY_HANDOFF;
    if (fid) {
        for (i32 fq = BRIDGE_FIDELITY_FULL; fq <= BRIDGE_FIDELITY_HANDOFF; fq++) {
            if (strcmp(fid, bridge_fidelity_name((BridgeFidelity)fq)) == 0) {
                out->fidelity = (BridgeFidelity)fq;
                break;
            }
        }
    }
    cJSON_Delete(root);
    return true;
}

u32 bridge_lineage_scan(bridge_lineage_cb cb, void *user, u32 limit) {
    if (!cb || limit == 0) return 0;
    char path[ATOMIC_FILE_PATH_CAP];
    if (!bridge_lineage_path(path, sizeof path)) return 0;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) { close(fd); return 0; }

    /* Read only the tail. Newest-first means the answer is almost always in
     * the last few lines, and a full read would scale with the journal. */
    usize want = (usize)st.st_size;
    off_t start = 0;
    if (want > LINEAGE_SCAN_TAIL) {
        start = (off_t)(want - LINEAGE_SCAN_TAIL);
        want  = LINEAGE_SCAN_TAIL;
    }
    char *buf = malloc(want + 1);
    if (!buf) { close(fd); return 0; }
    if (lseek(fd, start, SEEK_SET) != start) { free(buf); close(fd); return 0; }

    usize got = 0;
    while (got < want) {
        ssize_t n = read(fd, buf + got, want - got);
        if (n <= 0) break;
        got += (usize)n;
    }
    close(fd);
    buf[got] = '\0';

    /* A tail read almost certainly starts mid-line; drop the partial head. */
    usize begin = 0;
    if (start > 0) {
        char *nl = memchr(buf, '\n', got);
        begin = nl ? (usize)(nl - buf) + 1 : got;
    }

    /* Collect line bounds, then walk them backwards for newest-first. */
    usize *starts = NULL, *lens = NULL, count = 0, cap = 0;
    for (usize i = begin; i < got; ) {
        char *nl = memchr(buf + i, '\n', got - i);
        usize end = nl ? (usize)(nl - buf) : got;
        if (end > i && (end - i) < LINEAGE_LINE_CAP) {
            if (count == cap) {
                usize ncap = cap ? cap * 2 : 128;
                usize *s2 = realloc(starts, ncap * sizeof *s2);
                usize *l2 = realloc(lens,   ncap * sizeof *l2);
                if (s2) starts = s2;
                if (l2) lens   = l2;
                if (!s2 || !l2) break;
                cap = ncap;
            }
            starts[count] = i;
            lens[count]   = end - i;
            count++;
        }
        if (!nl) break;
        i = end + 1;
    }

    u32 visited = 0;
    for (usize k = count; k > 0 && visited < limit; k--) {
        BridgeLineage rec;
        if (!parse_line(buf + starts[k - 1], lens[k - 1], &rec)) continue;
        visited++;
        if (!cb(&rec, user)) break;
    }

    free(starts);
    free(lens);
    free(buf);
    return visited;
}

typedef struct {
    ChatTool       tool;
    const char    *sid;
    BridgeLineage *out;
    bool           forward;
    bool           found;
} FindCtx;

static bool find_cb(const BridgeLineage *rec, void *user) {
    FindCtx *c = user;
    ChatTool t   = c->forward ? rec->source_tool : rec->target_tool;
    const char *id = c->forward ? rec->source_session_id : rec->target_session_id;
    if (t == c->tool && strcmp(id, c->sid) == 0) {
        *c->out = *rec;
        c->found = true;
        return false;   /* newest-first, so the first hit is the latest */
    }
    return true;
}

bool bridge_lineage_find_forward(ChatTool tool, const char *session_id,
                                 BridgeLineage *out) {
    if (!session_id || !session_id[0] || !out) return false;
    FindCtx c = { tool, session_id, out, true, false };
    bridge_lineage_scan(find_cb, &c, 4096);
    return c.found;
}

bool bridge_lineage_find_back(ChatTool tool, const char *session_id,
                              BridgeLineage *out) {
    if (!session_id || !session_id[0] || !out) return false;
    FindCtx c = { tool, session_id, out, false, false };
    bridge_lineage_scan(find_cb, &c, 4096);
    return c.found;
}
