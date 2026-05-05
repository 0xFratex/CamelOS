#ifndef GDT_H
#define GDT_H

void init_gdt(void);
void tss_set_kernel_stack(uint32_t esp0);

// TSS selector for reference
#define TSS_SELECTOR 0x28

// User-mode selectors
#define USER_CODE_SELECTOR 0x18
#define USER_DATA_SELECTOR 0x20

#endif
