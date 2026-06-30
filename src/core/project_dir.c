#include "core/project_dir.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* mkdir one level, tolerating pre-existing dirs. */
static bool pd_mkdir(const char *path) {
    return mkdir(path, 0700) == 0 || errno == EEXIST;
}

bool liu_project_data_dir(const char *project_root, char *out, usize cap) {
    if (!project_root || !project_root[0] || !out || cap == 0) return false;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return false;

    /* Claude-style slug: every path separator (and the drive colon)
     * becomes '-', so "/Volumes/src/app" -> "-Volumes-src-app". Readable in
     * a file browser and collision-free enough for project roots. */
    char slug[768];
    usize n = 0;
    for (const char *p2 = project_root; *p2 && n + 1 < sizeof slug; p2++) {
        char c = *p2;
        slug[n++] = (c == '/' || c == '\\' || c == ':') ? '-' : c;
    }
    while (n > 1 && slug[n - 1] == '-') n--;   /* trailing slash -> drop */
    slug[n] = '\0';
    if (n == 0) return false;

    int w = snprintf(out, cap, "%s/.config/Liu/projects/%s", home, slug);
    if (w <= 0 || (usize)w >= cap) return false;

    /* Create the chain; the deepest mkdir result is the verdict. */
    char tmp[1024];
    if ((usize)snprintf(tmp, sizeof tmp, "%s/.config", home) < sizeof tmp)
        (void)pd_mkdir(tmp);
    if ((usize)snprintf(tmp, sizeof tmp, "%s/.config/Liu", home) < sizeof tmp)
        (void)pd_mkdir(tmp);
    if ((usize)snprintf(tmp, sizeof tmp, "%s/.config/Liu/projects", home) < sizeof tmp)
        (void)pd_mkdir(tmp);
    return pd_mkdir(out);
}
