// kernel/kernel.c
#include "../sys/api.h"
#include "../hal/drivers/serial.h"
#include "../hal/drivers/pci.h"
#include "../core/net.h"
#include "../core/dns.h"

extern int kbd_ctrl_pressed;
extern int kbd_shift_pressed;
extern uint32_t _bss_end;

void kernel_init_hal() {
    extern void init_gdt(); init_gdt();
    extern void init_idt(); init_idt();
    extern void init_keyboard(); init_keyboard();
    extern void init_serial(); init_serial();
    
    uint32_t heap_start = (uint32_t)&_bss_end;
    if (heap_start % 16 != 0) heap_start += 16 - (heap_start % 16);
    extern void init_heap(uint32_t, uint32_t);
    init_heap(heap_start, 32 * 1024 * 1024);
    
    extern void init_paging(); init_paging();
    extern void init_apic(); init_apic();
    extern void init_timer(int); init_timer(50);
}

void kernel_main(void* mboot_ptr) {
    kernel_init_hal(); 
    s_printf("\n[KERNEL] Booting...\n");

    extern void gfx_init_hal(void*);
    gfx_init_hal(mboot_ptr);
    
    sys_fs_mount();
    
    s_printf("\n--- Hardware Enumeration ---\n");
    pci_init();
    s_printf("----------------------------\n");
    
    net_init(); 
    
    // === MANUAL NETWORK CONFIG (Network Byte Order / Big Endian) ===
    // IP: 10.0.2.15 -> 0x0A00020F (network byte order: 10.0.2.15)
    // Gateway: 10.0.2.2 -> 0x0A000202 (network byte order: 10.0.2.2)
    // Netmask: 255.255.255.0 -> 0xFFFFFF00 (network byte order)
    
    extern void rtl8139_configure_ip(uint32_t, uint32_t, uint32_t);
    rtl8139_configure_ip(0x0A00020F, 0x0A000202, 0xFFFFFF00);
    
    s_printf("[KERNEL] Network Configured.\n");
    
    // Test DNS
    char ip[32];
    if (dns_resolve("example.com", ip, 32) == 0) {
        s_printf("[KERNEL] DNS SUCCESS: example.com -> ");
        s_printf(ip);
        s_printf("\n");
    } else {
        s_printf("[KERNEL] DNS FAILED.\n");
    }
    
    extern void start_bubble_view();
    start_bubble_view();
    
    while(1) {
        extern void rtl8139_poll();
        rtl8139_poll();
        asm("hlt");
    }
}