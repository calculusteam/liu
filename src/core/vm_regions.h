/*
 * Classify process memory regions used to reconcile the application's
 * allocation ledger with the operating system's physical-footprint accounting.
 */
#ifndef CORE_VM_REGIONS_H
#define CORE_VM_REGIONS_H

#include "core/types.h"

/* The buckets a footprint decomposes into. Ordered so the report reads from
 * "memory Liu asked for" down to "memory the platform put there". */
typedef enum {
    VMR_MALLOC = 0,     /* every libmalloc zone: nano/tiny/small/large/huge  */
    VMR_STACK,          /* thread stacks — one per live thread, and they are
                         * 512 KiB each by default, so a pool that grows to 76
                         * threads is ~38 MiB that no ledger category owns    */
    VMR_GPU,            /* IOAccelerator / IOKit / LayerKit: Metal's private
                         * allocations, the drawable pool, IOSurfaces         */
    VMR_IMAGE,          /* CoreGraphics / CGImage / ImageIO staging           */
    VMR_SQLITE,         /* the vault's page cache and WAL mappings            */
    VMR_FRAMEWORK,      /* AppKit / Foundation / CoreUI                       */
    VMR_DYLIB,          /* private __DATA of loaded images, unshared pmap     */
    VMR_MAPPED,         /* OS_ALLOC_ONCE and other one-shot platform maps     */
    VMR_TOOL,           /* a sanitizer's own shadow, when one is present      */
    VMR_UNTAGGED,       /* tag 0: Liu's own __DATA/__bss, guard pages, misc   */
    VMR_OTHER,          /* tagged, but not by anything above                  */
    VMR_COUNT
} VmrCategory;

typedef struct {
    u64 dirty[VMR_COUNT];      /* private dirty bytes                        */
    u64 swapped[VMR_COUNT];    /* private, swapped out — still charged       */
    u64 virt[VMR_COUNT];       /* address space, for context only            */
    u32 regions[VMR_COUNT];

    u64 total_dirty;           /* sum of dirty + swapped over every category */

    /* THE KERNEL'S OWN BREAKDOWN OF THE SAME FOOTPRINT, from TASK_VM_INFO.
     *
     * The region walk above reconstructs a headless process's footprint
     * closely but not a GPU-backed app's. The reason is structural, not a flaw
     * in the walk: macOS charges IOKit and graphics mappings and the
     * compressor's pages to phys_footprint WITHOUT them appearing as private
     * dirty pages in the task's own VM map. A walk that only sums private
     * dirty CANNOT see them.
     *
     * So these terms are asked of the kernel directly instead of being left in
     * a residual, which lets a report name graphics and compressed pages
     * rather than an unexplained remainder. */
    u64 internal;              /* anonymous memory charged to the task        */
    u64 compressed;            /* held by the compressor, still charged       */
    u64 external;              /* file-backed and resident                    */
    u64 device;                /* device (GPU) memory                         */
    u64 gfx_footprint;         /* ledger_tag_graphics_footprint               */
    u64 media_footprint;       /* ledger_tag_media_footprint                  */
    u64 phys_footprint;        /* what all of the above is being explained    */
    u64 shared_resident;       /* excluded from the total, reported so the
                                * reader can see how much was excluded       */
    u32 region_count;
    bool available;            /* false where the platform cannot answer     */
} VmrSnapshot;

/* Fill `out` from the CURRENT process. Safe to call from any thread; it takes
 * no locks of ours and the kernel serialises its own map. */
void vmr_snapshot(VmrSnapshot *out);

const char *vmr_category_name(VmrCategory c);

#endif /* CORE_VM_REGIONS_H */
