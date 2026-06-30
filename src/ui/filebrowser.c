/*
 * Liu - file browser implementation
 * Local filesystem + SFTP browsing, markdown/code viewer.
 */
#include "ui/filebrowser.h"
#include "ui/fb_asset_icons.h"
#include "ui/icon.h"
#include "ui/chrome_palette.h"
#include "ui/markdown/md_parse.h"
#include "ui/markdown/md_render.h"
#include "ui/markdown/md_image.h"
#include "ui/markdown/md_graph.h"
#ifdef HAVE_FB_ICON
#include "ui/fb_icon.h"
#else
typedef struct {
    u32             codepoint;
    Color           color;
    FbAssetIconKind asset;
} FbIcon;

static FbIcon fb_icon_for(const char *name, bool is_dir) {
    (void)name;
    (void)is_dir;
    return (FbIcon){0};
}
#endif
#include "ui/layout.h"
#include "core/config.h"
#include "core/utf8.h"
#include "platform/platform.h"
#include "ssh/ssh_session.h"
#include <stdlib.h>
#include <ctype.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <math.h>

/* Forward declare editor helper */
static void editor_pre_edit(FileBrowser *fb);
static i32 editor_line_len(FileBrowser *fb, i32 line);
static char *editor_line_start(FileBrowser *fb, i32 line);
static void fb_editor_doc_stats(const FileBrowser *fb, u32 *words, u32 *chars, u32 *read_min);
static void fb_graph_build_reap(FileBrowser *fb);   /* join an in-flight graph build */
#include <dirent.h>
#include <sys/stat.h>

#include <libssh2.h>
#include <libssh2_sftp.h>

#include "stb_image.h"

/* =========================================================================
 * Init / Destroy
 * ========================================================================= */

void fb_init(FileBrowser *fb) {
    memset(fb, 0, sizeof(*fb));
    fb->selected = -1;
    fb->selection_anchor = -1;
    fb->width = 240;
    fb->entries = NULL;
    fb->entry_cap = 0;
    fb->graph_drag_slider = -1;   /* 0 is a valid slider index; -1 = none */
}

void fb_entry_set_name(FileEntry *e, const char *src) {
    if (!e) return;
    free(e->name);
    if (!src) { e->name = NULL; return; }
    /* Filesystems on external volumes (HFS+, ExFAT, some SMB shares) return
     * filenames in NFD-decomposed form: "ş" comes back as "s" + U+0327
     * combining cedilla. Our renderer pushes one glyph per codepoint at a
     * fixed cell width, so the combining mark lands in its own cell where
     * it visually disappears (the mark is offset off the cell's printable
     * box). Normalizing to NFC up front turns the pair back into a single
     * precomposed codepoint that the font has a real glyph for. */
    char buf[1024];
    if (platform_utf8_normalize_nfc(src, buf, sizeof buf)) {
        src = buf;
    }
    usize n = strlen(src);
    e->name = malloc(n + 1);
    if (e->name) memcpy(e->name, src, n + 1);
}

void fb_entries_reset(FileBrowser *fb) {
    if (!fb || !fb->entries) { if (fb) fb->entry_count = 0; return; }
    for (i32 i = 0; i < fb->entry_count; i++) {
        free(fb->entries[i].name);
        fb->entries[i].name = NULL;
    }
    fb->entry_count = 0;
}

/* =========================================================================
 * Multi-selection bitmap
 *
 * FB_MAX_ENTRIES is small enough (1024) that a flat bitmap fits in 128 bytes.
 * `selection_count` is the popcount cache; keeping it accurate in set/toggle
 * lets callers short-circuit on empty selections without scanning 128 bytes.
 * ========================================================================= */

void fb_sel_clear(FileBrowser *fb) {
    if (!fb) return;
    memset(fb->selection_set, 0, sizeof(fb->selection_set));
    fb->selection_count = 0;
    fb->selection_anchor = -1;
}

bool fb_sel_has(const FileBrowser *fb, i32 idx) {
    if (!fb || idx < 0 || idx >= fb->entry_count) return false;
    return (fb->selection_set[idx >> 3] & (1u << (idx & 7))) != 0;
}

void fb_sel_set(FileBrowser *fb, i32 idx, bool on) {
    if (!fb || idx < 0 || idx >= fb->entry_count) return;
    u8 *byte = &fb->selection_set[idx >> 3];
    u8 mask  = (u8)(1u << (idx & 7));
    bool was = (*byte & mask) != 0;
    if (on && !was) { *byte = (u8)(*byte | mask);  fb->selection_count++; }
    if (!on && was) { *byte = (u8)(*byte & ~mask); fb->selection_count--; }
}

void fb_sel_toggle(FileBrowser *fb, i32 idx) {
    if (!fb || idx < 0 || idx >= fb->entry_count) return;
    fb_sel_set(fb, idx, !fb_sel_has(fb, idx));
}

void fb_sel_range(FileBrowser *fb, i32 a, i32 b) {
    if (!fb) return;
    if (a > b) { i32 t = a; a = b; b = t; }
    if (a < 0) a = 0;
    if (b >= fb->entry_count) b = fb->entry_count - 1;
    for (i32 i = a; i <= b; i++) fb_sel_set(fb, i, true);
}

i32 fb_sel_collect(const FileBrowser *fb, i32 *out, i32 cap) {
    if (!fb || !out || cap <= 0) return 0;
    i32 n = 0;
    for (i32 i = 0; i < fb->entry_count && n < cap; i++) {
        if (fb->selection_set[i >> 3] & (1u << (i & 7))) out[n++] = i;
    }
    return n;
}

void fb_destroy(FileBrowser *fb) {
    fb_graph_build_reap(fb);   /* join any in-flight async graph build first */
    fb_entries_reset(fb);
    free(fb->entries);
    free(fb->view_content);
    free(fb->view_spans);
    if (fb->md_images) { md_image_cache_destroy(fb->md_images); fb->md_images = NULL; }
    if (fb->md_arena.base) { arena_destroy(&fb->md_arena); }
    fb->md_doc = NULL;
    free(fb->md_link_rects);
    fb->md_link_rects = NULL;
    fb->md_link_count = 0;
    free(fb->md_task_rects);
    fb->md_task_rects = NULL;
    fb->md_task_count = 0;
    free(fb->md_glyph_rects);
    fb->md_glyph_rects = NULL;
    fb->md_glyph_count = 0;
    free(fb->md_outline);
    fb->md_outline = NULL;
    fb->md_outline_count = 0;
    free(fb->find_matches);
    fb->find_matches = NULL;
    fb->find_match_count = fb->find_match_cap = 0;
    free(fb->ac_items);
    fb->ac_items = NULL;
    fb->ac_count = fb->ac_cap = 0;
    if (fb->view_image_rgba) { stbi_image_free(fb->view_image_rgba); fb->view_image_rgba = NULL; }
    free(fb->view_image_frame_delays);
    /* Free undo frames + baseline mirrors. */
    for (i32 i = 0; i < fb->undo_count; i++) {
        free(fb->undo_frames[i].old_bytes);
        free(fb->undo_frames[i].new_bytes);
    }
    free(fb->undo_base);
    free(fb->undo_tip);
    /* The knowledge graph is otherwise only freed by fb_close_viewer; a tab
     * closed while still in graph mode would leak the whole MdGraph (~2 MB). */
    if (fb->graph) { md_graph_free(fb->graph); fb->graph = NULL; }
    free(fb->graph_lbl_grid);  fb->graph_lbl_grid = NULL;
    free(fb->graph_lbl_key);   fb->graph_lbl_key = NULL;
    free(fb->graph_nbr_set);   fb->graph_nbr_set = NULL;
    memset(fb, 0, sizeof(*fb));
}

/* =========================================================================
 * Helpers
 * ========================================================================= */

const char *fb_format_size(u64 bytes) {
    static char buf[32];
    if (bytes < 1024) snprintf(buf, sizeof(buf), "%lluB", (unsigned long long)bytes);
    else if (bytes < 1024*1024) snprintf(buf, sizeof(buf), "%.1fK", bytes/1024.0);
    else if (bytes < 1024*1024*1024) snprintf(buf, sizeof(buf), "%.1fM", bytes/(1024.0*1024));
    else snprintf(buf, sizeof(buf), "%.1fG", bytes/(1024.0*1024*1024));
    return buf;
}

const char *fb_file_icon(const char *name, bool is_dir) {
    if (is_dir) return "[D]";
    const char *ext = strrchr(name, '.');
    if (!ext) return "   ";
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0) return "[C]";
    if (strcmp(ext, ".py") == 0) return "[P]";
    if (strcmp(ext, ".js") == 0 || strcmp(ext, ".ts") == 0) return "[J]";
    if (strcmp(ext, ".rs") == 0) return "[R]";
    if (strcmp(ext, ".go") == 0) return "[G]";
    if (strcmp(ext, ".md") == 0) return "[M]";
    if (strcmp(ext, ".sh") == 0) return "[S]";
    if (strcmp(ext, ".txt") == 0) return "[T]";
    if (strcmp(ext, ".json") == 0) return "[J]";
    if (strcmp(ext, ".yaml") == 0 || strcmp(ext, ".yml") == 0) return "[Y]";
    if (strcmp(ext, ".toml") == 0) return "[T]";
    if (strcmp(ext, ".png") == 0 || strcmp(ext, ".jpg") == 0) return "[I]";
    return "   ";
}

FileViewMode fb_detect_mode(const char *filename) {
    const char *ext = strrchr(filename, '.');
    if (!ext) return FVIEW_TEXT;
    if (strcmp(ext, ".md") == 0 || strcmp(ext, ".markdown") == 0) return FVIEW_MARKDOWN;
    if (strcmp(ext, ".c") == 0 || strcmp(ext, ".h") == 0 ||
        strcmp(ext, ".py") == 0 || strcmp(ext, ".js") == 0 ||
        strcmp(ext, ".ts") == 0 || strcmp(ext, ".rs") == 0 ||
        strcmp(ext, ".go") == 0 || strcmp(ext, ".java") == 0 ||
        strcmp(ext, ".sh") == 0 || strcmp(ext, ".bash") == 0 ||
        strcmp(ext, ".zsh") == 0 || strcmp(ext, ".rb") == 0 ||
        strcmp(ext, ".swift") == 0 || strcmp(ext, ".m") == 0 ||
        strcmp(ext, ".S") == 0 || strcmp(ext, ".asm") == 0 ||
        strcmp(ext, ".json") == 0 || strcmp(ext, ".yaml") == 0 ||
        strcmp(ext, ".yml") == 0 || strcmp(ext, ".toml") == 0 ||
        strcmp(ext, ".xml") == 0 || strcmp(ext, ".html") == 0 ||
        strcmp(ext, ".css") == 0 || strcmp(ext, ".sql") == 0 ||
        strcmp(ext, ".zig") == 0 || strcmp(ext, ".lua") == 0 ||
        strcmp(ext, ".Makefile") == 0) return FVIEW_CODE;
    if (strcmp(ext, ".png")  == 0 || strcmp(ext, ".jpg")  == 0 ||
        strcmp(ext, ".jpeg") == 0 || strcmp(ext, ".gif")  == 0 ||
        strcmp(ext, ".bmp")  == 0 || strcmp(ext, ".tga")  == 0 ||
        strcmp(ext, ".psd")  == 0 || strcmp(ext, ".pnm")  == 0 ||
        strcmp(ext, ".ppm")  == 0 || strcmp(ext, ".pgm")  == 0 ||
        strcmp(ext, ".webp") == 0 || strcmp(ext, ".hdr")  == 0)
        return FVIEW_IMAGE;
    return FVIEW_TEXT;
}

void fb_entry_path(const FileBrowser *fb, const FileEntry *entry, char *out, usize out_size) {
    if (!out || out_size == 0) return;
    out[0] = '\0';
    if (!fb || !entry) return;

    if (strcmp(entry->name, "..") == 0) {
        if (strcmp(fb->cwd, "/") == 0) {
            snprintf(out, out_size, "/");
        } else {
            snprintf(out, out_size, "%s/..", fb->cwd);
        }
        return;
    }

    if (strcmp(fb->cwd, "/") == 0) {
        snprintf(out, out_size, "/%s", entry->name);
    } else {
        snprintf(out, out_size, "%s/%s", fb->cwd, entry->name);
    }
}

static int entry_cmp(const void *a, const void *b) {
    const FileEntry *ea = (const FileEntry *)a;
    const FileEntry *eb = (const FileEntry *)b;
    /* Dirs first, then alphabetical */
    if (ea->is_dir != eb->is_dir) return ea->is_dir ? -1 : 1;
    const char *na = ea->name ? ea->name : "";
    const char *nb = eb->name ? eb->name : "";
    return strcasecmp(na, nb);
}

/* =========================================================================
 * Local filesystem navigation
 * ========================================================================= */

bool fb_navigate(FileBrowser *fb, const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return false;

    fb->source = FB_SOURCE_LOCAL;
    /* Normalize to NFC for the same reason as fb_entry_set_name — the CWD
     * is rendered in the header strip with the same glyph pipeline, so an
     * NFD path would show base-char + invisible combining mark cells.
     * Normalize through a temp: fb_refresh() passes fb->cwd AS `path`, and
     * platform_utf8_normalize_nfc clears dst[0] before reading src — writing
     * straight to fb->cwd from an aliased src would blank the path (the
     * "folder name vanishes on refresh" bug). */
    char nav_cwd[sizeof fb->cwd];
    if (!platform_utf8_normalize_nfc(path, nav_cwd, sizeof nav_cwd))
        snprintf(nav_cwd, sizeof nav_cwd, "%s", path);
    snprintf(fb->cwd, sizeof(fb->cwd), "%s", nav_cwd);
    fb_entries_reset(fb);
    fb->md_only_entries = false;   /* full listing reloaded below */
    fb->selected = -1;
    fb_sel_clear(fb);
    fb->scroll_offset = 0;
    fb->scroll_offset_px = 0.0f;

    /* Lazy allocate entries */
    if (!fb->entries) {
        fb->entry_cap = 64;
        fb->entries = calloc((usize)fb->entry_cap, sizeof(FileEntry));
        if (!fb->entries) { closedir(dir); return false; }
    }

    struct dirent *de;
    while ((de = readdir(dir))) {
        if (de->d_name[0] == '.' && de->d_name[1] == '\0') continue;
        if (fb->entry_count >= FB_MAX_ENTRIES) break;

        /* Grow if needed */
        if (fb->entry_count >= fb->entry_cap) {
            i32 new_cap = fb->entry_cap * 2;
            if (new_cap > FB_MAX_ENTRIES) new_cap = FB_MAX_ENTRIES;
            FileEntry *grown = realloc(fb->entries, (usize)new_cap * sizeof(FileEntry));
            if (!grown) break;
            fb->entries = grown;
            fb->entry_cap = new_cap;
        }

        FileEntry *e = &fb->entries[fb->entry_count];
        memset(e, 0, sizeof(*e));
        fb_entry_set_name(e, de->d_name);

        if (de->d_name[0] == '.' && de->d_name[1] == '.' && de->d_name[2] == '\0') {
            e->is_dir = true;
        } else {
            struct stat st;
            char full_path[FB_MAX_PATH * 2];
            if (strcmp(path, "/") == 0) {
                snprintf(full_path, sizeof(full_path), "/%s", de->d_name);
            } else {
                snprintf(full_path, sizeof(full_path), "%s/%s", path, de->d_name);
            }
            if (stat(full_path, &st) == 0) {
                e->size = (u64)st.st_size;
                e->permissions = (u32)st.st_mode;
                e->mtime = (u64)st.st_mtime;
                e->is_dir = S_ISDIR(st.st_mode);
                e->is_symlink = S_ISLNK(st.st_mode);
            } else {
                /* stat failed — fall back to dirent type if available */
                fprintf(stderr, "stat failed: %s (errno=%d)\n", full_path, errno);
#ifdef DT_DIR
                e->is_dir = (de->d_type == DT_DIR);
                e->is_symlink = (de->d_type == DT_LNK);
#endif
            }
        }
        fb->entry_count++;
    }
    closedir(dir);

    /* Sort: dirs first, then alpha */
    if (fb->entry_count > 1) {
        /* Keep ".." at top */
        i32 start = 0;
        if (fb->entry_count > 0 && fb->entries[0].name && strcmp(fb->entries[0].name, "..") == 0) start = 1;
        qsort(fb->entries + start, (usize)(fb->entry_count - start), sizeof(FileEntry), entry_cmp);
    }

    return true;
}

/* =========================================================================
 * SFTP navigation
 * ========================================================================= */

bool fb_navigate_sftp(FileBrowser *fb, void *sftp_handle, const char *path) {
    LIBSSH2_SFTP *sftp = (LIBSSH2_SFTP *)sftp_handle;
    if (!sftp) return false;

    /* libssh2_sftp_readdir returns LIBSSH2_ERROR_EAGAIN (-37) on a non-
     * blocking session, which the loop below would mistake for EOF and
     * silently truncate the listing. Flip to blocking for the duration
     * of this call (main thread is the sole libssh2 user after CONNECTED,
     * so the flip is safe). No-op when fb->session is NULL. */
    Session *owner = (Session *)fb->session;
    session_sftp_scope_begin(owner);

    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_opendir(sftp, path);
    if (!dir) {
        session_sftp_scope_end(owner);
        return false;
    }

    fb->source = FB_SOURCE_SFTP;
    fb->sftp_handle = sftp_handle;
    /* Temp-normalize: fb_refresh() passes fb->cwd as `path`, which aliases
     * dst and would otherwise be blanked (see fb_navigate). */
    char nav_cwd[sizeof fb->cwd];
    if (!platform_utf8_normalize_nfc(path, nav_cwd, sizeof nav_cwd))
        snprintf(nav_cwd, sizeof nav_cwd, "%s", path);
    snprintf(fb->cwd, sizeof(fb->cwd), "%s", nav_cwd);
    fb_entries_reset(fb);
    fb->md_only_entries = false;   /* full listing reloaded below */
    fb->selected = -1;
    fb_sel_clear(fb);
    fb->scroll_offset = 0;
    fb->scroll_offset_px = 0.0f;

    if (!fb->entries) {
        fb->entry_cap = 64;
        fb->entries = calloc((usize)fb->entry_cap, sizeof(FileEntry));
        if (!fb->entries) {
            libssh2_sftp_closedir(dir);
            session_sftp_scope_end(owner);
            return false;
        }
    }

    /* Add ".." entry */
    FileEntry *up = &fb->entries[fb->entry_count++];
    memset(up, 0, sizeof(*up));
    fb_entry_set_name(up, "..");
    up->is_dir = true;

    char buf[512];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    while (1) {
        int rc = libssh2_sftp_readdir(dir, buf, sizeof(buf), &attrs);
        if (rc <= 0) break;
        if (buf[0] == '.' && (buf[1] == '\0' || (buf[1] == '.' && buf[2] == '\0'))) continue;
        if (fb->entry_count >= FB_MAX_ENTRIES) break;

        /* Grow if needed */
        if (fb->entry_count >= fb->entry_cap) {
            i32 new_cap = fb->entry_cap * 2;
            if (new_cap > FB_MAX_ENTRIES) new_cap = FB_MAX_ENTRIES;
            FileEntry *grown = realloc(fb->entries, (usize)new_cap * sizeof(FileEntry));
            if (!grown) break;
            fb->entries = grown;
            fb->entry_cap = new_cap;
        }

        FileEntry *e = &fb->entries[fb->entry_count];
        memset(e, 0, sizeof(*e));
        fb_entry_set_name(e, buf);
        e->size = attrs.filesize;
        e->permissions = attrs.permissions;
        e->mtime = attrs.mtime;
        e->is_dir = LIBSSH2_SFTP_S_ISDIR(attrs.permissions);
        fb->entry_count++;
    }
    libssh2_sftp_closedir(dir);
    session_sftp_scope_end(owner);

    qsort(fb->entries + 1, (usize)(fb->entry_count - 1), sizeof(FileEntry), entry_cmp);
    return true;
}

void fb_detach_sftp(FileBrowser *fb) {
    if (!fb) return;
    fb->sftp_handle = NULL;
    fb->session = NULL;
    fb_entries_reset(fb);
    fb->selected = -1;
    fb->scroll_offset = 0;
    fb->scroll_offset_px = 0.0f;
    fb->source = FB_SOURCE_LOCAL;
    fb->cwd[0] = '\0';
}

void fb_go_up(FileBrowser *fb) {
    char parent[FB_MAX_PATH];
    snprintf(parent, sizeof(parent), "%s", fb->cwd);
    char *last = strrchr(parent, '/');
    if (last && last != parent) *last = '\0';
    else snprintf(parent, sizeof(parent), "/");

    if (fb->source == FB_SOURCE_SFTP)
        fb_navigate_sftp(fb, fb->sftp_handle, parent);
    else
        fb_navigate(fb, parent);
}

void fb_refresh(FileBrowser *fb) {
    if (fb->source == FB_SOURCE_SFTP)
        fb_navigate_sftp(fb, fb->sftp_handle, fb->cwd);
    else
        fb_navigate(fb, fb->cwd);
}

/* =========================================================================
 * File viewer — load + markdown bring-up
 *
 * Markdown is parsed into an MdDoc the moment a .md file opens; the AST
 * lives in fb->md_arena and is freed wholesale on close. Inline images
 * are loaded lazily through fb->md_images and stay resident until close.
 * The legacy parse_markdown / md_draw_inline helpers (and the per-line
 * MdElement struct in the header) were retired in favour of
 * src/ui/markdown.
 * ========================================================================= */

/* Basic C/code syntax highlighting */
static const char *c_keywords[] = {
    "if","else","for","while","do","switch","case","break","continue","return",
    "struct","typedef","enum","union","sizeof","static","const","volatile",
    "extern","inline","void","int","char","float","double","long","short",
    "unsigned","signed","bool","true","false","NULL","include","define",
    "ifdef","ifndef","endif","pragma","import","fn","let","mut","pub","use",
    "impl","trait","mod","crate","self","super","async","await","match",
    "def","class","from","print","elif","lambda","try","except","finally",
    "function","var","const","require","export","default","new","this",
    NULL
};

static bool is_keyword(const char *word, i32 len) {
    for (i32 i = 0; c_keywords[i]; i++) {
        if ((i32)strlen(c_keywords[i]) == len && strncmp(word, c_keywords[i], (usize)len) == 0)
            return true;
    }
    return false;
}

static bool is_ident_char(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

bool fb_open_file(FileBrowser *fb, i32 index) {
    if (index < 0 || index >= fb->entry_count) return false;
    FileEntry *e = &fb->entries[index];
    char entry_path[FB_MAX_PATH * 2];
    fb_entry_path(fb, e, entry_path, sizeof(entry_path));

    if (e->is_dir) {
        if (strcmp(e->name, "..") == 0) {
            fb_go_up(fb);
        } else {
            if (fb->source == FB_SOURCE_SFTP)
                fb_navigate_sftp(fb, fb->sftp_handle, entry_path);
            else
                fb_navigate(fb, entry_path);
        }
        return true;
    }

    /* Load file content (local only for now) */
    if (fb->source == FB_SOURCE_LOCAL) {
        fprintf(stderr, "fb_open_file: index=%d name='%s' path='%s'\n", index, e->name, entry_path);
        /* Resolve the path once — the entry may stat under cwd-relative,
         * absolute, or rebuilt forms; we want the same string used by
         * stbi_load below. */
        char resolved_path[FB_MAX_PATH];
        snprintf(resolved_path, sizeof(resolved_path), "%s", entry_path);
        if (access(resolved_path, R_OK) != 0) {
            snprintf(resolved_path, sizeof(resolved_path), "%s/%s", fb->cwd, e->name);
            if (access(resolved_path, R_OK) != 0) {
                fprintf(stderr, "  access failed: %s errno=%d\n", resolved_path, errno);
                return false;
            }
        }

        FileViewMode mode = fb_detect_mode(e->name);

        /* Image files are decoded with stbi_load directly — bypass the
         * text-buffer path so we don't waste a malloc on raw bytes we'll
         * never look at, and so the 2 MB text cap doesn't reject 10 MB
         * photos. Free any previously-loaded image before claiming the
         * new one so back-to-back image clicks don't leak.
         *
         * GIFs go through stbi_load_gif_from_memory so multi-frame
         * animations expose all frames + per-frame delays. Static GIFs
         * fall through with frame_count == 1, indistinguishable from
         * a stbi_load() result at the renderer layer. */
        if (mode == FVIEW_IMAGE) {
            const char *xx = strrchr(e->name, '.');
            bool is_gif = (xx && (strcmp(xx, ".gif") == 0 || strcmp(xx, ".GIF") == 0));

            u8 *px = NULL;
            int iw = 0, ih = 0, comp = 0, frame_count = 1;
            int *delays = NULL;

            if (is_gif) {
                /* Read full file into memory for the gif decoder. Cap at
                 * 32 MiB on the file so a malformed gif can't blow past
                 * sane bounds — decoded RGBA frames are checked separately
                 * below against a 128 MiB ceiling. */
                FILE *gf = fopen(resolved_path, "rb");
                if (!gf) {
                    fprintf(stderr, "  fopen failed (gif): %s errno=%d\n",
                            resolved_path, errno);
                    return false;
                }
                fseek(gf, 0, SEEK_END);
                long fsz = ftell(gf);
                fseek(gf, 0, SEEK_SET);
                if (fsz <= 0 || fsz > 32 * 1024 * 1024) { fclose(gf); return false; }
                u8 *fbuf = (u8 *)malloc((usize)fsz);
                if (!fbuf) { fclose(gf); return false; }
                if (fread(fbuf, 1, (usize)fsz, gf) != (usize)fsz) {
                    free(fbuf);
                    fclose(gf);
                    return false;
                }
                fclose(gf);

                int z = 0;
                px = stbi_load_gif_from_memory(fbuf, (int)fsz, &delays,
                                               &iw, &ih, &z, &comp, 4);
                free(fbuf);
                frame_count = z > 0 ? z : 1;
                if (!px || iw <= 0 || ih <= 0 || frame_count <= 0) {
                    fprintf(stderr, "  stbi_load_gif failed: %s\n",
                            stbi_failure_reason());
                    if (px) stbi_image_free(px);
                    if (delays) free(delays);
                    return false;
                }
            } else {
                px = stbi_load(resolved_path, &iw, &ih, &comp, 4);
                if (!px || iw <= 0 || ih <= 0) {
                    fprintf(stderr, "  stbi_load failed: %s\n", stbi_failure_reason());
                    if (px) stbi_image_free(px);
                    return false;
                }
            }

            /* Reject absurd dimensions so a malicious file can't OOM the
             * viewer (mirrors md_image's 256 megapixel cap). The gif
             * buffer is `frame_count` * iw * ih * 4 — cap decoded RGBA
             * at 128 MiB. If a multi-frame gif blows past that, fall
             * back to single-frame display (frame 0 only) instead of
             * refusing to load entirely. */
            if ((i64)iw * (i64)ih > (i64)256 * 1024 * 1024) {
                stbi_image_free(px);
                if (delays) free(delays);
                return false;
            }
            i64 decoded_bytes = (i64)iw * (i64)ih * 4 * (i64)frame_count;
            if (decoded_bytes > (i64)128 * 1024 * 1024) {
                /* Single frame must itself fit; if even one frame is too
                 * big we genuinely cannot display this image. */
                if ((i64)iw * (i64)ih * 4 > (i64)128 * 1024 * 1024) {
                    stbi_image_free(px);
                    if (delays) free(delays);
                    return false;
                }
                /* Drop subsequent frames — first frame's RGBA sits at the
                 * head of the buffer, so we just clamp frame_count. The
                 * tail bytes are still owned by `px` and freed with it. */
                frame_count = 1;
                if (delays) { free(delays); delays = NULL; }
            }

            if (fb->view_image_rgba) stbi_image_free(fb->view_image_rgba);
            free(fb->view_image_frame_delays);
            fb->view_image_rgba = px;
            fb->view_image_w = iw;
            fb->view_image_h = ih;
            fb->view_image_frame_count   = frame_count;
            fb->view_image_frame_delays  = delays;          /* may be NULL */
            fb->view_image_frame_index   = 0;
            fb->view_image_frame_anchor  = platform_time_sec();
            fb->view_image_next_frame_at = 0.0;
            fb->view_image_is_animated   = (frame_count > 1);
            fb->view_image_gen++;
            fb->view_image_zoom  = 1.0f;
            fb->view_image_pan_x = 0.0f;
            fb->view_image_pan_y = 0.0f;

            /* The text-side buffer doesn't need the file bytes for
             * images, but the legacy fb_render_viewer guard rejects
             * the early return when view_content == NULL. Keep an empty
             * marker so the IMAGE branch can fire. */
            free(fb->view_content);
            fb->view_content = (char *)malloc(1);
            if (fb->view_content) fb->view_content[0] = '\0';
            fb->view_size = 0;
            fb->view_cap = fb->view_content ? 1 : 0;

            snprintf(fb->view_path, sizeof(fb->view_path), "%s", resolved_path);
            fb->view_mode = FVIEW_IMAGE;
            fb->editor_mode = false;
            fb->modified = false;
            fb->cursor_line = 0;
            fb->cursor_col = 0;
            fb->view_scroll = 0;
            fb->view_scroll_px = 0.0f;
            fb->view_content_px = 0.0f;
            fb->view_line_count = 0;
            fb_editor_reset_undo(fb);
            return true;
        }

        FILE *f = fopen(resolved_path, "rb");
        if (!f) {
            fprintf(stderr, "  fopen failed: %s errno=%d\n", resolved_path, errno);
            return false;
        }

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        /* ftell failure (-1) must bail: (usize)sz would be SIZE_MAX, giving a
         * malloc(0) followed by a SIZE_MAX-byte fread → heap overflow. */
        if (sz < 0 || sz > 2 * 1024 * 1024) { fclose(f); return false; } /* error / 2MB limit */
        fseek(f, 0, SEEK_SET);

        free(fb->view_content);
        fb->view_content = malloc((usize)sz + 1);
        if (!fb->view_content) { fclose(f); fb->view_cap = 0; return false; }
        /* Use the actual bytes read; a short read must not leave an
         * uninitialized tail that view_size then exposes. */
        usize got = fread(fb->view_content, 1, (usize)sz, f);
        fb->view_content[got] = '\0';
        fb->view_size = got;
        fb->view_cap  = (usize)sz + 1;
        fclose(f);

        snprintf(fb->view_path, sizeof(fb->view_path), "%s", resolved_path);
        fb->view_mode = mode;
        fb->view_scroll = 0;
        fb->view_scroll_px = 0.0f;
        fb->view_content_px = 0.0f;
        fb->editor_mode = (fb->view_mode == FVIEW_CODE || fb->view_mode == FVIEW_TEXT);
        fb->cursor_line = 0;
        fb->cursor_col = 0;
        fb->modified = false;
        /* Drop any markdown selection from the previously-viewed file. The
         * glyph rects backing it belong to a doc we're about to free (or to
         * a doc that simply no longer matches the new view). Without this,
         * fb_md_selection_active() keeps returning true after the user
         * navigates to a text/code/image file and the Cmd+C path copies
         * stale text from the old Markdown render. */
        fb->md_sel_active = false;

        /* Count lines */
        fb->view_line_count = 1;
        for (usize i = 0; i < fb->view_size; i++) {
            if (fb->view_content[i] == '\n') fb->view_line_count++;
        }

        if (fb->view_mode == FVIEW_MARKDOWN) {
            /* Every markdown open lands in Read mode — otherwise a note opened
             * while the previous one was in Edit inherits md_raw_mode=true with
             * editor_mode=false (raw shown but not editable, Cmd+E stuck). */
            fb->md_raw_mode = false;
            /* Open with a narrow list sidebar so the renderer fills the window
             * (consumed by render_fb_tab; resets even if a previous file left a
             * wide dragged divider). */
            fb->reset_split_narrow = true;
            /* Tear down any previous markdown state — fb_open_file may run
             * back-to-back when the user clicks through .md files. */
            if (fb->md_images) { md_image_cache_destroy(fb->md_images); fb->md_images = NULL; }
            if (fb->md_arena.base) { arena_destroy(&fb->md_arena); }
            fb->md_doc = NULL;
            /* md_sel_active was already cleared in the general-reset block
             * above; no per-MD-mode duplicate needed. */

            /* Size for the worst case: emphasis/link-dense markdown expands to
             * many MdInline nodes per source byte, so budget source*8 for the
             * copied strings (callout/quote bodies are additionally copied into
             * a clean arena buffer and re-parsed into child blocks, so a
             * quote-heavy doc duplicates body bytes once per nesting level)
             * PLUS headroom for ~one inline node AND one block node per 2
             * source bytes. Otherwise a dense doc overflows the arena mid-parse
             * and md_parse silently drops blocks (and everything after them).
             * The arena is demand-paged (mmap) so the larger reservation costs
             * no physical RAM until actually touched. */
            usize est = (usize)fb->view_size * 8
                      + ((usize)fb->view_size / 2) * (sizeof(MdInline) + sizeof(MdBlock));
            if (est < KB(64))   est = KB(64);
            if (est > MB(192))  est = MB(192);
            fb->md_arena = arena_create(est);
            fb->md_doc = md_parse(&fb->md_arena, (const u8 *)fb->view_content,
                                  fb->view_size, NULL);
            fb->md_images = md_image_cache_create();

            /* Capture base_dir for image-path resolution. The slice in MdDoc
             * borrows this pointer — store it inside the arena so it lives
             * with the doc. */
            if (fb->md_doc) {
                /* dirname of view_path */
                char tmp[FB_MAX_PATH];
                snprintf(tmp, sizeof tmp, "%s", fb->view_path);
                char *slash = strrchr(tmp, '/');
                if (slash) *slash = '\0';
                else snprintf(tmp, sizeof tmp, ".");
                usize bn = strlen(tmp) + 1;
                char *bd = arena_alloc(&fb->md_arena, bn);
                if (bd) { memcpy(bd, tmp, bn); fb->md_doc->base_dir = bd; }
            }
        }

        /* Establish a fresh undo baseline for the just-opened file. */
        fb_editor_reset_undo(fb);
        return true;
    }

    return false;
}

/* Re-parse the current (possibly edited) markdown buffer into a fresh MdDoc so
 * the rendered preview reflects edits made in write mode. Mirrors the parse the
 * open path does, but reads the live view_content/view_size. */
static void fb_md_reparse(FileBrowser *fb) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || !fb->view_content) return;
    if (fb->md_images) { md_image_cache_destroy(fb->md_images); fb->md_images = NULL; }
    if (fb->md_arena.base) { arena_destroy(&fb->md_arena); }
    fb->md_doc = NULL;
    /* The link/glyph/outline collectors hold url/text slices that point into
     * the arena just freed. Zero their counts now so no consumer (link/copy
     * hit-test, outline picker) dereferences a dangling pointer before the
     * next render repopulates them — important if md_parse below fails and the
     * rendered branch (which resets them) is skipped every frame. */
    fb->md_link_count = 0;
    fb->md_glyph_count = 0;
    fb->md_outline_count = 0;
    fb->md_task_count = 0;
    usize len = fb->view_size;
    usize est = len * 8 + (len / 2) * (sizeof(MdInline) + sizeof(MdBlock));
    if (est < KB(64))  est = KB(64);
    if (est > MB(192)) est = MB(192);
    fb->md_arena = arena_create(est);
    fb->md_doc = md_parse(&fb->md_arena, (const u8 *)fb->view_content, len, NULL);
    fb->md_images = md_image_cache_create();
    if (fb->md_doc) {
        char tmp[FB_MAX_PATH];
        snprintf(tmp, sizeof tmp, "%s", fb->view_path);
        char *slash = strrchr(tmp, '/');
        if (slash) *slash = '\0'; else snprintf(tmp, sizeof tmp, ".");
        usize bn = strlen(tmp) + 1;
        char *bd = arena_alloc(&fb->md_arena, bn);
        if (bd) { memcpy(bd, tmp, bn); fb->md_doc->base_dir = bd; }
    }
}

/* Toggle a markdown viewer between Read (rendered preview) and Edit (raw,
 * editable) modes — a two-pane preview/edit split. Entering Edit turns
 * on the text editor; leaving it re-parses the buffer so the preview is fresh. */
void fb_md_toggle_edit(FileBrowser *fb) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN) return;
    if (fb->md_raw_mode) {
        /* Edit → Read */
        fb->md_raw_mode = false;
        fb->editor_mode = false;
        fb_md_reparse(fb);
    } else {
        /* Read → Edit */
        fb->md_raw_mode = true;
        fb->editor_mode = true;
        fb->md_sel_active = false;   /* rendered-text selection is read-mode only */
        fb->cursor_line = 0;
        fb->cursor_col = 0;
        fb->view_scroll = 0;         /* the raw editor scrolls by line, not px */
        fb->view_scroll_px = 0.0f;
    }
}

/* True when a markdown viewer is in editable (write) mode. */
bool fb_md_edit_active(const FileBrowser *fb) {
    return fb && fb->view_mode == FVIEW_MARKDOWN && fb->md_raw_mode && fb->editor_mode;
}

/* Tear down the viewer. `keep_graph` is set ONLY by the paired return-to-graph
 * close sites (which call fb_reenter_graph right after); every other caller
 * uses fb_close_viewer (keep_graph=false), which frees the graph. graph_return
 * is always cleared here so it can never outlive a single note close. */
void fb_close_viewer_ex(FileBrowser *fb, bool keep_graph) {
    if (!keep_graph) fb_graph_build_reap(fb);  /* join an in-flight build before tearing down */
    free(fb->view_content);
    fb->view_content = NULL;
    fb->view_size = 0;
    fb->view_cap = 0;
    free(fb->view_spans);
    fb->view_spans = NULL;
    if (fb->md_images) { md_image_cache_destroy(fb->md_images); fb->md_images = NULL; }
    if (fb->md_arena.base) { arena_destroy(&fb->md_arena); }
    fb->md_doc = NULL;
    fb->md_raw_mode = false;
    fb->editor_mode = false;   /* leaving the viewer also leaves edit mode */
    /* The doc backing any active selection is gone; without this the next
     * Cmd+C (after the viewer reopens on a non-Markdown file or the user is
     * back in the file list) still routes through the MD copy path. */
    fb->md_sel_active = false;
    /* Release the per-render scratch buffers too. They are lazily
     * reallocated on the next MD render and the viewer-close path is the
     * earliest point at which we know the user has dismissed the doc —
     * leaving them pinned across closes turns every tab that ever showed
     * markdown into a permanent ~166 KiB hold on the heap. */
    free(fb->md_glyph_rects);
    fb->md_glyph_rects = NULL;
    fb->md_glyph_count = 0;
    free(fb->md_link_rects);
    fb->md_link_rects = NULL;
    fb->md_link_count = 0;
    free(fb->md_task_rects);
    fb->md_task_rects = NULL;
    fb->md_task_count = 0;
    free(fb->md_outline);
    fb->md_outline = NULL;
    fb->md_outline_count = 0;
    /* Find/Replace state is per-document; drop it with the buffer. */
    free(fb->find_matches);
    fb->find_matches = NULL;
    fb->find_match_count = 0;
    fb->find_match_cap = 0;
    fb->find_active = false;
    fb->find_replace_mode = false;
    fb->find_query_len = 0; fb->find_query[0] = '\0';
    fb->find_replace_len = 0; fb->find_replace[0] = '\0';
    fb->find_current = -1;
    free(fb->ac_items);
    fb->ac_items = NULL;
    fb->ac_count = fb->ac_cap = 0;
    fb->ac_active = false;
    if (fb->view_image_rgba) {
        stbi_image_free(fb->view_image_rgba);
        fb->view_image_rgba = NULL;
    }
    free(fb->view_image_frame_delays);
    fb->view_image_frame_delays = NULL;
    fb->view_image_frame_count  = 0;
    fb->view_image_frame_index  = 0;
    fb->view_image_is_animated  = false;
    fb->view_image_w = 0;
    fb->view_image_h = 0;
    fb->view_image_zoom = 0.0f;
    fb->view_image_pan_x = 0.0f;
    fb->view_image_pan_y = 0.0f;
    /* Preserve the graph (and its pan/zoom/layout) ONLY for the paired
     * return-to-graph close; every other caller frees it. graph_return is
     * cleared unconditionally so a stale flag can't leak the graph or teleport
     * a later Esc into a dead graph. */
    if (!keep_graph) {
        if (fb->graph) { md_graph_free(fb->graph); fb->graph = NULL; }
        fb->graph_fitted    = false;
        fb->graph_hover     = -1;
        fb->graph_focus_node = -1;
        fb->graph_focus_t   = 0.0f;
        fb->graph_drag_node = -1;
        fb->graph_panning   = false;
    }
    fb->graph_return = false;
    fb->view_mode = FVIEW_NONE;
    fb->view_path[0] = '\0';

    /* Drop all undo state with the closed buffer. */
    fb_editor_reset_undo(fb);

    /* If the list was narrowed to .md-only for graph mode, repopulate the full
     * folder listing now that the graph/viewer is closed. Skipped on the
     * keep-graph path — fb_reenter_graph rebuilds the notes-only list, so the
     * scan here would be wasted. fb_navigate clears the flag on success; only
     * force it false if the reload can't run. */
    if (fb->md_only_entries && !keep_graph) {
        if (!(fb->source == FB_SOURCE_LOCAL && fb->cwd[0] && fb_navigate(fb, fb->cwd)))
            fb->md_only_entries = false;
    }
}

/* Default close: free the graph and any md-only narrowing. The keep-graph
 * variant above is for the paired return-to-graph close only. */
void fb_close_viewer(FileBrowser *fb) {
    fb_close_viewer_ex(fb, false);
}

/* =========================================================================
 * Rendering — Sidebar file list
 * ========================================================================= */

/* UI text rendering with explicit step size (0 = use font cell_width) */
static f32 g_ui_step = 0;

static void draw_text_clipped(Renderer *r, const char *text, f32 x, f32 y,
                               Color fg, f32 max_w) {
    if (!text) return;
    /* Pre-rasterize any uncached non-ASCII codepoints synchronously.
     * The async path returns a space placeholder that only resolves on
     * the next frame, but the UI is event-driven — without a follow-up
     * event the file browser keeps re-rendering the placeholder and
     * Turkish/Cyrillic/CJK filenames look like they have empty cells. */
    font_warm_text_glyphs(&r->font, text);

    f32 step = g_ui_step > 0 ? g_ui_step : r->font.cell_width;
    f32 cx = x;
    const u8 *p = (const u8 *)text;
    while (*p) {
        if (cx + step > x + max_w) break;
        u32 cp;
        u32 consumed = utf8_decode(p, 4, &cp);
        if (consumed == 0) { p++; continue; }
        if (cp >= 32) {
            renderer_push_glyph(r, cx, y, cp, fg);
            cx += step;
        }
        p += consumed;
    }
}

static bool draw_fb_asset_icon(Renderer *r, FbAssetIconKind kind,
                               f32 x, f32 y, f32 size) {
    i32 iw = 0, ih = 0;
    const u8 *px = fb_asset_icon_rgba(kind, &iw, &ih);
    const void *key = fb_asset_icon_cache_key(kind);
    if (!px || !key || iw <= 0 || ih <= 0) return false;
    renderer_draw_image_cached(r, key, 0, px, iw, ih,
                               floorf(x), floorf(y), floorf(size), floorf(size));
    return true;
}

void fb_render_sidebar(FileBrowser *fb, Renderer *r, f32 x, f32 y, f32 w, f32 h,
                       const Color *bg, const Color *fg, const Color *sel,
                       const Color *dim, const Color *accent, f32 dpi,
                       f32 hover_x, f32 hover_y) {
    if (!fb->open || !fb->entries) return;
    /* Use fixed UI-size step for sidebar text */
    f32 cw = 8.0f * dpi;
    f32 ch = 16.0f * dpi;
    g_ui_step = cw;
    renderer_set_ui_scale(r, cw, ch);

    /* Background — solid, no transparency */
    renderer_draw_rect(r, x, y, w, h, *bg);

    /* Crisp separator. Keep the sidebar planar; no soft outward shadow. */
    Color border_clr = {dim->r + 0.02f, dim->g + 0.02f, dim->b + 0.02f, 0.62f};
    f32 chrome_line = fmaxf(1.0f, CHROME_LINE_PT * dpi);
    renderer_draw_rect(r, x + w - chrome_line, y, chrome_line, h, border_clr);

    /* CWD header — minimal, with path. Inherit alpha from the passed bg
     * so the header fades together with the sidebar body when window
     * opacity drops below 1.0 (otherwise the strip stays fully opaque
     * over a translucent body and looks detached). */
    f32 hdr_h = TOOLBAR_HEIGHT_PT * dpi;
    Color hdr_bg = {bg->r + 0.02f, bg->g + 0.02f, bg->b + 0.02f, bg->a};
    renderer_draw_rect(r, x, y, w, hdr_h, hdr_bg);
    Color hdr_sep = border_clr; hdr_sep.a *= bg->a;
    renderer_draw_rect(r, x, y + hdr_h - 1, w, 1, hdr_sep);

    /* CWD path */
    const char *cwd = fb->cwd;
    const char *home = getenv("HOME");
    char short_cwd[256];
    if (home && strncmp(cwd, home, strlen(home)) == 0)
        snprintf(short_cwd, sizeof(short_cwd), "~%s", cwd + strlen(home));
    else
        snprintf(short_cwd, sizeof(short_cwd), "%s", cwd);

    f32 hdr_pad = 16.0f * dpi;

    /* Header action icons — up / refresh / close, right-aligned. The 3*cw
     * click zones are unchanged (mirrored by the HIT_SIDEBAR handler in
     * src/main.c); only the visuals gain a hover chip + brighter icon. */
    f32 btn_w  = 3.0f * cw;
    f32 close_x   = x + w - hdr_pad - btn_w;
    f32 refresh_x = close_x - btn_w;
    f32 up_x      = refresh_x - btn_w;

    Color icon_idle  = { fg->r * 0.78f, fg->g * 0.78f, fg->b * 0.78f, 0.95f };
    Color icon_hot   = { fg->r, fg->g, fg->b, 1.0f };               /* hovered up/refresh */
    Color icon_close = { 0.86f, 0.45f, 0.45f, 0.95f };              /* reddish close */
    Color icon_close_hot = { 0.98f, 0.52f, 0.52f, 1.0f };

    /* Path: leave room for the three buttons on the right */
    Color path_clr = {fg->r * 0.82f, fg->g * 0.82f, fg->b * 0.82f, 1.0f};
    draw_text_clipped(r, short_cwd, x + hdr_pad, y + (hdr_h - ch) / 2,
                      path_clr, up_x - (x + hdr_pad) - 4 * dpi);

    f32 hdr_icon = 15.0f * dpi;
    f32 hdr_icon_y = y + (hdr_h - hdr_icon) * 0.5f;
    bool in_hdr = (hover_y >= y && hover_y < y + hdr_h);

    /* Hover chip geometry: a rounded square inset within each 3*cw zone. */
    f32 chip_h  = hdr_h - 12.0f * dpi;
    if (chip_h < 8.0f * dpi) chip_h = 8.0f * dpi;
    f32 chip_w  = btn_w - 4.0f * dpi;
    f32 chip_yy = y + (hdr_h - chip_h) * 0.5f;
    f32 chip_r  = 7.0f * dpi;

    struct { f32 bx; i32 icon; bool is_close; } hbtns[3] = {
        { up_x,      ICON_UP,      false },
        { refresh_x, ICON_REFRESH, false },
        { close_x,   ICON_CLOSE,   true  },
    };
    /* Pass 1: hover chips (rrects), flushed before the icons. */
    for (i32 b = 0; b < 3; b++) {
        bool hot = in_hdr && hover_x >= hbtns[b].bx && hover_x < hbtns[b].bx + btn_w;
        if (!hot) continue;
        Color chip = hbtns[b].is_close
            ? (Color){ 0.86f, 0.42f, 0.42f, 0.18f }
            : (Color){ fg->r, fg->g, fg->b, 0.12f };
        renderer_draw_rrect_simple(r, hbtns[b].bx + (btn_w - chip_w) * 0.5f,
                                   chip_yy, chip_w, chip_h, chip, chip_r);
    }
    renderer_flush_rrects(r);
    /* Pass 2: icons. */
    for (i32 b = 0; b < 3; b++) {
        bool hot = in_hdr && hover_x >= hbtns[b].bx && hover_x < hbtns[b].bx + btn_w;
        Color ic = hbtns[b].is_close ? (hot ? icon_close_hot : icon_close)
                                     : (hot ? icon_hot : icon_idle);
        icon_draw(r, hbtns[b].icon,
                  hbtns[b].bx + (btn_w - hdr_icon) * 0.5f, hdr_icon_y,
                  hdr_icon, ic);
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);

    /* File entries -- virtualized: only render visible entries */
    f32 entry_h = ch + 6;  /* slightly taller for breathing room */
    f32 list_y = y + hdr_h;
    f32 viewport_h = h - hdr_h;
    f32 indent = 12 * dpi; /* left indent */

    /* Update content/viewport metrics for scroll clamping */
    fb->content_height = (f32)fb->entry_count * entry_h;
    fb->viewport_height = viewport_h;

    /* Sync pixel scroll offset from legacy integer offset (first-time migration) */
    if (fb->scroll_offset_px == 0.0f && fb->scroll_offset > 0) {
        fb->scroll_offset_px = (f32)fb->scroll_offset * entry_h;
    }

    /* Clamp scroll offset */
    f32 max_scroll = fb->content_height - viewport_h;
    if (max_scroll < 0) max_scroll = 0;
    if (fb->scroll_offset_px < 0) fb->scroll_offset_px = 0;
    if (fb->scroll_offset_px > max_scroll) fb->scroll_offset_px = max_scroll;

    /* Compute visible entry range (virtual scrolling) */
    i32 first_visible = (i32)(fb->scroll_offset_px / entry_h);
    i32 last_visible  = first_visible + (i32)(viewport_h / entry_h) + 2; /* +2 for partial entries */
    if (first_visible < 0) first_visible = 0;
    if (last_visible > fb->entry_count) last_visible = fb->entry_count;

    /* Partial-entry Y offset for smooth sub-entry scrolling */
    f32 sub_offset = fb->scroll_offset_px - (f32)first_visible * entry_h;

    for (i32 i = first_visible; i < last_visible; i++) {
        FileEntry *e = &fb->entries[i];
        f32 ey = list_y + (f32)(i - first_visible) * entry_h - sub_offset;

        /* Cull entries completely outside viewport */
        if (ey + entry_h < list_y || ey > list_y + viewport_h) continue;

        /* Selection highlight: multi-selected entries get the base fill,
         * the "primary" (last-clicked) also gets the left accent bar so the
         * cursor-style focus ring remains visible in a sea of selections. */
        bool sel_hit = fb_sel_has(fb, i) || i == fb->selected;
        if (sel_hit) {
            Color sel_bg = {sel->r, sel->g, sel->b, 0.32f};
            renderer_draw_rect(r, x, ey, w - chrome_line, entry_h, sel_bg);
        }
        if (i == fb->selected) {
            renderer_draw_rect(r, x, ey, fmaxf(2.0f, 2.0f * chrome_line), entry_h,
                              (Color){0.4f, 0.65f, 0.95f, 1.0f});
        }

        /* File-type icon. Hidden files use the dimmed glyph fallback instead
         * of bright PNG assets so the row keeps one visual weight. ".." gets
         * the same SF-symbol up arrow as the header action. */
        f32 icon_size = 14.0f * dpi;
        f32 icon_x  = x + indent;
        f32 icon_y  = ey + (entry_h - icon_size) / 2;
        f32 glyph_icon_y = ey + (entry_h - ch) / 2;
        f32 icon_slot = icon_size + 6 * dpi;  /* icon width + gap before the name */
        bool is_up = (strcmp(e->name, "..") == 0);

        if (is_up) {
            Color up_clr = {0.60f, 0.78f, 0.98f, 1.0f};
            icon_draw(r, ICON_UP, icon_x, icon_y, icon_size, up_clr);
        } else {
            FbIcon ic = fb_icon_for(e->name, e->is_dir);
            bool subdued = (e->name[0] == '.');
            if (ic.asset != FB_ASSET_ICON_NONE &&
                (!subdued || !ic.codepoint) &&
                draw_fb_asset_icon(r, ic.asset, icon_x, icon_y, icon_size)) {
                /* preferred high-DPI PNG path */
            } else if (ic.codepoint) {
                Color ic_c = ic.color;
                if (subdued) {
                    ic_c = (Color){dim->r, dim->g, dim->b, 0.72f};
                }
                renderer_push_glyph(r, icon_x, glyph_icon_y, ic.codepoint, ic_c);
            }
        }

        /* Name -- with proper font color */
        f32 name_x = icon_x + icon_slot;
        Color name_clr = e->is_dir ? *accent : *fg;
        if (e->name[0] == '.') name_clr = (Color){dim->r, dim->g, dim->b, 0.72f};
        /* ".." stays the same accent colour as dirs so it reads as nav, not dim */
        if (is_up)            name_clr = *accent;

        f32 max_name_w = w - (name_x - x) - 12 - 8 * cw; /* leave room for size */
        const char *display = is_up ? ".. (up)" : e->name;
        draw_text_clipped(r, display, name_x, ey + (entry_h - ch) / 2, name_clr, max_name_w);

        /* Size -- right-aligned, dimmed (also for 0-byte / stat-failed files) */
        if (!e->is_dir) {
            const char *sz = fb_format_size(e->size);
            f32 sz_w = (f32)strlen(sz) * cw;
            Color sz_clr = {dim->r, dim->g, dim->b, 0.5f};
            draw_text_clipped(r, sz, x + w - sz_w - 10, ey + (entry_h - ch) / 2, sz_clr, sz_w);
        }
    }

    /* Scrollbar -- only draw when content exceeds viewport */
    if (fb->content_height > viewport_h && viewport_h > 0) {
        f32 bar_w = 4 * dpi;
        f32 bar_x = x + w - bar_w - 2;
        f32 thumb_h = (viewport_h / fb->content_height) * viewport_h;
        if (thumb_h < 20 * dpi) thumb_h = 20 * dpi;  /* minimum thumb size */
        f32 scroll_range = viewport_h - thumb_h;
        f32 thumb_y = list_y + (fb->scroll_offset_px / max_scroll) * scroll_range;

        /* Track + thumb — also follow the sidebar's window opacity so the
         * scrollbar doesn't pop opaque over a translucent panel. */
        Color track_clr = {bg->r + 0.03f, bg->g + 0.03f, bg->b + 0.03f, 0.3f * bg->a};
        renderer_draw_rect(r, bar_x, list_y, bar_w, viewport_h, track_clr);
        Color thumb_clr = {fg->r * 0.5f, fg->g * 0.5f, fg->b * 0.5f, 0.4f * bg->a};
        renderer_draw_rect(r, bar_x, thumb_y, bar_w, thumb_h, thumb_clr);
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r);
    g_ui_step = 0;
}

/* =========================================================================
 * Rendering — File viewer (markdown + code)
 * ========================================================================= */

static void fb_md_draw_selection(FileBrowser *fb, Renderer *r, const Theme *theme);

/* =========================================================================
 * Graph view (FVIEW_GRAPH)
 * ========================================================================= */

bool fb_graph_active(const FileBrowser *fb) {
    return fb && fb->view_mode == FVIEW_GRAPH && fb->graph;
}

/* Rebuild the list sidebar from the graph's note set so it mirrors the graph
 * exactly — every .md node (recursively, across subfolders), shown as a path
 * relative to the scoped folder. Opening an entry resolves cwd + relative-name,
 * so notes in subfolders open correctly. The full folder listing is restored
 * by fb_close_viewer via the md_only_entries flag. */
static void fb_set_entries_from_graph(FileBrowser *fb) {
    if (!fb) return;
    fb_entries_reset(fb);
    fb->selected = -1;
    fb_sel_clear(fb);
    fb->scroll_offset = 0;
    fb->scroll_offset_px = 0.0f;
    fb->md_only_entries = true;
    if (!fb->graph) return;   /* empty folder → empty notes list */
    if (!fb->entries) {
        fb->entry_cap = 64;
        fb->entries = calloc((usize)fb->entry_cap, sizeof(FileEntry));
        if (!fb->entries) return;
    }
    usize rootlen = strlen(fb->cwd);
    i32 nc = md_graph_node_count(fb->graph);
    for (i32 i = 0; i < nc && fb->entry_count < FB_MAX_ENTRIES; i++) {
        if (md_graph_node_is_tag(fb->graph, i)) continue;
        const char *path = md_graph_node_path(fb->graph, i);
        if (!path || !path[0]) continue;
        const char *rel = path;
        if (rootlen && strncmp(path, fb->cwd, rootlen) == 0) {
            rel = path + rootlen; while (*rel == '/') rel++;
        }
        if (!*rel) continue;
        if (fb->entry_count >= fb->entry_cap) {
            i32 newcap = fb->entry_cap * 2;
            if (newcap > FB_MAX_ENTRIES) newcap = FB_MAX_ENTRIES;
            FileEntry *grown = realloc(fb->entries, (usize)newcap * sizeof(FileEntry));
            if (!grown) break;
            fb->entries = grown; fb->entry_cap = newcap;
        }
        FileEntry *e = &fb->entries[fb->entry_count];
        memset(e, 0, sizeof *e);
        fb_entry_set_name(e, rel);
        e->is_dir = false;
        struct stat st;
        if (stat(path, &st) == 0) { e->size = (u64)st.st_size; e->mtime = (u64)st.st_mtime; }
        fb->entry_count++;
    }
    if (fb->entry_count > 1)
        qsort(fb->entries, (usize)fb->entry_count, sizeof(FileEntry), entry_cmp);
}

/* Background scan: md_graph_build walks the folder tree + parses every .md.
 * Runs off the main thread so a slow/network volume (or the macOS file-access
 * permission prompt) can't freeze the UI. */
static void *fb_graph_build_worker(void *arg) {
#ifdef __APPLE__
    pthread_setname_np("liu-graph-build");
#else
    pthread_setname_np(pthread_self(), "liu-graph-build");
#endif
    FileBrowser *fb = (FileBrowser *)arg;
    MdGraph *g = md_graph_build(fb->graph_build_dir);
    fb->graph_pending = g;                          /* published before the flag */
    atomic_store(&fb->graph_build_done, true);
    return NULL;
}

/* Join any in-flight build and discard an unconsumed result (cleanup / replace). */
static void fb_graph_build_reap(FileBrowser *fb) {
    if (!fb || !fb->graph_building) return;
    pthread_join(fb->graph_build_thread, NULL);
    fb->graph_building = false;
    if (fb->graph_pending) { md_graph_free(fb->graph_pending); fb->graph_pending = NULL; }
}

/* Kick off a background build of `dir`; the graph view shows a spinner until
 * fb_render_graph's poll swaps the result in. */
static void fb_graph_build_start(FileBrowser *fb, const char *dir) {
    fb_graph_build_reap(fb);                        /* finish/replace any prior build */
    if (fb->graph) { md_graph_free(fb->graph); fb->graph = NULL; }
    snprintf(fb->graph_build_dir, sizeof fb->graph_build_dir, "%s", dir);
    fb->graph_pending = NULL;
    atomic_store(&fb->graph_build_done, false);
    if (pthread_create(&fb->graph_build_thread, NULL, fb_graph_build_worker, fb) == 0) {
        fb->graph_building = true;
    } else {                                        /* rare: fall back to synchronous */
        /* Build synchronously and consume the result HERE. Do NOT set
         * graph_building: no thread was created, so the next fb_render_graph
         * poll / fb_graph_build_reap would pthread_join a stale/never-created
         * graph_build_thread (POSIX UB — and on a warm handle, a second join of
         * an already-joined thread). The prior fb->graph was freed just above. */
        fb->graph = md_graph_build(dir);            /* may be NULL (no .md under dir) */
        fb->graph_pending = NULL;
        atomic_store(&fb->graph_build_done, false);
        fb->graph_building = false;
    }
}

/* Toggle the knowledge graph, building it from `root` (e.g. the notes Vault)
 * rather than the file browser's cwd. NULL/empty root falls back to fb->cwd. */
bool fb_toggle_graph_root(FileBrowser *fb, const char *root) {
    if (!fb) return false;
    if (fb->view_mode == FVIEW_GRAPH) { fb_close_viewer(fb); return false; }
    if (!root || !root[0]) root = fb->cwd;
    /* Leaving any open file viewer first. */
    if (fb->view_mode != FVIEW_NONE) fb_close_viewer(fb);
    fb->graph_pan_x = fb->graph_pan_y = 0.0f;
    fb->graph_zoom = 1.0f;
    fb->graph_fitted = false;
    fb->graph_user_zoomed = false;
    fb->graph_fit_zoom = 0.0f;
    fb->graph_raster_gen = 1;
    fb->graph_hover = fb->graph_drag_node = -1;
    fb->graph_panning = false;
    fb->graph_moved = false;
    /* Open with a narrow file-list sidebar so the graph fills the window, even
     * if this tab's viewer divider was previously dragged wide for a file. The
     * tab layer consumes this on the next render (it owns fb_viewer_ratio). */
    fb->reset_split_narrow = true;
    fb->graph_return = false;   /* entering the graph view fresh */
    snprintf(fb->graph_root, sizeof fb->graph_root, "%s", root);
    fb->view_mode = FVIEW_GRAPH;
    snprintf(fb->view_path, sizeof fb->view_path, "%s", root);
    fb->entry_count = 0;               /* sidebar fills once the build completes */
    fb_graph_build_start(fb, root);    /* scan off the main thread */
    return true;
}

/* Graph the file browser's current folder (the original cwd-rooted behaviour). */
bool fb_toggle_graph(FileBrowser *fb) {
    return fb_toggle_graph_root(fb, fb ? fb->cwd : NULL);
}

/* Re-scope the knowledge graph to `dir` (recursively), keeping graph mode
 * active. Used to filter a codebase's many .md files down to one folder by
 * navigating into it from the graph's list sidebar. Returns true if `dir`
 * yielded any .md files; on an empty folder the graph is cleared (the viewer
 * shows an empty state) so the user can navigate back up. */
bool fb_graph_rescope(FileBrowser *fb, const char *dir) {
    if (!fb || !dir || !dir[0]) return false;
    fb->graph_pan_x = fb->graph_pan_y = 0.0f;
    fb->graph_zoom = 1.0f;
    fb->graph_fitted = false;
    fb->graph_user_zoomed = false;
    fb->graph_fit_zoom = 0.0f;
    fb->graph_raster_gen = 1;
    fb->graph_hover = fb->graph_drag_node = -1;
    fb->graph_panning = false;
    fb->graph_moved = false;
    fb->graph_next_frame_at = 0.0;
    fb->graph_return = false;   /* entering the graph view fresh */
    /* Track the live graph's root so return-from-note restores THIS scope, not
     * the folder the graph was first opened on. */
    snprintf(fb->graph_root, sizeof fb->graph_root, "%s", dir);
    fb->view_mode = FVIEW_GRAPH;
    snprintf(fb->view_path, sizeof fb->view_path, "%s", dir);
    fb->entry_count = 0;               /* sidebar fills once the build completes */
    fb_graph_build_start(fb, dir);     /* scan off the main thread */
    return true;
}

/* True when the viewer is showing a note opened from the graph and the graph is
 * still alive — i.e. closing the note should return to the graph. */
bool fb_graph_can_return(const FileBrowser *fb) {
    return fb && fb->graph_return && fb->graph &&
           fb->view_mode != FVIEW_GRAPH;
}

/* Re-enter the preserved graph after a note opened from it is closed. The graph
 * object (and its pan/zoom/settled layout) was kept alive across the note, so
 * this resumes exactly where the user left off — only the list sidebar and the
 * scoped cwd/path are restored. Call AFTER fb_close_viewer (which, with
 * graph_return set, left the graph intact). */
void fb_reenter_graph(FileBrowser *fb) {
    if (!fb || !fb->graph) { if (fb) fb->graph_return = false; return; }
    fb->graph_return = false;
    if (fb->graph_root[0]) {
        snprintf(fb->cwd, sizeof fb->cwd, "%s", fb->graph_root);
        snprintf(fb->view_path, sizeof fb->view_path, "%s", fb->graph_root);
    }
    fb->view_mode = FVIEW_GRAPH;
    fb->graph_next_frame_at = 0.0;
    fb_set_entries_from_graph(fb);   /* restore the notes-only list sidebar */
}

/* Distinct, evenly-spread colour per folder cluster (golden-angle hue), nudged
 * toward the theme node colour so it still sits in the palette. gi<0 → base. */
/* Small color helpers for the graph palette (theme-derived, polarity-aware). */
static inline Color gcol_blend(Color a, Color b, f32 t) {
    return (Color){ a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                    a.b + (b.b - a.b) * t, 1.0f };
}
static inline Color gcol_lift(Color c, f32 t) {   /* push toward white by t */
    return (Color){ c.r + (1.0f - c.r) * t, c.g + (1.0f - c.g) * t,
                    c.b + (1.0f - c.b) * t, 1.0f };
}

/* Distinct per-component cluster hue (golden-angle rotation), blended toward
 * the node base. `light` themes use richer/darker hues so they contrast on a
 * white canvas; dark themes keep the brighter tone. */
static Color graph_group_color(i32 gi, Color base, bool light) {
    if (gi < 0) return base;
    f32 h = fmodf((f32)gi * 137.508f, 360.0f) / 60.0f;   /* 0..6 hue sector */
    f32 s = light ? 0.62f : 0.50f, v = light ? 0.70f : 0.95f;
    f32 mix = light ? 0.35f : 0.20f;                     /* base-blend weight */
    i32 hi = (i32)h; f32 f = h - (f32)hi;
    f32 p = v * (1.0f - s), q = v * (1.0f - s * f), t = v * (1.0f - s * (1.0f - f));
    f32 rr, gg, bb;
    switch (hi % 6) {
        case 0: rr=v; gg=t; bb=p; break;
        case 1: rr=q; gg=v; bb=p; break;
        case 2: rr=p; gg=v; bb=t; break;
        case 3: rr=p; gg=q; bb=v; break;
        case 4: rr=t; gg=p; bb=v; break;
        default:rr=v; gg=p; bb=q; break;
    }
    return (Color){ rr*(1.0f-mix) + base.r*mix,
                    gg*(1.0f-mix) + base.g*mix,
                    bb*(1.0f-mix) + base.b*mix, 1.0f };
}

/* Coarse screen-space occupancy grid for label decluttering. All coordinates
 * are VIEW-RELATIVE (md_graph_node_screen returns sx/sy without the +x,+y view
 * origin — that's added only when pushing glyphs). Greedy: the highest-priority
 * labels are placed first and reserve their box; lower ones yield on collision. */
typedef struct {
    u8  *cells;          /* cols*rows, 0=free 1=taken */
    i32  cols, rows;
    f32  cell;           /* px per cell */
    f32  w, h;           /* view rect size */
} LblGrid;

static inline i32 lbl_clampi(i32 v, i32 lo, i32 hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}
/* View-relative AABB → clamped cell spans; false if fully off the grid. */
static bool lbl_span(const LblGrid *g, f32 x0, f32 y0, f32 x1, f32 y1,
                     i32 *c0, i32 *c1, i32 *r0, i32 *r1) {
    if (x1 < 0 || y1 < 0 || x0 > g->w || y0 > g->h) return false;
    *c0 = lbl_clampi((i32)floorf(x0 / g->cell), 0, g->cols - 1);
    *c1 = lbl_clampi((i32)floorf(x1 / g->cell), 0, g->cols - 1);
    *r0 = lbl_clampi((i32)floorf(y0 / g->cell), 0, g->rows - 1);
    *r1 = lbl_clampi((i32)floorf(y1 / g->cell), 0, g->rows - 1);
    return true;
}
static bool lbl_box_free(const LblGrid *g, f32 x0, f32 y0, f32 x1, f32 y1) {
    i32 c0, c1, r0, r1;
    if (!lbl_span(g, x0, y0, x1, y1, &c0, &c1, &r0, &r1)) return true; /* off-grid: no conflict */
    for (i32 rr = r0; rr <= r1; rr++)
        for (i32 cc = c0; cc <= c1; cc++)
            if (g->cells[rr * g->cols + cc]) return false;
    return true;
}
static void lbl_box_mark(LblGrid *g, f32 x0, f32 y0, f32 x1, f32 y1) {
    i32 c0, c1, r0, r1;
    if (!lbl_span(g, x0, y0, x1, y1, &c0, &c1, &r0, &r1)) return;
    for (i32 rr = r0; rr <= r1; rr++)
        for (i32 cc = c0; cc <= c1; cc++)
            g->cells[rr * g->cols + cc] = 1;
}
/* Descending sort of packed (priority<<20 | index) keys. */
static int lbl_key_desc(const void *a, const void *b) {
    u64 ka = *(const u64 *)a, kb = *(const u64 *)b;
    return (ka < kb) - (ka > kb);
}

/* Apply sane defaults once (all multipliers = 1, filters on, uniform
 * colour for the default graph view). */
static void graph_settings_ensure(FileBrowser *fb) {
    if (fb->gset_inited) return;
    fb->gset.center = fb->gset.repel = fb->gset.link = fb->gset.linkdist = 1.0f;
    fb->gset.node_size = fb->gset.link_thickness = fb->gset.text_fade = 1.0f;
    fb->gset.show_arrows  = false;
    fb->gset.show_orphans = true;
    fb->gset.show_tags    = true;
    fb->gset.color_mode   = 0;            /* uniform, single-color mode */
    fb->graph_drag_slider = -1;
    fb->gset_dirty = true;                /* apply defaults to the sim on first frame */
    fb->gset_inited = true;
}

/* A node hidden by the active filters (orphans toggle / tags toggle). */
static bool graph_node_hidden(const FileBrowser *fb, i32 i) {
    if (md_graph_node_is_tag(fb->graph, i)) return !fb->gset.show_tags;
    if (!fb->gset.show_orphans && md_graph_node_degree(fb->graph, i) <= 0) return true;
    return false;
}

/* Hit test that ignores filtered-out nodes. The renderer skips hidden orphans
 * and tags, so they must not be hoverable / draggable / openable either. */
static i32 graph_hit_visible(const FileBrowser *fb, f32 rx, f32 ry, f32 w, f32 h) {
    i32 node = md_graph_hit(fb->graph, rx, ry, (i32)w, (i32)h,
                            fb->graph_pan_x, fb->graph_pan_y, fb->graph_zoom);
    return (node >= 0 && graph_node_hidden(fb, node)) ? -1 : node;
}

/* ---- Graph controls panel (Forces / Display / Filters) ---------------- */

typedef enum { GC_SLIDER, GC_TOGGLE } GCKind;
typedef struct {
    GCKind      kind;
    const char *label;
    f32         minv, maxv;
    f32        *fval;        /* slider value */
    bool       *bval;        /* toggle bool, or NULL when ival is used */
    i32        *ival;        /* toggle 0/1 (colour mode) */
    bool        is_force;    /* force slider → reheat sim on change */
    f32         ry, rh;      /* row top + height (x/width come from the panel) */
} GCtl;

#define GRAPH_PANEL_CTLS 11

/* Compute the gear button + panel rect + each control's row. Render and the
 * input handlers both call this so their geometry can never drift. */
static i32 graph_panel_layout(FileBrowser *fb, f32 dpi, GCtl *out,
                              f32 *gx, f32 *gy, f32 *gs,
                              f32 *px, f32 *py, f32 *pw, f32 *ph,
                              f32 *cx, f32 *cw) {
    f32 vx = fb->graph_view_x, vy = fb->graph_view_y, vw = fb->graph_view_w;
    f32 margin = 10.0f*dpi, gear = 26.0f*dpi, panw = 216.0f*dpi, pad = 12.0f*dpi;
    /* The viewer reserves the top VIEWER_TITLE_H strip for an (invisible-in-graph)
     * title bar + close button — keep the gear BELOW it, otherwise the click is
     * hit-tested as the viewer close button and shuts the graph instead. */
    f32 title_h = VIEWER_TITLE_H_PT * dpi;
    *gs = gear;
    *gx = vx + vw - margin - gear;  *gy = vy + title_h + 8.0f*dpi;
    *px = vx + vw - margin - panw;  *py = *gy + gear + 6.0f*dpi;
    *pw = panw;  *cx = *px + pad;  *cw = panw - 2.0f*pad;

    f32 header_h = 22.0f*dpi, slider_h = 30.0f*dpi, toggle_h = 24.0f*dpi;
    f32 yy = *py + pad;
    struct { GCKind k; const char *l; f32 a, b; f32 *f; bool *bb; i32 *ii; bool force; }
    d[GRAPH_PANEL_CTLS] = {
        {GC_SLIDER, "Center",     0, 2, &fb->gset.center,         NULL, NULL, true},
        {GC_SLIDER, "Repel",      0, 2, &fb->gset.repel,          NULL, NULL, true},
        {GC_SLIDER, "Link",       0, 2, &fb->gset.link,           NULL, NULL, true},
        {GC_SLIDER, "Link dist",  0, 2, &fb->gset.linkdist,       NULL, NULL, true},
        {GC_SLIDER, "Node size",  0, 2, &fb->gset.node_size,      NULL, NULL, true},
        {GC_SLIDER, "Link width", 0, 3, &fb->gset.link_thickness, NULL, NULL, false},
        {GC_SLIDER, "Text fade",  0, 2, &fb->gset.text_fade,      NULL, NULL, false},
        {GC_TOGGLE, "Arrows",     0, 0, NULL, &fb->gset.show_arrows,  NULL, false},
        {GC_TOGGLE, "Orphans",    0, 0, NULL, &fb->gset.show_orphans, NULL, false},
        {GC_TOGGLE, "Tags",       0, 0, NULL, &fb->gset.show_tags,    NULL, false},
        {GC_TOGGLE, "Clusters",   0, 0, NULL, NULL, &fb->gset.color_mode, false},
    };
    for (i32 i = 0; i < GRAPH_PANEL_CTLS; i++) {
        if (i == 0 || i == 4 || i == 8) yy += header_h;  /* section gap */
        out[i].kind = d[i].k; out[i].label = d[i].l;
        out[i].minv = d[i].a; out[i].maxv = d[i].b;
        out[i].fval = d[i].f; out[i].bval = d[i].bb; out[i].ival = d[i].ii;
        out[i].is_force = d[i].force;
        out[i].rh = (d[i].k == GC_SLIDER) ? slider_h : toggle_h;
        out[i].ry = yy; yy += out[i].rh;
    }
    *ph = (yy + pad) - *py;
    return GRAPH_PANEL_CTLS;
}

/* Slider track geometry inside a control row (shared render/hit). */
static void gc_track(const GCtl *c, f32 cx, f32 cw, f32 dpi,
                     f32 *tx, f32 *ty, f32 *tw, f32 *th) {
    *tx = cx; *tw = cw; *th = 4.0f*dpi; *ty = c->ry + c->rh - 11.0f*dpi;
}
/* Toggle checkbox geometry (right-aligned in the row). */
static void gc_box(const GCtl *c, f32 cx, f32 cw, f32 dpi,
                   f32 *bx, f32 *by, f32 *bs) {
    *bs = 16.0f*dpi; *bx = cx + cw - *bs; *by = c->ry + (c->rh - *bs)*0.5f;
}

/* Draw the gear button and, when expanded, the controls panel. */
static void graph_render_panel(FileBrowser *fb, Renderer *r, f32 dpi,
                               const Theme *theme) {
    GCtl c[GRAPH_PANEL_CTLS];
    f32 gx, gy, gs, px, py, pw, ph, cx, cw;
    graph_panel_layout(fb, dpi, c, &gx,&gy,&gs, &px,&py,&pw,&ph, &cx,&cw);

    Color fg     = theme ? theme->fg     : (Color){0.9f,0.9f,0.92f,1};
    Color dim    = (Color){fg.r, fg.g, fg.b, 0.55f};
    Color accent = theme ? theme->ansi[12] : (Color){0.40f,0.62f,1.0f,1};
    Color panelbg= theme ? theme->bg     : (Color){0.06f,0.06f,0.07f,1};
    Color border = theme ? theme->border : (Color){0.2f,0.2f,0.24f,1};
    border.a = border.a > 0.05f ? border.a : 1.0f;
    Color track  = (Color){fg.r, fg.g, fg.b, 0.14f};
    Color clear  = (Color){0,0,0,0};

    Color gbtn = fb->graph_settings_open ? accent : (Color){fg.r,fg.g,fg.b,0.10f};
    renderer_draw_rrect_simple(r, gx, gy, gs, gs, gbtn, 6.0f*dpi);
    renderer_flush_rrects(r);
    Color gico = fb->graph_settings_open ? (Color){0.05f,0.06f,0.08f,0.95f} : dim;
    f32 lx0 = gx + 6*dpi, lx1 = gx + gs - 6*dpi;
    f32 ly1 = gy + gs*0.40f, ly2 = gy + gs*0.62f;
    renderer_draw_rect(r, lx0, ly1, lx1-lx0, fmaxf(1.0f,1.5f*dpi), gico);
    renderer_draw_rect(r, lx0, ly2, lx1-lx0, fmaxf(1.0f,1.5f*dpi), gico);
    renderer_flush_rects(r);
    renderer_draw_rrect_simple(r, gx+gs*0.60f-2*dpi, ly1-2.0f*dpi, 4*dpi,4*dpi, gico, 2*dpi);
    renderer_draw_rrect_simple(r, gx+gs*0.36f-2*dpi, ly2-2.0f*dpi, 4*dpi,4*dpi, gico, 2*dpi);
    renderer_flush_rrects(r);

    if (!fb->graph_settings_open) return;

    renderer_draw_rrect_bordered(r, px, py, pw, ph, panelbg, border, fmaxf(1.0f,dpi),
                                 10*dpi,10*dpi,10*dpi,10*dpi, 18*dpi,0.30f,0,4*dpi);
    renderer_flush_rrects(r);

    for (i32 i = 0; i < GRAPH_PANEL_CTLS; i++) {
        if (c[i].kind == GC_SLIDER) {
            f32 tx,ty,tw,th; gc_track(&c[i], cx, cw, dpi, &tx,&ty,&tw,&th);
            f32 t = (*c[i].fval - c[i].minv) / (c[i].maxv - c[i].minv);
            if (t < 0) t = 0; if (t > 1) t = 1;
            renderer_draw_rrect_simple(r, tx, ty, tw, th, track, th*0.5f);
            if (t > 0) renderer_draw_rrect_simple(r, tx, ty, tw*t, th, accent, th*0.5f);
            f32 kr = 6.0f*dpi, kx = tx + tw*t;
            renderer_draw_rrect_simple(r, kx-kr, ty+th*0.5f-kr, kr*2, kr*2, fg, kr);
        } else {
            f32 bx,by,bs; gc_box(&c[i], cx, cw, dpi, &bx,&by,&bs);
            bool on = c[i].ival ? (*c[i].ival != 0) : (c[i].bval && *c[i].bval);
            if (on) renderer_draw_rrect_simple(r, bx,by,bs,bs, accent, 4*dpi);
            else    renderer_draw_rrect_bordered(r, bx,by,bs,bs, clear, border,
                                                 fmaxf(1.0f,dpi), 4*dpi,4*dpi,4*dpi,4*dpi, 0,0,0,0);
        }
    }
    renderer_flush_rrects(r);

    f32 tcw = 7.0f*dpi, tch = 13.0f*dpi;
    g_ui_step = tcw; renderer_set_ui_scale(r, tcw, tch);
    const char *secs[3] = { "FORCES", "DISPLAY", "FILTERS" };
    i32 secidx[3] = { 0, 4, 8 };
    for (int s = 0; s < 3; s++)
        draw_text_clipped(r, secs[s], cx, c[secidx[s]].ry - 17.0f*dpi, dim, cw);
    for (i32 i = 0; i < GRAPH_PANEL_CTLS; i++) {
        f32 ly = (c[i].kind == GC_SLIDER) ? c[i].ry
                                          : c[i].ry + (c[i].rh - tch)*0.5f;
        draw_text_clipped(r, c[i].label, cx, ly, fg, cw);
        if (c[i].kind == GC_SLIDER) {
            char val[16]; snprintf(val, sizeof val, "%.2f", *c[i].fval);
            f32 vw = (f32)strlen(val) * tcw;
            draw_text_clipped(r, val, cx + cw - vw, c[i].ry, dim, vw + tcw);
        }
    }
    renderer_flush_glyphs(r);
    renderer_reset_ui_scale(r); g_ui_step = 0;
}

static void fb_render_graph(FileBrowser *fb, Renderer *r, f32 x, f32 y,
                            f32 w, f32 h, f32 dpi, const Theme *theme) {
    if (w < 4 || h < 4) return;
    graph_settings_ensure(fb);

    /* Swap in a finished async build (worker published graph_pending before the
     * done flag, so the seq-cst load here observes it). */
    if (fb->graph_building && atomic_load(&fb->graph_build_done)) {
        pthread_join(fb->graph_build_thread, NULL);
        fb->graph_building = false;
        fb->graph = fb->graph_pending;   /* may be NULL (no .md under dir) */
        fb->graph_pending = NULL;
        fb->graph_fitted = false;
        fb->graph_user_zoomed = false;
        fb->graph_fit_zoom = 0.0f;
        fb->graph_pan_x = fb->graph_pan_y = 0.0f;
        fb->graph_zoom = 1.0f;
        fb->graph_hover = fb->graph_drag_node = -1;
        fb->gset_dirty = true;           /* re-apply user params: build resets them to 1.0 */
        fb_set_entries_from_graph(fb);   /* sidebar mirrors the graph notes */
    }
    /* While the scan runs, show a spinner and keep the frame loop alive so the
     * poll above fires every frame. The heavy I/O is on the worker — the UI
     * stays responsive even behind the macOS file-access permission prompt. */
    if (fb->graph_building) {
        f32 cw = 8.0f * dpi, ch = 16.0f * dpi;
        g_ui_step = cw; renderer_set_ui_scale(r, cw, ch);
        const char *msg = "Scanning notes\xe2\x80\xa6";
        f32 mw = 15.0f * cw;
        Color dim = theme ? (Color){ theme->sidebar_fg.r, theme->sidebar_fg.g,
                                     theme->sidebar_fg.b, 0.6f }
                          : (Color){ 0.6f, 0.6f, 0.6f, 0.6f };
        draw_text_clipped(r, msg, x + (w - mw) * 0.5f, y + h * 0.5f - ch * 0.5f, dim, mw + cw);
        renderer_reset_ui_scale(r); g_ui_step = 0;
        fb->graph_next_frame_at = platform_time_sec();  /* keep polling */
        return;
    }

    if (!fb->graph) {
        /* Folder filter scoped to a folder with no .md files. Tell the user
         * rather than showing a blank pane — they can navigate back up. */
        f32 cw = 8.0f * dpi, ch = 16.0f * dpi;
        g_ui_step = cw;                       /* draw_text_clipped glyph advance */
        renderer_set_ui_scale(r, cw, ch);
        const char *msg = "No markdown files in this folder";
        f32 mw = (f32)strlen(msg) * cw;
        Color dim = theme ? (Color){ theme->sidebar_fg.r, theme->sidebar_fg.g,
                                     theme->sidebar_fg.b, 0.6f }
                          : (Color){ 0.6f, 0.6f, 0.6f, 0.6f };
        draw_text_clipped(r, msg, x + (w - mw) * 0.5f, y + h * 0.5f - ch * 0.5f, dim, mw + cw);
        renderer_reset_ui_scale(r);
        g_ui_step = 0;
        return;
    }
    fb->graph_view_x = x; fb->graph_view_y = y;
    fb->graph_view_w = w; fb->graph_view_h = h;
    fb->graph_render_dpi = dpi;
    i32 iw = (i32)w, ih = (i32)h;

    /* Mirror the controls panel into the live simulation only when a control
     * actually changed (set by graph_settings_ensure + the panel handlers).
     * reheat on a force change is done by the panel handler, not here. */
    if (fb->gset_dirty) {
        md_graph_apply_params(fb->graph, fb->gset.center, fb->gset.repel,
                              fb->gset.link, fb->gset.linkdist, fb->gset.node_size);
        fb->gset_dirty = false;
    }

    if (!fb->graph_fitted) {
        f32 R = md_graph_world_radius(fb->graph);
        f32 fit = fminf((f32)iw, (f32)ih) * 0.44f / fmaxf(R, 1.0f);
        /* Entrance: open slightly zoomed out + transparent. The camera glide
         * below eases the zoom up to the fit while graph_intro fades the graph
         * in — a calm "open with animation" instead of a hard cut. */
        fb->graph_zoom = fit * 0.86f;
        fb->graph_pan_x = fb->graph_pan_y = 0.0f;
        fb->graph_fit_zoom = fit;
        fb->graph_fitted = true;
        fb->graph_intro_start = platform_time_sec();
    }

    bool moving = md_graph_step(fb->graph, 1.0f / 60.0f);
    /* Keep the layout framed while it's still moving, but GLIDE the camera
     * toward the fit instead of snapping each frame — snapping to a zoom that is
     * the reciprocal of a per-frame-changing world radius is what made the view
     * judder. The graph normally opens already warm-started (settled), so this
     * is inert then; it only eases the rare residual motion (a node-drag reheat,
     * or a huge vault that hit the warm-start cap). Stops once the user takes
     * manual control (pan/zoom). */
    bool cam_moving = false;
    if (!fb->graph_user_zoomed) {
        f32 R = md_graph_world_radius(fb->graph);
        f32 target = fminf((f32)iw, (f32)ih) * 0.44f / fmaxf(R, 1.0f);
        f32 dz = target - fb->graph_zoom;
        bool unconverged = fabsf(dz) > target * 0.004f ||
                           fabsf(fb->graph_pan_x) > 0.5f || fabsf(fb->graph_pan_y) > 0.5f;
        if (moving || unconverged) {
            const f32 CAM_LERP = 0.12f;
            fb->graph_zoom  += dz * CAM_LERP;
            fb->graph_pan_x += (0.0f - fb->graph_pan_x) * CAM_LERP;
            fb->graph_pan_y += (0.0f - fb->graph_pan_y) * CAM_LERP;
            fb->graph_fit_zoom = fb->graph_zoom;   /* label-fade reference eases too */
            cam_moving = fabsf(target - fb->graph_zoom) > target * 0.004f ||
                         fabsf(fb->graph_pan_x) > 0.5f || fabsf(fb->graph_pan_y) > 0.5f;
        }
    }
    i32 hover = fb->graph_hover;

    /* Hover-focus animation. Easing graph_focus_t toward 1 while a node is
     * hovered (and back to 0 when not) lets the connected-web emphasis fade in
     * and out instead of snapping on/off. graph_focus_node centres the
     * highlight and outlives graph_hover through the fade-out, so the dimmed
     * neighbours keep their identity while the focus recedes. */
    if (hover >= 0) fb->graph_focus_node = hover;
    {
        f32 target = (hover >= 0) ? 1.0f : 0.0f;
        const f32 FOCUS_LERP = 0.22f;           /* matches the camera glide feel */
        f32 prev = fb->graph_focus_t;
        fb->graph_focus_t += (target - fb->graph_focus_t) * FOCUS_LERP;
        if (target > 0.5f && fb->graph_focus_t > 0.999f) fb->graph_focus_t = 1.0f;
        if (target < 0.5f && fb->graph_focus_t < 0.001f) fb->graph_focus_t = 0.0f;
        if (fb->graph_focus_t == 0.0f) fb->graph_focus_node = -1;
        if (fabsf(fb->graph_focus_t - prev) > 0.0001f)
            fb->graph_next_frame_at = platform_time_sec();   /* keep animating */
    }
    i32 focus_node = fb->graph_focus_node;
    f32 focus_t    = fb->graph_focus_t;

    /* Entrance fade (ease-out cubic). graph_intro_start is armed on first fit;
     * the graph content (edges/nodes/labels) is multiplied by intro_a so it
     * fades in as the camera glide eases the zoom up to the fit. */
    f32 intro_a = 1.0f;
    if (fb->graph_intro_start > 0.0) {
        f64 e = (platform_time_sec() - fb->graph_intro_start) / 0.45;
        if (e >= 1.0) { fb->graph_intro_start = 0.0; }
        else {
            f32 t = (f32)(e < 0.0 ? 0.0 : e);
            intro_a = 1.0f - (1.0f - t)*(1.0f - t)*(1.0f - t);
            fb->graph_next_frame_at = platform_time_sec();  /* keep animating */
        }
    }

    /* Theme-derived palette, anchored to the chrome surface + a legible ink so
     * it reads on BOTH light and dark themes (no hardcoded RGB). */
    ChromePalette cp = chrome_palette_for(theme);
    Color accent  = theme_ui_accent(theme);
    Color fgc     = theme ? theme->fg : (Color){0.82f, 0.82f, 0.88f, 1};
    Color canvas  = cp.surface_sunken;                 /* tone the graph reads against */
    Color legible = chrome_legible_on(canvas);         /* black on light, white on dark */
    /* Node base: fg pulled 22% toward the legible direction so it always
     * separates from the canvas. */
    Color node = { fgc.r*0.78f + legible.r*0.22f,
                   fgc.g*0.78f + legible.g*0.22f,
                   fgc.b*0.78f + legible.b*0.22f, 1.0f };
    /* Edge: canvas→node blend, so links sit a clear tier below nodes on both
     * polarities (light-mid grey on white, dark-mid grey on black). */
    Color edge = { canvas.r + (node.r-canvas.r)*0.55f,
                   canvas.g + (node.g-canvas.g)*0.55f,
                   canvas.b + (node.b-canvas.b)*0.55f, 1.0f };
    /* Tag pseudo-nodes: a distinct green, legibility-corrected per polarity. */
    Color tag_raw = theme ? theme->ansi[2] : (Color){0.45f, 0.78f, 0.55f, 1};
    f32   tag_lum = chrome_luminance(tag_raw);
    Color tag_col = (cp.is_light && tag_lum > 0.55f) ? gcol_blend(tag_raw, node, 0.45f)
                  : (!cp.is_light && tag_lum < 0.10f) ? gcol_lift(tag_raw, 0.30f)
                  : tag_raw;

    /* Strong hover focus: build the hovered node's neighbour bitset ONCE
     * (O(edges)) so edges, arrows, nodes and labels all share O(1) lookups
     * rather than per-node md_graph_connected scans. */
    /* focusing stays true through the whole fade (focus_t > 0), driven by
     * focus_node rather than the live hover, so the highlight animates out
     * after the pointer leaves instead of cutting. */
    bool focusing = (focus_node >= 0 && focus_t > 0.0f);
    const f32 FOCUS_DIM = 0.06f;
    i32 nc = md_graph_node_count(fb->graph);
    bool have_nbr = false;
    if (focusing && nc > 0) {
        if (fb->graph_nbr_cap < nc) {
            u8 *nn = realloc(fb->graph_nbr_set, (usize)nc);
            if (nn) { fb->graph_nbr_set = nn; fb->graph_nbr_cap = nc; }
        }
        if (fb->graph_nbr_cap >= nc) {
            memset(fb->graph_nbr_set, 0, (usize)nc);
            i32 ne = md_graph_edge_count(fb->graph);
            for (i32 e = 0; e < ne; e++) {
                i32 a = -1, b = -1;
                md_graph_edge(fb->graph, e, &a, &b);
                if      (a == focus_node && b >= 0 && b < nc) fb->graph_nbr_set[b] = 1;
                else if (b == focus_node && a >= 0 && a < nc) fb->graph_nbr_set[a] = 1;
            }
            have_nbr = true;
        }
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    renderer_push_scissor(r, x, y, w, h);

    /* Faint canvas wash so nodes/edges read against the exact tone their
     * contrast was derived from (surface_sunken). */
    { Color bgw = canvas; bgw.a = intro_a;
      renderer_draw_rect(r, x, y, w, h, bgw); renderer_flush_rects(r); }

    /* Edges — GPU anti-aliased lines (no CPU rasterization). */
    i32 ec = md_graph_edge_count(fb->graph);
    for (i32 e = 0; e < ec; e++) {
        i32 a = -1, b = -1;
        md_graph_edge(fb->graph, e, &a, &b);
        if (a < 0 || b < 0) continue;
        f32 ax, ay, ar, bx, by, br;
        md_graph_node_screen(fb->graph, a, iw, ih, fb->graph_pan_x,
                             fb->graph_pan_y, fb->graph_zoom, &ax, &ay, &ar);
        md_graph_node_screen(fb->graph, b, iw, ih, fb->graph_pan_x,
                             fb->graph_pan_y, fb->graph_zoom, &bx, &by, &br);
        if (graph_node_hidden(fb, a) || graph_node_hidden(fb, b)) continue;
        /* Edges incident to the focused node are the "connecting" edges — they
         * brighten toward accent while the rest melt to near-invisible, both
         * eased by focus_t so the transition glides in and out. */
        bool conn = (focusing && (a == focus_node || b == focus_node));
        Color ec_col = conn ? gcol_blend(edge, accent, 0.55f * focus_t) : edge;
        f32 ea_to = conn ? 0.90f : 0.05f;
        ec_col.a = (0.22f + (ea_to - 0.22f) * focus_t) * intro_a;
        f32 lt = fb->gset.link_thickness > 0 ? fb->gset.link_thickness : 1.0f;
        f32 ethick = (conn ? 0.85f + (1.6f - 0.85f) * focus_t : 0.85f) * dpi * lt;
        renderer_draw_line(r, x + ax, y + ay, x + bx, y + by, ethick, ec_col);
    }
    renderer_flush_lines(r);

    /* Arrowheads on directed links (Display ▸ Arrows). Drawn from the directed
     * src→dst edges so the head points the way the link actually goes; placed
     * just short of the destination node so it doesn't bury under the circle. */
    if (fb->gset.show_arrows) {
        i32 dec = md_graph_dir_edge_count(fb->graph);
        for (i32 e = 0; e < dec; e++) {
            i32 s = -1, d = -1;
            md_graph_dir_edge(fb->graph, e, &s, &d);
            if (s < 0 || d < 0) continue;
            if (graph_node_hidden(fb, s) || graph_node_hidden(fb, d)) continue;
            f32 sx, sy, sr, dx2, dy2, dr;
            md_graph_node_screen(fb->graph, s, iw, ih, fb->graph_pan_x,
                                 fb->graph_pan_y, fb->graph_zoom, &sx, &sy, &sr);
            md_graph_node_screen(fb->graph, d, iw, ih, fb->graph_pan_x,
                                 fb->graph_pan_y, fb->graph_zoom, &dx2, &dy2, &dr);
            f32 vx = dx2 - sx, vy = dy2 - sy;
            f32 len = sqrtf(vx*vx + vy*vy);
            if (len < dr + 6.0f) continue;
            f32 ux = vx / len, uy = vy / len;
            f32 tipx = dx2 - ux * (dr + 1.0f), tipy = dy2 - uy * (dr + 1.0f);
            f32 ah = 6.0f * dpi;                 /* arrowhead length */
            f32 px = -uy, py = ux;               /* perpendicular */
            bool conn = (focusing && (s == focus_node || d == focus_node));
            Color ac = conn ? gcol_blend(edge, accent, 0.55f * focus_t) : edge;
            f32 aa_to = conn ? 0.90f : 0.05f;
            ac.a = (0.40f + (aa_to - 0.40f) * focus_t) * intro_a;
            renderer_draw_line(r, x + tipx, y + tipy,
                               x + tipx - ux*ah + px*ah*0.55f,
                               y + tipy - uy*ah + py*ah*0.55f, 0.9f*dpi, ac);
            renderer_draw_line(r, x + tipx, y + tipy,
                               x + tipx - ux*ah - px*ah*0.55f,
                               y + tipy - uy*ah - py*ah*0.55f, 0.9f*dpi, ac);
        }
        renderer_flush_lines(r);
    }

    /* Nodes — GPU rounded circles (rrect with radius = size/2). */
    i32 gnc = md_graph_node_count(fb->graph);
    for (i32 i = 0; i < gnc; i++) {
        f32 sx, sy, rad;
        md_graph_node_screen(fb->graph, i, iw, ih, fb->graph_pan_x,
                             fb->graph_pan_y, fb->graph_zoom, &sx, &sy, &rad);
        if (sx < -rad || sx > w + rad || sy < -rad || sy > h + rad) continue;
        if (graph_node_hidden(fb, i)) continue;
        bool is_hover = (focusing && i == focus_node);
        bool is_neighbor = (focusing && i != focus_node && have_nbr && fb->graph_nbr_set[i]);
        bool is_tag = md_graph_node_is_tag(fb->graph, i);
        /* Default (color_mode 0): a single uniform node colour. Clusters mode (1):
         * hue per topic-component so a hub and its notes share a colour; isolated
         * notes stay neutral. Tags are always green so they read as a distinct kind. */
        Color c;
        if (is_tag) c = tag_col;
        else if (fb->gset.color_mode == 1 && md_graph_node_comp_size(fb->graph, i) >= 2)
            c = graph_group_color(md_graph_node_component(fb->graph, i), node, cp.is_light);
        else c = node;
        /* Focus mode: the focused node + its neighbours stay fully lit by ALPHA
         * (no fill flood to accent — the focused node's accent is the ring drawn
         * below); everything unrelated melts toward FOCUS_DIM. Each level is
         * eased by focus_t from the neutral 0.90 so the dim/brighten glides. */
        f32 rr = fmaxf(rad, 2.2f * dpi);     /* HiDPI visibility floor */
        f32 a;
        bool dimmed = !is_hover && !is_neighbor;
        if (is_hover)        { a = 0.90f + (1.0f - 0.90f) * focus_t; rr += 2.0f * dpi * focus_t; }
        else if (is_neighbor){ a = 0.90f + (1.0f - 0.90f) * focus_t; }
        else                 { a = 0.90f + (FOCUS_DIM - 0.90f) * focus_t; }
        c.a = a * intro_a;
        /* A handful of hubs (degree ≥ 8) get a faint SDF presence-glow; on a
         * dimmed (unrelated) node it fades out with focus_t rather than cutting. */
        f32 glow = (cp.is_light ? 0.08f : 0.12f) * intro_a;
        if (dimmed) glow *= (1.0f - focus_t);
        if (md_graph_node_degree(fb->graph, i) >= 8 && glow > 0.002f) {
            renderer_draw_rrect(r, x + sx - rr, y + sy - rr, rr * 2.0f, rr * 2.0f, c,
                                rr, rr, rr, rr,
                                rad * 0.9f, glow, 0, 0);
        } else {
            renderer_draw_rrect_simple(r, x + sx - rr, y + sy - rr,
                                       rr * 2.0f, rr * 2.0f, c, rr);
        }
    }
    renderer_flush_rrects(r);

    /* Hovered-node emphasis: an accent ring + soft halo (Metal SDF; OpenGL
     * degrades to just the ring). Drawn after the fill pass so it sits on top;
     * neighbours stay calm (lit by alpha alone, no ring). */
    if (focusing && focus_node >= 0 && !graph_node_hidden(fb, focus_node)) {
        f32 hsx, hsy, hrad;
        md_graph_node_screen(fb->graph, focus_node, iw, ih, fb->graph_pan_x,
                             fb->graph_pan_y, fb->graph_zoom, &hsx, &hsy, &hrad);
        bool h_tag = md_graph_node_is_tag(fb->graph, focus_node);
        Color hf = h_tag ? tag_col
                 : (fb->gset.color_mode == 1 && md_graph_node_comp_size(fb->graph, focus_node) >= 2)
                     ? graph_group_color(md_graph_node_component(fb->graph, focus_node), node, cp.is_light)
                     : node;
        hf.a = 1.0f * intro_a;
        /* Ring + halo strength tracks focus_t so the accent emphasis grows in
         * and recedes with the rest of the focus animation. */
        Color hb = accent; hb.a = 0.9f * intro_a * focus_t;
        f32 rr = fmaxf(hrad, 2.2f * dpi) + 2.0f * dpi * focus_t;
        renderer_draw_rrect_bordered(r, x + hsx - rr, y + hsy - rr, rr * 2.0f, rr * 2.0f,
                                     hf, hb, fmaxf(1.0f, 1.5f * dpi),
                                     rr, rr, rr, rr,
                                     8.0f * dpi, (cp.is_light ? 0.30f : 0.45f) * intro_a * focus_t, 0, 0);
        renderer_flush_rrects(r);
    }

    /* Node labels — DECLUTTERED. Drawing every label produced unreadable
     * overlapping mush, so labels are placed greedily by priority (hovered node
     * + its neighbours first, then hubs by degree) against a coarse screen-space
     * occupancy grid: a label is drawn only if its box doesn't collide with one
     * already placed. A degree gate + zoom fade reveal more labels at higher zoom;
     * hovering focuses to a node and its neighbours. */
    f32 lcw = 6.0f * dpi, lch = 12.0f * dpi;
    renderer_set_ui_scale(r, lcw, lch);
    /* Labels read as the node tone (inherits guaranteed canvas contrast on both
     * polarities); they go up to 0.98 alpha so they stay crisp over the nodes. */
    Color lbase = node;
    /* Fade is RELATIVE to the fitted zoom (the wider layout makes the absolute
     * fit zoom small, so an absolute gate would hide every label by default).
     * 0 at fit, 1 at ~2x fit. A legible floor keeps gated-in hubs readable. */
    f32 fitz = fb->graph_fit_zoom > 1e-4f ? fb->graph_fit_zoom : 1.0f;
    f32 zoom_t = (fb->graph_zoom / fitz) - 1.0f;
    if (zoom_t < 0.0f) zoom_t = 0.0f;
    if (zoom_t > 1.0f) zoom_t = 1.0f;
    f32 tf = fb->gset.text_fade > 0 ? fb->gset.text_fade : 0.0f;
    f32 base_alpha = (0.50f + 0.28f * zoom_t) * tf * intro_a;
    if (base_alpha > 1.0f) base_alpha = 1.0f;
    /* nc + the hovered-node neighbour bitset (have_nbr) were built once near the
     * top of the draw, shared by edges/arrows/nodes and reused here. */

    /* Occupancy grid (per-FileBrowser scratch, grown lazily, reused per frame). */
    LblGrid grid = { NULL, 0, 0, 26.0f * dpi, w, h };
    grid.cols = lbl_clampi((i32)(w / grid.cell) + 1, 1, 80);
    grid.rows = lbl_clampi((i32)(h / grid.cell) + 1, 1, 80);
    i32 ncell = grid.cols * grid.rows;
    if (fb->graph_lbl_grid_cap < ncell) {
        u8 *ng = realloc(fb->graph_lbl_grid, (usize)ncell);
        if (ng) { fb->graph_lbl_grid = ng; fb->graph_lbl_grid_cap = ncell; }
    }
    bool can_declutter = (fb->graph_lbl_grid_cap >= ncell);
    if (can_declutter) {
        grid.cells = fb->graph_lbl_grid;
        memset(grid.cells, 0, (usize)ncell);

        /* Reserve folder-cluster label boxes first so node labels yield to them
         * (the folder glyphs themselves are drawn in the block just below). */
        {
            f32 ccw = 7.0f * dpi, cch = 14.0f * dpi;
            i32 gcnt = md_graph_group_count(fb->graph);
            for (i32 gi = 0; gi < gcnt; gi++) {
                const char *gname = md_graph_group_name(fb->graph, gi);
                if (!gname || !gname[0]) continue;
                f32 gwx, gwy;
                if (!md_graph_group_centroid(fb->graph, gi, &gwx, &gwy)) continue;
                f32 gsx = w * 0.5f + fb->graph_pan_x + gwx * fb->graph_zoom;
                f32 gsy = h * 0.5f + fb->graph_pan_y + gwy * fb->graph_zoom;
                const char *bn = strrchr(gname, '/'); bn = bn ? bn + 1 : gname;
                i32 bl = (i32)strlen(bn); if (bl > 24) bl = 24;
                f32 gcells = (f32)utf8_len((const u8 *)bn, (usize)bl);
                f32 bx0 = gsx - gcells * ccw * 0.5f, by0 = gsy - 20.0f * dpi;
                lbl_box_mark(&grid, bx0, by0, bx0 + gcells * ccw, by0 + cch);
            }
        }

        /* Gather candidates as packed (priority<<20 | index) keys, then sort
         * descending so the highest-priority labels claim grid space first.
         * Small vaults skip the degree gate (no clutter to thin out). */
        i32 deg_gate = (nc <= 30) ? 0 : (i32)(2.0f * (1.0f - zoom_t) + 0.5f);
        if (fb->graph_lbl_cap < nc) {
            u64 *nk = realloc(fb->graph_lbl_key, (usize)nc * sizeof(u64));
            if (nk) { fb->graph_lbl_key = nk; fb->graph_lbl_cap = nc; }
        }
        i32 ncand = 0;
        if (fb->graph_lbl_cap >= nc) {
            for (i32 i = 0; i < nc; i++) {
                if (graph_node_hidden(fb, i)) continue;
                bool hov = (i == focus_node);
                bool nbr = (have_nbr && i != focus_node && fb->graph_nbr_set[i]);
                i32 deg = md_graph_node_degree(fb->graph, i);
                if (!hov && !nbr) {
                    if (base_alpha < 0.04f) continue;
                    if (deg < deg_gate) continue;
                }
                f32 sx, sy, rad;
                md_graph_node_screen(fb->graph, i, iw, ih, fb->graph_pan_x,
                                     fb->graph_pan_y, fb->graph_zoom, &sx, &sy, &rad);
                if (sx < -60 || sx > w + 60 || sy < -20 || sy > h + 20) continue;
                u32 special = hov ? 3u : (nbr ? 2u : 0u);
                u32 dd = (u32)(deg > 0xFFF ? 0xFFF : (deg < 0 ? 0 : deg));
                i32 csz = md_graph_node_comp_size(fb->graph, i);
                u32 cs = (u32)(csz > 0xFFF ? 0xFFF : (csz < 0 ? 0 : csz));
                u32 pri = (special << 28) | (dd << 12) | cs;
                fb->graph_lbl_key[ncand++] = ((u64)pri << 20) | (u32)i;
            }
            qsort(fb->graph_lbl_key, (usize)ncand, sizeof(u64), lbl_key_desc);
        }

        /* Greedy placement, capped so a pathological hover-fan can't flood. */
        i32 placed = 0;
        for (i32 k = 0; k < ncand && placed < 140; k++) {
            i32 i = (i32)(fb->graph_lbl_key[k] & 0xFFFFFu);
            bool hov = (i == focus_node);
            bool nbr = (have_nbr && i != focus_node && fb->graph_nbr_set[i]);
            /* Focus mode: focus_t lifts the focused node + its neighbours and
             * mutes everything else, eased so labels fade in step with the
             * nodes/edges instead of blinking on at hover. */
            f32 la;
            if (hov)      la = base_alpha + (0.98f - base_alpha) * focus_t;
            else if (nbr) la = base_alpha + (0.85f - base_alpha) * focus_t;
            else          la = base_alpha * (1.0f - focus_t);
            if (la < 0.04f) continue;
            f32 sx, sy, rad;
            md_graph_node_screen(fb->graph, i, iw, ih, fb->graph_pan_x,
                                 fb->graph_pan_y, fb->graph_zoom, &sx, &sy, &rad);
            const char *txt = md_graph_node_label(fb->graph, i);
            if (!txt || !txt[0]) continue;
            i32 blen = (i32)strlen(txt);
            if (blen > 28) blen = 28;
            f32 cells = (f32)utf8_len((const u8 *)txt, (usize)blen);
            if (cells < 1.0f) continue;
            f32 tw = cells * lcw, th = lch;
            f32 bx0 = sx - tw * 0.5f, by0 = sy + rad + 5.0f * dpi;
            f32 bx1 = bx0 + tw, by1 = by0 + th;
            bool force = hov || nbr;     /* always show; still reserve its box */
            if (!force && !lbl_box_free(&grid, bx0, by0, bx1, by1)) continue;
            lbl_box_mark(&grid, bx0, by0, bx1, by1);
            placed++;
            f32 gx = x + bx0, gy = y + by0;
            Color label = lbase; label.a = la;
            const u8 *p = (const u8 *)txt; usize rem = (usize)blen;
            while (rem > 0) {
                u32 cp = 0;
                u32 nn = utf8_decode(p, rem, &cp);
                if (nn == 0) break;
                if (cp >= 32) renderer_push_glyph(r, gx, gy, cp, label);
                gx += lcw; p += nn; rem -= nn;
            }
        }
    }
    renderer_flush_glyphs(r);

    /* Folder cluster labels — the directory name at each cluster's centroid, in
     * the cluster's colour, so the groups read at a glance. Drawn at their own
     * (slightly larger) glyph scale after the node labels are flushed. */
    {
        f32 ccw = 7.0f * dpi, cch = 14.0f * dpi;
        renderer_set_ui_scale(r, ccw, cch);
        i32 gcnt = md_graph_group_count(fb->graph);
        for (i32 gi = 0; gi < gcnt; gi++) {
            const char *gname = md_graph_group_name(fb->graph, gi);
            if (!gname || !gname[0]) continue;           /* root files: no label */
            f32 gwx, gwy;
            if (!md_graph_group_centroid(fb->graph, gi, &gwx, &gwy)) continue;
            f32 gsx = (f32)w * 0.5f + fb->graph_pan_x + gwx * fb->graph_zoom;
            f32 gsy = (f32)h * 0.5f + fb->graph_pan_y + gwy * fb->graph_zoom;
            if (gsx < -80 || gsx > w + 80 || gsy < -40 || gsy > h + 40) continue;
            const char *bn = strrchr(gname, '/'); bn = bn ? bn + 1 : gname;
            i32 blen = (i32)strlen(bn); if (blen > 24) blen = 24;
            f32 cells = (f32)utf8_len((const u8 *)bn, (usize)blen);
            f32 gx = x + gsx - cells * ccw * 0.5f;
            f32 gy = y + gsy - 20.0f * dpi;
            /* Folder labels are neutral now that node colour conveys topic
             * clusters (components), not folders — a group-coloured label would
             * imply a node-colour match that no longer holds. */
            Color lc = node; lc.a = 0.50f * intro_a;
            const u8 *p = (const u8 *)bn; usize rem = (usize)blen;
            while (rem > 0) {
                u32 cp = 0; u32 nb = utf8_decode(p, rem, &cp);
                if (nb == 0) break;
                if (cp >= 32) renderer_push_glyph(r, gx, gy, cp, lc);
                gx += ccw; p += nb; rem -= nb;
            }
        }
        renderer_flush_glyphs(r);
    }

    renderer_pop_scissor(r);
    renderer_reset_ui_scale(r);

    /* Controls panel floats above the (unclipped) canvas. */
    graph_render_panel(fb, r, dpi, theme);

    /* Keep redrawing while the sim is moving OR the camera is still gliding to
     * its fit, so the ease finishes instead of stopping mid-glide. */
    if (moving || cam_moving) fb->graph_next_frame_at = platform_time_sec();
    else                      fb->graph_next_frame_at = 0.0;
}

/* ---- Graph interaction (rel coords within the viewer content rect) ---- */

void fb_graph_hover(FileBrowser *fb, f32 rx, f32 ry, f32 w, f32 h) {
    if (!fb_graph_active(fb)) return;
    if (fb->graph_panning || fb->graph_drag_node >= 0) return;
    fb->graph_hover = graph_hit_visible(fb, rx, ry, w, h);
}

void fb_graph_press(FileBrowser *fb, f32 rx, f32 ry, f32 w, f32 h) {
    if (!fb_graph_active(fb)) return;
    fb->graph_press_x = fb->graph_last_x = rx;
    fb->graph_press_y = fb->graph_last_y = ry;
    fb->graph_moved = false;
    i32 node = graph_hit_visible(fb, rx, ry, w, h);
    if (node >= 0) { fb->graph_drag_node = node; fb->graph_panning = false; }
    else           { fb->graph_panning = true;   fb->graph_drag_node = -1; }
}

void fb_graph_move(FileBrowser *fb, f32 rx, f32 ry, f32 w, f32 h) {
    if (!fb_graph_active(fb)) return;
    f32 ddx = rx - fb->graph_last_x, ddy = ry - fb->graph_last_y;
    fb->graph_last_x = rx; fb->graph_last_y = ry;
    if (fabsf(rx - fb->graph_press_x) > 3.0f || fabsf(ry - fb->graph_press_y) > 3.0f)
        fb->graph_moved = true;
    if (fb->graph_drag_node >= 0) {
        f32 wx, wy;
        md_graph_screen_to_world((i32)w, (i32)h, fb->graph_pan_x, fb->graph_pan_y,
                                 fb->graph_zoom, rx, ry, &wx, &wy);
        md_graph_set_node_world(fb->graph, fb->graph_drag_node, wx, wy, true);
        md_graph_reheat(fb->graph);
        /* The reheat restarts motion; without this the auto re-fit would stomp
         * the user's pan/zoom back to the fitted view every frame of the drag. */
        fb->graph_user_zoomed = true;
        fb->graph_next_frame_at = platform_time_sec();
    } else if (fb->graph_panning) {
        fb->graph_pan_x += ddx; fb->graph_pan_y += ddy;
        fb->graph_user_zoomed = true;
    }
}

const char *fb_graph_release(FileBrowser *fb, f32 rx, f32 ry, f32 w, f32 h) {
    if (!fb_graph_active(fb)) return NULL;
    const char *open_path = NULL;
    if (!fb->graph_moved) {
        i32 node = graph_hit_visible(fb, rx, ry, w, h);
        if (node >= 0) open_path = md_graph_node_path(fb->graph, node);
    }
    /* Unpin a dragged node so the layout can keep relaxing it. */
    if (fb->graph_drag_node >= 0) {
        f32 wx, wy;
        md_graph_screen_to_world((i32)w, (i32)h, fb->graph_pan_x, fb->graph_pan_y,
                                 fb->graph_zoom, rx, ry, &wx, &wy);
        md_graph_set_node_world(fb->graph, fb->graph_drag_node, wx, wy, false);
    }
    fb->graph_drag_node = -1;
    fb->graph_panning = false;
    return open_path;
}

void fb_graph_scroll(FileBrowser *fb, f32 dy, f32 rx, f32 ry, f32 w, f32 h) {
    if (!fb_graph_active(fb)) return;
    /* Zoom toward the cursor: keep the world point under (rx,ry) fixed. */
    f32 wx, wy;
    md_graph_screen_to_world((i32)w, (i32)h, fb->graph_pan_x, fb->graph_pan_y,
                             fb->graph_zoom, rx, ry, &wx, &wy);
    f32 factor = expf(dy * 0.0015f);
    f32 nz = fb->graph_zoom * factor;
    if (nz < 0.05f) nz = 0.05f; if (nz > 12.0f) nz = 12.0f;
    fb->graph_zoom = nz;
    fb->graph_user_zoomed = true;   /* user took control: stop auto re-fit */
    /* Recompute pan so (wx,wy) stays under the cursor. */
    fb->graph_pan_x = rx - (f32)w * 0.5f - wx * nz;
    fb->graph_pan_y = ry - (f32)h * 0.5f - wy * nz;
}

bool fb_graph_gesture(const FileBrowser *fb) {
    return fb && fb->view_mode == FVIEW_GRAPH &&
           (fb->graph_panning || fb->graph_drag_node >= 0 ||
            fb->graph_drag_slider >= 0);
}

/* ---- Graph controls panel input (absolute framebuffer coords) ---------- */

bool fb_graph_panel_hit(const FileBrowser *fb, f32 sx, f32 sy) {
    if (!fb_graph_active(fb)) return false;
    FileBrowser *m = (FileBrowser *)fb;   /* layout only reads geometry/settings */
    GCtl c[GRAPH_PANEL_CTLS];
    f32 gx,gy,gs,px,py,pw,ph,cx,cw;
    f32 dpi = fb->graph_render_dpi > 0 ? fb->graph_render_dpi : 1.0f;
    graph_panel_layout(m, dpi, c, &gx,&gy,&gs, &px,&py,&pw,&ph, &cx,&cw);
    if (sx >= gx && sx < gx+gs && sy >= gy && sy < gy+gs) return true;
    if (fb->graph_settings_open && sx >= px && sx < px+pw && sy >= py && sy < py+ph)
        return true;
    return false;
}

bool fb_graph_panel_press(FileBrowser *fb, f32 sx, f32 sy) {
    if (!fb_graph_active(fb)) return false;
    GCtl c[GRAPH_PANEL_CTLS];
    f32 gx,gy,gs,px,py,pw,ph,cx,cw;
    f32 dpi = fb->graph_render_dpi > 0 ? fb->graph_render_dpi : 1.0f;
    graph_panel_layout(fb, dpi, c, &gx,&gy,&gs, &px,&py,&pw,&ph, &cx,&cw);

    if (sx >= gx && sx < gx+gs && sy >= gy && sy < gy+gs) {
        fb->graph_settings_open = !fb->graph_settings_open;
        return true;
    }
    if (!fb->graph_settings_open) return false;
    if (!(sx >= px && sx < px+pw && sy >= py && sy < py+ph)) return false;

    for (i32 i = 0; i < GRAPH_PANEL_CTLS; i++) {
        if (sy < c[i].ry || sy >= c[i].ry + c[i].rh) continue;
        if (c[i].kind == GC_SLIDER) {
            f32 tx,ty,tw,th; gc_track(&c[i], cx, cw, dpi, &tx,&ty,&tw,&th);
            f32 t = (sx - tx) / tw; if (t < 0) t = 0; if (t > 1) t = 1;
            *c[i].fval = c[i].minv + t * (c[i].maxv - c[i].minv);
            fb->graph_drag_slider = i;
            fb->gset_dirty = true;
            if (c[i].is_force && fb->graph) md_graph_reheat(fb->graph);
        } else {
            if (c[i].ival)      *c[i].ival = *c[i].ival ? 0 : 1;
            else if (c[i].bval) *c[i].bval = !*c[i].bval;
            fb->gset_dirty = true;
        }
        return true;
    }
    return true;   /* swallow clicks anywhere inside the panel */
}

void fb_graph_panel_drag(FileBrowser *fb, f32 sx, f32 sy) {
    (void)sy;
    if (!fb || fb->graph_drag_slider < 0) return;
    GCtl c[GRAPH_PANEL_CTLS];
    f32 gx,gy,gs,px,py,pw,ph,cx,cw;
    f32 dpi = fb->graph_render_dpi > 0 ? fb->graph_render_dpi : 1.0f;
    graph_panel_layout(fb, dpi, c, &gx,&gy,&gs, &px,&py,&pw,&ph, &cx,&cw);
    i32 i = fb->graph_drag_slider;
    if (i < 0 || i >= GRAPH_PANEL_CTLS || c[i].kind != GC_SLIDER) return;
    f32 tx,ty,tw,th; gc_track(&c[i], cx, cw, dpi, &tx,&ty,&tw,&th);
    f32 t = (sx - tx) / tw; if (t < 0) t = 0; if (t > 1) t = 1;
    f32 nv = c[i].minv + t * (c[i].maxv - c[i].minv);
    if (nv == *c[i].fval) return;          /* no-op move: don't re-arm the O(n^2) sim */
    *c[i].fval = nv;
    fb->gset_dirty = true;
    if (c[i].is_force && fb->graph) md_graph_reheat(fb->graph);
}

void fb_graph_panel_release(FileBrowser *fb) {
    if (fb) fb->graph_drag_slider = -1;
}

bool fb_open_md_path(FileBrowser *fb, const char *abs_path) {
    if (!fb || !abs_path || !abs_path[0]) return false;
    /* A wikilink/embed hop from a note opened off the graph stays "in the
     * graph": preserve the graph and re-pin graph_return so closing the
     * destination note still returns to the graph. */
    bool keep = fb->graph_return && fb->graph;
    char dir[FB_MAX_PATH];
    snprintf(dir, sizeof dir, "%s", abs_path);
    char *slash = strrchr(dir, '/');
    char base[256];
    snprintf(base, sizeof base, "%s", slash ? slash + 1 : dir);
    if (slash) *slash = '\0';
    else snprintf(dir, sizeof dir, ".");
    fb_close_viewer_ex(fb, keep);        /* leave the old doc, keep the graph */
    if (!fb_navigate(fb, dir)) { if (keep) fb->graph_return = true; return false; }
    for (i32 i = 0; i < fb->entry_count; i++) {
        if (fb->entries[i].name && strcmp(fb->entries[i].name, base) == 0) {
            bool ok = fb_open_file(fb, i);
            if (keep && fb->graph && fb->view_mode != FVIEW_GRAPH)
                fb->graph_return = true;
            return ok;
        }
    }
    if (keep) fb->graph_return = true;   /* not found: Esc can still return */
    return false;
}

/* Draw one source line's inline markdown styled, with the syntax markers
 * HIDDEN — the Live-Preview look for any line that doesn't hold the cursor.
 * Colours mirror the read-mode renderer (bold = brighter, italic/strike = dim,
 * code/link/tag tinted). Monospace: each visible glyph advances by cw. The
 * UI glyph scale must already be set to (cw,ch). Returns the advanced x. */
static f32 fb_live_inline(Renderer *r, const Theme *th, const char *s, i32 len,
                          f32 x, f32 y, f32 cw, f32 ch, Color fg) {
    Color bright = { fminf(1.0f,fg.r+0.18f), fminf(1.0f,fg.g+0.18f), fminf(1.0f,fg.b+0.18f), fg.a };
    Color dim    = { fg.r*0.60f, fg.g*0.60f, fg.b*0.60f, fg.a };
    Color link   = th ? th->ansi[12] : (Color){0.45f,0.62f,1.0f,1.0f};
    Color tagc   = th ? th->ansi[2]  : (Color){0.45f,0.78f,0.55f,1.0f};
    Color codefg = th ? th->ansi[11] : (Color){0.92f,0.80f,0.52f,1.0f};
    Color codebg = (Color){1,1,1,0.07f};
    Color hlbg   = (Color){0.95f,0.85f,0.20f,0.22f};
    i32 i = 0;
#define LIV_RUN(from,to,col) do { for (i32 _k=(from); _k<(to);) { u32 _cp; \
        u32 _nb=utf8_decode((const u8*)s+_k,(usize)(len-_k),&_cp); if(!_nb){_k++;continue;} \
        if(_cp>=32){ renderer_push_glyph(r,x,y,_cp,(col)); x+=cw; } _k+=_nb; } } while(0)
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c=='*' && i+1<len && s[i+1]=='*') {                 /* **bold** */
            i32 j=i+2; while(j+1<len && !(s[j]=='*'&&s[j+1]=='*')) j++;
            if (j+1<len) { LIV_RUN(i+2,j,bright); i=j+2; continue; }
        }
        if (c=='*' || c=='_') {                                 /* *italic* _italic_ */
            i32 j=i+1; while(j<len && s[j]!=(char)c) j++;
            if (j<len && j>i+1) { LIV_RUN(i+1,j,dim); i=j+1; continue; }
        }
        if (c=='`') {                                           /* `code` */
            i32 j=i+1; while(j<len && s[j]!='`') j++;
            if (j<len) {
                f32 span=(f32)utf8_len((const u8*)s+i+1,(usize)(j-i-1))*cw;
                renderer_draw_rect(r, x-1.0f, y-1.0f, span+2.0f, ch+2.0f, codebg);
                LIV_RUN(i+1,j,codefg); i=j+1; continue;
            }
        }
        if (c=='~' && i+1<len && s[i+1]=='~') {                 /* ~~strike~~ */
            i32 j=i+2; while(j+1<len && !(s[j]=='~'&&s[j+1]=='~')) j++;
            if (j+1<len) { LIV_RUN(i+2,j,dim); i=j+2; continue; }
        }
        if (c=='=' && i+1<len && s[i+1]=='=') {                 /* ==highlight== */
            i32 j=i+2; while(j+1<len && !(s[j]=='='&&s[j+1]=='=')) j++;
            if (j+1<len) {
                f32 span=(f32)utf8_len((const u8*)s+i+2,(usize)(j-i-2))*cw;
                renderer_draw_rect(r, x-1.0f, y, span+2.0f, ch, hlbg);
                LIV_RUN(i+2,j,fg); i=j+2; continue;
            }
        }
        if (c=='[' && i+1<len && s[i+1]=='[') {                 /* [[wiki|alias]] */
            i32 j=i+2; while(j+1<len && !(s[j]==']'&&s[j+1]==']')) j++;
            if (j+1<len) {
                i32 ds=i+2, de=j;
                for (i32 b=i+2;b<j;b++) if (s[b]=='|') { ds=b+1; break; }
                LIV_RUN(ds,de,link); i=j+2; continue;
            }
        }
        if (c=='[') {                                           /* [text](url) */
            i32 j=i+1; while(j<len && s[j]!=']') j++;
            if (j<len && j+1<len && s[j+1]=='(') {
                i32 p=j+2; while(p<len && s[p]!=')') p++;
                if (p<len) { LIV_RUN(i+1,j,link); i=p+1; continue; }
            }
        }
        if (c=='#' && (i==0 || s[i-1]==' ' || s[i-1]=='\t') && i+1<len &&
            (isalnum((unsigned char)s[i+1])||s[i+1]=='_'||s[i+1]=='/')) {   /* #tag */
            i32 j=i+1; while(j<len && (isalnum((unsigned char)s[j])||s[j]=='_'||s[j]=='-'||s[j]=='/')) j++;
            LIV_RUN(i,j,tagc); i=j; continue;
        }
        { u32 cp; u32 nb=utf8_decode((const u8*)s+i,(usize)(len-i),&cp);
          if(!nb){i++;continue;} if(cp>=32){ renderer_push_glyph(r,x,y,cp,fg); x+=cw; } i+=nb; }
    }
#undef LIV_RUN
    return x;
}

/* Active-line variant of fb_live_inline: the cursor's source line is shown RAW
 * (every marker stays visible so editing is literal) but the markers are dimmed
 * and the wrapped text is tinted, so the raw line is readable instead of flat.
 * Every codepoint advances exactly one cell — matching the caret's per-codepoint
 * column count in the caller — so the caret stays glued to the right glyph.
 * `marker_len` is the leading block marker (heading #, quote >, list/-[ ] …). */
static void fb_live_inline_raw(Renderer *r, const Theme *th, const char *s, i32 len,
                               i32 marker_len, f32 x, f32 y, f32 cw, f32 ch, Color base) {
    Color bright = { fminf(1.0f,base.r+0.18f), fminf(1.0f,base.g+0.18f), fminf(1.0f,base.b+0.18f), base.a };
    Color dim    = { base.r*0.50f, base.g*0.50f, base.b*0.50f, base.a };
    Color link   = th ? th->ansi[12] : (Color){0.45f,0.62f,1.0f,1.0f};
    Color tagc   = th ? th->ansi[2]  : (Color){0.45f,0.78f,0.55f,1.0f};
    Color codefg = th ? th->ansi[11] : (Color){0.92f,0.80f,0.52f,1.0f};
    Color codebg = (Color){1,1,1,0.07f};
    Color hlbg   = (Color){0.95f,0.85f,0.20f,0.22f};
    f32 gx = x;
    /* Emit [from,to): one cell per codepoint (advance even for control chars so
     * the run stays column-aligned with the caret's codepoint count). */
#define RAW_RUN(from,to,col) do { for (i32 _k=(from); _k<(to);) { u32 _cp; \
        u32 _nb=utf8_decode((const u8*)s+_k,(usize)(len-_k),&_cp); if(!_nb){_k++;continue;} \
        if(_cp>=32) renderer_push_glyph(r,gx,y,_cp,(col)); gx+=cw; _k+=_nb; } } while(0)
    i32 i = 0;
    if (marker_len > 0) { i32 m = marker_len < len ? marker_len : len; RAW_RUN(0,m,dim); i = m; }
    while (i < len) {
        unsigned char c = (unsigned char)s[i];
        if (c=='*' && i+1<len && s[i+1]=='*') {                 /* **bold** */
            i32 j=i+2; while(j+1<len && !(s[j]=='*'&&s[j+1]=='*')) j++;
            if (j+1<len) { RAW_RUN(i,i+2,dim); RAW_RUN(i+2,j,bright); RAW_RUN(j,j+2,dim); i=j+2; continue; }
        }
        if (c=='*' || c=='_') {                                 /* *italic* _italic_ */
            i32 j=i+1; while(j<len && s[j]!=(char)c) j++;
            if (j<len && j>i+1) { RAW_RUN(i,i+1,dim); RAW_RUN(i+1,j,base); RAW_RUN(j,j+1,dim); i=j+1; continue; }
        }
        if (c=='`') {                                           /* `code` */
            i32 j=i+1; while(j<len && s[j]!='`') j++;
            if (j<len) {
                f32 span=(f32)utf8_len((const u8*)s+i,(usize)(j+1-i))*cw;
                renderer_draw_rect(r, gx-1.0f, y-1.0f, span+2.0f, ch+2.0f, codebg);
                RAW_RUN(i,i+1,dim); RAW_RUN(i+1,j,codefg); RAW_RUN(j,j+1,dim); i=j+1; continue;
            }
        }
        if (c=='~' && i+1<len && s[i+1]=='~') {                 /* ~~strike~~ */
            i32 j=i+2; while(j+1<len && !(s[j]=='~'&&s[j+1]=='~')) j++;
            if (j+1<len) { RAW_RUN(i,i+2,dim); RAW_RUN(i+2,j,dim); RAW_RUN(j,j+2,dim); i=j+2; continue; }
        }
        if (c=='=' && i+1<len && s[i+1]=='=') {                 /* ==highlight== */
            i32 j=i+2; while(j+1<len && !(s[j]=='='&&s[j+1]=='=')) j++;
            if (j+1<len) {
                f32 span=(f32)utf8_len((const u8*)s+i,(usize)(j+2-i))*cw;
                renderer_draw_rect(r, gx-1.0f, y, span+2.0f, ch, hlbg);
                RAW_RUN(i,i+2,dim); RAW_RUN(i+2,j,base); RAW_RUN(j,j+2,dim); i=j+2; continue;
            }
        }
        if (c=='[' && i+1<len && s[i+1]=='[') {                 /* [[wiki|alias]] */
            i32 j=i+2; while(j+1<len && !(s[j]==']'&&s[j+1]==']')) j++;
            if (j+1<len) { RAW_RUN(i,i+2,dim); RAW_RUN(i+2,j,link); RAW_RUN(j,j+2,dim); i=j+2; continue; }
        }
        if (c=='[') {                                           /* [text](url) */
            i32 j=i+1; while(j<len && s[j]!=']') j++;
            if (j<len && j+1<len && s[j+1]=='(') {
                i32 q=j+2; while(q<len && s[q]!=')') q++;
                if (q<len) {
                    RAW_RUN(i,i+1,dim); RAW_RUN(i+1,j,base);     /* [text] */
                    RAW_RUN(j,j+2,dim); RAW_RUN(j+2,q,dim); RAW_RUN(q,q+1,dim); /* ](url) */
                    i=q+1; continue;
                }
            }
        }
        if (c=='#' && (i==0 || s[i-1]==' ' || s[i-1]=='\t') && i+1<len &&
            (isalnum((unsigned char)s[i+1])||s[i+1]=='_'||s[i+1]=='/')) {   /* #tag */
            i32 j=i+1; while(j<len && (isalnum((unsigned char)s[j])||s[j]=='_'||s[j]=='-'||s[j]=='/')) j++;
            RAW_RUN(i,j,tagc); i=j; continue;
        }
        { u32 cp; u32 nb=utf8_decode((const u8*)s+i,(usize)(len-i),&cp);
          if(!nb){i++;continue;} if(cp>=32) renderer_push_glyph(r,gx,y,cp,base); gx+=cw; i+=nb; }
    }
#undef RAW_RUN
}

void fb_render_viewer(FileBrowser *fb, Renderer *r, f32 x, f32 y, f32 w, f32 h, f32 dpi, const Theme *theme, f32 opacity) {
    if (fb->view_mode == FVIEW_GRAPH) { fb_render_graph(fb, r, x, y, w, h, dpi, theme); return; }
    if (fb->view_mode == FVIEW_NONE || !fb->view_content) return;

    /* Fixed UI scale for viewer */
    f32 cw = 8.0f * dpi;
    f32 ch = 16.0f * dpi;
    g_ui_step = cw;
    renderer_set_ui_scale(r, cw, ch);
    f32 pad = 12 * dpi;

    /* Theme-derived colors for the editor. The viewer is part of the app
     * chrome (lives next to sidebar/toolbar), so its panel/title/code
     * backgrounds fade together with the window opacity — otherwise the
     * viewer stays fully opaque over a translucent terminal body. */
    if (opacity < 0.0f) opacity = 0.0f;
    if (opacity > 1.0f) opacity = 1.0f;
    Color bg_color     = theme ? theme->bg      : (Color){0.10f, 0.10f, 0.12f, 1.0f};
    Color fg_color     = theme ? theme->fg      : (Color){0.82f, 0.82f, 0.85f, 1.0f};
    Color border_color = theme ? theme->border  : (Color){0.18f, 0.18f, 0.20f, 1.0f};
    Color sel_color    = theme ? theme->selection: (Color){0.20f, 0.30f, 0.50f, 1.0f};
    bg_color.a *= opacity;
    /* Title bar: slightly lighter than bg */
    Color title_bg = bg_color;
    title_bg.r = fminf(1.0f, title_bg.r + 0.04f);
    title_bg.g = fminf(1.0f, title_bg.g + 0.04f);
    title_bg.b = fminf(1.0f, title_bg.b + 0.04f);
    /* Syntax colors from ANSI palette */
    Color clr_keyword = theme ? theme->ansi[4]  : (Color){0.55f, 0.65f, 0.95f, 1.0f}; /* blue */
    Color clr_string  = theme ? theme->ansi[3]  : (Color){0.80f, 0.58f, 0.36f, 1.0f}; /* yellow/orange */
    Color clr_comment = theme ? theme->ansi[2]  : (Color){0.45f, 0.52f, 0.45f, 1.0f}; /* green */
    Color clr_number  = theme ? theme->ansi[10] : (Color){0.70f, 0.80f, 0.55f, 1.0f}; /* bright green */
    Color clr_linenum = border_color;
    clr_linenum.a = 0.6f;
    Color clr_heading __attribute__((unused)) =
        theme ? theme->ansi[12] : (Color){0.5f, 0.75f, 1.0f, 1.0f};
    Color clr_code_bg = bg_color;
    clr_code_bg.r = fmaxf(0.0f, clr_code_bg.r - 0.04f);
    clr_code_bg.g = fmaxf(0.0f, clr_code_bg.g - 0.04f);
    clr_code_bg.b = fmaxf(0.0f, clr_code_bg.b - 0.04f);
    (void)sel_color;

    /* Left-edge hairline + soft outward shadow that bleeds into the terminal
     * area, giving the viewer a subtle elevated look (mirror of the sidebar's
     * right-edge shadow). Drawn first so the bg fill paints over the inner
     * half of the shadow gradient. */
    Color edge_clr = border_color; edge_clr.a = fmaxf(edge_clr.a, 0.55f);
    renderer_draw_rrect(r,
        x, y, 1.0f, h,
        edge_clr,
        0.0f, 0.0f, 0.0f, 0.0f,
        10.0f * dpi, 0.22f, -5.0f * dpi, 0.0f);

    /* Background */
    renderer_draw_rect(r, x, y, w, h, bg_color);

    f32 title_h = VIEWER_TITLE_H_PT * dpi;
    f32 content_y = y + title_h + 4;
    f32 content_h = h - title_h - 8;
    i32 visible_lines = (i32)(content_h / ch);

    if (fb->view_mode == FVIEW_IMAGE && fb->view_image_rgba &&
        fb->view_image_w > 0 && fb->view_image_h > 0) {
        /* Animated GIF — wall-clock catch-up.
         *
         * The render may be called at any rate (ProMotion can throttle
         * the display down to ~24 Hz when the user is idle, dragging
         * the effective render rate with it). To stay correct we don't
         * advance "one frame per render call" — we compute the elapsed
         * wall-clock time since the start of the loop and pick the
         * frame that *should* be on screen now.
         *
         * stb stores per-frame delays in MILLISECONDS (the GIF wire
         * format uses centiseconds; stbi_load_gif_from_memory
         * pre-multiplies by 10). 0-delay frames mean "as fast as
         * possible" — we clamp to 100 ms so a malformed file doesn't
         * peg the CPU.
         *
         * `view_image_frame_anchor` is the wall-clock time at which
         * the *current* frame_index started. After catch-up it points
         * at the start of whatever frame the algorithm landed on, so
         * future advances measure forward from there. This keeps
         * timing stable across frame drops. */
        if (fb->view_image_is_animated && fb->view_image_frame_count > 1 &&
            fb->view_image_frame_delays) {
            f64 now = platform_time_sec();

            /* Total loop duration — one full pass through every frame. */
            f64 loop_total = 0.0;
            for (i32 i = 0; i < fb->view_image_frame_count; i++) {
                i32 ms = fb->view_image_frame_delays[i];
                if (ms <= 0) ms = 100;
                loop_total += (f64)ms / 1000.0;
            }
            if (loop_total < 0.001) loop_total = 0.001;

            /* Elapsed since the current frame_index began playing.
             * If `now` jumps far ahead (sleep/suspend), we mod by
             * loop_total so we never iterate more than one full loop
             * — bounded work regardless of how long we slept. */
            f64 elapsed = now - fb->view_image_frame_anchor;
            if (elapsed >= loop_total) {
                f64 whole_loops = floor(elapsed / loop_total);
                fb->view_image_frame_anchor += whole_loops * loop_total;
                elapsed -= whole_loops * loop_total;
            }

            /* Walk forward from the current frame, consuming each
             * frame's delay until we find the one that contains `now`.
             * Bounded by frame_count iterations because elapsed is
             * already < loop_total. */
            i32 idx = fb->view_image_frame_index;
            for (i32 guard = 0; guard < fb->view_image_frame_count; guard++) {
                i32 ms = fb->view_image_frame_delays[idx];
                if (ms <= 0) ms = 100;
                f64 dur = (f64)ms / 1000.0;
                if (elapsed < dur) break;
                elapsed -= dur;
                fb->view_image_frame_anchor += dur;
                idx = (idx + 1) % fb->view_image_frame_count;
            }
            if (idx != fb->view_image_frame_index) {
                fb->view_image_frame_index = idx;
                fb->view_image_gen++;
            }

            /* Schedule the main loop's wake-up at the next frame
             * boundary so we don't burn cycles between frames. */
            i32 cur_ms = fb->view_image_frame_delays[idx];
            if (cur_ms <= 0) cur_ms = 100;
            f64 cur_dur = (f64)cur_ms / 1000.0;
            fb->view_image_next_frame_at = fb->view_image_frame_anchor + cur_dur;
        } else {
            fb->view_image_next_frame_at = 0.0;
        }

        /* Aspect-fit the decoded image into the content area, then apply
         * the user's zoom + pan. dx/dy is the top-left of the drawn image
         * in viewer coords; we scissor-clip so a zoomed image doesn't
         * spill over the title bar / panel border. */
        f32 area_x = x + pad;
        f32 area_y = content_y;
        f32 area_w = w - 2 * pad;
        f32 area_h = content_h;
        if (area_w < 1.0f) area_w = 1.0f;
        if (area_h < 1.0f) area_h = 1.0f;

        f32 sx = area_w / (f32)fb->view_image_w;
        f32 sy = area_h / (f32)fb->view_image_h;
        f32 fit = sx < sy ? sx : sy;
        f32 zoom = fb->view_image_zoom > 0.0f ? fb->view_image_zoom : 1.0f;
        f32 dw = (f32)fb->view_image_w * fit * zoom;
        f32 dh = (f32)fb->view_image_h * fit * zoom;

        f32 dx = area_x + (area_w - dw) * 0.5f + fb->view_image_pan_x;
        f32 dy = area_y + (area_h - dh) * 0.5f + fb->view_image_pan_y;

        /* Checkerboard so transparent PNGs don't blend into the panel
         * background and look "lost". 12-pixel cells, alpha 0.4. */
        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_push_scissor(r, area_x, area_y, area_w, area_h);
        f32 cell = 12.0f * dpi;
        if (cell < 4.0f) cell = 4.0f;
        Color cb_a = {0.18f, 0.18f, 0.20f, 0.55f};
        Color cb_b = {0.10f, 0.10f, 0.12f, 0.55f};
        f32 first_x = dx - fmodf(dx - area_x, cell * 2.0f);
        f32 first_y = dy - fmodf(dy - area_y, cell * 2.0f);
        for (f32 yy = first_y; yy < dy + dh; yy += cell) {
            for (f32 xx = first_x; xx < dx + dw; xx += cell) {
                if (xx + cell <= dx || yy + cell <= dy) continue;
                int gx = (int)floorf((xx - first_x) / cell);
                int gy = (int)floorf((yy - first_y) / cell);
                Color cc = ((gx + gy) & 1) ? cb_a : cb_b;
                f32 ox = xx > dx ? xx : dx;
                f32 oy = yy > dy ? yy : dy;
                f32 ow = (xx + cell) - ox;
                f32 oh = (yy + cell) - oy;
                if (ox + ow > dx + dw) ow = (dx + dw) - ox;
                if (oy + oh > dy + dh) oh = (dy + dh) - oy;
                if (ow > 0 && oh > 0)
                    renderer_draw_rect(r, ox, oy, ow, oh, cc);
            }
        }
        renderer_flush_rects(r);

        /* Cached upload: stable key per file (FB pointer XOR generation),
         * so opening another image bumps the generation and invalidates
         * the previous texture without leaking. For animated GIFs the
         * generation also bumps on every frame change so the cache
         * re-uploads the new frame's pixels. */
        const void *key = (const void *)fb;
        usize frame_stride = (usize)fb->view_image_w * (usize)fb->view_image_h * 4u;
        const u8 *frame_px = fb->view_image_rgba +
                             (usize)fb->view_image_frame_index * frame_stride;
        renderer_draw_image_cached(r, key, fb->view_image_gen,
                                   frame_px,
                                   fb->view_image_w, fb->view_image_h,
                                   floorf(dx), floorf(dy),
                                   floorf(dw), floorf(dh));
        renderer_pop_scissor(r);
    } else if (fb->view_mode == FVIEW_MARKDOWN && fb->md_doc && !fb->md_raw_mode) {
        /* Lazily allocate the click-target buffer — only paid the first
         * time a markdown doc is rendered. Freed in fb_destroy. */
        if (!fb->md_link_rects) {
            fb->md_link_rects = (MdLinkRect *)calloc(FB_MD_LINK_CAP,
                                                    sizeof(MdLinkRect));
        }
        if (!fb->md_task_rects) {
            fb->md_task_rects = (MdTaskRect *)calloc(FB_MD_TASK_CAP,
                                                     sizeof(MdTaskRect));
        }
        if (!fb->md_glyph_rects) {
            fb->md_glyph_rects = (MdGlyphRect *)calloc(FB_MD_GLYPH_CAP,
                                                       sizeof(MdGlyphRect));
        }
        if (!fb->md_outline) {
            fb->md_outline = (MdOutlineItem *)calloc(FB_MD_OUTLINE_CAP,
                                                     sizeof(MdOutlineItem));
        }
        fb->md_link_count = 0;
        fb->md_task_count = 0;
        /* NB: md_glyph_count is intentionally NOT reset here. md_render clears
         * it at its top (via glyph_rect_count), and fb_md_draw_selection runs
         * just before md_render reading the previous frame's glyph rects to
         * paint the highlight behind the text — zeroing it here would blank
         * the selection. */
        /* Cache the content origin so the mouse/key handlers can map
         * framebuffer coords into content space without re-deriving the
         * (sidebar vs. tab) viewer layout. */
        fb->md_origin_x = x + pad;
        fb->md_origin_y = content_y;
        MdRenderCtx mctx = {
            .r        = r,
            .theme    = theme,
            .x        = x + pad,
            .y        = content_y,
            .w        = w - 2 * pad,
            .h        = content_h,
            .cw       = cw,
            .ch       = ch,
            .dpi      = dpi,
            .scroll_y = fb->view_scroll_px,
            .images   = fb->md_images,
            .base_dir = fb->md_doc->base_dir,
            .src_base = (const u8 *)fb->view_content,
            .src_len  = (u32)fb->view_size,
            .link_rects      = fb->md_link_rects,
            .link_rect_cap   = fb->md_link_rects ? FB_MD_LINK_CAP : 0u,
            .link_rect_count = fb->md_link_rects ? &fb->md_link_count : NULL,
            .glyph_rects      = fb->md_glyph_rects,
            .glyph_rect_cap   = fb->md_glyph_rects ? FB_MD_GLYPH_CAP : 0u,
            .glyph_rect_count = fb->md_glyph_rects ? &fb->md_glyph_count : NULL,
            .outline          = fb->md_outline,
            .outline_cap      = fb->md_outline ? FB_MD_OUTLINE_CAP : 0u,
            .outline_count    = fb->md_outline ? &fb->md_outline_count : NULL,
            .task_rects       = fb->md_task_rects,
            .task_rect_cap    = fb->md_task_rects ? FB_MD_TASK_CAP : 0u,
            .task_rect_count  = fb->md_task_rects ? &fb->md_task_count : NULL,
        };
        /* Clip markdown output to the viewer body. Without this, blocks
         * scrolled past the top render into the terminal/sidebar behind
         * the panel — visible as a faint "watermark" of older content
         * because the rest of the UI didn't paint over those pixels. */
        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_push_scissor(r, x, content_y, w, content_h);
        /* Selection highlight FIRST, so it paints behind this frame's text.
         * md_render flushes its own glyph batch before returning, so pushing
         * the highlight after it would draw on top and dim the text. md_render
         * clears the glyph-rect collector only at its top, so the rects from
         * the previous frame are still intact here. Index mapping uses the
         * captured md_glyph_scroll_px (see md_sel_index_at) so endpoints land
         * on the right glyphs even mid-scroll; the visual highlight still
         * lags one frame in framebuffer pixels, which is imperceptible. */
        fb_md_draw_selection(fb, r, theme);
        fb_find_draw_matches(fb, r, theme);   /* find highlights, also behind text */
        renderer_flush_rects(r);
        fb->view_content_px = md_render(fb->md_doc, &mctx);
        /* Record the scroll the rects were captured at so the next
         * md_sel_index_at call converts them back to doc space using THIS
         * frame's scroll, not whatever it has drifted to by then. */
        fb->md_glyph_scroll_px = fb->view_scroll_px;
        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        renderer_pop_scissor(r);
    } else if (fb->view_mode == FVIEW_MARKDOWN && fb->md_raw_mode) {
        /* ---- Markdown Live Preview editor ----
         * Each SOURCE line is drawn independently: the line holding the cursor
         * is shown raw (syntax markers visible, dimmed) with the caret, every
         * other line is rendered styled with its markers hidden. Line-based, so
         * it maps 1:1 onto the existing line/col edit buffer + cursor. */
        static const f32 H_SCALE[7] = { 1.0f, 1.6f, 1.4f, 1.25f, 1.12f, 1.05f, 1.0f };
        f32 lx = x + pad;
        f32 maxx = x + w - pad;
        Color fg   = theme ? theme->fg : (Color){0.86f,0.86f,0.90f,1.0f};
        Color dimc = { fg.r*0.5f, fg.g*0.5f, fg.b*0.5f, fg.a };
        Color hcol = { fminf(1.0f,fg.r+0.12f), fminf(1.0f,fg.g+0.12f), fminf(1.0f,fg.b+0.12f), fg.a };
        Color accent = theme ? theme->ansi[12] : (Color){0.45f,0.62f,1.0f,1.0f};

        fb->md_origin_x = lx; fb->md_origin_y = content_y;
        fb->live_cw = cw; fb->live_ch = ch; fb->live_dpi = dpi;
        fb->md_link_count = 0;   /* no clickable read-mode link rects in edit mode */
        fb->md_task_count = 0;

        renderer_flush_rects(r); renderer_flush_glyphs(r);
        renderer_push_scissor(r, x, content_y, w, content_h);

        /* Walk to the first visible line, tracking fenced-code state. */
        char *p = fb->view_content ? fb->view_content : (char*)"";
        i32 li = 0;
        bool in_code = false;
        for (; li < fb->view_scroll && *p; li++) {
            char *eol = strchr(p, '\n'); i32 ll = eol ? (i32)(eol-p) : (i32)strlen(p);
            i32 ind = 0; while (ind < ll && (p[ind]==' '||p[ind]=='\t')) ind++;
            if (ll-ind >= 3 && (p[ind]=='`'||p[ind]=='~') && p[ind]==p[ind+1] && p[ind+1]==p[ind+2])
                in_code = !in_code;
            p = eol ? eol+1 : p+ll;
        }

        f32 cy = content_y;
        for (; *p && cy < content_y + content_h; li++) {
            char *eol = strchr(p, '\n');
            i32 ll = eol ? (i32)(eol - p) : (i32)strlen(p);
            i32 ind = 0; while (ind < ll && (p[ind]==' '||p[ind]=='\t')) ind++;
            const char *t = p + ind; i32 tl = ll - ind;        /* trimmed line */
            bool active = (li == fb->cursor_line);

            /* classify */
            i32 hlvl = 0;
            bool is_fence = (tl >= 3 && (t[0]=='`'||t[0]=='~') && t[0]==t[1] && t[1]==t[2]);
            bool was_code = in_code;
            if (is_fence) in_code = !in_code;
            bool code_line = was_code && !is_fence;
            i32 task = -1;          /* 0 unchecked, 1 checked */
            bool bullet = false, ordered = false, quote = false, hr = false;
            i32 marker_len = 0;     /* bytes of the leading block marker (incl trailing space) */
            i32 onum = 0;
            if (!code_line && !is_fence) {
                if (t[0]=='#') { i32 k=0; while(k<tl&&k<6&&t[k]=='#')k++; if(k>0&&k<tl&&t[k]==' '){hlvl=k; marker_len=ind+k+1;} }
                if (!hlvl && tl>=2 && t[0]=='>' ) { quote=true; marker_len=ind+1; if(tl>1&&t[1]==' ')marker_len++; }
                if (!hlvl && !quote && tl>=3 && (t[0]=='-'||t[0]=='*'||t[0]=='_')) {
                    bool allsame=true; for(i32 k=0;k<tl;k++){char ch=t[k]; if(ch!=t[0]&&ch!=' '){allsame=false;break;}}
                    if (allsame) hr=true;
                }
                if (!hlvl && !quote && !hr) {
                    if (tl>=5 && (t[0]=='-'||t[0]=='*'||t[0]=='+') && t[1]==' ' && t[2]=='[' &&
                        (t[3]==' '||t[3]=='x'||t[3]=='X') && t[4]==']' ) {
                        task = (t[3]=='x'||t[3]=='X') ? 1 : 0; marker_len = ind+6;
                    } else if (tl>=2 && (t[0]=='-'||t[0]=='*'||t[0]=='+') && t[1]==' ') {
                        bullet=true; marker_len=ind+2;
                    } else { i32 k=0; while(k<tl&&isdigit((unsigned char)t[k]))k++;
                        if (k>0&&k+1<tl&&t[k]=='.'&&t[k+1]==' '){ordered=true;onum=atoi(t);marker_len=ind+k+2;} }
                }
            }
            f32 lcw = cw, lch = ch;
            if (hlvl) { lcw = roundf(cw * H_SCALE[hlvl]); lch = ch * H_SCALE[hlvl]; }
            f32 lh = lch + (hlvl ? 6.0f*dpi : 2.0f*dpi);

            if (cy + lh < content_y) { cy += lh; p = eol?eol+1:p+ll; continue; }  /* above view */

            renderer_set_ui_scale(r, lcw, lch);
            f32 indent_px = (f32)(ind) * cw + (quote ? 10.0f*dpi : 0.0f);

            /* Find-match highlight for this raw source line (behind the text).
             * Raw lines render every codepoint at one cell, so byte offsets map
             * straight to columns. */
            if (fb->find_active && fb->find_match_count)
                fb_find_draw_line(fb, r, (usize)(p - fb->view_content), ll,
                                  lx, cy, lcw, lch, theme);

            if (active) {
                /* Raw source: markers visible but colored (dimmed markers,
                 * tinted text) so the edited line reads like highlighted source
                 * instead of a flat run, + caret. */
                fb_live_inline_raw(r, theme, p, ll, marker_len, lx, cy, lcw, lch, fg);
                i32 cb = fb->cursor_col; if (cb < 0) cb = 0; if (cb > ll) cb = ll;
                /* cursor_col is a BYTE offset; the caret's visual column is the
                 * count of codepoints before it (a multi-byte char is 1 column). */
                i32 vcol = 0;
                for (i32 k = 0; k < cb; ) {
                    u32 cp2; u32 nb2 = utf8_decode((const u8*)p+k,(usize)(ll-k),&cp2);
                    k += nb2 ? (i32)nb2 : 1; vcol++;
                }
                renderer_flush_glyphs(r);
                Color cur = theme ? theme->cursor : (Color){0.8f,0.8f,0.85f,0.9f}; cur.a = 0.9f;
                renderer_draw_rect(r, lx + (f32)vcol * lcw, cy, fmaxf(2.0f,2.0f*dpi), lch, cur);
                /* Remember the caret pixel position so the autocomplete popup
                 * can anchor under it next frame. */
                fb->caret_px_x = lx + (f32)vcol * lcw;
                fb->caret_px_y = cy;
                fb->caret_px_h = lch;
            } else if (is_fence || code_line) {
                if (!is_fence) {                                   /* code interior (mono, dim) */
                    renderer_draw_rect(r, lx-2.0f*dpi, cy-1.0f*dpi, maxx-lx+4.0f*dpi, lch+2.0f*dpi, (Color){1,1,1,0.05f});
                    fb_live_inline(r, NULL, p, ll, lx, cy, cw, ch, dimc); /* literal, no styling */
                }
                /* fence line itself: hidden */
            } else if (hr) {
                renderer_draw_rect(r, lx, cy + lch*0.5f, maxx - lx, fmaxf(1.0f,1.0f*dpi), dimc);
            } else if (hlvl) {
                fb_live_inline(r, theme, t + (marker_len-ind), tl - (marker_len-ind), lx, cy, lcw, lch, hcol);
            } else if (task >= 0) {
                Color box = task ? accent : dimc;
                renderer_push_glyph(r, lx+indent_px, cy, task ? 0x2611 : 0x2610, box);  /* ☑ / ☐ */
                fb_live_inline(r, theme, t+(marker_len-ind), tl-(marker_len-ind),
                               lx+indent_px+lcw*1.5f, cy, cw, ch, fg);
            } else if (bullet) {
                renderer_push_glyph(r, lx+indent_px, cy, 0x2022, accent);              /* • */
                fb_live_inline(r, theme, t+(marker_len-ind), tl-(marker_len-ind),
                               lx+indent_px+lcw*1.5f, cy, cw, ch, fg);
            } else if (ordered) {
                char num[12]; i32 nn = snprintf(num, sizeof num, "%d.", onum);
                for (i32 k=0;k<nn;k++) renderer_push_glyph(r, lx+indent_px+(f32)k*cw, cy, (u32)num[k], accent);
                fb_live_inline(r, theme, t+(marker_len-ind), tl-(marker_len-ind),
                               lx+indent_px+(f32)(nn+1)*cw, cy, cw, ch, fg);
            } else if (quote) {
                renderer_draw_rect(r, lx, cy, 3.0f*dpi, lch, accent);
                fb_live_inline(r, theme, t+(marker_len-ind), tl-(marker_len-ind), lx+indent_px, cy, cw, ch, dimc);
            } else {
                fb_live_inline(r, theme, t, tl, lx+indent_px, cy, cw, ch, fg);
            }

            cy += lh;
            p = eol ? eol + 1 : p + ll;
        }
        fb->view_content_px = cy - content_y + (f32)fb->view_scroll * ch;  /* rough scroll extent */
        renderer_flush_rects(r); renderer_flush_glyphs(r);
        renderer_pop_scissor(r);
        renderer_reset_ui_scale(r);

    } else {
        /* Code / plain text with line numbers and syntax */
        char *line = fb->view_content;
        i32 line_num = 0;
        f32 gutter = 5 * cw; /* line number gutter */

        /* Skip to scroll offset */
        for (i32 s = 0; s < fb->view_scroll && *line; s++) {
            char *nl = strchr(line, '\n');
            line = nl ? nl + 1 : line + strlen(line);
            line_num++;
        }

        for (i32 vi = 0; vi < visible_lines && *line; vi++) {
            char *eol = strchr(line, '\n');
            i32 len = eol ? (i32)(eol - line) : (i32)strlen(line);
            f32 ly = content_y + vi * ch;
            line_num++;

            /* Same async-raster trap as draw_text_clipped: pre-cache any
             * non-ASCII glyphs in this line before push_glyph runs the
             * codepoint scan below. */
            font_warm_text_glyphs_n(&r->font, line, (usize)len);

            /* Line number */
            char lnbuf[8];
            snprintf(lnbuf, sizeof(lnbuf), "%4d", line_num);
            draw_text_clipped(r, lnbuf, x + 4, ly, clr_linenum, gutter);

            /* Gutter separator */
            renderer_draw_rect(r, x + gutter + 2, content_y, 1, content_h, border_color);

            /* Code content with basic syntax highlighting */
            f32 cx = x + gutter + 8;
            i32 ci = 0;
            bool in_string = false;
            char string_char = 0;
            bool in_comment = false;

            /* Check for // comment */
            if (len >= 2 && line[0] == '/' && line[1] == '/') in_comment = true;
            /* Check for # comment (Python/shell) */
            if (len >= 1 && line[0] == '#') in_comment = true;

            while (ci < len && cx < x + w - pad) {
                Color cc = fg_color; /* default: theme foreground */

                if (in_comment) {
                    cc = clr_comment;
                } else if (in_string) {
                    cc = clr_string;
                    if (line[ci] == string_char && (ci == 0 || line[ci-1] != '\\')) {
                        in_string = false;
                    }
                } else if (line[ci] == '"' || line[ci] == '\'') {
                    cc = clr_string;
                    in_string = true;
                    string_char = line[ci];
                } else if (line[ci] >= '0' && line[ci] <= '9') {
                    cc = clr_number;
                } else if (line[ci] == '/' && ci + 1 < len && line[ci+1] == '/') {
                    in_comment = true;
                    cc = clr_comment;
                } else if (is_ident_char(line[ci]) && (ci == 0 || !is_ident_char(line[ci-1]))) {
                    /* Check if this is a keyword */
                    i32 wlen = 0;
                    while (ci + wlen < len && is_ident_char(line[ci + wlen])) wlen++;
                    if (is_keyword(line + ci, wlen)) {
                        cc = clr_keyword;
                        /* Render entire keyword (ASCII identifiers — safe to step byte-by-byte). */
                        for (i32 k = 0; k < wlen && cx < x + w - pad; k++) {
                            u32 cp = (u32)(unsigned char)line[ci];
                            if (cp >= 32)
                                renderer_push_glyph(r, cx, ly, cp, cc);
                            cx += cw;
                            ci++;
                        }
                        continue;
                    }
                }

                /* UTF-8 aware emit: decode one codepoint, advance ci by the
                 * actual byte count so multi-byte characters (Turkish ş,
                 * emoji, CJK) render as a single glyph instead of mojibake. */
                u32 cp = 0;
                u32 nbytes = utf8_decode((const u8 *)line + ci, (usize)(len - ci), &cp);
                if (nbytes == 0) { ci++; continue; }
                if (cp >= 32)
                    renderer_push_glyph(r, cx, ly, cp, cc);
                cx += cw;
                ci += (i32)nbytes;
            }

            line = eol ? eol + 1 : line + len;
        }

        /* Editor cursor */
        if (fb->editor_mode) {
            i32 cline = fb->cursor_line - fb->view_scroll;
            if (cline >= 0 && cline < visible_lines) {
                i32 line_len = editor_line_len(fb, fb->cursor_line);
                i32 cur_col = fb->cursor_col;
                if (cur_col < 0) cur_col = 0;
                if (cur_col > line_len) cur_col = line_len;
                /* cursor_col is a byte offset — visual column counts codepoints. */
                const char *cls = editor_line_start(fb, fb->cursor_line);
                i32 vcol = 0;
                for (i32 k = 0; k < cur_col; ) {
                    u32 cp3; u32 nb3 = utf8_decode((const u8*)cls+k,(usize)(cur_col-k),&cp3);
                    k += nb3 ? (i32)nb3 : 1; vcol++;
                }
                f32 cur_x = x + gutter + 8 + vcol * cw;
                f32 cur_y = content_y + cline * ch;
                Color cur_clr = theme ? theme->cursor : (Color){0.8f, 0.8f, 0.85f, 0.9f};
                cur_clr.a = 0.9f;
                renderer_draw_rect(r, cur_x, cur_y, 2, ch, cur_clr);
            }
        }
    }

    /* Sticky title bar — drawn AFTER markdown / code content so a partial
     * top-line bleed during scroll gets overdrawn rather than peeking
     * through (the renderer has no GPU scissor for the viewport). The bar
     * is fixed at y=top of the viewer regardless of scroll position. */
    {
        const char *fname = strrchr(fb->view_path, '/');
        fname = fname ? fname + 1 : fb->view_path;

        renderer_flush_rects(r);
        renderer_flush_glyphs(r);
        /* Pin the chrome glyph scale to the viewer's 8x16 explicitly. The
         * markdown-edit branch ends with renderer_reset_ui_scale (→ the atlas's
         * natural ~25x49 metrics), so without this the title text (filename,
         * [e] badge, Read/Edit toggle) rendered oversized in edit mode while
         * read mode — which leaves the scale at 8x16 — looked correct. */
        renderer_set_ui_scale(r, cw, ch);
        f32 title_pad = VIEWER_TITLE_PAD_PT * dpi;
        f32 close_w = VIEWER_CLOSE_W_PT * dpi;
        f32 close_x = x + w - close_w;
        f32 toggle_w = (fb->view_mode == FVIEW_MARKDOWN)
                     ? VIEWER_MD_TOGGLE_W_PT * dpi : 0.0f;
        f32 toggle_gap = 6.0f * dpi;
        f32 toggle_x = close_x - toggle_gap - toggle_w;
        f32 actions_x = toggle_w > 0.0f ? toggle_x : close_x;

        renderer_draw_rect(r, x, y, w, title_h, title_bg);
        Color title_line = border_color;
        title_line.a = fminf(0.75f, fmaxf(0.32f, title_line.a * opacity));
        renderer_draw_rect(r, x, y + title_h - 1.0f, w, 1.0f, title_line);

        /* Live word/read-time readout (markdown edit mode only). Right-align it
         * just left of the Read/Edit toggle and dim it so it sits quietly; the
         * filename + unsaved-pip region (info_right) shrinks to make room. */
        f32 info_right = actions_x;
        char  stats_buf[48]; f32 stats_w = 0.0f, stats_x = 0.0f;
        bool  show_stats = false;
        if (fb->editor_mode && fb->view_mode == FVIEW_MARKDOWN) {
            u32 words = 0, chars_ct = 0, rmin = 0;
            fb_editor_doc_stats(fb, &words, &chars_ct, &rmin);
            (void)chars_ct;
            snprintf(stats_buf, sizeof stats_buf, "%u w \xc2\xb7 %u min", words, rmin);
            u32 cps = 0;                       /* glyph count == codepoints, not bytes */
            for (const u8 *q = (const u8 *)stats_buf; *q; q++)
                if ((*q & 0xC0) != 0x80) cps++;
            stats_w = (f32)cps * cw;
            stats_x = actions_x - 8.0f * dpi - stats_w;
            if (stats_x > x + title_pad + 6.0f * cw) {   /* only if it doesn't crowd the name */
                info_right = stats_x - 8.0f * dpi;
                show_stats = true;
            }
        }

        f32 title_text_x = x + title_pad;
        f32 title_max_w = info_right - title_text_x - 8.0f * dpi;
        if (title_max_w < 0.0f) title_max_w = 0.0f;
        /* True drawn glyph height. UI text is rasterized to the font's real
         * cell aspect (taller than the layout's nominal `ch` for fonts like
         * JetBrains Mono), so centering by `ch` parks text slightly low and
         * looks cramped inside tight controls. Center by the actual height. */
        f32 ui_glyph_h = (r->font.cell_width > 0.0f)
                       ? cw * (r->font.cell_height / r->font.cell_width) : ch;
        if (show_stats) {
            Color stats_fg = { fg_color.r, fg_color.g, fg_color.b, 0.42f * opacity };
            draw_text_clipped(r, stats_buf, stats_x,
                              y + (title_h - ui_glyph_h) * 0.5f, stats_fg, stats_w + cw);
        }
        /* Filename with a dimmed extension: the stem reads at full strength
         * and the ".md" tail recedes, giving the title a touch of typographic
         * hierarchy instead of one flat run. A leading-dot dotfile (".bashrc")
         * has no stem, so draw it whole. */
        {
            f32 name_y = y + (title_h - ui_glyph_h) * 0.5f;
            const char *ext = strrchr(fname, '.');
            if (ext && ext != fname) {
                char stem[256];
                usize sl = (usize)(ext - fname);
                if (sl >= sizeof(stem)) sl = sizeof(stem) - 1;
                memcpy(stem, fname, sl);
                stem[sl] = '\0';
                draw_text_clipped(r, stem, title_text_x, name_y, fg_color, title_max_w);
                /* Advance by codepoints, not bytes — draw_text_clipped steps one
                 * cell per codepoint, so a multibyte stem (e.g. Turkish ç/ş/ğ)
                 * would otherwise push the dimmed extension too far right. */
                u32 stem_cps = 0;
                for (const u8 *q = (const u8 *)stem; *q; q++)
                    if ((*q & 0xC0) != 0x80) stem_cps++;
                f32 ext_x   = title_text_x + (f32)stem_cps * cw;
                f32 ext_max = title_max_w - (f32)stem_cps * cw;
                if (ext_max > 0.0f) {
                    Color ext_fg = fg_color;
                    ext_fg.a *= 0.5f;
                    draw_text_clipped(r, ext, ext_x, name_y, ext_fg, ext_max);
                }
            } else {
                draw_text_clipped(r, fname, title_text_x, name_y, fg_color, title_max_w);
            }
        }

        if (fb->view_mode == FVIEW_MARKDOWN) {
            /* Segmented Read/Edit control: a sunken pill track with a raised,
             * softly-shadowed thumb behind the active label. The thumb takes a
             * faint accent tint so the active mode reads at a glance and the
             * control feels elevated/native — without the full-saturation
             * accent fill the very first version used, which read as harsh.
             * Segments are content-sized with balanced padding so both labels
             * sit centered. */
            ChromePalette cp = chrome_palette_for(theme);
            Color accent = theme_ui_accent(theme);
            f32 seg_h = title_h - 8.0f * dpi;
            f32 seg_y = y + 4.0f * dpi;
            const char *render_lbl = "Read";
            const char *raw_lbl    = "Edit";
            f32 render_w = (f32)strlen(render_lbl) * cw;
            f32 raw_w    = (f32)strlen(raw_lbl) * cw;
            f32 slack = toggle_w - render_w - raw_w;
            if (slack < 8.0f * dpi) slack = 8.0f * dpi;
            f32 render_seg = render_w + slack * 0.5f;
            f32 raw_seg    = toggle_w - render_seg;

            /* Sunken track */
            Color track = cp.surface_sunken;
            track.a = opacity;
            renderer_draw_rrect_simple(r, toggle_x, seg_y, toggle_w, seg_h,
                                       track, seg_h * 0.5f);

            /* Raised, accent-tinted thumb over the active half */
            bool raw_on = fb->md_raw_mode;
            f32 thumb_x = raw_on ? toggle_x + render_seg : toggle_x;
            f32 thumb_w = raw_on ? raw_seg : render_seg;
            f32 inset   = 2.0f * dpi;
            f32 thumb_h = seg_h - 2.0f * inset;
            Color thumb = cp.surface_raised;
            f32 tint = 0.16f;
            thumb.r = thumb.r * (1.0f - tint) + accent.r * tint;
            thumb.g = thumb.g * (1.0f - tint) + accent.g * tint;
            thumb.b = thumb.b * (1.0f - tint) + accent.b * tint;
            thumb.a = opacity;
            renderer_draw_rrect(r, thumb_x + inset, seg_y + inset,
                                thumb_w - 2.0f * inset, thumb_h, thumb,
                                thumb_h * 0.5f, thumb_h * 0.5f,
                                thumb_h * 0.5f, thumb_h * 0.5f,
                                5.0f * dpi, 0.30f * opacity, 0.0f, 1.0f * dpi);

            Color active_fg = chrome_legible_on(thumb);
            active_fg.a = 0.96f * opacity;
            Color inactive_fg = { fg_color.r, fg_color.g, fg_color.b, 0.46f * opacity };
            f32 label_y = y + (title_h - ui_glyph_h) * 0.5f;
            draw_text_clipped(r, render_lbl,
                              toggle_x + (render_seg - render_w) * 0.5f, label_y,
                              raw_on ? inactive_fg : active_fg, render_w);
            draw_text_clipped(r, raw_lbl,
                              toggle_x + render_seg + (raw_seg - raw_w) * 0.5f, label_y,
                              raw_on ? active_fg : inactive_fg, raw_w);
        }

        /* Close — a muted neutral glyph, no always-on tinted box. The old
         * red destructive fill read as alarming for a routine close. */
        f32 close_icon = 11.0f * dpi;
        Color close_fg = { fg_color.r, fg_color.g, fg_color.b, 0.60f * opacity };
        icon_draw(r, ICON_CLOSE,
                  close_x + (close_w - close_icon) * 0.5f,
                  y + (title_h - close_icon) * 0.5f,
                  close_icon, close_fg);

        /* Unsaved-changes pip — a clean amber dot after the filename, shown
         * only when the buffer has edits. Replaces the old bracketed "[*]"/
         * "[e]" tags; edit mode itself is already obvious from the toggle. */
        if (fb->editor_mode && fb->modified) {
            f32 name_w = fminf((f32)strlen(fname) * cw, title_max_w);
            f32 dot_d  = 5.0f * dpi;
            f32 dot_x  = title_text_x + name_w + 7.0f * dpi;
            f32 dot_y  = y + (title_h - dot_d) * 0.5f;
            Color pip = theme ? theme->ansi[3] : (Color){ 0.92f, 0.72f, 0.33f, 1.0f };
            pip.a = 0.95f * opacity;
            if (dot_x + dot_d < info_right)
                renderer_draw_rrect_simple(r, dot_x, dot_y, dot_d, dot_d,
                                           pip, dot_d * 0.5f);
        }
    }

    renderer_flush_rects(r);
    renderer_flush_glyphs(r);
    g_ui_step = 0;
    renderer_reset_ui_scale(r);

    /* Find / Replace bar overlays everything at the top of the viewer. */
    if (fb->find_active)
        fb_find_render(fb, r, x, y + (VIEWER_TITLE_H_PT * dpi), w, dpi, theme, opacity);
    /* Autocomplete popup anchored under the caret. */
    if (fb->ac_active)
        fb_ac_render(fb, r, dpi, theme, opacity);
}

/* =========================================================================
 * Editor operations
 * ========================================================================= */

/* Get pointer to start of line N in view_content */
static char *editor_line_start(FileBrowser *fb, i32 line) {
    char *p = fb->view_content;
    for (i32 i = 0; i < line && p && *p; i++) {
        char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + strlen(p);
    }
    return p;
}

static i32 editor_line_len(FileBrowser *fb, i32 line) {
    char *start = editor_line_start(fb, line);
    if (!start) return 0;
    char *nl = strchr(start, '\n');
    return nl ? (i32)(nl - start) : (i32)strlen(start);
}

static void editor_clamp_cursor(FileBrowser *fb) {
    if (!fb || !fb->view_content) return;
    if (fb->view_line_count < 1) fb->view_line_count = 1;
    if (fb->cursor_line < 0) fb->cursor_line = 0;
    if (fb->cursor_line >= fb->view_line_count) fb->cursor_line = fb->view_line_count - 1;

    i32 line_len = editor_line_len(fb, fb->cursor_line);
    if (fb->cursor_col < 0) fb->cursor_col = 0;
    if (fb->cursor_col > line_len) fb->cursor_col = line_len;
}

void fb_editor_toggle(FileBrowser *fb) {
    if (fb->view_mode == FVIEW_NONE) return;
    fb->editor_mode = !fb->editor_mode;
    if (fb->editor_mode) {
        fb->cursor_line = 0;
        fb->cursor_col = 0;
    }
}

/* Ensure view_content holds at least `need` bytes, growing geometrically so a
 * run of keystrokes doesn't realloc-and-copy the whole buffer each time
 * (O(n^2) → amortized O(1) reallocs). Keeps view_cap == the live allocation. */
static bool editor_ensure_cap(FileBrowser *fb, usize need) {
    if (need <= fb->view_cap) return true;
    usize cap = fb->view_cap ? fb->view_cap : 64;
    while (cap < need) cap *= 2;
    char *b = realloc(fb->view_content, cap);
    if (!b) return false;
    fb->view_content = b;
    fb->view_cap = cap;
    return true;
}

/* UTF-8-aware cursor stepping. cursor_col is a BYTE offset within the line, so
 * naive +-1 movement and single-byte delete land INSIDE a multi-byte sequence
 * and corrupt non-ASCII text (Turkish ö/ş/ğ, CJK, emoji, …). These step a whole
 * codepoint. */
static i32 editor_cp_len_at(const char *line, i32 col, i32 line_len) {
    if (col < 0 || col >= line_len) return 0;
    unsigned char c = (unsigned char)line[col];
    i32 n = (c < 0xC0) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
    if (col + n > line_len) return 1;
    for (i32 i = 1; i < n; i++)
        if (((unsigned char)line[col + i] & 0xC0) != 0x80) return 1;  /* bad seq → 1 */
    return n;
}
static i32 editor_cp_back_at(const char *line, i32 col) {
    if (col <= 0) return 0;
    i32 i = col - 1;
    while (i > 0 && ((unsigned char)line[i] & 0xC0) == 0x80) i--;   /* skip continuation */
    return col - i;
}

void fb_editor_insert_char(FileBrowser *fb, u32 cp) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    editor_pre_edit(fb);

    /* Find insert position */
    char *line_start = editor_line_start(fb, fb->cursor_line);
    if (!line_start) return;
    usize offset = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;

    /* Encode codepoint as UTF-8 */
    u8 utf[4];
    u32 enc_len = utf8_encode(cp, utf);
    if (enc_len == 0) return;

    /* Grow buffer (geometric, capacity-tracked) */
    usize new_size = fb->view_size + enc_len;
    if (!editor_ensure_cap(fb, new_size + 1)) return;

    /* Recalculate offset after possible realloc */
    line_start = editor_line_start(fb, fb->cursor_line);
    offset = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;

    /* Shift right and insert */
    memmove(fb->view_content + offset + enc_len, fb->view_content + offset, fb->view_size - offset + 1);
    memcpy(fb->view_content + offset, utf, enc_len);
    fb->view_size = new_size;
    fb->cursor_col += (i32)enc_len;
    fb->modified = true;
}

void fb_editor_backspace(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    editor_pre_edit(fb);

    if (fb->cursor_col > 0) {
        char *line_start = editor_line_start(fb, fb->cursor_line);
        i32 back = editor_cp_back_at(line_start, fb->cursor_col);
        if (back < 1) back = 1;
        usize gc = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;
        memmove(fb->view_content + gc - (usize)back, fb->view_content + gc,
                fb->view_size - gc + 1);
        fb->view_size -= (usize)back;
        fb->cursor_col -= back;
        fb->modified = true;
    } else if (fb->cursor_line > 0) {
        /* Join with previous line */
        i32 prev_len = editor_line_len(fb, fb->cursor_line - 1);
        char *line_start = editor_line_start(fb, fb->cursor_line);
        usize nl_offset = (usize)(line_start - fb->view_content) - 1; /* the \n */
        memmove(fb->view_content + nl_offset, fb->view_content + nl_offset + 1, fb->view_size - nl_offset);
        fb->view_size--;
        fb->view_line_count--;
        fb->cursor_line--;
        fb->cursor_col = prev_len;
        fb->modified = true;
    }
}

void fb_editor_delete(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    editor_pre_edit(fb);

    char *line_start = editor_line_start(fb, fb->cursor_line);
    i32 line_len = editor_line_len(fb, fb->cursor_line);

    if (fb->cursor_col < line_len) {
        i32 dl = editor_cp_len_at(line_start, fb->cursor_col, line_len);
        if (dl < 1) dl = 1;
        usize gc = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;
        memmove(fb->view_content + gc, fb->view_content + gc + (usize)dl,
                fb->view_size - gc - (usize)dl + 1);
        fb->view_size -= (usize)dl;
        fb->modified = true;
    } else if (fb->cursor_line < fb->view_line_count - 1) {
        /* Join with next line — delete the \n */
        usize offset = (usize)(line_start - fb->view_content) + (usize)line_len;
        memmove(fb->view_content + offset, fb->view_content + offset + 1, fb->view_size - offset);
        fb->view_size--;
        fb->view_line_count--;
        fb->modified = true;
    }
}

void fb_editor_newline(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    editor_pre_edit(fb);

    char *line_start = editor_line_start(fb, fb->cursor_line);
    usize offset = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;

    usize new_size = fb->view_size + 1;
    if (!editor_ensure_cap(fb, new_size + 1)) return;

    memmove(fb->view_content + offset + 1, fb->view_content + offset, fb->view_size - offset + 1);
    fb->view_content[offset] = '\n';
    fb->view_size = new_size;
    fb->view_line_count++;
    fb->cursor_line++;
    fb->cursor_col = 0;
    fb->modified = true;
}

/* Enter inside a markdown list: continue the marker ("- ", "* ", "N. ",
 * "- [ ] "); an empty item instead clears the marker. */
void fb_md_editor_newline(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) { if (fb) fb_editor_newline(fb); return; }
    editor_clamp_cursor(fb);
    const char *ls = editor_line_start(fb, fb->cursor_line);
    i32 ll = editor_line_len(fb, fb->cursor_line);
    i32 ind = 0; while (ind < ll && (ls[ind]==' '||ls[ind]=='\t')) ind++;
    const char *t = ls + ind; i32 tl = ll - ind;
    i32 mlen = 0, is_task = 0, onum = 0; char bm = 0;
    if (tl>=5 && (t[0]=='-'||t[0]=='*'||t[0]=='+') && t[1]==' ' && t[2]=='[' &&
        (t[3]==' '||t[3]=='x'||t[3]=='X') && t[4]==']') { mlen=ind+6; is_task=1; }
    else if (tl>=2 && (t[0]=='-'||t[0]=='*'||t[0]=='+') && t[1]==' ') { mlen=ind+2; bm=t[0]; }
    else { i32 k=0; while(k<tl && isdigit((unsigned char)t[k])) k++;
           if (k>0 && k+1<tl && t[k]=='.' && t[k+1]==' ') { mlen=ind+k+2; onum=atoi(t); } }
    if (mlen == 0) { fb_editor_newline(fb); return; }

    bool empty = true;
    for (i32 k = mlen; k < ll; k++) if (ls[k] != ' ' && ls[k] != '\t') { empty = false; break; }
    if (empty) {                              /* blank item → drop the marker */
        fb->cursor_col = ll;
        while (fb->cursor_col > ind) fb_editor_backspace(fb);
        return;
    }
    fb_editor_newline(fb);
    for (i32 k = 0; k < ind; k++) fb_editor_insert_char(fb, ' ');
    if (is_task)      { const char *m = "- [ ] "; for (const char *c=m; *c; c++) fb_editor_insert_char(fb, (u32)*c); }
    else if (onum>0)  { char b[16]; snprintf(b, sizeof b, "%d. ", onum+1); for (char *c=b; *c; c++) fb_editor_insert_char(fb, (u32)*c); }
    else              { fb_editor_insert_char(fb, (u32)(bm?bm:'-')); fb_editor_insert_char(fb, ' '); }
}

/* Map a click in the live-preview body (framebuffer coords) to a (line, col)
 * and move the cursor there. Replays the per-line heights from the last render
 * so variable heading heights line up. */
bool fb_md_live_locate(FileBrowser *fb, f32 mx, f32 my) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || !fb->md_raw_mode || !fb->view_content) return false;
    if (fb->live_cw <= 0.0f) return false;
    static const f32 HS[7] = { 1.0f, 1.6f, 1.4f, 1.25f, 1.12f, 1.05f, 1.0f };
    f32 cw = fb->live_cw, ch = fb->live_ch, dpi = fb->live_dpi;
    f32 cy = fb->md_origin_y;
    char *p = fb->view_content;
    i32 li = 0;
    for (; li < fb->view_scroll && *p; li++) { char *e = strchr(p,'\n'); p = e?e+1:p+strlen(p); }
    for (; *p; li++) {
        char *eol = strchr(p, '\n'); i32 ll = eol ? (i32)(eol-p) : (i32)strlen(p);
        i32 ind = 0; while (ind<ll && (p[ind]==' '||p[ind]=='\t')) ind++;
        i32 hlvl = 0; if (p[ind]=='#') { i32 k=0; while(k<ll-ind&&k<6&&p[ind+k]=='#')k++; if(k>0&&ind+k<ll&&p[ind+k]==' ')hlvl=k; }
        f32 lch = hlvl ? ch*HS[hlvl] : ch;
        f32 lh = lch + (hlvl ? 6.0f*dpi : 2.0f*dpi);
        if (my < cy + lh) {
            i32 vcol = (i32)roundf((mx - fb->md_origin_x) / (hlvl ? roundf(cw*HS[hlvl]) : cw));
            if (vcol < 0) vcol = 0;
            /* vcol is a visual column; cursor_col is a BYTE offset. Walk that
             * many codepoints into the line so a click past multi-byte chars
             * lands on a sequence boundary (not mid-char). */
            i32 bc = 0, seen = 0;
            while (bc < ll && seen < vcol) {
                u32 cpx; u32 nbx = utf8_decode((const u8*)p+bc, (usize)(ll-bc), &cpx);
                bc += nbx ? (i32)nbx : 1; seen++;
            }
            fb->cursor_line = li; fb->cursor_col = bc;
            editor_clamp_cursor(fb);
            return true;
        }
        cy += lh;
        p = eol ? eol+1 : p+ll;
    }
    fb->cursor_line = (fb->view_line_count>0)?fb->view_line_count-1:0;
    editor_clamp_cursor(fb);
    return true;
}

void fb_editor_move(FileBrowser *fb, i32 dcol, i32 drow) {
    if (!fb->editor_mode) return;

    fb->cursor_line += drow;
    if (fb->cursor_line < 0) fb->cursor_line = 0;
    if (fb->cursor_line >= fb->view_line_count) fb->cursor_line = fb->view_line_count - 1;

    i32 line_len = editor_line_len(fb, fb->cursor_line);
    char *line_start = editor_line_start(fb, fb->cursor_line);

    /* Horizontal motion steps whole codepoints (cursor_col is a byte offset). */
    if (dcol > 0) {
        for (i32 s = 0; s < dcol && fb->cursor_col < line_len; s++)
            fb->cursor_col += editor_cp_len_at(line_start, fb->cursor_col, line_len);
    } else if (dcol < 0) {
        for (i32 s = 0; s < -dcol && fb->cursor_col > 0; s++)
            fb->cursor_col -= editor_cp_back_at(line_start, fb->cursor_col);
    }
    if (fb->cursor_col < 0) fb->cursor_col = 0;
    if (fb->cursor_col > line_len) fb->cursor_col = line_len;

    /* Ensure cursor is visible */
    i32 visible = 30; /* approximate */
    if (fb->cursor_line < fb->view_scroll) fb->view_scroll = fb->cursor_line;
    if (fb->cursor_line >= fb->view_scroll + visible) fb->view_scroll = fb->cursor_line - visible + 1;
    editor_clamp_cursor(fb);
}

bool fb_editor_save(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->modified || !fb->view_content) return false;
    if (fb->source != FB_SOURCE_LOCAL) return false;

    FILE *f = fopen(fb->view_path, "wb");
    if (!f) return false;
    fwrite(fb->view_content, 1, fb->view_size, f);
    fclose(f);
    fb->modified = false;
    return true;
}

/* =========================================================================
 * Editor undo/redo — delta-based stack
 *
 * Why not full snapshots? The previous implementation stored a full buffer
 * copy per frame: a 2 MiB file × 32 slots = ~66 MiB resident per open file.
 * Real edits change a handful of bytes at a time, so we store only the
 * splice: (offset, old_bytes, new_bytes). One base mirror keeps state[0],
 * one tip mirror keeps state[undo_count] (so diffs are computed in O(buf)
 * without replaying the stack), and every frame in between holds just its
 * delta. A 32-frame stack of typical edits costs a few KiB of diffs on top
 * of the base+tip pair.
 * ========================================================================= */

/* Free a single frame's owned byte buffers. */
static void undo_frame_free(struct FbUndoFrame *f) {
    if (!f) return;
    free(f->old_bytes);
    free(f->new_bytes);
    f->old_bytes = NULL;
    f->new_bytes = NULL;
    f->old_len = 0;
    f->new_len = 0;
}

/* Compute the minimal splice (common prefix/suffix trim) between two buffers.
 * Fills `frame` with (offset, old_len, new_len) and allocates old/new_bytes.
 * Returns false on allocation failure; leaves `frame` zeroed on failure. */
static bool undo_frame_compute_diff(struct FbUndoFrame *frame,
                                    const char *prev, usize prev_sz,
                                    const char *curr, usize curr_sz) {
    memset(frame, 0, sizeof(*frame));

    usize min_sz = prev_sz < curr_sz ? prev_sz : curr_sz;
    usize pref = 0;
    while (pref < min_sz && prev[pref] == curr[pref]) pref++;

    usize suf = 0;
    while (suf < (prev_sz - pref) && suf < (curr_sz - pref) &&
           prev[prev_sz - 1 - suf] == curr[curr_sz - 1 - suf]) {
        suf++;
    }

    frame->offset  = pref;
    frame->old_len = prev_sz - pref - suf;
    frame->new_len = curr_sz - pref - suf;

    if (frame->old_len > 0) {
        frame->old_bytes = malloc(frame->old_len);
        if (!frame->old_bytes) { memset(frame, 0, sizeof(*frame)); return false; }
        memcpy(frame->old_bytes, prev + pref, frame->old_len);
    }
    if (frame->new_len > 0) {
        frame->new_bytes = malloc(frame->new_len);
        if (!frame->new_bytes) {
            free(frame->old_bytes);
            memset(frame, 0, sizeof(*frame));
            return false;
        }
        memcpy(frame->new_bytes, curr + pref, frame->new_len);
    }
    return true;
}

/* Apply `frame` to (*buf, *size) in place.
 * `forward` = true : state[i] → state[i+1] (replace old_bytes with new_bytes).
 * `forward` = false: state[i+1] → state[i] (replace new_bytes with old_bytes).
 * Returns true on success. On failure, (*buf, *size) are unchanged. */
static bool undo_apply(char **buf, usize *size, const struct FbUndoFrame *frame,
                        bool forward) {
    if (!buf || !*buf || !size) return false;

    usize in_len  = forward ? frame->old_len : frame->new_len;
    usize out_len = forward ? frame->new_len : frame->old_len;
    const char *out_bytes = forward ? frame->new_bytes : frame->old_bytes;

    if (frame->offset + in_len > *size) return false;

    usize tail = *size - frame->offset - in_len;
    usize new_size = *size - in_len + out_len;

    if (out_len > in_len) {
        /* Need to grow first */
        char *grown = realloc(*buf, new_size + 1);
        if (!grown) return false;
        *buf = grown;
        /* Shift tail RIGHT after growing */
        memmove(*buf + frame->offset + out_len,
                *buf + frame->offset + in_len, tail);
    } else if (out_len < in_len) {
        /* Shift tail LEFT first, then shrink */
        memmove(*buf + frame->offset + out_len,
                *buf + frame->offset + in_len, tail);
        char *shrunk = realloc(*buf, new_size + 1);
        if (shrunk) *buf = shrunk;  /* shrink failure is non-fatal */
    }
    /* in_len == out_len: no shift needed */

    if (out_len > 0) {
        memcpy(*buf + frame->offset, out_bytes, out_len);
    }
    (*buf)[new_size] = '\0';
    *size = new_size;
    return true;
}

static void fb_editor_prime_undo_base(FileBrowser *fb) {
    if (!fb || !fb->view_content) return;
    if (fb->undo_base || fb->undo_tip) return;

    fb->undo_base = malloc(fb->view_size + 1);
    if (fb->undo_base) {
        memcpy(fb->undo_base, fb->view_content, fb->view_size + 1);
        fb->undo_base_size = fb->view_size;
    }
    fb->undo_tip = malloc(fb->view_size + 1);
    if (fb->undo_tip) {
        memcpy(fb->undo_tip, fb->view_content, fb->view_size + 1);
        fb->undo_tip_size = fb->view_size;
    }
    fb->undo_base_cursor_line = fb->cursor_line;
    fb->undo_base_cursor_col  = fb->cursor_col;
}

/* Reset undo state.
 * Called on file open / close so no stale deltas survive the switch.
 * Full baseline mirrors are allocated lazily on the first edit instead of
 * doubling file memory at open time. */
void fb_editor_reset_undo(FileBrowser *fb) {
    for (i32 i = 0; i < fb->undo_count; i++) {
        undo_frame_free(&fb->undo_frames[i]);
    }
    fb->undo_count = 0;
    fb->undo_pos = 0;
    free(fb->undo_base);
    free(fb->undo_tip);
    fb->undo_base = NULL;
    fb->undo_tip = NULL;
    fb->undo_base_size = 0;
    fb->undo_tip_size = 0;
}

void fb_editor_push_undo(FileBrowser *fb) {
    if (!fb->view_content) return;
    /* Lazy-init baseline if we were opened before the new undo API existed
     * (e.g. an external caller mutated view_content without fb_open_file). */
    if (!fb->undo_base || !fb->undo_tip) {
        fb_editor_prime_undo_base(fb);
        if (!fb->undo_base || !fb->undo_tip) return;
    }

    /* 1) Discard any redo states above current position. We also have to
     *    walk the tip back from state[undo_count] to state[undo_pos] by
     *    applying reverse diffs, so it tracks the new top of the stack. */
    for (i32 i = fb->undo_count - 1; i >= fb->undo_pos; i--) {
        undo_apply(&fb->undo_tip, &fb->undo_tip_size, &fb->undo_frames[i], false);
        undo_frame_free(&fb->undo_frames[i]);
    }
    fb->undo_count = fb->undo_pos;

    /* 2) Stack full: fold the oldest frame forward into the base so we keep
     *    undo depth without unbounded storage. */
    if (fb->undo_count >= 32) {
        undo_apply(&fb->undo_base, &fb->undo_base_size, &fb->undo_frames[0], true);
        fb->undo_base_cursor_line = fb->undo_frames[0].cursor_line;
        fb->undo_base_cursor_col  = fb->undo_frames[0].cursor_col;
        undo_frame_free(&fb->undo_frames[0]);
        memmove(&fb->undo_frames[0], &fb->undo_frames[1],
                31 * sizeof(fb->undo_frames[0]));
        memset(&fb->undo_frames[31], 0, sizeof(fb->undo_frames[31]));
        fb->undo_count = 31;
        fb->undo_pos--;
    }

    /* 3) No-op if view_content exactly matches the tip — avoids storing a
     *    zero-delta frame when the user redoes then pushes without editing. */
    if (fb->undo_tip_size == fb->view_size &&
        (fb->view_size == 0 ||
         memcmp(fb->undo_tip, fb->view_content, fb->view_size) == 0)) {
        return;
    }

    /* 4) Diff tip → current, append as new frame, advance tip. */
    struct FbUndoFrame frame;
    if (!undo_frame_compute_diff(&frame, fb->undo_tip, fb->undo_tip_size,
                                  fb->view_content, fb->view_size)) {
        return;
    }
    frame.cursor_line = fb->cursor_line;
    frame.cursor_col  = fb->cursor_col;

    /* Advance tip = view_content via the same diff (keeps invariant). */
    if (!undo_apply(&fb->undo_tip, &fb->undo_tip_size, &frame, true)) {
        undo_frame_free(&frame);
        return;
    }

    fb->undo_frames[fb->undo_count] = frame;
    fb->undo_count++;
    fb->undo_pos = fb->undo_count;
}

/* Recount \n boundaries in view_content — called after undo/redo splice. */
static void fb_recount_lines(FileBrowser *fb) {
    fb->view_line_count = 1;
    for (usize i = 0; i < fb->view_size; i++)
        if (fb->view_content[i] == '\n') fb->view_line_count++;
}

/* Low-level splice: replace `oldlen` bytes at byte-offset `off` with
 * `newbytes[newlen]`. Keeps view_content NUL-terminated, recounts lines and
 * sets `modified`. Does NOT touch the undo stack or the cursor — callers decide
 * coalescing (call fb_editor_push_undo before a discrete op) and reposition the
 * cursor afterwards. This is the shared primitive behind paste, find/replace and
 * the formatting shortcuts. Returns false only on allocation failure. */
bool fb_editor_splice(FileBrowser *fb, usize off, usize oldlen,
                      const char *newbytes, usize newlen) {
    if (!fb || !fb->view_content) return false;
    if (off > fb->view_size) off = fb->view_size;
    if (off + oldlen > fb->view_size) oldlen = fb->view_size - off;

    usize new_size = fb->view_size - oldlen + newlen;
    if (!editor_ensure_cap(fb, new_size + 1)) return false;

    /* Move the tail (including the trailing NUL) to its new position. memmove
     * handles the overlap for both grow and shrink. */
    memmove(fb->view_content + off + newlen,
            fb->view_content + off + oldlen,
            fb->view_size - off - oldlen + 1);
    if (newlen && newbytes) memcpy(fb->view_content + off, newbytes, newlen);

    fb->view_size = new_size;
    fb->view_content[fb->view_size] = '\0';
    fb_recount_lines(fb);
    fb->modified = true;
    return true;
}

/* Insert UTF-8 text at the cursor as ONE coalesced edit (paste, autocomplete
 * accept, …). Normalizes CRLF and lone CR to LF, advances the cursor past the
 * inserted run. Caller owns undo coalescing around this. */
void fb_editor_insert_text(FileBrowser *fb, const char *utf8, usize len) {
    if (!fb || !fb->editor_mode || !fb->view_content || !utf8 || len == 0) return;
    editor_clamp_cursor(fb);

    /* Normalize newlines into a scratch buffer (CRLF / lone CR → LF). */
    char *norm = malloc(len + 1);
    if (!norm) return;
    usize nlen = 0;
    for (usize i = 0; i < len; i++) {
        char c = utf8[i];
        if (c == '\r') {
            norm[nlen++] = '\n';
            if (i + 1 < len && utf8[i + 1] == '\n') i++;  /* swallow LF of CRLF */
        } else {
            norm[nlen++] = c;
        }
    }
    norm[nlen] = '\0';
    if (nlen == 0) { free(norm); return; }

    char *line_start = editor_line_start(fb, fb->cursor_line);
    usize off = (usize)(line_start - fb->view_content) + (usize)fb->cursor_col;

    if (!fb_editor_splice(fb, off, 0, norm, nlen)) { free(norm); return; }

    /* Advance the cursor: count inserted newlines to land at the tail. */
    i32 added_lines = 0; usize last_nl = (usize)-1;
    for (usize i = 0; i < nlen; i++)
        if (norm[i] == '\n') { added_lines++; last_nl = i; }
    if (added_lines == 0) {
        fb->cursor_col += (i32)nlen;
    } else {
        fb->cursor_line += added_lines;
        fb->cursor_col = (i32)(nlen - last_nl - 1);
    }
    free(norm);
}

/* Byte offset of the cursor within view_content (line start + cursor_col). */
usize fb_editor_cursor_offset(const FileBrowser *fb) {
    if (!fb || !fb->view_content) return 0;
    const char *p = fb->view_content;
    for (i32 i = 0; i < fb->cursor_line && *p; i++) {
        const char *nl = strchr(p, '\n');
        p = nl ? nl + 1 : p + strlen(p);
    }
    usize off = (usize)(p - fb->view_content) + (usize)fb->cursor_col;
    if (off > fb->view_size) off = fb->view_size;
    return off;
}

/* Document statistics for the editor status readout: codepoint count, word
 * count (whitespace-delimited runs), and an estimated read time at 200 wpm. */
static void fb_editor_doc_stats(const FileBrowser *fb, u32 *words, u32 *chars,
                                u32 *read_min) {
    u32 w = 0, c = 0; bool in_word = false;
    if (fb && fb->view_content) {
        const u8 *p = (const u8 *)fb->view_content;
        usize n = fb->view_size;
        for (usize i = 0; i < n; i++) {
            u8 b = p[i];
            if ((b & 0xC0) != 0x80) c++;   /* UTF-8 lead bytes == codepoints */
            bool ws = (b==' '||b=='\t'||b=='\n'||b=='\r'||b=='\f'||b=='\v');
            if (!ws && !in_word) { w++; in_word = true; }
            else if (ws) in_word = false;
        }
    }
    if (words) *words = w;
    if (chars) *chars = c;
    if (read_min) *read_min = (w + 199) / 200;   /* ceil at 200 wpm; 0 → 0 */
}

/* Paste the system clipboard at the cursor as a single undo unit. */
void fb_editor_paste(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    const char *clip = platform_clipboard_get();   /* internal buffer — do not free */
    if (!clip || !clip[0]) return;
    fb_editor_push_undo(fb);                 /* flush pending typed edits to their own frame */
    fb_editor_insert_text(fb, clip, strlen(clip));
    fb_editor_push_undo(fb);                 /* commit the paste as its own undo unit */
}

/* Wrap a span with `open`/`close` markers. With no selection (selection in the
 * live editor is rendered-view only), insert the pair at the cursor and park
 * the caret between them — type, and you're inside the emphasis. open/close are
 * ASCII markdown markers (`**`, `*`, `` ` ``). One undo unit. */
void fb_editor_wrap_or_insert(FileBrowser *fb, const char *open, const char *close) {
    if (!fb || !fb->editor_mode || !fb->view_content || !open || !close) return;
    editor_clamp_cursor(fb);
    fb_editor_push_undo(fb);
    usize olen = strlen(open), clen = strlen(close);
    char buf[16];
    if (olen + clen >= sizeof buf) return;
    memcpy(buf, open, olen);
    memcpy(buf + olen, close, clen);
    char *ls = editor_line_start(fb, fb->cursor_line);
    usize off = (usize)(ls - fb->view_content) + (usize)fb->cursor_col;
    if (!fb_editor_splice(fb, off, 0, buf, olen + clen)) return;
    fb->cursor_col += (i32)olen;             /* caret between the markers */
    fb_editor_push_undo(fb);
}

/* Insert a markdown link skeleton "[]()" at the cursor and park the caret
 * inside the brackets so the user types the link text first. */
void fb_editor_make_link(FileBrowser *fb) {
    if (!fb || !fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    fb_editor_push_undo(fb);
    char *ls = editor_line_start(fb, fb->cursor_line);
    usize off = (usize)(ls - fb->view_content) + (usize)fb->cursor_col;
    if (!fb_editor_splice(fb, off, 0, "[]()", 4)) return;
    fb->cursor_col += 1;                     /* between the [ ] */
    fb_editor_push_undo(fb);
}

/* Toggle a line-leading marker ("> ", "- ", "1. ") on the cursor's line. */
void fb_editor_toggle_line_prefix(FileBrowser *fb, const char *prefix) {
    if (!fb || !fb->editor_mode || !fb->view_content || !prefix) return;
    editor_clamp_cursor(fb);
    fb_editor_push_undo(fb);
    char *ls = editor_line_start(fb, fb->cursor_line);
    usize line_off = (usize)(ls - fb->view_content);
    usize plen = strlen(prefix);
    i32 ll = editor_line_len(fb, fb->cursor_line);
    if ((usize)ll >= plen && memcmp(ls, prefix, plen) == 0) {
        /* Remove it. */
        fb_editor_splice(fb, line_off, plen, NULL, 0);
        fb->cursor_col = fb->cursor_col > (i32)plen ? fb->cursor_col - (i32)plen : 0;
    } else {
        fb_editor_splice(fb, line_off, 0, prefix, plen);
        fb->cursor_col += (i32)plen;
    }
    fb_editor_push_undo(fb);
}

void fb_editor_undo(FileBrowser *fb) {
    if (fb->undo_pos <= 0 || !fb->view_content) return;

    /* Capture any draft edits the user made past the top of the stack so
     * they become redoable — matches the original snapshot semantics. */
    if (fb->undo_pos == fb->undo_count) {
        fb_editor_push_undo(fb);
    }
    if (fb->undo_pos <= 0) return;

    const struct FbUndoFrame *frame = &fb->undo_frames[fb->undo_pos - 1];
    if (!undo_apply(&fb->view_content, &fb->view_size, frame, false)) return;
    fb->view_cap = fb->view_size + 1;  /* undo_apply reallocs to exactly this */
    fb->undo_pos--;

    /* Cursor at state[undo_pos] — stored in the frame we just passed over,
     * or in the base for state[0]. */
    if (fb->undo_pos == 0) {
        fb->cursor_line = fb->undo_base_cursor_line;
        fb->cursor_col  = fb->undo_base_cursor_col;
    } else {
        fb->cursor_line = fb->undo_frames[fb->undo_pos - 1].cursor_line;
        fb->cursor_col  = fb->undo_frames[fb->undo_pos - 1].cursor_col;
    }

    fb_recount_lines(fb);
    fb->modified = true;
}

void fb_editor_redo(FileBrowser *fb) {
    if (fb->undo_pos >= fb->undo_count || !fb->view_content) return;
    const struct FbUndoFrame *frame = &fb->undo_frames[fb->undo_pos];
    if (!undo_apply(&fb->view_content, &fb->view_size, frame, true)) return;
    fb->view_cap = fb->view_size + 1;  /* undo_apply reallocs to exactly this */
    fb->undo_pos++;
    fb->cursor_line = frame->cursor_line;
    fb->cursor_col  = frame->cursor_col;
    fb_recount_lines(fb);
    fb->modified = true;
}

char *fb_editor_get_line(FileBrowser *fb, i32 line) {
    return editor_line_start(fb, line);
}

void fb_editor_duplicate_line(FileBrowser *fb) {
    if (!fb->view_content || !fb->editor_mode) return;
    fb_editor_push_undo(fb);

    char *line = editor_line_start(fb, fb->cursor_line);
    i32 len = editor_line_len(fb, fb->cursor_line);
    /* Find end of line (after \n) */
    char *line_end = line + len;
    if (*line_end == '\n') line_end++;
    i32 insert_len = (i32)(line_end - line);

    /* Capture positions as byte offsets BEFORE the (possible) realloc — the
     * `line`/`line_end` pointers dangle if the buffer moves. The source span
     * [line_off, insert_pos) sits before insert_pos, so the tail memmove below
     * doesn't disturb it and we can copy from the new base afterwards. */
    usize line_off   = (usize)(line - fb->view_content);
    usize insert_pos = (usize)(line_end - fb->view_content);

    usize new_size = fb->view_size + (usize)insert_len;
    if (!editor_ensure_cap(fb, new_size + 1)) return;

    memmove(fb->view_content + insert_pos + insert_len,
            fb->view_content + insert_pos,
            fb->view_size - insert_pos + 1);
    memcpy(fb->view_content + insert_pos, fb->view_content + line_off, (usize)insert_len);
    fb->view_size = new_size;
    fb->view_line_count++;
    fb->cursor_line++;
    fb->modified = true;
}

void fb_editor_cut_line(FileBrowser *fb) {
    if (!fb->view_content || !fb->editor_mode) return;
    fb_editor_push_undo(fb);

    char *line = editor_line_start(fb, fb->cursor_line);
    i32 len = editor_line_len(fb, fb->cursor_line);
    char *line_end = line + len;
    if (*line_end == '\n') line_end++;
    i32 cut_len = (i32)(line_end - line);

    /* Copy line to clipboard */
    char tmp[4096];
    if (cut_len < (i32)sizeof(tmp)) {
        memcpy(tmp, line, (usize)cut_len);
        tmp[cut_len] = '\0';
        platform_clipboard_set(tmp);
    }

    /* Remove line */
    usize cut_pos = (usize)(line - fb->view_content);
    memmove(fb->view_content + cut_pos,
            fb->view_content + cut_pos + cut_len,
            fb->view_size - cut_pos - (usize)cut_len + 1);
    fb->view_size -= (usize)cut_len;
    fb->view_line_count--;
    if (fb->cursor_line >= fb->view_line_count && fb->view_line_count > 0)
        fb->cursor_line = fb->view_line_count - 1;
    fb->cursor_col = 0;
    fb->modified = true;
}

void fb_editor_copy_line(FileBrowser *fb) {
    if (!fb->view_content || !fb->editor_mode) return;

    char *line = editor_line_start(fb, fb->cursor_line);
    i32 len = editor_line_len(fb, fb->cursor_line);
    char *line_end = line + len;
    if (*line_end == '\n') line_end++;
    i32 copy_len = (i32)(line_end - line);

    char tmp[4096];
    if (copy_len < (i32)sizeof(tmp)) {
        memcpy(tmp, line, (usize)copy_len);
        tmp[copy_len] = '\0';
        platform_clipboard_set(tmp);
    }
}

void fb_editor_goto_line_start(FileBrowser *fb) {
    editor_clamp_cursor(fb);
    fb->cursor_col = 0;
}

void fb_editor_goto_line_end(FileBrowser *fb) {
    editor_clamp_cursor(fb);
    fb->cursor_col = editor_line_len(fb, fb->cursor_line);
}

/* ---- macOS-native word / line / document editing -----------------------
 * cursor_col is a byte offset within the line. A "word" is a run of
 * non-whitespace, matching readline / Option-Backspace expectations; UTF-8
 * continuation/lead bytes (>= 0x80) count as word chars so a glyph is never
 * split. Mirrors fb_editor_move's view_scroll follow so the caret stays
 * on-screen after a long jump. */
static inline bool ed_is_ws(char c) { return c == ' ' || c == '\t'; }

static void editor_scroll_to_cursor(FileBrowser *fb) {
    i32 visible = 30; /* approximate; matches fb_editor_move */
    if (fb->cursor_line < fb->view_scroll) fb->view_scroll = fb->cursor_line;
    if (fb->cursor_line >= fb->view_scroll + visible)
        fb->view_scroll = fb->cursor_line - visible + 1;
    if (fb->view_scroll < 0) fb->view_scroll = 0;
}

void fb_editor_move_word_left(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    if (fb->cursor_col == 0) {
        if (fb->cursor_line > 0) {
            fb->cursor_line--;
            fb->cursor_col = editor_line_len(fb, fb->cursor_line);
        }
    } else {
        char *ls = editor_line_start(fb, fb->cursor_line);
        i32 c = fb->cursor_col;
        while (c > 0 &&  ed_is_ws(ls[c-1])) c--;
        while (c > 0 && !ed_is_ws(ls[c-1])) c--;
        fb->cursor_col = c;
    }
    editor_scroll_to_cursor(fb);
}

void fb_editor_move_word_right(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    i32 len = editor_line_len(fb, fb->cursor_line);
    if (fb->cursor_col >= len) {
        if (fb->cursor_line < fb->view_line_count - 1) {
            fb->cursor_line++;
            fb->cursor_col = 0;
        }
    } else {
        char *ls = editor_line_start(fb, fb->cursor_line);
        i32 c = fb->cursor_col;
        while (c < len &&  ed_is_ws(ls[c])) c++;
        while (c < len && !ed_is_ws(ls[c])) c++;
        fb->cursor_col = c;
    }
    editor_scroll_to_cursor(fb);
}

/* Delete a byte run [a,b) within the current line and place the cursor at a. */
static void editor_delete_run(FileBrowser *fb, char *line_start, i32 a, i32 b) {
    if (b <= a) return;
    editor_pre_edit(fb);
    usize off = (usize)(line_start - fb->view_content) + (usize)a;
    i32 del = b - a;
    memmove(fb->view_content + off, fb->view_content + off + del,
            fb->view_size - off - (usize)del + 1);   /* +1 keeps the NUL */
    fb->view_size -= (usize)del;
    fb->cursor_col = a;
    fb->modified = true;
}

void fb_editor_delete_word_left(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    if (fb->cursor_col == 0) { fb_editor_backspace(fb); return; }  /* joins lines */
    char *ls = editor_line_start(fb, fb->cursor_line);
    i32 c = fb->cursor_col;
    while (c > 0 &&  ed_is_ws(ls[c-1])) c--;
    while (c > 0 && !ed_is_ws(ls[c-1])) c--;
    editor_delete_run(fb, ls, c, fb->cursor_col);
}

void fb_editor_delete_word_right(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    i32 len = editor_line_len(fb, fb->cursor_line);
    if (fb->cursor_col >= len) { fb_editor_delete(fb); return; }   /* joins lines */
    char *ls = editor_line_start(fb, fb->cursor_line);
    i32 c = fb->cursor_col;
    while (c < len &&  ed_is_ws(ls[c])) c++;
    while (c < len && !ed_is_ws(ls[c])) c++;
    i32 from = fb->cursor_col;
    editor_delete_run(fb, ls, from, c);
    fb->cursor_col = from;   /* forward delete leaves the caret in place */
}

void fb_editor_delete_to_line_start(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    editor_clamp_cursor(fb);
    if (fb->cursor_col == 0) { fb_editor_backspace(fb); return; }
    char *ls = editor_line_start(fb, fb->cursor_line);
    editor_delete_run(fb, ls, 0, fb->cursor_col);
}

void fb_editor_goto_doc_start(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    fb->cursor_line = 0;
    fb->cursor_col = 0;
    fb->view_scroll = 0;
}

void fb_editor_goto_doc_end(FileBrowser *fb) {
    if (!fb->editor_mode || !fb->view_content) return;
    fb->cursor_line = fb->view_line_count > 0 ? fb->view_line_count - 1 : 0;
    fb->cursor_col = editor_line_len(fb, fb->cursor_line);
    editor_scroll_to_cursor(fb);
}

/* Push undo before destructive edits */
static void editor_pre_edit(FileBrowser *fb) {
    fb_editor_prime_undo_base(fb);
    /* Push undo every ~10 edits to avoid excessive snapshots. The counter lives
     * on the instance — a function-local static was shared across every open
     * FileBrowser, so edits in one pane skewed another pane's snapshot timing. */
    if (++fb->edit_counter >= 10) {
        fb_editor_push_undo(fb);
        fb->edit_counter = 0;
    }
}

/* =========================================================================
 * Input handling
 * ========================================================================= */

bool fb_handle_click(FileBrowser *fb, f32 rel_x, f32 rel_y, f32 w, f32 h, f32 dpi) {
    (void)w; (void)rel_x; (void)h;
    f32 ch = 16.0f * dpi;  /* match UI scale used in render */
    f32 hdr_h = TOOLBAR_HEIGHT_PT * dpi;
    f32 entry_h = ch + 6.0f; /* must match fb_render_sidebar */

    if (rel_y < hdr_h) return true; /* header click */

    i32 idx = (i32)((rel_y - hdr_h + fb->scroll_offset_px) / entry_h);
    if (idx >= 0 && idx < fb->entry_count) {
        if (idx == fb->selected) {
            /* Double-click: open */
            fb_open_file(fb, idx);
        } else {
            fb->selected = idx;
        }
        return true;
    }
    return false;
}

/* =========================================================================
 * Rendered-markdown text selection
 *
 * md_render fills fb->md_glyph_rects each frame in reading order (document
 * order, framebuffer px). Selection endpoints are stored in CONTENT space
 * (screen − origin + scroll-at-mouse-down) so they're stable across scroll
 * and reflow. Each resolve maps the captured glyph rects back into doc
 * space using `md_glyph_scroll_px` — the scroll the rects were filled at,
 * not whatever scroll is current — so endpoints stay aligned with the
 * actual glyphs even when the user is scrolling and selecting at once.
 * ========================================================================= */

/* Map a content-space point to a reading-order insertion index (0..count).
 *
 * Uses `md_glyph_scroll_px` — the scroll value in effect when md_render
 * filled the rect buffer — not the current `view_scroll_px`. The rects'
 * framebuffer y reflects that older scroll; subtracting origin and adding
 * THAT scroll cancels back to doc-space coordinates. Using the current
 * scroll instead would offset every doc-space y by (now − then), which
 * mid-scroll-drag selects the wrong glyphs. */
static u32 md_sel_index_at(const FileBrowser *fb, f32 px, f32 py) {
    f32 ox = fb->md_origin_x, oy = fb->md_origin_y;
    f32 sc = fb->md_glyph_scroll_px;
    u32 idx = 0;
    for (u32 i = 0; i < fb->md_glyph_count; i++) {
        const MdGlyphRect *g = &fb->md_glyph_rects[i];
        f32 gx = g->x - ox;          /* content x */
        f32 gy = g->y - oy + sc;     /* content y */
        bool before;
        if (gy + g->h <= py)      before = true;                 /* line above */
        else if (gy <= py)        before = (px >= gx + g->w * 0.5f); /* same line */
        else                      before = false;                /* line below */
        if (before) idx = i + 1;
    }
    return idx;
}

/* Resolve the stored selection to a [lo, hi) glyph index range. */
static bool md_sel_range(const FileBrowser *fb, u32 *lo, u32 *hi) {
    if (!fb->md_sel_active || !fb->md_glyph_rects || fb->md_glyph_count == 0)
        return false;
    u32 a = md_sel_index_at(fb, fb->md_sel_ax, fb->md_sel_ay);
    u32 b = md_sel_index_at(fb, fb->md_sel_hx, fb->md_sel_hy);
    if (a == b) return false;
    *lo = a < b ? a : b;
    *hi = a < b ? b : a;
    return true;
}

static void fb_md_draw_selection(FileBrowser *fb, Renderer *r, const Theme *theme) {
    u32 lo, hi;
    if (!md_sel_range(fb, &lo, &hi)) return;
    Color sel = theme ? theme->selection : (Color){0.20f, 0.30f, 0.50f, 1.0f};
    /* Drawn before md_render's text (see fb_render_viewer), so the highlight
     * sits behind every glyph — body and headings alike. Kept translucent so
     * block backgrounds md_render paints on top (code fences, callouts) don't
     * fully bury a selection that runs through them. */
    sel.a = 0.40f;
    /* Coalesce selected glyphs into one rect per visual line. Glyphs on a
     * line share a y from md_render's line layout, but inline elements with
     * a different cell height (mixed font sizes, sub/superscript, future
     * inline images) can shift the baseline by a fraction of a pixel — so
     * compare with a half-pixel tolerance rather than `==`. A real line
     * break is at least ~ch apart, comfortably above the tolerance. */
    u32 i = lo;
    while (i < hi) {
        f32 line_y = fb->md_glyph_rects[i].y;
        f32 x0 = fb->md_glyph_rects[i].x;
        f32 x1 = x0 + fb->md_glyph_rects[i].w;
        f32 hgt = fb->md_glyph_rects[i].h;
        u32 j = i + 1;
        while (j < hi && fabsf(fb->md_glyph_rects[j].y - line_y) < 0.5f) {
            const MdGlyphRect *g = &fb->md_glyph_rects[j];
            if (g->x < x0)          x0 = g->x;
            if (g->x + g->w > x1)   x1 = g->x + g->w;
            if (g->h > hgt)         hgt = g->h;
            j++;
        }
        renderer_draw_rect(r, x0, line_y, x1 - x0, hgt, sel);
        i = j;
    }
}

void fb_md_selection_begin(FileBrowser *fb, f32 mx, f32 my) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return;
    f32 cx = mx - fb->md_origin_x;
    f32 cy = my - fb->md_origin_y + fb->view_scroll_px;
    fb->md_sel_ax = fb->md_sel_hx = cx;
    fb->md_sel_ay = fb->md_sel_hy = cy;
    fb->md_sel_active = true;
}

void fb_md_selection_update(FileBrowser *fb, f32 mx, f32 my) {
    if (!fb || !fb->md_sel_active) return;
    fb->md_sel_hx = mx - fb->md_origin_x;
    fb->md_sel_hy = my - fb->md_origin_y + fb->view_scroll_px;
}

void fb_md_selection_clear(FileBrowser *fb) {
    if (fb) fb->md_sel_active = false;
}

bool fb_md_selection_active(const FileBrowser *fb) {
    if (!fb || !fb->md_sel_active) return false;
    /* Defensive gate matching fb_md_selection_begin: the selection is only
     * meaningful while the rendered Markdown viewer is on screen. If a stale
     * md_sel_active flag survives a viewer close or a non-Markdown open,
     * never report it as active — the glyph rects point at a doc that is no
     * longer being rendered. */
    if (fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    u32 lo, hi;
    return md_sel_range(fb, &lo, &hi);
}

bool fb_md_selection_copy(FileBrowser *fb) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    u32 lo, hi;
    if (!md_sel_range(fb, &lo, &hi)) return false;
    /* Worst case per glyph: 4 UTF-8 bytes + 1 line break = 5 bytes; +1 NUL
     * +64 slack covers the trailing terminator and any rounding. The
     * previous `*4 + 64` formula silently truncated multi-line selections
     * once 64 lines/newlines were exceeded. */
    usize cap = (usize)(hi - lo) * 5 + 64;
    char *buf = (char *)malloc(cap);
    if (!buf) return false;
    usize n = 0;
    f32 prev_y = fb->md_glyph_rects[lo].y;
    for (u32 i = lo; i < hi && n + 5 < cap; i++) {
        const MdGlyphRect *g = &fb->md_glyph_rects[i];
        /* Match the half-pixel tolerance fb_md_draw_selection uses, so the
         * copy's line breaks line up with what the highlight visually
         * groups. */
        if (fabsf(g->y - prev_y) >= 0.5f) { buf[n++] = '\n'; prev_y = g->y; }
        u8 enc[4];
        u32 el = utf8_encode(g->cp, enc);
        for (u32 k = 0; k < el && n + 1 < cap; k++) buf[n++] = (char)enc[k];
    }
    buf[n] = '\0';
    platform_clipboard_set(buf);
    free(buf);
    return true;
}

u32 fb_md_outline_count(const FileBrowser *fb) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return 0;
    if (!fb->md_outline) return 0;
    return fb->md_outline_count;
}

u8 fb_md_outline_get(const FileBrowser *fb, u32 idx, char *out, usize out_cap) {
    if (out && out_cap) out[0] = '\0';
    if (!fb || !fb->md_outline || idx >= fb->md_outline_count) return 0;
    const MdOutlineItem *it = &fb->md_outline[idx];
    if (out && out_cap) {
        usize n = it->text_len;
        if (n > out_cap - 1) n = out_cap - 1;
        if (it->text && n) memcpy(out, it->text, n);
        out[n] = '\0';
    }
    return it->level ? it->level : 1;
}

void fb_md_scroll_to_heading(FileBrowser *fb, u32 idx) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return;
    if (!fb->md_outline || idx >= fb->md_outline_count) return;
    /* Land the heading near the top with a small gap above it. The wheel
     * handler applies the precise content/viewport clamp on the next event;
     * here we only need to avoid negative/over-scroll. */
    f32 target = fb->md_outline[idx].doc_y - 8.0f;
    if (target < 0.0f) target = 0.0f;
    if (fb->view_content_px > 0.0f && target > fb->view_content_px)
        target = fb->view_content_px;
    fb->view_scroll_px = target;
}

bool fb_md_hit_link(const FileBrowser *fb, f32 vx, f32 vy,
                    char *out, usize out_cap, u8 *out_kind) {
    if (!fb || !out || out_cap == 0) return false;
    if (fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    if (!fb->md_link_rects || fb->md_link_count == 0) return false;
    for (u32 i = 0; i < fb->md_link_count; i++) {
        const MdLinkRect *L = &fb->md_link_rects[i];
        if (L->kind == MD_LINKRECT_COPY) continue;   /* handled by fb_md_hit_copy */
        if (vx >= L->x && vx < L->x + L->w &&
            vy >= L->y && vy < L->y + L->h) {
            usize n = L->url_len < out_cap - 1 ? L->url_len : out_cap - 1;
            memcpy(out, L->url, n);
            out[n] = '\0';
            if (out_kind) *out_kind = L->kind;
            return true;
        }
    }
    return false;
}

/* Hit-test rendered task checkboxes (viewport px, same space as fb_md_hit_link).
 * On a hit, toggle the source "[ ]"<->"[x]" — validated against the live buffer
 * so a stale or recursive (callout/quote) offset can never corrupt the file —
 * then reparse, mark modified, and persist. Returns true when a box was hit. */
bool fb_md_toggle_task_at(FileBrowser *fb, f32 vx, f32 vy) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    if (!fb->md_task_rects || fb->md_task_count == 0 || !fb->view_content) return false;
    if (fb->source != FB_SOURCE_LOCAL) return false;   /* only local files persist */
    for (u32 i = 0; i < fb->md_task_count; i++) {
        const MdTaskRect *T = &fb->md_task_rects[i];
        if (vx < T->x || vx >= T->x + T->w || vy < T->y || vy >= T->y + T->h) continue;
        usize off = (usize)T->src_off;
        /* The state char must sit between '[' and ']' in the live buffer; a
         * recursive/arena-copied source maps elsewhere and fails this — so no
         * mutation happens on a desynced offset (the click is still consumed). */
        if (off == 0 || off + 1 >= fb->view_size) return true;
        char *c = &fb->view_content[off];
        if (c[-1] != '[' || c[1] != ']') return true;
        *c = (*c == ' ') ? 'x' : ' ';   /* toggle; any non-space => uncheck */
        fb->modified = true;
        fb_md_reparse(fb);              /* rebuild md_doc from the mutated buffer */
        FILE *wf = fopen(fb->view_path, "wb");
        if (wf) {
            fwrite(fb->view_content, 1, fb->view_size, wf);
            fclose(wf);
            fb->modified = false;
        }
        return true;
    }
    return false;
}

bool fb_md_hit_copy(const FileBrowser *fb, f32 vx, f32 vy,
                    const u8 **out_text, u32 *out_len) {
    if (!fb || fb->view_mode != FVIEW_MARKDOWN || fb->md_raw_mode) return false;
    if (!fb->md_link_rects || fb->md_link_count == 0) return false;
    for (u32 i = 0; i < fb->md_link_count; i++) {
        const MdLinkRect *L = &fb->md_link_rects[i];
        if (L->kind != MD_LINKRECT_COPY) continue;
        if (vx >= L->x && vx < L->x + L->w &&
            vy >= L->y && vy < L->y + L->h) {
            if (out_text) *out_text = L->url;
            if (out_len)  *out_len  = L->url_len;
            return true;
        }
    }
    return false;
}

void fb_handle_scroll(FileBrowser *fb, f32 dy, bool precise) {
    /* macOS NSEvent.scrollingDeltaY semantics:
     *   precise=true  → pixel delta (trackpad / Magic Mouse), already in
     *                   pixels — multiplying by 40 here was producing absurd
     *                   "warp" scrolls on every two-finger swipe.
     *   precise=false → line-unit delta (legacy wheel); 22 px ≈ one entry row,
     *                   so one click moves ~one entry. */
    f32 dy_px = precise ? dy : (dy * 22.0f);
    fb->scroll_offset_px -= dy_px;

    /* Clamp to valid range */
    if (fb->scroll_offset_px < 0) fb->scroll_offset_px = 0;
    f32 max_scroll = fb->content_height - fb->viewport_height;
    if (max_scroll < 0) max_scroll = 0;
    if (fb->scroll_offset_px > max_scroll) fb->scroll_offset_px = max_scroll;

    /* Keep legacy integer offset in sync for hit-testing */
    f32 entry_h = 22.0f;  /* approximate; exact value set during render */
    fb->scroll_offset = (i32)(fb->scroll_offset_px / entry_h);
    if (fb->scroll_offset < 0) fb->scroll_offset = 0;
    if (fb->scroll_offset >= fb->entry_count) fb->scroll_offset = fb->entry_count - 1;
    if (fb->scroll_offset < 0) fb->scroll_offset = 0;
}
