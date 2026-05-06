/**
 * vmm.c - Virtual Memory Manager for CamelOS
 *
 * Implements the full virtual memory subsystem:
 *   - Physical Frame Allocator (PMM) using a bitmap
 *   - Per-process address spaces with VMA tracking
 *   - Copy-on-write (COW) fork
 *   - Demand paging and stack growth
 *   - mmap/munmap and brk syscalls
 *
 * Integration notes:
 *   - Call vmm_init() after init_paging() and init_heap().
 *   - Replace page_fault_handler() in paging.c with a call to
 *         vmm_handle_page_fault(fault_addr, regs.err_code);
 *   - The kernel address space wraps the existing kernel_directory.
 */

#include "vmm.h"
#include "memory.h"
#include "string.h"
#include "task.h"
#include "scheduler.h"
#include "../hal/cpu/paging.h"
#include "../hal/drivers/serial.h"

/* ========================================================================
 * External References
 * ======================================================================== */

/* Defined in hal/cpu/paging.c */
extern page_directory_t* kernel_directory;
extern page_directory_t* current_directory;

/* Defined in linker script - end of kernel BSS */
extern uint32_t _bss_end;

/* ========================================================================
 * Logging Helper
 * ======================================================================== */

/* printk is available from string.c and supports %s, %d, %c, %02X */
#define VMM_LOG(fmt, ...)  do { printk("[VMM] " fmt, ##__VA_ARGS__); } while(0)
#define VMM_WARN(fmt, ...) do { printk("[VMM WARN] " fmt, ##__VA_ARGS__); } while(0)
#define VMM_ERR(fmt, ...)  do { printk("[VMM ERR] " fmt, ##__VA_ARGS__); } while(0)

/* ========================================================================
 * PMM - Physical Frame Allocator (Bitmap)
 * ======================================================================== */

/*
 * The bitmap tracks the allocation state of every 4KB physical frame.
 * Bit set (1) = frame is in use / reserved.
 * Bit clear (0) = frame is free.
 *
 * We store the bitmap as an array of uint32_t, each holding 32 bits.
 */

static uint32_t* pmm_bitmap     = 0;   /* Bitmap storage        */
static uint32_t  pmm_total_frames = 0; /* Total number of frames */
static uint32_t  pmm_used_frames  = 0; /* Frames currently used  */
static uint32_t  pmm_reserved     = 0; /* Frames permanently reserved */
static uint32_t  pmm_bitmap_size  = 0; /* Number of uint32_t entries */

/* Statistics snapshot for vmm_get_frame_stats() */
static frame_stats_t pmm_stats;

/* ---- Bitmap helpers ---- */

static inline void bitmap_set(uint32_t frame)
{
    pmm_bitmap[frame / 32] |= (1u << (frame % 32));
}

static inline void bitmap_clear(uint32_t frame)
{
    pmm_bitmap[frame / 32] &= ~(1u << (frame % 32));
}

static inline int bitmap_test(uint32_t frame)
{
    return (pmm_bitmap[frame / 32] >> (frame % 32)) & 1;
}

/* ---- PMM Implementation ---- */

void pmm_init(uint32_t total_memory_kb)
{
    pmm_total_frames = (total_memory_kb * 1024) / PAGE_SIZE;
    pmm_bitmap_size  = (pmm_total_frames + 31) / 32;

    /* Allocate bitmap storage from the kernel heap */
    pmm_bitmap = (uint32_t*)kmalloc(pmm_bitmap_size * sizeof(uint32_t));
    if (!pmm_bitmap) {
        VMM_ERR("PMM: failed to allocate bitmap!\n");
        return;
    }

    /* Start with all frames free */
    memset(pmm_bitmap, 0, pmm_bitmap_size * sizeof(uint32_t));
    pmm_used_frames = 0;
    pmm_reserved    = 0;

    VMM_LOG("PMM initialized: %d frames (%d MB), bitmap %d bytes\n",
            pmm_total_frames,
            (pmm_total_frames * PAGE_SIZE) / (1024 * 1024),
            pmm_bitmap_size * 4);
}

uint32_t pmm_alloc_frame(void)
{
    /* Linear scan for a free frame.
     * For performance we could keep a hint index; for simplicity we scan. */
    for (uint32_t i = 0; i < pmm_total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            pmm_used_frames++;
            return i * PAGE_SIZE;
        }
    }
    VMM_WARN("PMM: out of physical frames!\n");
    return 0;  /* Failure */
}

void pmm_free_frame(uint32_t frame_addr)
{
    if (frame_addr == 0) return;
    if (frame_addr & 0xFFF) {
        VMM_WARN("PMM: free_frame addr 0x%x not page-aligned\n", frame_addr);
        return;
    }
    uint32_t frame = frame_addr / PAGE_SIZE;
    if (frame >= pmm_total_frames) {
        VMM_WARN("PMM: free_frame index %d out of range\n", frame);
        return;
    }
    if (!bitmap_test(frame)) {
        VMM_WARN("PMM: double-free of frame 0x%x\n", frame_addr);
        return;
    }
    bitmap_clear(frame);
    pmm_used_frames--;
}

uint32_t pmm_alloc_frames(uint32_t count)
{
    if (count == 0) return 0;
    if (count == 1) return pmm_alloc_frame();

    /* Find 'count' consecutive free frames */
    uint32_t run = 0;
    for (uint32_t i = 0; i < pmm_total_frames; i++) {
        if (!bitmap_test(i)) {
            run++;
            if (run == count) {
                uint32_t start = i - count + 1;
                for (uint32_t j = start; j <= i; j++) {
                    bitmap_set(j);
                    pmm_used_frames++;
                }
                return start * PAGE_SIZE;
            }
        } else {
            run = 0;
        }
    }
    VMM_WARN("PMM: alloc_frames(%d) - not enough contiguous frames\n", count);
    return 0;
}

void pmm_free_frames(uint32_t start_addr, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        pmm_free_frame(start_addr + i * PAGE_SIZE);
    }
}

void pmm_mark_region_used(uint32_t start, uint32_t size)
{
    uint32_t first = start / PAGE_SIZE;
    uint32_t last  = (start + size - 1) / PAGE_SIZE;

    if (size == 0) return;

    for (uint32_t i = first; i <= last && i < pmm_total_frames; i++) {
        if (!bitmap_test(i)) {
            bitmap_set(i);
            pmm_used_frames++;
            pmm_reserved++;
        }
    }
}

void pmm_mark_region_free(uint32_t start, uint32_t size)
{
    uint32_t first = start / PAGE_SIZE;
    uint32_t last  = (start + size - 1) / PAGE_SIZE;

    if (size == 0) return;

    for (uint32_t i = first; i <= last && i < pmm_total_frames; i++) {
        if (bitmap_test(i)) {
            bitmap_clear(i);
            pmm_used_frames--;
            if (pmm_reserved > 0) pmm_reserved--;
        }
    }
}

/* ========================================================================
 * COW Reference Counting
 * ======================================================================== */

/*
 * Each physical frame has a reference count tracking how many page table
 * entries point to it.  When ref_count == 1 the owner can make the page
 * writable again.  When ref_count drops to 0 the frame should already
 * have been freed via pmm_free_frame().
 */

static uint32_t* cow_ref_counts = 0;

static inline uint32_t frame_index(uint32_t phys_addr)
{
    return phys_addr / PAGE_SIZE;
}

static inline void cow_ref_inc(uint32_t phys_addr)
{
    uint32_t idx = frame_index(phys_addr);
    if (idx < pmm_total_frames)
        cow_ref_counts[idx]++;
}

static inline uint32_t cow_ref_get(uint32_t phys_addr)
{
    uint32_t idx = frame_index(phys_addr);
    if (idx < pmm_total_frames)
        return cow_ref_counts[idx];
    return 0;
}

static inline void cow_ref_dec(uint32_t phys_addr)
{
    uint32_t idx = frame_index(phys_addr);
    if (idx < pmm_total_frames && cow_ref_counts[idx] > 0)
        cow_ref_counts[idx]--;
}

/* ========================================================================
 * VMM Globals
 * ======================================================================== */

address_space_t* kernel_address_space = 0;

/* ========================================================================
 * Internal Helpers
 * ======================================================================== */

/** Page-align an address downward. */
static inline uint32_t page_floor(uint32_t addr)
{
    return addr & ~(PAGE_SIZE - 1);
}

/** Page-align an address upward. */
static inline uint32_t page_ceil(uint32_t addr)
{
    return (addr + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
}

/** Check if an address is page-aligned. */
static inline int is_page_aligned(uint32_t addr)
{
    return (addr & 0xFFF) == 0;
}

/**
 * Get the page directory for an address space.
 * Falls back to kernel_directory if space is NULL.
 */
static inline page_directory_t* get_dir(address_space_t* space)
{
    if (space && space->page_dir)
        return space->page_dir;
    return kernel_directory;
}

/* ========================================================================
 * Page Table Manipulation (implements map_page logic)
 * ======================================================================== */

/**
 * Ensure a page table exists for the given page directory index.
 * Allocates and links a new page table if needed.
 *
 * @return Pointer to the page_table_t, or NULL on allocation failure.
 */
static page_table_t* ensure_page_table(page_directory_t* dir, uint32_t table_idx)
{
    if (table_idx >= TABLES_PER_DIR)
        return 0;

    if (dir->tables[table_idx]) {
        return dir->tables[table_idx];
    }

    /* Allocate a new page table (4KB aligned) */
    uint32_t pt_phys;
    page_table_t* pt = (page_table_t*)kmalloc_ap(sizeof(page_table_t), &pt_phys);
    if (!pt) {
        VMM_ERR("Failed to allocate page table for index %d\n", table_idx);
        return 0;
    }

    memset(pt, 0, sizeof(page_table_t));

    /* Link into the page directory.
     * tablesPhysical stores the physical address | permission flags.
     * The CPU reads tablesPhysical when walking the page tables. */
    dir->tables[table_idx]         = pt;
    dir->tablesPhysical[table_idx] = pt_phys | VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER;

    return pt;
}

/**
 * Set a page table entry in a page directory.
 *
 * @param dir        Page directory
 * @param virt       Virtual address (page-aligned)
 * @param phys       Physical frame address (page-aligned)
 * @param flags      VMM_FLAG_* bits (low 12 bits of the PTE)
 * @return 0 on success, -1 on failure
 */
static int pt_set_entry(page_directory_t* dir, uint32_t virt, uint32_t phys,
                        uint32_t flags)
{
    uint32_t table_idx = virt >> 22;            /* virt / 0x400000 */
    uint32_t page_idx  = (virt >> 12) & 0x3FF;  /* (virt / 0x1000) % 1024 */

    page_table_t* pt = ensure_page_table(dir, table_idx);
    if (!pt) return -1;

    /* Merge physical address (bits 12-31) with flags (bits 0-11).
     * The COW flag is software-only and lives in the reserved bits. */
    pt->entries[page_idx] = (phys & 0xFFFFF000) | (flags & 0xFFF);

    /* Flush the TLB entry for this page */
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");

    return 0;
}

/**
 * Clear a page table entry (unmap the page).
 *
 * @return 0 on success, -1 if page was not present
 */
static int pt_clear_entry(page_directory_t* dir, uint32_t virt)
{
    uint32_t table_idx = virt >> 22;
    uint32_t page_idx  = (virt >> 12) & 0x3FF;

    if (!dir->tables[table_idx])
        return -1;

    page_table_t* pt = dir->tables[table_idx];
    if (!(pt->entries[page_idx] & VMM_FLAG_PRESENT))
        return -1;

    pt->entries[page_idx] = 0;

    /* Flush TLB */
    asm volatile("invlpg (%0)" :: "r"(virt) : "memory");

    return 0;
}

/**
 * Read a page table entry.
 *
 * @return The raw PTE value, or 0 if not present / table doesn't exist.
 */
static uint32_t pt_get_entry(page_directory_t* dir, uint32_t virt)
{
    uint32_t table_idx = virt >> 22;
    uint32_t page_idx  = (virt >> 12) & 0x3FF;

    if (!dir->tables[table_idx])
        return 0;

    return dir->tables[table_idx]->entries[page_idx];
}

/* ========================================================================
 * VMM Core
 * ======================================================================== */

void vmm_init(void)
{
    VMM_LOG("Initializing Virtual Memory Manager...\n");

    /*
     * 1. Initialize the PMM for 128 MB of physical memory.
     *
     * The kernel heap is initialized with 64 MB starting at _bss_end.
     * We mark all frames from 0 to (heap_end) as used so the PMM will
     * not hand them out for user pages.
     *
     * Since the kernel is identity-mapped, virtual == physical here.
     */
    pmm_init(128 * 1024);  /* 128 MB in KB */

    /* Allocate COW reference count array (one entry per frame) */
    cow_ref_counts = (uint32_t*)kmalloc(pmm_total_frames * sizeof(uint32_t));
    if (cow_ref_counts) {
        memset(cow_ref_counts, 0, pmm_total_frames * sizeof(uint32_t));
    } else {
        VMM_WARN("COW refcount array allocation failed; COW will be limited\n");
    }

    /* Mark kernel-used memory as reserved.
     *
     * The heap spans from _bss_end for 64 MB.  Any frame below that
     * region plus the kernel code/data is in use.
     */
    uint32_t kernel_end = (uint32_t)&_bss_end;
    /* Round up to page boundary */
    kernel_end = page_ceil(kernel_end);
    /* Add the heap: 64 MB allocated in init_heap */
    uint32_t heap_end = kernel_end + 64 * 1024 * 1024;

    pmm_mark_region_used(0, heap_end);
    VMM_LOG("Kernel region 0 - 0x%x marked reserved\n", heap_end);

    /*
     * 2. Wrap the existing kernel page directory in an address_space_t.
     */
    kernel_address_space = (address_space_t*)kmalloc(sizeof(address_space_t));
    if (!kernel_address_space) {
        VMM_ERR("Failed to allocate kernel address space!\n");
        return;
    }
    memset(kernel_address_space, 0, sizeof(address_space_t));

    kernel_address_space->page_dir   = kernel_directory;
    kernel_address_space->vma_list   = 0;
    kernel_address_space->brk        = 0;
    kernel_address_space->stack_top  = 0;
    kernel_address_space->code_start = 0;
    kernel_address_space->code_end   = KERNEL_SPACE_END;
    kernel_address_space->ref_count  = 1;

    VMM_LOG("VMM initialized. Kernel address space at 0x%x\n",
            (uint32_t)kernel_address_space);
}

address_space_t* vmm_create_address_space(void)
{
    /* Allocate the address space structure */
    address_space_t* space = (address_space_t*)kmalloc(sizeof(address_space_t));
    if (!space) {
        VMM_ERR("create_address_space: kmalloc failed\n");
        return 0;
    }
    memset(space, 0, sizeof(address_space_t));

    /* Allocate a new page directory (page-aligned, get physical address) */
    uint32_t dir_phys;
    page_directory_t* dir = (page_directory_t*)kmalloc_ap(sizeof(page_directory_t), &dir_phys);
    if (!dir) {
        VMM_ERR("create_address_space: page directory alloc failed\n");
        kfree(space);
        return 0;
    }
    memset(dir, 0, sizeof(page_directory_t));

    /* The physical address of tablesPhysical is what CR3 needs.
     * tablesPhysical is at offset 0 in the struct, and kmalloc_ap
     * returns a page-aligned block, so dir_phys == &tablesPhysical[0]. */
    dir->physicalAddr = dir_phys;

    /* Share the kernel page tables (entries 0 through USER_DIR_INDEX-1).
     * These map the identity-mapped 0-128MB kernel region.
     * Both the virtual pointers and the physical+flags entries are shared. */
    for (uint32_t i = 0; i < USER_DIR_INDEX; i++) {
        dir->tables[i]         = kernel_directory->tables[i];
        dir->tablesPhysical[i] = kernel_directory->tablesPhysical[i];
    }

    /* Entries USER_DIR_INDEX..1023 are left zeroed (user space, not yet mapped). */

    space->page_dir   = dir;
    space->vma_list   = 0;
    space->brk        = USER_HEAP_BASE;
    space->stack_top  = USER_STACK_TOP;
    space->code_start = USER_CODE_START;
    space->code_end   = USER_CODE_START;  /* No code loaded yet */
    space->ref_count  = 1;

    /* Create default VMAs for the new process */

    /* Stack VMA: grows downward from USER_STACK_TOP.
     * We initially reserve USER_STACK_INIT bytes.  The stack bottom
     * is the start, the top is the end (exclusive). */
    uint32_t stack_bottom = USER_STACK_TOP - USER_STACK_INIT + PAGE_SIZE;
    uint32_t stack_end    = USER_STACK_TOP + PAGE_SIZE;  /* One page above top */
    vma_t* stack_vma = vmm_create_vma(stack_bottom, stack_end,
                                       VMA_TYPE_STACK,
                                       VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER,
                                       "[stack]");
    if (stack_vma) {
        vmm_add_vma(space, stack_vma);
    }

    /* Heap VMA: initially zero-length, grows via brk() */
    vma_t* heap_vma = vmm_create_vma(USER_HEAP_BASE, USER_HEAP_BASE,
                                      VMA_TYPE_HEAP,
                                      VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER,
                                      "[heap]");
    if (heap_vma) {
        vmm_add_vma(space, heap_vma);
    }

    VMM_LOG("Created address space 0x%x, dir 0x%x (phys 0x%x)\n",
            (uint32_t)space, (uint32_t)dir, dir_phys);

    return space;
}

void vmm_destroy_address_space(address_space_t* space)
{
    if (!space) return;

    VMM_LOG("Destroying address space 0x%x\n", (uint32_t)space);

    /* 1. Free all user VMAs and their backing pages */
    vma_t* vma = space->vma_list;
    while (vma) {
        vma_t* next = vma->next;

        /* Unmap and free physical frames for this VMA's pages */
        page_directory_t* dir = space->page_dir;
        if (dir) {
            for (uint32_t addr = page_floor(vma->start);
                 addr < page_ceil(vma->end);
                 addr += PAGE_SIZE)
            {
                uint32_t pte = pt_get_entry(dir, addr);
                if (pte & VMM_FLAG_PRESENT) {
                    uint32_t phys = pte & 0xFFFFF000;
                    /* Only free if not COW-shared with another process */
                    if (cow_ref_counts && cow_ref_get(phys) <= 1) {
                        pmm_free_frame(phys);
                    } else {
                        cow_ref_dec(phys);
                    }
                }
                pt_clear_entry(dir, addr);
            }
        }

        /* Free user-space page tables that we allocated.
         * Kernel page tables (indices 0..31) are shared, not freed.
         * We free a page table only if all its entries are clear. */
        if (dir) {
            for (uint32_t ti = USER_DIR_INDEX; ti < TABLES_PER_DIR; ti++) {
                page_table_t* pt = dir->tables[ti];
                if (!pt) continue;

                /* Check if all entries are zero */
                int empty = 1;
                for (int j = 0; j < PAGES_PER_TABLE; j++) {
                    if (pt->entries[j] != 0) { empty = 0; break; }
                }
                if (empty) {
                    dir->tables[ti] = 0;
                    dir->tablesPhysical[ti] = 0;
                    kfree(pt);
                }
            }
        }

        kfree(vma);
        vma = next;
    }

    /* 2. Free remaining user-space page tables */
    if (space->page_dir) {
        for (uint32_t ti = USER_DIR_INDEX; ti < TABLES_PER_DIR; ti++) {
            page_table_t* pt = space->page_dir->tables[ti];
            if (pt) {
                space->page_dir->tables[ti] = 0;
                space->page_dir->tablesPhysical[ti] = 0;
                kfree(pt);
            }
        }
        /* 3. Free the page directory structure itself */
        kfree(space->page_dir);
    }

    /* 4. Free the address space structure */
    kfree(space);
}

address_space_t* vmm_fork_address_space(address_space_t* parent)
{
    if (!parent || !parent->page_dir) return 0;

    VMM_LOG("Forking address space from 0x%x\n", (uint32_t)parent);

    /* Create a new address space with shared kernel mappings */
    address_space_t* child = vmm_create_address_space();
    if (!child) return 0;

    page_directory_t* pdir = parent->page_dir;
    page_directory_t* cdir = child->page_dir;

    /* Copy user-space page table entries with COW.
     *
     * For each present user page:
     *   - Mark the parent's entry as read-only + COW
     *   - Map the same physical frame in the child as read-only + COW
     *   - Increment the COW reference count
     *
     * Non-present entries are left as-is (demand-paged pages will be
     * allocated independently when either process touches them).
     */
    for (uint32_t ti = USER_DIR_INDEX; ti < TABLES_PER_DIR; ti++) {
        if (!pdir->tables[ti]) continue;

        /* Ensure the child has a page table for this index.
         * If the parent has entries here, we need a matching table. */
        page_table_t* cpt = ensure_page_table(cdir, ti);
        if (!cpt) {
            VMM_ERR("Fork: failed to allocate child page table %d\n", ti);
            continue;
        }

        page_table_t* ppt = pdir->tables[ti];

        for (uint32_t pi = 0; pi < PAGES_PER_TABLE; pi++) {
            uint32_t pte = ppt->entries[pi];
            if (!(pte & VMM_FLAG_PRESENT)) continue;

            uint32_t phys  = pte & 0xFFFFF000;
            uint32_t flags = pte & 0xFFF;

            /* Skip kernel-only pages (shouldn't happen in user range, but
             * be defensive) */
            if (!(flags & VMM_FLAG_USER)) continue;

            /* Mark as read-only + COW in the parent */
            flags &= ~VMM_FLAG_WRITABLE;
            flags |= VMM_FLAG_COW;
            ppt->entries[pi] = phys | flags;

            /* Map the same physical frame in the child as read-only + COW */
            cpt->entries[pi] = phys | flags;

            /* Increment COW reference count */
            if (cow_ref_counts) {
                cow_ref_inc(phys);
                /* First fork: the original mapping wasn't counted yet.
                 * If ref_count is now 2, that's correct (parent + child).
                 * If it was already >1 (multiple forks), increment is still correct. */
                if (cow_ref_get(phys) == 1) {
                    cow_ref_inc(phys);  /* Account for the original mapping */
                }
            }
        }
    }

    /* Flush the entire TLB for the parent by reloading CR3 */
    if (current_directory == pdir) {
        switch_page_directory(pdir);
    }

    /* Copy VMA list from parent to child */
    vma_t* pvma = parent->vma_list;
    while (pvma) {
        vma_t* cvma = vmm_create_vma(pvma->start, pvma->end, pvma->type,
                                      pvma->flags | VMM_FLAG_COW, pvma->name);
        if (cvma) {
            cvma->offset = pvma->offset;
            vmm_add_vma(child, cvma);
        }
        pvma = pvma->next;
    }

    /* Copy process memory metadata */
    child->brk        = parent->brk;
    child->stack_top  = parent->stack_top;
    child->code_start = parent->code_start;
    child->code_end   = parent->code_end;

    VMM_LOG("Forked address space: child 0x%x\n", (uint32_t)child);
    return child;
}

void vmm_switch_address_space(address_space_t* space)
{
    if (!space || !space->page_dir) {
        /* Switch to kernel directory */
        if (kernel_directory)
            switch_page_directory(kernel_directory);
        return;
    }
    switch_page_directory(space->page_dir);
}

/* ========================================================================
 * VMA Management
 * ======================================================================== */

vma_t* vmm_create_vma(uint32_t start, uint32_t end, vma_type_t type,
                       uint32_t flags, const char* name)
{
    vma_t* vma = (vma_t*)kmalloc(sizeof(vma_t));
    if (!vma) {
        VMM_ERR("create_vma: kmalloc failed\n");
        return 0;
    }
    memset(vma, 0, sizeof(vma_t));

    vma->start  = page_floor(start);
    vma->end    = page_ceil(end);
    vma->type   = type;
    vma->flags  = flags;
    vma->offset = 0;
    vma->name   = name;
    vma->next   = 0;

    return vma;
}

void vmm_destroy_vma(vma_t* vma)
{
    if (vma) kfree(vma);
}

vma_t* vmm_find_vma(address_space_t* space, uint32_t addr)
{
    if (!space) return 0;

    vma_t* vma = space->vma_list;
    while (vma) {
        if (addr >= vma->start && addr < vma->end)
            return vma;
        vma = vma->next;
    }
    return 0;
}

int vmm_add_vma(address_space_t* space, vma_t* vma)
{
    if (!space || !vma) return -1;

    /* Insert into sorted list (by start address) */
    vma_t** pp = &space->vma_list;
    while (*pp && (*pp)->start < vma->start) {
        pp = &(*pp)->next;
    }

    /* Check for overlap with existing VMA */
    if (*pp && vma->end > (*pp)->start) {
        VMM_WARN("add_vma: overlap with existing VMA [0x%x-0x%x)\n",
                 (*pp)->start, (*pp)->end);
        /* For simplicity, we still insert but the overlap is a bug.
         * A production kernel would merge or reject. */
    }

    vma->next = *pp;
    *pp = vma;
    return 0;
}

int vmm_remove_vma(address_space_t* space, uint32_t start, uint32_t end)
{
    if (!space) return -1;

    uint32_t rm_start = page_floor(start);
    uint32_t rm_end   = page_ceil(end);

    vma_t** pp = &space->vma_list;
    int removed = 0;

    while (*pp) {
        vma_t* vma = *pp;

        /* Check for overlap */
        if (vma->start >= rm_end) break;  /* Past the removal range */
        if (vma->end <= rm_start) {        /* Before the removal range */
            pp = &vma->next;
            continue;
        }

        /* This VMA overlaps the removal range.
         * Three cases: fully contained, overlaps left, overlaps right. */

        if (vma->start >= rm_start && vma->end <= rm_end) {
            /* Fully contained: remove entire VMA */
            *pp = vma->next;

            /* Unmap pages in this range */
            if (space->page_dir) {
                for (uint32_t addr = vma->start; addr < vma->end; addr += PAGE_SIZE) {
                    uint32_t pte = pt_get_entry(space->page_dir, addr);
                    if (pte & VMM_FLAG_PRESENT) {
                        uint32_t phys = pte & 0xFFFFF000;
                        if (cow_ref_counts && cow_ref_get(phys) <= 1) {
                            pmm_free_frame(phys);
                        } else {
                            cow_ref_dec(phys);
                        }
                    }
                    pt_clear_entry(space->page_dir, addr);
                }
            }

            kfree(vma);
            removed++;
            /* Don't advance pp; it already points to the next node */
        } else if (vma->start < rm_start && vma->end > rm_end) {
            /* Removal splits the VMA into two parts.
             * For simplicity, just shrink the existing VMA to the left part
             * and create a new VMA for the right part. */
            uint32_t old_end = vma->end;
            vma->end = rm_start;

            /* Unmap pages in the removed range */
            if (space->page_dir) {
                for (uint32_t addr = rm_start; addr < rm_end; addr += PAGE_SIZE) {
                    uint32_t pte = pt_get_entry(space->page_dir, addr);
                    if (pte & VMM_FLAG_PRESENT) {
                        uint32_t phys = pte & 0xFFFFF000;
                        if (cow_ref_counts && cow_ref_get(phys) <= 1) {
                            pmm_free_frame(phys);
                        } else {
                            cow_ref_dec(phys);
                        }
                    }
                    pt_clear_entry(space->page_dir, addr);
                }
            }

            /* Create right VMA if there's space */
            if (rm_end < old_end) {
                vma_t* right = vmm_create_vma(rm_end, old_end, vma->type,
                                               vma->flags, vma->name);
                if (right) {
                    right->next = vma->next;
                    vma->next = right;
                }
            }
            removed++;
            pp = &vma->next;
        } else if (vma->start < rm_start) {
            /* Overlaps on the right: shrink the VMA from the right */
            /* Unmap pages in the overlap */
            if (space->page_dir) {
                for (uint32_t addr = rm_start; addr < vma->end; addr += PAGE_SIZE) {
                    uint32_t pte = pt_get_entry(space->page_dir, addr);
                    if (pte & VMM_FLAG_PRESENT) {
                        uint32_t phys = pte & 0xFFFFF000;
                        if (cow_ref_counts && cow_ref_get(phys) <= 1) {
                            pmm_free_frame(phys);
                        } else {
                            cow_ref_dec(phys);
                        }
                    }
                    pt_clear_entry(space->page_dir, addr);
                }
            }
            vma->end = rm_start;
            removed++;
            pp = &vma->next;
        } else {
            /* Overlaps on the left: shrink the VMA from the left */
            if (space->page_dir) {
                for (uint32_t addr = vma->start; addr < rm_end && addr < vma->end; addr += PAGE_SIZE) {
                    uint32_t pte = pt_get_entry(space->page_dir, addr);
                    if (pte & VMM_FLAG_PRESENT) {
                        uint32_t phys = pte & 0xFFFFF000;
                        if (cow_ref_counts && cow_ref_get(phys) <= 1) {
                            pmm_free_frame(phys);
                        } else {
                            cow_ref_dec(phys);
                        }
                    }
                    pt_clear_entry(space->page_dir, addr);
                }
            }
            vma->start = rm_end;
            removed++;
            pp = &vma->next;
        }
    }

    return removed > 0 ? 0 : -1;
}

/**
 * Find a free region of the specified size in the user address space.
 * Scans the VMA list for gaps between existing VMAs.
 *
 * @param space  Address space to search
 * @param size   Required size in bytes (will be page-aligned)
 * @param hint   Preferred start address (0 = no preference)
 * @return Start address of the free region, or 0 if none found
 */
static uint32_t vmm_find_free_region(address_space_t* space, uint32_t size,
                                      uint32_t hint)
{
    uint32_t aligned_size = page_ceil(size);
    uint32_t search_start = USER_MMAP_BASE;

    if (hint && hint >= USER_SPACE_BASE && hint + aligned_size <= USER_SPACE_END) {
        search_start = page_floor(hint);
    }

    /* Simple scan: walk the VMA list and check gaps.
     * The VMA list is sorted by start address. */
    uint32_t cursor = search_start;

    vma_t* vma = space->vma_list;
    while (vma && cursor < USER_MMAP_END) {
        /* Skip VMAs below the search region */
        if (vma->end <= cursor) {
            vma = vma->next;
            continue;
        }

        /* Check the gap before this VMA */
        if (vma->start > cursor) {
            uint32_t gap = vma->start - cursor;
            if (gap >= aligned_size) {
                return cursor;
            }
        }

        /* Move cursor past this VMA */
        if (vma->end > cursor)
            cursor = vma->end;

        vma = vma->next;
    }

    /* Check after the last VMA */
    if (cursor + aligned_size <= USER_MMAP_END) {
        return cursor;
    }

    /* No free region found */
    return 0;
}

/* ========================================================================
 * Page Operations
 * ======================================================================== */

int vmm_map_page(address_space_t* space, uint32_t virt, uint32_t phys,
                 uint32_t flags)
{
    if (!is_page_aligned(virt) || !is_page_aligned(phys)) {
        VMM_ERR("map_page: addresses not page-aligned (v=0x%x p=0x%x)\n",
                virt, phys);
        return -1;
    }

    page_directory_t* dir = get_dir(space);
    if (!dir) return -1;

    return pt_set_entry(dir, virt, phys, flags);
}

int vmm_unmap_page(address_space_t* space, uint32_t virt)
{
    page_directory_t* dir = get_dir(space);
    if (!dir) return -1;

    return pt_clear_entry(dir, virt);
}

uint32_t vmm_get_physical(address_space_t* space, uint32_t virt)
{
    page_directory_t* dir = get_dir(space);
    if (!dir) return 0;

    uint32_t pte = pt_get_entry(dir, virt);
    if (pte & VMM_FLAG_PRESENT)
        return pte & 0xFFFFF000;

    return 0;
}

/* ========================================================================
 * Page Fault Handler
 * ======================================================================== */

int vmm_handle_page_fault(uint32_t fault_addr, uint32_t error_code)
{
    int present = !(error_code & 0x1);  /* Bit 0: 0 = page not present */
    int write   = (error_code & 0x2);   /* Bit 1: 1 = write access     */
    int user    = (error_code & 0x4);   /* Bit 2: 1 = user mode        */

    page_directory_t* dir = current_directory;
    if (!dir) return -1;

    /* Resolve the current task's address space for per-process VM operations */
    task_t* cur_task = scheduler_get_current();
    address_space_t* cur_space = 0;
    if (cur_task && cur_task->address_space) {
        cur_space = (address_space_t*)cur_task->address_space;
    }

    /* --- Case 1: Write to a present page (COW or protection violation) --- */
    if (present && write) {
        uint32_t pte = pt_get_entry(dir, fault_addr);

        /* Check if this is a COW page */
        if (pte & VMM_FLAG_COW) {
            if (cow_ref_counts) {
                uint32_t phys = pte & 0xFFFFF000;
                if (cow_ref_get(phys) > 1) {
                    /* More than one reference: must duplicate */
                    return vmm_cow_duplicate_page(cur_space, fault_addr);
                } else {
                    /* Only one reference left: reclaim the page as writable */
                    pte &= ~VMM_FLAG_COW;
                    pte |= VMM_FLAG_WRITABLE;
                    pt_set_entry(dir, page_floor(fault_addr), pte & 0xFFFFF000, pte & 0xFFF);
                    cow_ref_dec(phys);
                    return 0;
                }
            }
            /* No ref counts: always duplicate */
            return vmm_cow_duplicate_page(cur_space, fault_addr);
        }

        /* Not COW: genuine protection violation */
        VMM_ERR("Protection fault at 0x%x (write to read-only page)\n",
                fault_addr);
        return -1;
    }

    /* --- Case 2: Page not present (demand paging or stack growth) --- */
    if (!present) {
        /* Use the current task's address space resolved above.
         * Falls back to kernel_address_space if no per-process space. */

        vma_t* vma = 0;
        address_space_t* active_space = cur_space ? cur_space : kernel_address_space;

        if (fault_addr >= KERNEL_SPACE_END) {
            /* User-space fault: find the VMA in the current process's
             * address space. Falls back to kernel_address_space if no
             * per-process space is available. */
            vma = vmm_find_vma(active_space, fault_addr);
        } else {
            /* Kernel-space fault on a non-present page is always fatal */
            VMM_ERR("Kernel page fault at 0x%x (non-present)\n", fault_addr);
            return -1;
        }

        /* --- Stack growth --- */
        if (!vma && fault_addr >= KERNEL_SPACE_END) {
            /* Check if fault is near the stack region.
             * Stack grows downward; if fault is below the current stack
             * VMA, we extend it. */
            vma_t* svma = 0;
            vma_t* scan = 0;

            /* Find the stack VMA in the current process's address space.
             * Use active_space resolved above from scheduler_get_current(). */
            scan = active_space ? active_space->vma_list : 0;
            while (scan) {
                if (scan->type == VMA_TYPE_STACK) { svma = scan; break; }
                scan = scan->next;
            }

            if (svma && fault_addr >= svma->start - USER_STACK_INIT &&
                fault_addr < svma->end)
            {
                /* The fault is within stack growth range.
                 * Extend the stack VMA downward. */
                uint32_t new_start = page_floor(fault_addr);
                if (new_start < svma->start) {
                    svma->start = new_start;
                }
                vma = svma;  /* Treat as a demand-page within the stack VMA */
            }
        }

        /* --- Demand paging --- */
        if (vma) {
            /* Allocate a physical frame and map it */
            uint32_t frame = pmm_alloc_frame();
            if (!frame) {
                VMM_ERR("Out of memory for demand paging at 0x%x\n", fault_addr);
                return -1;
            }

            /* Zero-fill the frame (important for security and BSS pages) */
            memset((void*)frame, 0, PAGE_SIZE);

            /* Determine page flags from the VMA */
            uint32_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;
            if (vma->flags & VMM_FLAG_WRITABLE) flags |= VMM_FLAG_WRITABLE;

            uint32_t page_addr = page_floor(fault_addr);

            if (pt_set_entry(dir, page_addr, frame, flags) != 0) {
                VMM_ERR("Failed to map demand page at 0x%x\n", fault_addr);
                pmm_free_frame(frame);
                return -1;
            }

            /* Track the frame in COW ref counts */
            if (cow_ref_counts) {
                cow_ref_counts[frame_index(frame)] = 1;
            }

            return 0;  /* Handled */
        }

        /* No VMA found: segmentation fault equivalent */
        VMM_ERR("Segmentation fault at 0x%x (no VMA, present=%d write=%d user=%d)\n",
                fault_addr, present, write, user);
        return -1;
    }

    /* Other fault types (reserved bit, etc.) */
    VMM_ERR("Unhandled page fault at 0x%x (err=0x%x)\n", fault_addr, error_code);
    return -1;
}

/* ========================================================================
 * Memory Operations: mmap, munmap, brk
 * ======================================================================== */

void* vmm_mmap(address_space_t* space, uint32_t addr, uint32_t length,
               int prot, int flags, uint32_t offset)
{
    if (!space || length == 0) return (void*)0;

    uint32_t aligned_length = page_ceil(length);
    uint32_t map_addr;

    if (flags & MAP_FIXED) {
        /* Use the exact address provided */
        map_addr = page_floor(addr);
        if (map_addr < USER_SPACE_BASE || map_addr + aligned_length > USER_SPACE_END) {
            VMM_WARN("mmap MAP_FIXED addr 0x%x out of range\n", addr);
            return (void*)0;
        }
        /* Unmap any existing mappings in this range */
        vmm_remove_vma(space, map_addr, map_addr + aligned_length);
    } else {
        /* Find a free region */
        map_addr = vmm_find_free_region(space, aligned_length, addr);
        if (!map_addr) {
            VMM_WARN("mmap: no free region for %d bytes\n", aligned_length);
            return (void*)0;
        }
    }

    /* Create a VMA for this mapping */
    vma_type_t type = (flags & MAP_ANONYMOUS) ? VMA_TYPE_MMAP : VMA_TYPE_MMAP;
    uint32_t vma_flags = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_MMAP;

    if (prot & PROT_WRITE) vma_flags |= VMM_FLAG_WRITABLE;
    if (flags & MAP_SHARED) vma_flags |= VMM_FLAG_SHARED;

    vma_t* vma = vmm_create_vma(map_addr, map_addr + aligned_length,
                                 type, vma_flags,
                                 (flags & MAP_ANONYMOUS) ? "[anon]" : "[mmap]");
    if (!vma) {
        VMM_ERR("mmap: failed to create VMA\n");
        return (void*)0;
    }
    vma->offset = offset;

    if (vmm_add_vma(space, vma) != 0) {
        kfree(vma);
        return (void*)0;
    }

    /* For anonymous mappings, we use demand paging: pages are NOT allocated
     * now.  They will be allocated on first access (page fault).
     * This is the standard Linux approach and saves memory for sparse maps.
     *
     * For file-backed mappings, the filesystem layer would read pages
     * on demand.  Since we don't have a VFS mmap implementation yet,
     * all mmaps behave as anonymous for now.
     */

    return (void*)map_addr;
}

int vmm_munmap(address_space_t* space, uint32_t addr, uint32_t length)
{
    if (!space || length == 0) return -1;

    uint32_t start = page_floor(addr);
    uint32_t end   = page_ceil(addr + length);

    /* Range check */
    if (start < USER_SPACE_BASE || end > USER_SPACE_END)
        return -1;

    /* Remove VMAs and unmap pages in the range */
    return vmm_remove_vma(space, start, end);
}

int vmm_brk(address_space_t* space, uint32_t new_brk)
{
    if (!space) return -1;

    /* If new_brk is 0, return the current brk */
    if (new_brk == 0)
        return (int)space->brk;

    /* The new brk must be in user space and above the heap base */
    if (new_brk < USER_HEAP_BASE) {
        VMM_WARN("brk: 0x%x below heap base 0x%x\n", new_brk, USER_HEAP_BASE);
        return -1;
    }

    /* Cap the heap at a reasonable limit (64 MB above the base) */
    uint32_t heap_limit = USER_HEAP_BASE + 64 * 1024 * 1024;
    if (new_brk > heap_limit) {
        VMM_WARN("brk: 0x%x exceeds heap limit 0x%x\n", new_brk, heap_limit);
        return -1;
    }

    uint32_t old_brk = space->brk;
    page_directory_t* dir = space->page_dir;

    if (new_brk > old_brk) {
        /* Expanding the heap: map new pages.
         * We allocate physical frames for the new range.
         * The heap VMA is updated to cover the new range. */

        /* Find the heap VMA */
        vma_t* heap_vma = 0;
        vma_t* scan = space->vma_list;
        while (scan) {
            if (scan->type == VMA_TYPE_HEAP) { heap_vma = scan; break; }
            scan = scan->next;
        }

        if (heap_vma) {
            heap_vma->end = page_ceil(new_brk);
        }

        /* Map pages from old_brk to new_brk (demand-paged: we allocate now
         * for the heap since programs expect immediate access) */
        if (dir) {
            for (uint32_t addr = page_ceil(old_brk); addr < page_ceil(new_brk); addr += PAGE_SIZE) {
                /* Only map if not already present */
                if (!(pt_get_entry(dir, addr) & VMM_FLAG_PRESENT)) {
                    uint32_t frame = pmm_alloc_frame();
                    if (!frame) {
                        VMM_ERR("brk: out of memory at 0x%x\n", addr);
                        /* Roll back: set brk to last successful allocation */
                        space->brk = addr;
                        if (heap_vma) heap_vma->end = addr;
                        return -1;
                    }
                    memset((void*)frame, 0, PAGE_SIZE);
                    pt_set_entry(dir, addr, frame,
                                 VMM_FLAG_PRESENT | VMM_FLAG_WRITABLE | VMM_FLAG_USER);

                    if (cow_ref_counts)
                        cow_ref_counts[frame_index(frame)] = 1;
                }
            }
        }
    } else if (new_brk < old_brk) {
        /* Shrinking the heap: unmap and free pages */
        if (dir) {
            for (uint32_t addr = page_ceil(new_brk); addr < page_ceil(old_brk); addr += PAGE_SIZE) {
                uint32_t pte = pt_get_entry(dir, addr);
                if (pte & VMM_FLAG_PRESENT) {
                    uint32_t phys = pte & 0xFFFFF000;
                    if (cow_ref_counts && cow_ref_get(phys) <= 1) {
                        pmm_free_frame(phys);
                    } else {
                        cow_ref_dec(phys);
                    }
                }
                pt_clear_entry(dir, addr);
            }
        }

        /* Update heap VMA */
        vma_t* heap_vma = 0;
        vma_t* scan = space->vma_list;
        while (scan) {
            if (scan->type == VMA_TYPE_HEAP) { heap_vma = scan; break; }
            scan = scan->next;
        }
        if (heap_vma) {
            heap_vma->end = page_ceil(new_brk);
        }
    }

    space->brk = new_brk;
    return (int)new_brk;
}

/* ========================================================================
 * Copy-on-Write
 * ======================================================================== */

int vmm_cow_duplicate_page(address_space_t* space, uint32_t virt)
{
    page_directory_t* dir = get_dir(space);
    if (!dir) dir = current_directory;
    if (!dir) return -1;

    uint32_t page_addr = page_floor(virt);
    uint32_t pte = pt_get_entry(dir, page_addr);

    if (!(pte & VMM_FLAG_PRESENT)) {
        VMM_ERR("COW duplicate: page 0x%x not present\n", virt);
        return -1;
    }

    uint32_t old_phys = pte & 0xFFFFF000;
    uint32_t flags    = pte & 0xFFF;

    /* Allocate a new physical frame */
    uint32_t new_phys = pmm_alloc_frame();
    if (!new_phys) {
        VMM_ERR("COW duplicate: out of memory\n");
        return -1;
    }

    /* Copy the page content from the old frame to the new frame.
     * Both addresses are identity-mapped (virtual == physical). */
    memcpy((void*)new_phys, (void*)old_phys, PAGE_SIZE);

    /* Decrement COW reference count on the old frame */
    if (cow_ref_counts) {
        cow_ref_dec(old_phys);
    }

    /* Map the new frame as writable, removing the COW flag */
    flags &= ~(VMM_FLAG_COW);
    flags |= VMM_FLAG_WRITABLE;

    if (pt_set_entry(dir, page_addr, new_phys, flags) != 0) {
        VMM_ERR("COW duplicate: failed to remap 0x%x\n", virt);
        pmm_free_frame(new_phys);
        return -1;
    }

    /* Set ref count for the new frame */
    if (cow_ref_counts) {
        cow_ref_counts[frame_index(new_phys)] = 1;
    }

    return 0;
}

void vmm_cow_mark_range(address_space_t* space, uint32_t start, uint32_t end)
{
    if (!space || !space->page_dir) return;

    page_directory_t* dir = space->page_dir;
    uint32_t page_start = page_floor(start);
    uint32_t page_end   = page_ceil(end);

    for (uint32_t addr = page_start; addr < page_end; addr += PAGE_SIZE) {
        uint32_t pte = pt_get_entry(dir, addr);
        if (!(pte & VMM_FLAG_PRESENT)) continue;

        uint32_t phys  = pte & 0xFFFFF000;
        uint32_t flags = pte & 0xFFF;

        /* Only mark writable user pages as COW */
        if ((flags & VMM_FLAG_USER) && (flags & VMM_FLAG_WRITABLE)) {
            flags &= ~VMM_FLAG_WRITABLE;
            flags |= VMM_FLAG_COW;
            pt_set_entry(dir, addr, phys, flags);

            if (cow_ref_counts)
                cow_ref_inc(phys);
        }
    }

    /* Flush TLB by reloading CR3 */
    if (current_directory == dir) {
        switch_page_directory(dir);
    }
}

/* ========================================================================
 * Utilities
 * ======================================================================== */

frame_stats_t* vmm_get_frame_stats(void)
{
    pmm_stats.total_frames    = pmm_total_frames;
    pmm_stats.used_frames     = pmm_used_frames;
    pmm_stats.free_frames     = pmm_total_frames - pmm_used_frames;
    pmm_stats.reserved_frames = pmm_reserved;
    return &pmm_stats;
}

void vmm_dump_address_space(address_space_t* space)
{
    if (!space) {
        VMM_LOG("Address space: NULL\n");
        return;
    }

    VMM_LOG("=== Address Space 0x%x ===\n", (uint32_t)space);
    VMM_LOG("  Page dir: 0x%x (phys 0x%x)\n",
            (uint32_t)space->page_dir,
            space->page_dir ? space->page_dir->physicalAddr : 0);
    VMM_LOG("  brk:       0x%x\n", space->brk);
    VMM_LOG("  stack_top: 0x%x\n", space->stack_top);
    VMM_LOG("  code:      0x%x - 0x%x\n", space->code_start, space->code_end);
    VMM_LOG("  ref_count: %d\n", space->ref_count);
    VMM_LOG("  VMAs:\n");

    vma_t* vma = space->vma_list;
    int idx = 0;
    while (vma) {
        const char* type_names[] = {
            "UNUSED", "CODE", "DATA", "HEAP", "STACK", "MMAP", "SHARED"
        };
        const char* tname = (vma->type <= VMA_TYPE_SHARED)
                            ? type_names[vma->type] : "???";

        VMM_LOG("    [%d] 0x%08x - 0x%08x %s flags=0x%x %s\n",
                idx, vma->start, vma->end, tname, vma->flags,
                vma->name ? vma->name : "");
        vma = vma->next;
        idx++;
    }

    /* Print frame stats */
    frame_stats_t* fs = vmm_get_frame_stats();
    VMM_LOG("  PMM: %d total, %d used, %d free, %d reserved\n",
            fs->total_frames, fs->used_frames,
            fs->free_frames, fs->reserved_frames);

    VMM_LOG("=== End Address Space ===\n");
}
