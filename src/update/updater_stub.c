/* updater_stub.c — no-op platform shim for non-macOS builds.
 * The auto-updater is macOS-only; these keep the call sites compiling.
 */
#include "update/updater.h"
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>

const char *updater_plat_arch(void) { return "unknown"; }
bool updater_plat_os_at_least(const char *min_os) { (void)min_os; return true; }
bool updater_plat_can_autoinstall(void) { return false; }

void updater_plat_fetch_feed(UpdateState *st, const char *url) {
    (void)url;
    snprintf(st->io_err, sizeof st->io_err, "Updates are macOS-only.");
    atomic_store(&st->io_ok, false);
    atomic_store(&st->io_done, true);
}
void updater_plat_download(UpdateState *st, const char *url, long long sz) {
    (void)url; (void)sz;
    atomic_store(&st->io_ok, false);
    atomic_store(&st->io_done, true);
}
void updater_plat_cancel(UpdateState *st) { (void)st; }
bool updater_plat_extract(const char *zip, char *out, size_t cap) { (void)zip; (void)out; (void)cap; return false; }
bool updater_plat_bundle_version(const char *app, char *out, size_t cap) { (void)app; (void)out; (void)cap; return false; }
void updater_plat_strip_quarantine(const char *path) { (void)path; }
bool updater_plat_install(const char *a, char *out, size_t cap) { (void)a; (void)out; (void)cap; return false; }
void updater_plat_relaunch_and_quit(const char *app) { (void)app; }
void updater_plat_cleanup_temp(const char *path) { (void)path; }
