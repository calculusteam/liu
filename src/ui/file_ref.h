/*
 * Liu — when are two file references the same file?
 *
 * A path is only half of a file's name. "/shared/item.txt" on this machine, on
 * server A and on server B are three files, so "is this a paste onto itself?"
 * cannot be answered by comparing the two path STRINGS alone: a copy from one
 * server to the same path on another would be refused as a self-paste before a
 * byte moved.
 *
 * A reference here is (remote?, namespace, path). The namespace of a remote
 * reference is the Session it came through — an opaque pointer that is only
 * ever COMPARED. The file clipboard holds a retained reference on that Session
 * (FileClipboardItem.session), so the address cannot be reused by another
 * session while an item still names it. Local files share one namespace.
 *
 * Deliberately not attempted: recognising that two different sessions reach the
 * same server and filesystem. Nothing on this side of an SFTP channel can know
 * that (user, chroot and mount all change it), and the answer does not need it
 * — the destination's no-clobber check refuses to write over an existing file
 * whichever session it is seen through.
 *
 * Pure: no ui.h, nothing dereferenced.
 */
#ifndef UI_FILE_REF_H
#define UI_FILE_REF_H

#include "core/types.h"
#include <string.h>

static inline bool file_ref_same_namespace(bool a_remote, const void *a_ns,
                                           bool b_remote, const void *b_ns) {
    if (a_remote != b_remote) return false;
    if (!a_remote) return true;                 /* one local namespace */
    return a_ns != NULL && a_ns == b_ns;        /* the same live session */
}

static inline bool file_ref_same(bool a_remote, const void *a_ns, const char *a_path,
                                 bool b_remote, const void *b_ns, const char *b_path) {
    if (!a_path || !b_path) return false;
    return file_ref_same_namespace(a_remote, a_ns, b_remote, b_ns) &&
           strcmp(a_path, b_path) == 0;
}

#endif /* UI_FILE_REF_H */
