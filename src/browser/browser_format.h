#ifndef BROWSER_FORMAT_H
#define BROWSER_FORMAT_H

#include "browser/browser_types.h"

/* Keep page control characters visible as text rather than PTY keystrokes. */
usize browser_context_escape_controls(const char *in, char *out, usize cap);
usize browser_format_error_md(const char *url, const char *reason, i32 code,
                              const char *project_root, char *out, usize cap);

/* Format pick as agent-facing markdown/plain text.
 * Returns bytes written (excluding NUL), or 0 on failure.
 * out_cap should be >= BROWSER_PASTE_CAP. */
usize browser_format_pick_md(const BrowserPick *pick,
                             const char *project_root,
                             char *out, usize out_cap);

#endif
