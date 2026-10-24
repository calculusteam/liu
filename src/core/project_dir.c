#include "core/project_dir.h"
#include "core/net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* mkdir one level, tolerating pre-existing dirs. */
static bool pd_mkdir(const char *path) {
    if (!path || !path[0]) return false;
    if (mkdir(path, 0700) == 0) return true;
    if (errno != EEXIST) return false;
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

bool liu_project_data_dir(const char *project_root, char *out, usize cap) {
    if (!project_root || !project_root[0] || !out || cap == 0) return false;
    const char *home = net_home_dir();
    if (!home || !home[0]) return false;

    /* Retain the legacy slug for migration and a readable name prefix. */
    char slug[768];
    usize n = 0;
    for (const char *p2 = project_root; *p2; p2++) {
        /* Silent truncation aliases two distinct long roots to the same
         * history/suggestion directory. Fail instead. */
        if (n + 1u >= sizeof slug) return false;
        char c = *p2;
        slug[n++] = (c == '/' || c == '\\' || c == ':') ? '-' : c;
    }
    while (n > 1 && slug[n - 1] == '-') n--;   /* trailing slash -> drop */
    slug[n] = '\0';
    if (n == 0 || strcmp(slug, ".") == 0 || strcmp(slug, "..") == 0)
        return false;

    /* Hash the original bytes, including dashes and trailing separators. */
    u64 hash = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)project_root; *p; p++)
        hash = (hash ^ *p) * 1099511628211ULL;
    /* The readable prefix is capped at 80 BYTES, and printf's `%.80s` cuts
     * at a byte, not a codepoint: a multibyte UTF-8 sequence split there
     * leaves the name ending in a lone lead byte, which APFS refuses outright
     * (mkdir → EILSEQ, not EEXIST), silently disabling per-folder history for
     * that root. Back the cut off to a codepoint boundary first. */
    usize cut = n < 80 ? n : 80;
    while (cut > 0 && cut < n && ((unsigned char)slug[cut] & 0xC0) == 0x80) cut--;
    int w = snprintf(out, cap, "%s/.config/Liu/projects/%.*s--%016llx",
                     home, (int)cut, slug, (unsigned long long)hash);
    if (w <= 0 || (usize)w >= cap) return false;

    /* Create the chain; the deepest mkdir result is the verdict. */
    char tmp[1024];
    int n1 = snprintf(tmp, sizeof tmp, "%s/.config", home);
    if (n1 < 0 || (usize)n1 >= sizeof tmp || !pd_mkdir(tmp)) return false;
    n1 = snprintf(tmp, sizeof tmp, "%s/.config/Liu", home);
    if (n1 < 0 || (usize)n1 >= sizeof tmp || !pd_mkdir(tmp)) return false;
    n1 = snprintf(tmp, sizeof tmp, "%s/.config/Liu/projects", home);
    if (n1 < 0 || (usize)n1 >= sizeof tmp || !pd_mkdir(tmp)) return false;
    /* Migrate only a legacy directory whose explicit identity matches.
     * Ambiguous, unstamped stores remain untouched for legacy recovery. */
    struct stat st;
    if (lstat(out, &st) != 0 && errno == ENOENT) {
        char old[1024], stamp[1100], identity[1024];
        int n = snprintf(old, sizeof old, "%s/.config/Liu/projects/%s", home, slug);
        if (n > 0 && (usize)n < sizeof old) {
            snprintf(stamp, sizeof stamp, "%s/path", old);
            FILE *f = fopen(stamp, "r");
            if (f) {
                bool match = fgets(identity, sizeof identity, f) != NULL;
                fclose(f);
                if (match) {
                    identity[strcspn(identity, "\r\n")] = 0;
                    if (!strcmp(identity, project_root)) (void)rename(old, out);
                }
            }
        }
    }
    return pd_mkdir(out);
}

void liu_project_data_stamp(const char *project_root) {
    char dir[1024];
    if (!liu_project_data_dir(project_root, dir, sizeof dir)) return;

    char stamp[1100];
    int n = snprintf(stamp, sizeof stamp, "%s/path", dir);
    if (n <= 0 || (usize)n >= sizeof stamp) return;

    /* The exact identity lets folder discovery avoid reversing a slug. */
    char have[1100];
    FILE *r = fopen(stamp, "r");
    if (r) {
        usize n = fread(have, 1, sizeof have - 1, r);
        fclose(r);
        have[n] = '\0';
        char *nl = strchr(have, '\n');
        if (nl) *nl = '\0';
        if (strcmp(have, project_root) == 0) return;       /* already correct */
    }

    FILE *f = fopen(stamp, "w");
    if (!f) return;
    fprintf(f, "%s\n", project_root);
    fclose(f);
}
