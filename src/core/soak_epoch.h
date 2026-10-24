/* An epoch ends after both its operation quota and minimum duration are
 * reached. The caller continues operations until both conditions hold. */
#ifndef LIU_SOAK_EPOCH_H
#define LIU_SOAK_EPOCH_H
#include "core/types.h"

static inline bool soak_epoch_after_operation(u64 *remaining, f64 began,
                                               f64 now, f64 minimum_seconds) {
    if (*remaining) --*remaining;
    return *remaining == 0 &&
           (minimum_seconds <= 0.0 || now - began >= minimum_seconds);
}
#endif
