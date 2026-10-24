/*
 * liu-history/bridge - JSONL line driver. See src_jsonl.h.
 */
#include "history/bridge/sources/src_jsonl.h"
#include "history/util.h"

#include "cJSON.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BRIDGE_LINE_CAP     ((usize)MB(8))
#define BRIDGE_SCRATCH_SIZE ((usize)MB(12))

u64 bridge_jsonl_run(const char *path, BridgeHistory *h,
                     bridge_jsonl_cb cb, void *user, u64 *out_bad) {
    if (out_bad) *out_bad = 0;
    if (!path || !h || !cb) return 0;

    /* Same guard parser_run.c uses: a symlink planted under an agent's store
     * must not be able to point the read anywhere else. BRIDGE_LINE_CAP is a
     * real allocation limit here, not just a parse gate — the reader never
     * grows past cap+1, where plain getline() materialised the whole line
     * first and rejected it afterwards. */
    HistLineReader rd;
    if (!hist_line_reader_open(&rd, path, true)) return 0;

    Arena scratch = arena_create(BRIDGE_SCRATCH_SIZE);
    if (!scratch.base) { hist_line_reader_close(&rd); return 0; }

    u64 delivered = 0, bad = 0;

    for (;;) {
        const char *line = NULL;
        usize n = 0;
        HistLineResult r = hist_line_reader_next(&rd, BRIDGE_LINE_CAP,
                                                 &line, &n);
        if (r == HIST_LINE_EOF || r == HIST_LINE_IO_ERROR) break;
        if (r == HIST_LINE_TOO_LONG) { bad++; continue; }
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) n--;
        if (n == 0) continue;

        arena_reset(&scratch);
        cJSON *root = cJSON_ParseWithLength(line, n);
        if (!root) { bad++; continue; }
        bool go = cb(root, &scratch, h, user);
        cJSON_Delete(root);
        delivered++;
        if (!go) break;
    }

    arena_destroy(&scratch);
    hist_line_reader_close(&rd);
    if (out_bad) *out_bad = bad;
    return delivered;
}

void *bridge_json_slurp(const char *path, usize max_bytes) {
    if (!path) return NULL;
    char *buf = NULL;
    usize len = 0;
    /* hist_slurp_file_heap already refuses symlinks and oversized files, and
     * uses the heap rather than an arena because cJSON copies every string it
     * keeps — so the source bytes are dead the moment the parse returns and
     * arena memory could not be handed back. */
    if (!hist_slurp_file_heap(path, max_bytes, &buf, &len)) return NULL;
    cJSON *root = cJSON_ParseWithLength(buf, len);
    free(buf);
    return root;
}
