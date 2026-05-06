// cpu/gdt.c — GDT with TSS support for Ring 3 user-mode transitions
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

// TSS descriptor occupies two GDT entries (64-bit descriptor in 32-bit mode)
struct gdt_ptr_struct {
    uint16_t limit;
    uint32_t base;
} PACKED;

// Task State Segment structure (x86 32-bit)
struct tss_struct {
    uint16_t prev_task;     uint16_t : 16;
    uint32_t esp0;          // Kernel stack pointer for Ring 0
    uint16_t ss0;           // Kernel stack segment (0x10)
    uint16_t : 16;
    uint32_t esp1;          uint16_t ss1;  uint16_t : 16;
    uint32_t esp2;          uint16_t ss2;  uint16_t : 16;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint16_t es;            uint16_t : 16;
    uint16_t cs;            uint16_t : 16;
    uint16_t ss;            uint16_t : 16;
    uint16_t ds;            uint16_t : 16;
    uint16_t fs;            uint16_t : 16;
    uint16_t gs;            uint16_t : 16;
    uint16_t ldt;           uint16_t : 16;
    uint16_t trap;
    uint16_t iomap_base;    // I/O Map Base Address (0x68 + 8K for full map, or >= limit for none)
} PACKED;

// 7 entries: Null, Kernel Code, Kernel Data, User Code, User Data, TSS Low, TSS High
#define GDT_ENTRIES 7
#define TSS_ENTRY   5   // TSS starts at GDT index 5 (selector 0x28)

struct gdt_entry_struct gdt_entries[GDT_ENTRIES];
struct gdt_ptr_struct   gdt_ptr;
struct tss_struct       tss;

// Global kernel stack pointer used by the sysenter MSR.
// When a Ring-3 task executes sysenter, the CPU loads ESP from
// IA32_SYSENTER_ESP.  The scheduler updates this variable on
// every context switch so the next sysenter uses the correct
// kernel stack for the incoming task.
uint32_t tss_esp0_for_sysenter = 0;

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

// Set a 64-bit TSS descriptor (occupies two GDT entries)
void gdt_set_tss_gate(int num, uint32_t base, uint32_t limit) {
    // Entry n: low 32 bits of TSS descriptor
    gdt_entries[num].limit_low   = (limit & 0xFFFF);
    gdt_entries[num].base_low    = (base & 0xFFFF);
    gdt_entries[num].base_middle = (base >> 16) & 0xFF;
    gdt_entries[num].access      = 0x89;  // Present, Ring 0, 32-bit TSS (busy=0)
    gdt_entries[num].granularity = ((limit >> 16) & 0x0F) | 0x00; // No granularity
    gdt_entries[num].base_high   = (base >> 24) & 0xFF;

    // Entry n+1: high 32 bits (reserved, must be zero for 32-bit TSS)
    gdt_zero(&gdt_entries[num + 1], sizeof(struct gdt_entry_struct));
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
    tss.ss0 = 0x10;  // Kernel data segment
}

// Task 7: Public wrapper to set TSS kernel stack for Ring 3 privilege returns
void gdt_setup_tss_stack(uint32_t kernel_stack) {
    tss_set_kernel_stack(kernel_stack);
}

void init_gdt() {
    // 1. Setup the GDT Pointer (7 entries)
    gdt_ptr.limit = (sizeof(struct gdt_entry_struct) * GDT_ENTRIES) - 1;
    gdt_ptr.base  = (uint32_t)&gdt_entries;

    // 2. Clear the GDT memory to avoid garbage causing triple faults
    gdt_zero(&gdt_entries, sizeof(gdt_entries));

    // 3. Zero the TSS
    gdt_zero(&tss, sizeof(tss));
    tss.ss0 = 0x10;           // Kernel data segment
    tss.esp0 = 0;             // Will be set by scheduler on task switch
    tss.iomap_base = sizeof(struct tss_struct); // No I/O permission bitmap

    // 4. Setup Segments
    gdt_set_gate(0, 0, 0, 0, 0);                // Null segment
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF); // Code segment (0x08)
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF); // Data segment (0x10)
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF); // User mode code (0x18)
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF); // User mode data (0x20)

    // 5. Setup TSS descriptor (selector 0x28)
    gdt_set_tss_gate(TSS_ENTRY, (uint32_t)&tss, sizeof(struct tss_struct) - 1);

    // 6. Load GDT and reload segments
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

    // 7. Load the Task Register with TSS selector
    asm volatile(
        "movw $0x28, %%ax\n\t"      // TSS selector = GDT index 5 * 8 = 0x28
        "ltr %%ax\n\t"
        :
        :
        : "eax"
    );
}
