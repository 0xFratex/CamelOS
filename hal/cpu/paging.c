#include "paging.h"
#include "../../core/memory.h"
#include "../../core/string.h"
#include "../../hal/drivers/vga.h"
#include "../../hal/drivers/serial.h"
#include "isr.h"

// Kernel Page Directory
page_directory_t* kernel_directory = 0;
page_directory_t* current_directory = 0;

// Need a way to allocate page-aligned physical memory.
// Since we don't have a separate PMM yet, we piggyback on kmalloc
// and assume virtual == physical for the kernel heap initially.
extern void* kmalloc_a(size_t size); // We will add this to memory.c
extern void* kmalloc_ap(size_t size, uint32_t* phys); // Aligned + Physical return

void page_fault_handler(registers_t regs) {
    uint32_t faulting_address;
    asm volatile("mov %%cr2, %0" : "=r" (faulting_address));

    int present   = regs.err_code & 0x1;
    int rw        = regs.err_code & 0x2;
    int us        = regs.err_code & 0x4;
    int reserved  = regs.err_code & 0x8;

    // Try the VMM page fault handler first (handles COW, demand paging, stack growth)
    extern int vmm_handle_page_fault(uint32_t fault_addr, uint32_t error_code);
    if (vmm_handle_page_fault(faulting_address, regs.err_code) == 0) {
        // VMM handled the fault successfully (COW duplicate, demand page, stack growth)
        return;
    }

    // VMM could not handle it — this is a genuine fatal page fault
    s_printf("\n[PAGING] Page Fault at 0x%x (present=%d rw=%d us=%d reserved=%d)\n",
             faulting_address, present, rw, us, reserved);

    // UNMUTE LOGS SO WE CAN SEE THE PANIC ON SCREEN
    vga_mute_log(0);

    extern void panic(const char* msg, registers_t* regs);
    panic("Page Fault", &regs);
}

void switch_page_directory(page_directory_t* dir) {
    current_directory = dir;
    asm volatile("mov %0, %%cr3" :: "r"(dir->physicalAddr));
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80010000; // Enable paging (PG) + Write Protect (WP)!
    // WP: Ring 0 must honor read-only user PTEs (required for COW and for
    // copy-on-write fork to ever fault correctly).
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
}

page_table_t* clone_table(page_table_t* src, uint32_t* physAddr) {
    // Allocate a new page table, which is 4KB aligned
    page_table_t* table = (page_table_t*)kmalloc_ap(sizeof(page_table_t), physAddr);
    if (!table) return 0;
    memset(table, 0, sizeof(page_table_t));

    // Copy entries
    for (int i = 0; i < 1024; i++) {
        if (src->entries[i] != 0) {
            // Link to the same physical page for now (Copy on Write would go here later)
            // For kernel tables, we want full sharing.
            table->entries[i] = src->entries[i];
        }
    }
    return table;
}

void init_paging() {
    s_printf("[PAGING] Initializing...\n");

    // Allocate a page directory (aligned 4K)
    kernel_directory = (page_directory_t*)kmalloc_a(sizeof(page_directory_t));
    if (!kernel_directory) {
        s_printf("[PAGING] FATAL: failed to allocate kernel page directory!\n");
        return;
    }
    memset(kernel_directory, 0, sizeof(page_directory_t));

    // We need the physical address of tablesPhysical to load into CR3
    // Since we are currently in identity mapped memory (pre-paging),
    // Virtual Address == Physical Address.
    kernel_directory->physicalAddr = (uint32_t)kernel_directory->tablesPhysical;

    // === FIX: Map 128MB instead of 16MB to cover the larger Kernel Heap ===
    // 128MB = 32 Page Tables
    for (int i = 0; i < 1024 * 32; i++) {
        uint32_t phys_addr = i * 0x1000;
        uint32_t virt_addr = phys_addr; // Identity Map

        uint32_t table_idx = virt_addr / 0x400000;
        uint32_t page_idx = (virt_addr / 0x1000) % 1024;

        if (!kernel_directory->tables[table_idx]) {
            uint32_t t_phys;
            page_table_t* new_table = (page_table_t*)kmalloc_ap(sizeof(page_table_t), &t_phys);
            if (!new_table) {
                s_printf("[PAGING] FATAL: kmalloc_ap failed for identity map table %d\n", table_idx);
                return;
            }
            memset(new_table, 0, sizeof(page_table_t));
            kernel_directory->tables[table_idx] = new_table;
            // SECURITY: kernel pages must be supervisor-only (0x3 = P|RW).
            // The old 0x7 set the User bit, which let any Ring 3 process
            // read/write ALL kernel memory through the shared identity map.
            kernel_directory->tablesPhysical[table_idx] = t_phys | 0x3;
        }

        page_table_t* table = kernel_directory->tables[table_idx];
        table->entries[page_idx] = phys_addr | 0x3; // P|RW, supervisor-only
    }

    // Register Page Fault Handler (ISR 14)
    // Handled in isr.c by dispatch logic, but we make sure it calls us.

    // Enable Paging
    switch_page_directory(kernel_directory);
    s_printf("[PAGING] Enabled (0-64MB Identity Mapped).\n");
}

// ============================================================================
// Task 7: Mark pages as user-accessible
// ============================================================================

void paging_set_user_page(uint32_t virtual_addr, int user_accessible) {
    if (!current_directory && !kernel_directory) return;

    page_directory_t* dir = current_directory ? current_directory : kernel_directory;
    if (!dir) return;

    uint32_t table_idx = virtual_addr / 0x400000;
    uint32_t page_idx = (virtual_addr / 0x1000) % 1024;

    if (table_idx >= 1024) return;

    // If the page table doesn't exist, we can't modify it
    if (!dir->tables[table_idx]) return;

    uint32_t entry = dir->tables[table_idx]->entries[page_idx];

    if (user_accessible) {
        // Set the User/Supervisor bit (bit 2) to allow Ring 3 access
        entry |= PAGING_FLAG_USER;
    } else {
        // Clear the User/Supervisor bit - Ring 3 access causes page fault
        entry &= ~PAGING_FLAG_USER;
    }

    dir->tables[table_idx]->entries[page_idx] = entry;

    // Invalidate TLB entry for this page
    asm volatile(
        "invlpg (%0)"
        :
        : "r"(virtual_addr)
        : "memory"
    );
}

// Map a region of physical memory into the virtual address space.
// Maps into kernel_directory and syncs to current_directory if different.
// CRITICAL: This function must add NULL checks for kmalloc_ap failures
// to prevent writing through NULL pointers (which would cause immediate
// page faults at address 0 instead of deferred ones at the target address).
void paging_map_region(uint32_t phys_addr, uint32_t virt_addr, uint32_t size, uint32_t flags) {
    if (!kernel_directory) return;

    uint32_t start_virt = virt_addr & 0xFFFFF000; // Align virtual address to 4KB
    uint32_t end_virt = (virt_addr + size + 0xFFF) & 0xFFFFF000;
    uint32_t start_phys = phys_addr & 0xFFFFF000; // Align physical address to 4KB

    uint32_t page_count = (end_virt - start_virt) / 0x1000;

    for (uint32_t i = 0; i < page_count; i++) {
        uint32_t curr_virt = start_virt + (i * 0x1000);
        uint32_t curr_phys = start_phys + (i * 0x1000);

        uint32_t table_idx = curr_virt / 0x400000;
        uint32_t page_idx = (curr_virt / 0x1000) % 1024;

        // If table doesn't exist in kernel_directory, create it
        if (!kernel_directory->tables[table_idx]) {
            uint32_t t_phys;
            page_table_t* new_table = (page_table_t*)kmalloc_ap(sizeof(page_table_t), &t_phys);
            if (!new_table) {
                s_printf("[PAGING] FATAL: kmalloc_ap failed for page table idx %d (virt 0x%x)\n",
                         table_idx, curr_virt);
                return;  // Abort mapping — caller must handle the failure
            }
            memset(new_table, 0, sizeof(page_table_t));
            kernel_directory->tables[table_idx] = new_table;
            // PDE must carry at least the flags the PTEs use: Ring 3 access
            // requires the User bit at BOTH directory and table level.
            kernel_directory->tablesPhysical[table_idx] =
                t_phys | PAGING_FLAG_PRESENT | (flags & (PAGING_FLAG_RW | PAGING_FLAG_USER));
        }

        // Map the page: virtual address -> physical address with flags
        kernel_directory->tables[table_idx]->entries[page_idx] = curr_phys | flags;

        // Also apply to current_directory if it's different from kernel_directory.
        // This ensures new mappings are immediately visible in the active
        // address space, even if a user process's page directory is loaded.
        if (current_directory && current_directory != kernel_directory) {
            // Share the page table pointer from kernel_directory
            if (!current_directory->tables[table_idx]) {
                current_directory->tables[table_idx]         = kernel_directory->tables[table_idx];
                current_directory->tablesPhysical[table_idx] = kernel_directory->tablesPhysical[table_idx];
            }
            // Copy the PTE (if the tables are shared, this is a no-op;
            // if the current dir has its own table, we need to set the entry)
            if (current_directory->tables[table_idx] != kernel_directory->tables[table_idx]) {
                current_directory->tables[table_idx]->entries[page_idx] = curr_phys | flags;
            }
        }
    } 

    // Flush TLB WITHOUT clobbering a user process's active CR3.
    // The old code force-switched to kernel_directory here, which would
    // desync CR3 from the running user task (scheduler.c switches CR3 per
    // task). Reloading the CURRENT CR3 value flushes all TLB entries and
    // keeps the active address space unchanged.
    uint32_t cr3;
    asm volatile("mov %%cr3, %0" : "=r"(cr3));
    asm volatile("mov %0, %%cr3" :: "r"(cr3) : "memory");
}
