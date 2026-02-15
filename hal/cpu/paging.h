#ifndef PAGING_H
#define PAGING_H

#include "../../include/types.h"
#include "isr.h"

// Page Entry Flags
#define PAGING_FLAG_PRESENT  0x01
#define PAGING_FLAG_RW       0x02 // Read/Write
#define PAGING_FLAG_USER     0x04 // User Mode
#define PAGING_FLAG_WRITE_THROUGH 0x08
#define PAGING_FLAG_NO_CACHE      0x10
#define PAGING_FLAG_ACCESSED      0x20
#define PAGING_FLAG_DIRTY         0x40

typedef struct {
    uint32_t entries[1024];
} page_table_t;

typedef struct {
    uint32_t tablesPhysical[1024];
    page_table_t* tables[1024]; // Virtual pointers to tables
    uint32_t physicalAddr;      // Physical address of tablesPhysical
} page_directory_t;

void init_paging();
void switch_page_directory(page_directory_t* dir);
void map_page(page_directory_t* dir, uint32_t virtual_addr, uint32_t physical_addr, uint32_t flags);
page_directory_t* create_page_directory();
void page_fault_handler(registers_t regs);

// Map a region of physical memory into the virtual address space
void paging_map_region(uint32_t phys_addr, uint32_t virt_addr, uint32_t size, uint32_t flags);

// Align a pointer to the next 4KB boundary
uint32_t align_4k(uint32_t addr);

#endif
