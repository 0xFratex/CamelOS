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

    int present   = !(regs.err_code & 0x1);
    int rw        = regs.err_code & 0x2;
    int us        = regs.err_code & 0x4;
    int reserved  = regs.err_code & 0x8;

    s_printf("\n[PAGING] Page Fault at 0x");
    char* chars = "0123456789ABCDEF";
    for (int i = 28; i >= 0; i -= 4) write_serial(chars[(faulting_address >> i) & 0xF]);
    s_printf("\n");

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
    cr0 |= 0x80000000; // Enable paging!
    asm volatile("mov %0, %%cr0" :: "r"(cr0));
}

page_table_t* clone_table(page_table_t* src, uint32_t* physAddr) {
    // Allocate a new page table, which is 4KB aligned
    page_table_t* table = (page_table_t*)kmalloc_ap(sizeof(page_table_t), physAddr);
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
    uint32_t phys;
    kernel_directory = (page_directory_t*)kmalloc_a(sizeof(page_directory_t));
    memset(kernel_directory, 0, sizeof(page_directory_t));

    // We need the physical address of tablesPhysical to load into CR3
    // Since we are currently in identity mapped memory (pre-paging),
    // Virtual Address == Physical Address.
    kernel_directory->physicalAddr = (uint32_t)kernel_directory->tablesPhysical;

    // === FIX: Map 64MB instead of 16MB to cover the larger Kernel Heap ===
    // 64MB = 16 Page Tables
    for (int i = 0; i < 1024 * 16; i++) {
        uint32_t phys_addr = i * 0x1000;
        uint32_t virt_addr = phys_addr; // Identity Map

        uint32_t table_idx = virt_addr / 0x400000;
        uint32_t page_idx = (virt_addr / 0x1000) % 1024;

        if (!kernel_directory->tables[table_idx]) {
            uint32_t t_phys;
            kernel_directory->tables[table_idx] = (page_table_t*)kmalloc_ap(sizeof(page_table_t), &t_phys);
            memset(kernel_directory->tables[table_idx], 0, sizeof(page_table_t));
            kernel_directory->tablesPhysical[table_idx] = t_phys | 0x7;
        }

        page_table_t* table = kernel_directory->tables[table_idx];
        table->entries[page_idx] = phys_addr | 0x7; 
    }

    // Register Page Fault Handler (ISR 14)
    // Handled in isr.c by dispatch logic, but we make sure it calls us.

    // Enable Paging
    switch_page_directory(kernel_directory);
    s_printf("[PAGING] Enabled (0-64MB Identity Mapped).\n");
}

// 1. Add this function to map specific regions (like Video RAM)
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

        // If table doesn't exist, create it
        if (!kernel_directory->tables[table_idx]) {
            uint32_t t_phys;
            kernel_directory->tables[table_idx] = (page_table_t*)kmalloc_ap(sizeof(page_table_t), &t_phys);
            memset(kernel_directory->tables[table_idx], 0, sizeof(page_table_t));
            kernel_directory->tablesPhysical[table_idx] = t_phys | 0x7; // Present, RW, User
        }

        // Map the page: virtual address -> physical address with flags
        kernel_directory->tables[table_idx]->entries[page_idx] = curr_phys | flags;
    } 
    
    // Reload CR3 to flush TLB
    switch_page_directory(kernel_directory);
}
