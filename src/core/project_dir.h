#ifndef LIU_CORE_PROJECT_DIR_H
#define LIU_CORE_PROJECT_DIR_H

#include "core/types.h"

/* Resolve/create a per-project data directory using a readable prefix and
 * a hash of the complete original path. Explicitly stamped legacy stores
 * migrate automatically; ambiguous unstamped stores remain untouched. */
bool liu_project_data_dir(const char *project_root, char *out, usize cap);

/* Persist the original folder identity for Folder History. Call from write
 * paths; an identical existing stamp is left untouched. */
void liu_project_data_stamp(const char *project_root);

#endif /* LIU_CORE_PROJECT_DIR_H */
