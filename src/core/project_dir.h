#ifndef LIU_CORE_PROJECT_DIR_H
#define LIU_CORE_PROJECT_DIR_H

#include "core/types.h"

/* Resolve Liu's central per-project data dir for `project_root`, creating the
 * directory chain. Writes "~/.config/Liu/projects/<slug>" into `out` where the
 * slug is the project root with every path separator (and the drive colon)
 * turned into '-' (Claude-style: "/Volumes/src/app" -> "-Volumes-src-app").
 *
 * Returns true on success (dir exists/created and path fit in `out`). Used for
 * per-project sidecar storage (command history, suggestions). */
bool liu_project_data_dir(const char *project_root, char *out, usize cap);

#endif /* LIU_CORE_PROJECT_DIR_H */
