// kernel/kernel.c — Main kernel entry point with all subsystems wired up
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

    // === Initialize VMM after paging ===
    extern void vmm_init(void);
    vmm_init();
    s_printf("[KERNEL] VMM initialized.\n");

    extern void init_apic(); init_apic();
    extern void init_timer(int); init_timer(50);
}

void kernel_main(void* mboot_ptr) {
    kernel_init_hal(); 
    s_printf("\n[KERNEL] Booting...\n");

    // === Initialize core subsystems (Phase 0: Wire Up Dead Code) ===

    // Scheduler — enables preemptive multitasking
    extern void scheduler_init(void);
    scheduler_init();
    s_printf("[KERNEL] Scheduler initialized.\n");

    // Process management — fork/exec/wait support
    extern void process_init(void);
    process_init();
    s_printf("[KERNEL] Process manager initialized.\n");

    // Signals — POSIX signal delivery
    extern void signal_init(void);
    signal_init();
    s_printf("[KERNEL] Signal subsystem initialized.\n");

    // Pipes — anonymous pipes and named FIFOs
    extern void pipe_init(void);
    pipe_init();
    s_printf("[KERNEL] Pipe subsystem initialized.\n");

    // IPC — Mach-style port messaging, shared memory, RPC
    extern void ipc_init(void);
    ipc_init();
    s_printf("[KERNEL] IPC subsystem initialized.\n");

    // Kernel logger — ring buffer with log levels
    extern void klog_init(void);
    klog_init();
    s_printf("[KERNEL] Kernel logger initialized.\n");

    // Crash reporter — stack unwinding, crash log persistence
    extern void crash_reporter_init(void);
    crash_reporter_init();
    s_printf("[KERNEL] Crash reporter initialized.\n");

    // Notification center — macOS-style toast notifications
    extern void notify_init(void);
    notify_init();
    s_printf("[KERNEL] Notification center initialized.\n");

    // === Graphics and filesystem ===
    extern void gfx_init_hal(void*);
    gfx_init_hal(mboot_ptr);
    
    // VFS and FAT32 registration
    extern void vfs_init(void);
    vfs_init();
    s_printf("[KERNEL] VFS initialized.\n");

    // Register FAT32 filesystem driver with VFS
    extern void fat32_register_with_vfs(void);
    fat32_register_with_vfs();
    s_printf("[KERNEL] FAT32 registered with VFS.\n");

    sys_fs_mount();
    
    s_printf("\n--- Hardware Enumeration ---\n");
    pci_init();
    s_printf("----------------------------\n");
    
    net_init(); 
    
    // === MANUAL NETWORK CONFIG (Network Byte Order / Big Endian) ===
    extern void rtl8139_configure_ip(uint32_t, uint32_t, uint32_t);
    rtl8139_configure_ip(0x0A00020F, 0x0A000202, 0xFFFFFF00);
    
    s_printf("[KERNEL] Network Configured.\n");
    
    // Test DNS
    char ip[32];
    if (dns_resolve("example.com", ip, 32) == 0) {
        s_printf("[KERNEL] DNS SUCCESS: example.com -> %s\n", ip);
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
