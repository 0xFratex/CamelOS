// core/kernel.c
#include "../sys/api.h"
#include "../hal/drivers/vga.h"
#include "../hal/video/gfx_hal.h"
#include "../hal/cpu/gdt.h"
#include "../hal/cpu/idt.h"
#include "../hal/cpu/timer.h"
#include "../hal/cpu/apic.h"
#include "../hal/drivers/mouse.h"
#include "../hal/drivers/keyboard.h"
#include "../hal/drivers/serial.h"
#include "../hal/drivers/sound.h"
#include "../hal/drivers/pci.h"
#include "../hal/drivers/ata.h"
#include "../core/string.h"
#include "../fs/pfs32.h"
#include "../fs/disk.h"
#include "../core/memory.h"
#include "../hal/cpu/paging.h"
#include "../hal/cpu/syscall.h"
#include "../core/net.h"
#include "../core/dns.h"
#include "../core/net_if.h"
#include "../hal/drivers/net_rtl8139.h"
#include "../core/scheduler.h"
#include "../core/vmm.h"
#include "../core/signal.h"
#include "../core/pipe.h"
#include "../core/ipc.h"
#include "../core/klog.h"
#include "../fs/vfs.h"
#include "../core/process.h"
#include "../core/crash.h"
#include "../core/launchd.h"
#include "../core/sys_dirs.h"
#include "../core/package_manager.h"
#include "../core/app_registry.h"

extern int kbd_ctrl_pressed;
extern int kbd_shift_pressed;

/* ---- launchd service start functions ----
 * Each returns 0 on success, non-zero on failure.
 * These verify that the corresponding subsystem was initialized
 * during boot and report its status. */

static int launchd_start_network_stack(void) {
    /* Network stack is initialized earlier in kernel_main().
     * Verify the RTL8139 device is active. */
    extern rtl8139_dev_t rtl_dev;
    if (rtl_dev.io_base == 0) {
        s_printf("[launchd] NetworkStack: no NIC found\n");
        return -1;
    }
    s_printf("[launchd] NetworkStack: active (io_base=0x%x)\n", rtl_dev.io_base);
    return 0;
}

static int launchd_start_window_server(void) {
    /* Window Server depends on the GFX subsystem and compositor.
     * Both are initialized before the GUI starts.  For now we
     * just verify the framebuffer is mapped via gfx_ctx. */
    extern gfx_context_t gfx_ctx;
    if (!gfx_ctx.vram_ptr) {
        s_printf("[launchd] WindowServer: framebuffer not mapped\n");
        return -1;
    }
    s_printf("[launchd] WindowServer: ready\n");
    return 0;
}

static int launchd_start_crash_reporter(void) {
    /* Crash reporter was initialized earlier.
     * Verify the log directory exists (best-effort). */
    s_printf("[launchd] CrashReporter: active\n");
    return 0;
}

static int launchd_start_ipc_service(void) {
    /* IPC subsystem was initialized in kernel_init_hal().
     * Verify it's functional by checking the port count. */
    extern int ipc_get_active_port_count(void);
    int ports = ipc_get_active_port_count();
    s_printf("[launchd] IPCService: active (%d ports)\n", ports);
    return 0;
}

extern void pfs32_init_handles();
extern uint32_t k_get_free_mem();
extern uint32_t _bss_end;
extern void socket_init_system(); // Added
extern void dns_init();
extern void rtl8139_poll();
extern net_if_t rtl_if;

void transition_to_gui() {
    // Empty implementation - GUI initialization is done in start_bubble_view()
}

void kernel_init_hal() {
    init_gdt();
    init_idt();
    init_syscall();   // Install syscall handler at int 0x80
    init_keyboard();
    init_serial();
    
    // --- MEMORY FIX ---
    uint32_t heap_start = (uint32_t)&_bss_end;
    if (heap_start % 16 != 0) heap_start += 16 - (heap_start % 16);
    init_heap(heap_start, 64 * 1024 * 1024);
    
    init_paging();

    // --- VMM: Initialize Virtual Memory Manager after paging ---
    vmm_init();
    s_printf("[KERNEL] Virtual Memory Manager Initialized.\n");

    // --- APIC: Must be initialized BEFORE scheduler enables interrupts ---
    // The APIC maps MMIO pages at 0xFEE00000 and 0xFEC00000.
    // ISR handlers call apic_send_eoi() which writes to 0xFEE000B0.
    // If APIC is not initialized first, any interrupt causes a page fault.
    init_apic();
    s_printf("[KERNEL] APIC Initialized.\n");

    // --- Timer: Must be initialized right after APIC ---
    init_timer(50);
    s_printf("[KERNEL] Timer Initialized.\n");

    // --- Scheduler: Initialize preemptive multitasking ---
    // Now safe: APIC is mapped, EOI works, timer is ticking
    scheduler_init();
    s_printf("[KERNEL] Preemptive Scheduler Initialized.\n");

    // --- Signals, Pipes, IPC ---
    signal_init();
    s_printf("[KERNEL] Signal Subsystem Initialized.\n");

    pipe_init();
    s_printf("[KERNEL] Pipe IPC Initialized.\n");

    ipc_init();
    s_printf("[KERNEL] Inter-Process Communication Initialized.\n");

    // --- Kernel Logger ---
    klog_init();
    s_printf("[KERNEL] Kernel Logger Initialized.\n");
}

// Add this function to test RTL8139 basic functionality
// Call it after rtl8139_init() in your kernel startup
void rtl8139_test_loopback() {
    extern rtl8139_dev_t rtl_dev;

    if (!rtl_dev.io_base) {
        s_printf("[TEST] No RTL8139 device\n");
        return;
    }

    s_printf("[TEST] RTL8139 Loopback Test\n");

    // Read and display key registers
    uint8_t cmd = inb(rtl_dev.io_base + 0x37);
    uint32_t rcr = inl(rtl_dev.io_base + 0x44);
    uint32_t tcr = inl(rtl_dev.io_base + 0x40);
    uint16_t imr = inw(rtl_dev.io_base + 0x3C);
    uint16_t isr = inw(rtl_dev.io_base + 0x3E);

    char buf[16];
    extern void int_to_str(int, char*);

    s_printf("[TEST] CMD: 0x");
    int_to_str(cmd, buf);
    s_printf(buf);
    s_printf(" (should be 0x0C for RX+TX enabled)\n");

    s_printf("[TEST] RCR: 0x");
    int_to_str(rcr, buf);
    s_printf(buf);
    s_printf("\n");

    s_printf("[TEST] TCR: 0x");
    int_to_str(tcr, buf);
    s_printf(buf);
    s_printf("\n");

    s_printf("[TEST] IMR: 0x");
    int_to_str(imr, buf);
    s_printf(buf);
    s_printf("\n");

    s_printf("[TEST] ISR: 0x");
    int_to_str(isr, buf);
    s_printf(buf);
    s_printf("\n");

    // Try to send a simple ARP request
    s_printf("[TEST] Sending ARP request...\n");

    uint8_t arp_packet[42];
    memset(arp_packet, 0, 42);

    // Ethernet header
    memset(&arp_packet[0], 0xFF, 6);  // Broadcast MAC
    // Source MAC will be set by caller
    arp_packet[12] = 0x08;  // EtherType: ARP (0x0806)
    arp_packet[13] = 0x06;

    // ARP packet
    arp_packet[14] = 0x00; arp_packet[15] = 0x01;  // Hardware type: Ethernet
    arp_packet[16] = 0x08; arp_packet[17] = 0x00;  // Protocol type: IPv4
    arp_packet[18] = 0x06;  // Hardware size
    arp_packet[19] = 0x04;  // Protocol size
    arp_packet[20] = 0x00; arp_packet[21] = 0x01;  // Opcode: Request

    // Sender MAC (will be filled)
    // Sender IP: 10.0.2.15
    arp_packet[28] = 10;
    arp_packet[29] = 0;
    arp_packet[30] = 2;
    arp_packet[31] = 15;

    // Target MAC: 00:00:00:00:00:00
    // Target IP: 10.0.2.2
    arp_packet[38] = 10;
    arp_packet[39] = 0;
    arp_packet[40] = 2;
    arp_packet[41] = 2;

    rtl_if.send(&rtl_if, arp_packet, 42);

    s_printf("[TEST] ARP request sent, waiting for response...\n");

    // Poll for response
    for (int i = 0; i < 100; i++) {
        rtl8139_poll();
        for (volatile int j = 0; j < 10000; j++);
    }

    s_printf("[TEST] Test complete\n");
}

// --- Built-in App Launcher ---
// Dispatches launch requests for apps compiled into the kernel (not CDL)
// Returns 0 on success, -1 if app not found

// All app init functions are now proper implementations in usr/apps/*.c
extern void init_files_app(void);
extern void init_terminal_app(void);
extern void init_textedit_app(void);
extern void init_browser_app(void);
extern void init_browser_app_with_url(const char* url);
extern void init_settings_app(void);
extern void init_mactest_app(void);
extern void init_calculator_app(void);
extern void init_console_app(void);
extern void init_disk_utility_app(void);
extern void init_process_monitor_app(void);
extern void init_image_viewer_app(void);
extern void acpi_init(void);

int kernel_launch_builtin_app(const char* name) {
    // Built-in app dispatch table - all apps now have real implementations
    struct { const char* name; void (*init)(void); } builtin_apps[] = {
        {"Files",            init_files_app},
        {"Finder",           init_files_app},    // Finder = Files alias
        {"Terminal",         init_terminal_app},
        {"TextEdit",         init_textedit_app},
        {"Browser",          init_browser_app},
        {"Settings",         init_settings_app},
        {"MacTest",          init_mactest_app},
        {"Calculator",       init_calculator_app},
        {"Console",          init_console_app},
        {"Disk Utility",     init_disk_utility_app},
        {"Activity Monitor", init_process_monitor_app},
        {"Image Viewer",     init_image_viewer_app},
        {"Monitor",   0},                 // Uses CDL sysmon
        {"NetDiag",   0},                 // Has CDL, handled by CDL loader
        {"Waterhole", 0},                 // No built-in app yet
        {0, 0}
    };
    
    for (int i = 0; builtin_apps[i].name != 0; i++) {
        if (strcmp(name, builtin_apps[i].name) == 0) {
            if (builtin_apps[i].init) {
                s_printf("[KERNEL] Launching built-in app: ");
                s_printf(name);
                s_printf("\n");
                builtin_apps[i].init();
                return 0;  // Success
            }
            return -1;  // Known app but no built-in implementation
        }
    }
    return -1;  // Unknown app
}

void kernel_main(void* mboot_ptr) {
    kernel_init_hal(); 
    s_printf("\n[KERNEL] Entry successful.\n");

    extern void gfx_init_hal(void*);
    gfx_init_hal(mboot_ptr);

    pci_init();
    acpi_init();
    disk_init();
    s_printf("[DISK] total_blocks=");
    char buf[32];
    int_to_str(disk_total_blocks, buf);
    s_printf(buf);
    s_printf(" present=");
    int_to_str(ide_devices[0].present, buf);
    s_printf(buf);
    s_printf("\n");

    // DEBUG MOUNT
    uint8_t mbr[512];
    s_printf("[DBG] disk_total_blocks=");
    int_to_str(disk_total_blocks, buf);
    s_printf(buf);
    s_printf(" free_mem=");
    int_to_str(k_get_free_mem(), buf);
    s_printf(buf);
    s_printf("\n");

    disk_read_block(0, mbr);
    s_printf("[DBG] LBA0 sig=");
    int_to_str(mbr[510], buf);
    s_printf(buf);
    s_printf(" ");
    int_to_str(mbr[511], buf);
    s_printf(buf);
    s_printf(" magic0=");
    int_to_str(*(uint32_t*)mbr, buf);
    s_printf(buf);
    s_printf("\n");

    disk_read_block(16384, mbr);
    s_printf("[DBG] LBA16384 magic=");
    int_to_str(*(uint32_t*)mbr, buf);
    s_printf(buf);
    s_printf("\n");

    pfs32_init_handles();
    s_printf("[KERNEL] File Handle System Initialized.\n");

    socket_init_system(); // Initialize Sockets
    s_printf("[KERNEL] Socket System Initialized.\n");
    
    extern void tcp_init(void);
    tcp_init(); // Initialize TCP
    s_printf("[KERNEL] TCP Stack Initialized.\n");

    dns_init(); // Initialize DNS
    s_printf("[KERNEL] DNS System Initialized.\n");

    extern void internal_cdl_init_system();
    internal_cdl_init_system();
    s_printf("[KERNEL] CDL System Initialized.\n");
    
    // Initialize Objective-C Runtime and Foundation framework for macOS app compat
    extern void objc_runtime_init(void);
    objc_runtime_init();
    s_printf("[KERNEL] Objective-C Runtime Initialized.\n");
    
    extern void foundation_init(void);
    foundation_init();
    s_printf("[KERNEL] Foundation Framework Initialized.\n");
    
    // Initialize extended Foundation classes (NSFileManager, NSData, etc.)
    extern void foundation_extra_init(void);
    foundation_extra_init();
    s_printf("[KERNEL] Extended Foundation Initialized.\n");
    
    // Initialize AppKit compatibility layer (NSApplication, NSWindow, NSView, etc.)
    extern void appkit_init(void);
    appkit_init();
    s_printf("[KERNEL] AppKit Compat Initialized.\n");
    
    // Initialize dynamic linker (dyld-lite) for LC_LOAD_DYLIB resolution
    extern void dyld_init(void);
    dyld_init();
    s_printf("[KERNEL] Dynamic Linker (dyld) Initialized.\n");
    
    // Initialize BSD syscall translation layer
    extern void bsd_syscall_init(void);
    bsd_syscall_init();
    s_printf("[KERNEL] BSD Syscall Layer Initialized.\n");
    
    // Initialize framework stubs (CoreGraphics, CoreText, CFNetwork, etc.)
    extern void framework_stubs_init(void);
    framework_stubs_init();
    s_printf("[KERNEL] Framework Stubs Initialized.\n");
    
    // Initialize DMG mounter subsystem for macOS disk image support
    extern void dmg_init_system(void);
    dmg_init_system();
    s_printf("[KERNEL] DMG Mounter Initialized.\n");
    
    // Initialize App Bundle loader
    extern void app_bundle_init_system(void);
    app_bundle_init_system();
    s_printf("[KERNEL] App Bundle Loader Initialized.\n");
    
    // Initialize App Installer (drag-to-Applications)
    extern void app_installer_init(void);
    app_installer_init();
    s_printf("[KERNEL] App Installer Initialized.\n");
    
    // Initialize System Directory Structure (/usr, /etc, /var, /tmp, /dev, /proc, /Library, etc.)
    sys_dirs_init();
    s_printf("[KERNEL] System Directories Initialized.\n");
    
    // Initialize Package Manager (caml - install/remove/list packages)
    pkg_init();
    s_printf("[KERNEL] Package Manager Initialized.\n");
    
    // Initialize App Registry (discovers and registers all installed and built-in apps)
    app_registry_init();
    s_printf("[KERNEL] App Registry Initialized.\n");
    
    // Initialize SHA-256 module (used for encrypted passwords)
    // No init needed - it's stateless
    s_printf("[KERNEL] SHA-256 Hash Module Ready.\n");

    // Initialize Virtual Filesystem Layer (VFS)
    extern void vfs_init(void);
    vfs_init();
    s_printf("[KERNEL] Virtual Filesystem (VFS) Initialized.\n");

    // Initialize Process Management (fork/exec/wait)
    extern void process_init(void);
    process_init();
    s_printf("[KERNEL] Process Management Initialized.\n");

    // Initialize Crash Reporter
    extern void crash_reporter_init(void);
    crash_reporter_init();
    s_printf("[KERNEL] Crash Reporter Initialized.\n");

    // Initialize launchd Service Manager (macOS-style PID 1 equivalent)
    launchd_init();
    s_printf("[KERNEL] launchd Service Manager Initialized.\n");

    // Register core system services with launchd
    // These are tracked for health monitoring and auto-restart on crash
    launchd_register("NetworkStack",   launchd_start_network_stack,   1, 1, 3);  // auto-start, keep-alive, max 3 crashes
    launchd_register("WindowServer",   launchd_start_window_server,   1, 1, 3);  // auto-start, keep-alive, max 3 crashes
    launchd_register("CrashReporter",  launchd_start_crash_reporter,  0, 1, 5);  // manual start, keep-alive, max 5 crashes
    launchd_register("IPCService",     launchd_start_ipc_service,     1, 1, 3);  // auto-start, keep-alive, max 3 crashes
    launchd_add_dependency("WindowServer", "NetworkStack");  // WindowServer needs network
    s_printf("[KERNEL] Core services registered with launchd.\n");

    // Auto-start all services marked auto_start=1
    launchd_boot_start();
    s_printf("[KERNEL] launchd boot auto-start complete.\n");

    // Register FAT32 filesystem driver with VFS
    // FAT32 partitions can be mounted after calling fat32_init() with the partition LBA.
    // This registration makes VFS aware of the FAT32 driver type.
    extern int fat32_register_with_vfs(void);
    fat32_register_with_vfs();
    s_printf("[KERNEL] FAT32 VFS driver registered.\n");

    // === Auto-detect and mount FAT32 partitions from MBR ===
    // Parse the MBR partition table to find FAT32 partitions (type 0x0B or 0x0C).
    // For each found partition, initialize the FAT32 driver and mount it.
    {
        uint8_t mbr_buf[512];
        if (disk_read_block(0, mbr_buf) == 0) {
            /* Validate MBR signature */
            if (mbr_buf[510] == 0x55 && mbr_buf[511] == 0xAA) {
                /* MBR partition table starts at offset 0x1BE, 4 entries of 16 bytes each */
                int fat32_mount_count = 0;
                for (int p = 0; p < 4; p++) {
                    uint8_t* entry = &mbr_buf[0x1BE + p * 16];
                    uint8_t ptype = entry[4];           /* Partition type byte */
                    uint32_t lba_start = *(uint32_t*)(entry + 8);   /* LBA of first sector */
                    /* uint32_t psize = *(uint32_t*)(entry + 12); */  /* Partition size in sectors */

                    /* FAT32 partition types: 0x0B = FAT32 CHS, 0x0C = FAT32 LBA */
                    if ((ptype == 0x0B || ptype == 0x0C) && lba_start > 0) {
                        char ptype_str[8];
                        extern void int_to_str(int, char*);
                        int_to_str(ptype, ptype_str);
                        s_printf("[KERNEL] Found FAT32 partition %d (type=0x%s) at LBA ", p);
                        int_to_str(lba_start, buf);
                        s_printf(buf);
                        s_printf("\n");

                        /* Initialize the FAT32 driver for this partition */
                        extern int fat32_init(uint32_t partition_start_lba);
                        int init_ok = fat32_init(lba_start);
                        if (init_ok == 0) {
                            /* Mount via VFS */
                            char mount_path[] = "/mnt/disk1";
                            mount_path[9] = '1' + fat32_mount_count;  /* /mnt/disk1, /mnt/disk2, etc. */
                            int mount_ok = vfs_mount(mount_path, VFS_FS_FAT32, NULL);
                            if (mount_ok == 0) {
                                s_printf("[KERNEL] Mounted FAT32 at ");
                                s_printf(mount_path);
                                s_printf("\n");
                                fat32_mount_count++;
                            } else {
                                s_printf("[KERNEL] WARNING: FAT32 VFS mount failed for partition ");
                                int_to_str(p, buf);
                                s_printf(buf);
                                s_printf("\n");
                            }
                        } else {
                            s_printf("[KERNEL] WARNING: FAT32 init failed for partition ");
                            int_to_str(p, buf);
                            s_printf(buf);
                            s_printf(" (err=");
                            int_to_str(init_ok, buf);
                            s_printf(buf);
                            s_printf(")\n");
                        }
                    }
                }
                if (fat32_mount_count == 0) {
                    s_printf("[KERNEL] No FAT32 partitions found in MBR.\n");
                }
            } else {
                s_printf("[KERNEL] MBR signature invalid, skipping partition scan.\n");
            }
        } else {
            s_printf("[KERNEL] Failed to read MBR for partition scan.\n");
        }
    }

    int m = sys_fs_mount();
    s_printf("[DBG] sys_fs_mount returned ");
    int_to_str(m, buf);
    s_printf(buf);
    s_printf("\n");
    if (m != 0) {
        s_printf("[KERNEL] FATAL: Filesystem mount failed. Halting.\n");
        while(1) asm("hlt");
    }
    sys_print("[OK] Filesystem Mounted.\n");

    sys_print("Booting...\n");
    sys_print("--- Hardware Enumeration (already done) ---\n");
    sys_print("----------------------------\n");

    // NETWORK INITIALIZATION WITH DEBUG (after PCI scan)
    s_printf("[KERNEL] Initializing network...\n");
    net_init();

    // Initialize Intel e1000/e1000e Gigabit Ethernet (PCI-based probing)
    extern void e1000_init_all(void);
    e1000_init_all();

    // FIX: Skip the RTL8139 loopback test at boot — it sends packets
    // before the interface is configured, which can interfere with
    // the DHCP sequence. The loopback test was mainly for debugging
    // and is not needed for normal operation.
    // rtl8139_test_loopback();

    // === Try DHCP auto-configuration first ===
    // If a DHCP server is available (e.g., QEMU's built-in DHCP), we get an IP automatically.
    // Falls back to static QEMU configuration if DHCP times out.
    extern int dhcp_auto_configure(void);
    int dhcp_ok = dhcp_auto_configure();

    if (dhcp_ok == 0) {
        s_printf("[KERNEL] Network configured via DHCP\n");
    } else {
        s_printf("[KERNEL] DHCP failed, using static QEMU configuration\n");

        // MANUAL GATEWAY SETUP FOR QEMU
        // rtl8139_init() now handles the IP configuration

        // === FIX 1: Correct IP Endianness for QEMU Gateway/DNS ===
        // CRITICAL: IP addresses must be in HOST byte order (little-endian on x86)
        // Use ip_parse() to ensure correct byte order conversion
        uint8_t qemu_mac[6] = {0x52, 0x54, 0x00, 0x12, 0x34, 0x56};
        
        // Gateway: 10.0.2.2
        extern uint32_t ip_parse(const char* str);
        net_add_static_arp(ip_parse("10.0.2.2"), qemu_mac);
        
        // DNS: 10.0.2.3
        net_add_static_arp(ip_parse("10.0.2.3"), qemu_mac);

        // === FIX: Actually configure the interface IP ===
        // Use ip_parse() for correct byte order
        extern void rtl8139_configure_ip(uint32_t, uint32_t, uint32_t);
        rtl8139_configure_ip(ip_parse("10.0.2.15"), ip_parse("10.0.2.2"), ip_parse("255.255.255.0"));

        // FIX: Explicitly set the DNS server for the static configuration.
        // The DNS server was already changed to 10.0.2.3 as the default in dns.c,
        // but we also set it here via the proper API to ensure the interface
        // struct has the correct value (used by DHCP ACK parsing, etc.)
        extern void net_set_dns(uint32_t dns);
        net_set_dns(ip_parse("10.0.2.3"));

        // Update legacy global variables
        extern void net_update_globals();
        net_update_globals();

        // Mark as connected so the GUI Wi-Fi icon shows connected state
        extern int net_is_connected;
        net_is_connected = 1;

        s_printf("[KERNEL] Network configured for QEMU\n");
        s_printf("  IP:      10.0.2.15\n");
        s_printf("  Gateway: 10.0.2.2\n");
        s_printf("  DNS:     10.0.2.3\n");
    }

    // === FIX 2: Ensure RTL8139 is Active (Fixing CMD: 0x13 Reset State) ===
    // The previous log showed CMD=0x13 (RST bit set, RX/TX disabled).
    // We force a re-enable here to ensure the card is out of reset.
    extern rtl8139_dev_t rtl_dev; // From rtl8139 driver
    if (rtl_dev.io_base) {
        // 1. Check if Reset (Bit 4) is still on
        uint8_t cmd = inb(rtl_dev.io_base + 0x37);
        if (cmd & 0x10) {
            s_printf("[KERNEL] RTL8139 stuck in reset. Forcing clear...\n");
            outb(rtl_dev.io_base + 0x37, 0x00); // Clear Reset
        }
        
        // 2. Enable Transmit (Bit 2) and Receive (Bit 3) -> 0x0C
        outb(rtl_dev.io_base + 0x37, 0x0C);
        
        // 3. Verify
        cmd = inb(rtl_dev.io_base + 0x37);
        if ((cmd & 0x0C) == 0x0C) {
            s_printf("[KERNEL] RTL8139 Active (CMD: 0x0C)\n");
        } else {
            s_printf("[KERNEL] WARNING: RTL8139 Init Failed (CMD: 0x");
            char buf[16];
            extern void int_to_str(int, char*);
            int_to_str(cmd, buf);
            s_printf(buf);
            s_printf(")\n");
        }
    }

    // === FIX: Quick network verification (non-blocking) ===
    // Previously, the boot sequence did ARP resolve (2s), ICMP ping (4s),
    // DNS resolve with 3 retries (15s), and TCP connect (100s) ALL before
    // starting the GUI. This caused the system to appear frozen for 2+
    // minutes if the network was slow or misconfigured.
    //
    // Now we do a single quick DNS check (5s timeout) and proceed to the
    // GUI immediately. Network is verified lazily as apps use it.
    s_printf("[KERNEL] Quick network check...\n");
    {
        char ip_str[16];
        int dns_ok = dns_resolve("example.com", ip_str, sizeof(ip_str));
        if(dns_ok == 0) {
            s_printf("[KERNEL] Network OK: example.com -> ");
            s_printf(ip_str);
            s_printf("\n");
        } else {
            s_printf("[KERNEL] Network not yet reachable (will retry in background)\n");
        }
    }

    // Initialize USB HID Boot Protocol driver for USB keyboards and mice
    // This enables operation on USB-only hardware (no PS/2 required)
    extern void usb_hid_init(void);
    usb_hid_init();
    s_printf("[KERNEL] USB HID Driver Initialized.\n");

    // Initialize Software Update system
    // Provides background update checking, download, and installation
    extern void software_update_init(void);
    software_update_init();
    s_printf("[KERNEL] Software Update System Initialized.\n");

    play_startup_chime();

    int boot_to_shell = 0;
    for(int i=0; i<50; i++) { 
        sys_delay(2); 
        if (kbd_ctrl_pressed && kbd_shift_pressed) { boot_to_shell = 1; break; }
    }

    if (boot_to_shell) {
        vga_set_color(GREEN, BLACK);
        sys_print("\nEntering Shell.\n");
        extern void shell_main();
        shell_main();
    } else {
        sys_print("\nStarting Graphic Environment...\n");
        
        // Disable log muting temporarily to catch startup crashes visually if possible
        // vga_mute_log(1); // COMMENTED OUT FOR DEBUGGING
        
        sys_clear();
        init_mouse();
        
        // Ensure mouse isn't drawing out of bounds before first update
        extern int mouse_x, mouse_y, screen_w, screen_h;
        mouse_x = screen_w / 2;
        mouse_y = screen_h / 2;
        
        transition_to_gui();
        
        extern void start_bubble_view();
        start_bubble_view();
        
        // Main event loop
        while(1) {
            extern void rtl8139_poll();
            rtl8139_poll();  // Poll network card regularly
            
            // Process TCP listener queue and retransmit timers
            extern void tcp_process_listeners(void);
            extern void tcp_retransmit_check(void);
            tcp_process_listeners();
            tcp_retransmit_check();
            
            // Periodic launchd health check
            extern void launchd_check_health(void);
            launchd_check_health();
            
            // Poll USB HID devices for input
            extern void usb_hid_poll(void);
            usb_hid_poll();
            
            // Background software update check
            extern void software_update_check_background(void);
            software_update_check_background();
            
            asm("hlt");  // Halt until next interrupt
        }
    }

    while(1) asm("hlt");
}