/**
 * vmm.h - Virtual Memory Manager for CamelOS
 *
 * Manages per-process address spaces, virtual memory areas (VMAs),
 * physical frame allocation, copy-on-write fork, demand paging,
 * mmap/munmap, and brk heap extension.
 *
 * Design:
 *   - Kernel space (0-128MB) is identity-mapped and shared across all processes.
 *   - User space starts at 0x08000000 and extends to 0x7FFFFFFF.
 *   - Each process has its own page directory and VMA list.
 *   - COW: on fork, parent and child share physical pages read-only;
 *     pages are duplicated lazily on write fault.
 *   - PMM: bitmap-based physical frame allocator.
 */

#ifndef VMM_H
#define VMM_H

#include "../include/types.h"
#include "../hal/cpu/paging.h"

/* ========================================================================
 * Page Constants
 * ======================================================================== */

#define PAGE_SIZE           0x1000      /* 4KB */
#define PAGE_SHIFT          12
#define PAGES_PER_TABLE     1024
#define TABLES_PER_DIR      1024
#define KERNEL_BASE         0xC0000000  /* 3GB - kernel virtual base (future) */

/* ========================================================================
 * VMM Page Flags (extend x86 paging flags)
 * ======================================================================== */

#define VMM_FLAG_PRESENT    0x01
#define VMM_FLAG_WRITABLE   0x02
#define VMM_FLAG_USER       0x04
#define VMM_FLAG_COW        0x200       /* Copy-on-write marker (software-only) */
#define VMM_FLAG_SHARED     0x400       /* Shared memory marker */
#define VMM_FLAG_MMAP       0x800       /* Memory-mapped file marker */

/* ========================================================================
 * User Address Space Layout
 * ======================================================================== */

#define KERNEL_SPACE_END    0x08000000  /* 128MB - end of identity-mapped kernel */
#define USER_CODE_START     0x08000000  /* Default load address for user code */
#define USER_HEAP_BASE      0x10000000  /* 256MB - heap starts here */
#define USER_MMAP_BASE      0x40000000  /* 1GB   - mmap region start */
#define USER_SPACE_BASE     KERNEL_SPACE_END  /* Start of user-accessible space */
#define USER_MMAP_END       0x6F000000  /* End of mmap region */
#define USER_STACK_TOP      0x7FFFF000u /* Top of user stack (page-aligned) */
#define USER_STACK_INIT     0x400000    /* 4MB initial stack size */
#define USER_SPACE_END      0x80000000u /* 2GB - end of user space */

/* First page directory entry index for user space */
#define USER_DIR_INDEX      (KERNEL_SPACE_END / 0x400000)  /* = 32 */

/* First page directory entry index for high kernel MMIO space.
 * Entries 512-1023 cover addresses 0x80000000-0xFFFFFFFF, which is where
 * device MMIO (VRAM at 0xFD000000, APIC at 0xFEE00000, etc.) is mapped.
 * These tables are shared across all address spaces. */
#define KERNEL_HIGH_DIR_INDEX  512

/* ========================================================================
 * Virtual Memory Region Types
 * ======================================================================== */

typedef enum {
    VMA_TYPE_UNUSED = 0,
    VMA_TYPE_CODE,          /* .text section */
    VMA_TYPE_DATA,          /* .data/.bss section */
    VMA_TYPE_HEAP,          /* Process heap (brk) */
    VMA_TYPE_STACK,         /* Process stack */
    VMA_TYPE_MMAP,          /* Memory-mapped file/anonymous */
    VMA_TYPE_SHARED,        /* Shared memory segment */
} vma_type_t;

/* ========================================================================
 * Virtual Memory Area Descriptor
 * ======================================================================== */

typedef struct vma {
    uint32_t start;         /* Start virtual address (page-aligned) */
    uint32_t end;           /* End virtual address (page-aligned, exclusive) */
    vma_type_t type;        /* Region type */
    uint32_t flags;         /* VMM_FLAG_* combination */
    uint32_t offset;        /* File offset for mmap (0 if not file-backed) */
    const char* name;       /* Optional region name for debugging */
    struct vma* next;       /* Next VMA in list (sorted by start address) */
} vma_t;

/* ========================================================================
 * Process Address Space
 * ======================================================================== */

typedef struct address_space {
    page_directory_t* page_dir;    /* Page directory for this process */
    vma_t* vma_list;               /* Sorted list of virtual memory areas */
    uint32_t brk;                  /* Current program break (heap end) */
    uint32_t stack_top;            /* Stack top address */
    uint32_t code_start;           /* Code section start */
    uint32_t code_end;             /* Code section end */
    uint32_t ref_count;            /* Reference count for shared spaces */
} address_space_t;

/* ========================================================================
 * Physical Frame Allocator Statistics
 * ======================================================================== */

typedef struct {
    uint32_t total_frames;
    uint32_t used_frames;
    uint32_t free_frames;
    uint32_t reserved_frames;
} frame_stats_t;

/* ========================================================================
 * mmap Protection and Mapping Flags
 * ======================================================================== */

#define PROT_NONE    0x0
#define PROT_READ    0x1
#define PROT_WRITE   0x2
#define PROT_EXEC    0x4

#define MAP_SHARED   0x01
#define MAP_PRIVATE  0x02
#define MAP_FIXED    0x10
#define MAP_ANONYMOUS 0x20

/* ========================================================================
 * Physical Frame Allocator (PMM)
 * ======================================================================== */

/**
 * Initialize the physical frame allocator.
 * @param total_memory_kb  Total physical memory in kilobytes
 */
void pmm_init(uint32_t total_memory_kb);

/**
 * Allocate a single 4KB physical frame.
 * @return Physical address of the frame, or 0 on failure
 */
uint32_t pmm_alloc_frame(void);

/**
 * Free a single 4KB physical frame.
 * @param frame_addr  Physical address of the frame (must be page-aligned)
 */
void pmm_free_frame(uint32_t frame_addr);

/**
 * Allocate N contiguous physical frames.
 * @param count  Number of frames to allocate
 * @return Physical address of the first frame, or 0 on failure
 */
uint32_t pmm_alloc_frames(uint32_t count);

/**
 * Free N contiguous physical frames.
 * @param start_addr  Physical address of the first frame
 * @param count       Number of frames to free
 */
void pmm_free_frames(uint32_t start_addr, uint32_t count);

/**
 * Mark a region of physical memory as used (reserved).
 * @param start  Physical start address
 * @param size   Size in bytes
 */
void pmm_mark_region_used(uint32_t start, uint32_t size);

/**
 * Mark a region of physical memory as free.
 * @param start  Physical start address
 * @param size   Size in bytes
 */
void pmm_mark_region_free(uint32_t start, uint32_t size);

/* ========================================================================
 * VMM Core
 * ======================================================================== */

/** Initialize the VMM subsystem. Creates kernel address space. */
void vmm_init(void);

/** Create a new address space for a user process. */
address_space_t* vmm_create_address_space(void);

/** Destroy an address space, freeing all resources. */
void vmm_destroy_address_space(address_space_t* space);

/**
 * Fork an address space using copy-on-write.
 * The parent's user pages are marked read-only; the child gets a new
 * page directory pointing to the same physical frames.
 */
address_space_t* vmm_fork_address_space(address_space_t* parent);

/** Switch to the given address space (loads CR3). */
void vmm_switch_address_space(address_space_t* space);

/* ========================================================================
 * VMA Management
 * ======================================================================== */

/** Create a new VMA descriptor. */
vma_t* vmm_create_vma(uint32_t start, uint32_t end, vma_type_t type,
                       uint32_t flags, const char* name);

/** Destroy a VMA descriptor (frees the struct only, does not unmap pages). */
void vmm_destroy_vma(vma_t* vma);

/** Find the VMA containing the given address, or NULL. */
vma_t* vmm_find_vma(address_space_t* space, uint32_t addr);

/** Insert a VMA into the address space's sorted list. */
int vmm_add_vma(address_space_t* space, vma_t* vma);

/** Remove and destroy VMAs overlapping [start, end). Unmaps their pages. */
int vmm_remove_vma(address_space_t* space, uint32_t start, uint32_t end);

/* ========================================================================
 * Page Operations
 * ======================================================================== */

/**
 * Map a virtual page to a physical frame in the given address space.
 * @param space  Address space (NULL for kernel)
 * @param virt   Virtual address (page-aligned)
 * @param phys   Physical frame address (page-aligned)
 * @param flags  VMM_FLAG_* combination
 * @return 0 on success, -1 on failure
 */
int vmm_map_page(address_space_t* space, uint32_t virt, uint32_t phys,
                 uint32_t flags);

/**
 * Unmap a virtual page in the given address space.
 * @return 0 on success, -1 if page was not mapped
 */
int vmm_unmap_page(address_space_t* space, uint32_t virt);

/**
 * Get the physical address mapped at a virtual address.
 * @return Physical address, or 0 if not mapped
 */
uint32_t vmm_get_physical(address_space_t* space, uint32_t virt);

/**
 * Handle a page fault. Called from the ISR page fault handler.
 * @param fault_addr  Virtual address that caused the fault (from CR2)
 * @param error_code  CPU error code (bit 0: present, bit 1: write, bit 2: user)
 * @return 0 if handled, -1 if unrecoverable
 */
int vmm_handle_page_fault(uint32_t fault_addr, uint32_t error_code);

/* ========================================================================
 * Memory Operations
 * ======================================================================== */

/**
 * Map a region of memory into the process address space.
 * For MAP_ANONYMOUS, pages are allocated on demand (demand paging).
 * For MAP_FIXED, the exact address is used; otherwise a free region is found.
 */
void* vmm_mmap(address_space_t* space, uint32_t addr, uint32_t length,
               int prot, int flags, uint32_t offset);

/**
 * Unmap a region of memory from the process address space.
 * Frees physical frames and removes VMAs.
 */
int vmm_munmap(address_space_t* space, uint32_t addr, uint32_t length);

/**
 * Change the program break (heap end).
 * @param new_brk  Desired new break address; if 0, returns current brk
 * @return New break address, or (uint32_t)-1 on failure
 */
int vmm_brk(address_space_t* space, uint32_t new_brk);

/* ========================================================================
 * Copy-on-Write
 * ======================================================================== */

/**
 * Duplicate a COW page: allocate a new frame, copy content, remap writable.
 * @return 0 on success, -1 on failure
 */
int vmm_cow_duplicate_page(address_space_t* space, uint32_t virt);

/**
 * Mark a range of pages as copy-on-write (read-only + COW flag).
 * Increments the COW reference count for each physical frame.
 */
void vmm_cow_mark_range(address_space_t* space, uint32_t start, uint32_t end);

/* ========================================================================
 * Utilities
 * ======================================================================== */

/** Get physical frame allocator statistics. */
frame_stats_t* vmm_get_frame_stats(void);

/** Dump address space info to serial for debugging. */
void vmm_dump_address_space(address_space_t* space);

/* Current address space (kernel's during boot, user's when scheduled) */
extern address_space_t* kernel_address_space;

#endif /* VMM_H */
