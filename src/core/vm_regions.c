/* See core/vm_regions.h for what this measures and why resident is not it. */
#include "core/vm_regions.h"
#include "core/pty_reader.h"   /* PTY_READER_VM_TAG */

#include <string.h>

#if defined(PLATFORM_MACOS) || defined(__APPLE__)
  #include <mach/mach.h>
  #include <mach/mach_vm.h>
  #include <mach/vm_statistics.h>
#else
  #include <stdio.h>
#endif

static const char *const kNames[VMR_COUNT] = {
    "malloc", "thread-stacks", "gpu/iokit", "image/cg", "sqlite",
    "framework", "dylib-data", "mapped", "sanitizer", "untagged", "other",
};

const char *vmr_category_name(VmrCategory c) {
    return (c >= 0 && c < VMR_COUNT) ? kNames[c] : "?";
}

#if defined(PLATFORM_MACOS) || defined(__APPLE__)

/* The kernel's user_tag namespace is flat and sparse; this is the mapping from
 * the tags a Liu process actually produces to the buckets the audit reports.
 * Anything unlisted lands in VMR_OTHER rather than being silently folded into
 * a neighbour — an unattributed byte has to stay visible. */
static VmrCategory tag_category(unsigned tag) {
    switch (tag) {
    case VM_MEMORY_MALLOC:
    case VM_MEMORY_MALLOC_SMALL:
    case VM_MEMORY_MALLOC_LARGE:
    case VM_MEMORY_MALLOC_HUGE:
    case VM_MEMORY_MALLOC_TINY:
    case VM_MEMORY_MALLOC_NANO:
    case VM_MEMORY_REALLOC:
    case VM_MEMORY_MALLOC_LARGE_REUSABLE:
    case VM_MEMORY_MALLOC_LARGE_REUSED:          return VMR_MALLOC;
    /* The pty gather rings: Liu's own heap by another route. They are
     * mapped directly (pty_reader.c, ring_map) so that they can be
     * wired. */
    case PTY_READER_VM_TAG:                      return VMR_MALLOC;
    case VM_MEMORY_STACK:                        return VMR_STACK;
    case VM_MEMORY_IOKIT:
    case VM_MEMORY_IOACCELERATOR:
    case VM_MEMORY_LAYERKIT:                     return VMR_GPU;
    case VM_MEMORY_COREGRAPHICS:
    case VM_MEMORY_CGIMAGE:
    case VM_MEMORY_IMAGEIO:                      return VMR_IMAGE;
    case VM_MEMORY_SQLITE:                       return VMR_SQLITE;
    case VM_MEMORY_APPKIT:
    case VM_MEMORY_FOUNDATION:
    case VM_MEMORY_COREUI:                       return VMR_FRAMEWORK;
    case VM_MEMORY_DYLIB:
    case VM_MEMORY_UNSHARED_PMAP:                return VMR_DYLIB;
    case VM_MEMORY_OS_ALLOC_ONCE:                return VMR_MAPPED;
    case VM_MEMORY_ANALYSIS_TOOL:                return VMR_TOOL;
    case 0:                                      return VMR_UNTAGGED;
    default:                                     return VMR_OTHER;
    }
}

void vmr_snapshot(VmrSnapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);

    vm_size_t page = 0;
    if (host_page_size(mach_host_self(), &page) != KERN_SUCCESS || page == 0)
        page = 4096;

    mach_vm_address_t addr = 0;
    for (;;) {
        mach_vm_size_t size = 0;
        vm_region_submap_info_data_64_t info;
        mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        /* depth is reset every iteration on purpose: recurse() uses it as an
         * in/out cursor and carrying it forward walks into a submap and never
         * comes back out. */
        natural_t depth = 1;
        if (mach_vm_region_recurse(mach_task_self(), &addr, &size, &depth,
                                   (vm_region_recurse_info_t)&info,
                                   &cnt) != KERN_SUCCESS)
            break;
        if (size == 0) break;   /* belt: a zero-length region would not advance */

        VmrCategory c = tag_category(info.user_tag & 0xffu);
        /* PRIVATE only. A shared region — the dyld cache above all — is
         * resident in this task and charged to none; including it overstates
         * the footprint several times over. SM_COW counts because a
         * copy-on-write page that has been WRITTEN is private and dirty, and
         * `pages_dirtied` is exactly the ones that were. */
        bool priv = (info.share_mode == SM_PRIVATE ||
                     info.share_mode == SM_PRIVATE_ALIASED ||
                     info.share_mode == SM_COW);
        if (priv) {
            u64 d = (u64)info.pages_dirtied      * (u64)page;
            u64 s = (u64)info.pages_swapped_out  * (u64)page;
            out->dirty[c]   += d;
            out->swapped[c] += s;
            out->total_dirty += d + s;
        } else {
            out->shared_resident += (u64)info.pages_resident * (u64)page;
        }
        out->virt[c] += (u64)size;
        out->regions[c]++;
        out->region_count++;

        addr += size;
    }
    /* The kernel's own ledger for the same footprint. See the header: the
     * region walk cannot see IOKit/graphics mappings or compressed pages, and
     * in a GPU app those ARE the footprint. */
    task_vm_info_data_t vi;
    mach_msg_type_number_t vn = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &vn) == KERN_SUCCESS) {
        out->internal   = (u64)vi.internal;
        out->compressed = (u64)vi.compressed;
        out->external   = (u64)vi.external;
        out->device     = (u64)vi.device;
        if (vn >= TASK_VM_INFO_REV1_COUNT) out->phys_footprint = (u64)vi.phys_footprint;
        /* The tagged ledgers arrive in later revisions; a shorter answer
         * leaves them zero rather than reading past what the kernel filled. */
        if (vn >= TASK_VM_INFO_REV4_COUNT) {
            out->gfx_footprint   = (u64)vi.ledger_tag_graphics_footprint;
            out->media_footprint = (u64)vi.ledger_tag_media_footprint;
        }
    }
    out->available = true;
}

#else  /* Linux */

/* /proc/self/smaps has the same per-mapping Private_Dirty the mach walk sums,
 * but no tag namespace — so everything lands in one bucket and the report says
 * so rather than inventing an attribution the platform did not give. */
void vmr_snapshot(VmrSnapshot *out) {
    if (!out) return;
    memset(out, 0, sizeof *out);
    FILE *f = fopen("/proc/self/smaps_rollup", "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned long kb = 0;
        if (sscanf(line, "Private_Dirty: %lu kB", &kb) == 1)
            out->dirty[VMR_UNTAGGED] += (u64)kb * 1024u;
        else if (sscanf(line, "Swap: %lu kB", &kb) == 1)
            out->swapped[VMR_UNTAGGED] += (u64)kb * 1024u;
        else if (sscanf(line, "Shared_Dirty: %lu kB", &kb) == 1)
            out->shared_resident += (u64)kb * 1024u;
    }
    fclose(f);
    out->total_dirty = out->dirty[VMR_UNTAGGED] + out->swapped[VMR_UNTAGGED];
    out->region_count = 1;
    out->available = out->total_dirty > 0;
}

#endif
