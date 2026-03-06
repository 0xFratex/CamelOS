// cpu/gdt.c
typedef unsigned char uint8_t;
typedef unsigned short uint16_t;
typedef unsigned int uint32_t;

// GCC macro to pack structures strictly
#define PACKED __attribute__((packed))

struct gdt_entry_struct {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_middle;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} PACKED;

struct gdt_ptr_struct {
    uint16_t limit;
    uint32_t base;
} PACKED;

struct gdt_entry_struct gdt_entries[5];
struct gdt_ptr_struct   gdt_ptr;

// Helper to zero memory (Simple memset)
void gdt_zero(void* ptr, int size) {
    unsigned char* p = (unsigned char*)ptr;
    for(int i=0; i<size; i++) p[i] = 0;
}

void gdt_set_gate(int num, uint32_t base, uint32_t limit, uint8_t access, uint8_t gran) {
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].granularity = (limit >> 16) & 0x0F;

    gdt_entries[num].granularity |= gran & 0xF0;
    gdt_entries[num].access      = access;
}

void init_gdt() {
    // 1. Setup the GDT Pointer
    gdt_ptr.limit = (sizeof(struct gdt_entry_struct) * 5) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    // 2. Clear the GDT memory to avoid garbage causing triple faults
    gdt_zero(&gdt_entries, sizeof(gdt_entries));

    // 3. Setup Segments
    gdt_set_gate(0, 0, 0, 0, 0);                // Null segment
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Code segment (0x08)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Data segment (0x10)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // User mode code
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User mode data

    // 4. Load GDT and Reload Segments using Inline Assembly
    //    This does: lgdt, jumps to code segment, and reloads data registers.
    asm volatile(
        "lgdt (%0)\n\t"             // Load GDT from gdt_ptr
        "mov $0x10, %%ax\n\t"       // 0x10 is our Data Segment
        "mov %%ax, %%ds\n\t"
        "mov %%ax, %%es\n\t"
        "mov %%ax, %%fs\n\t"
        "mov %%ax, %%gs\n\t"
        "mov %%ax, %%ss\n\t"
        "ljmp $0x08, $.flush\n\t"   // Far jump to Code Segment (0x08)
        ".flush:\n\t"
        : 
        : "r" (&gdt_ptr) 
        : "eax", "memory"
    );
}