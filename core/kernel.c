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
#include "../core/string.h"
#include "../fs/pfs32.h"
#include "../core/memory.h"
#include "../hal/cpu/paging.h"
#include "../core/net.h"
#include "../core/dns.h"
#include "../core/net_if.h"
#include "../hal/drivers/net_rtl8139.h"

extern int kbd_ctrl_pressed;
extern int kbd_shift_pressed;
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
    init_keyboard();
    init_serial();
    
    // --- MEMORY FIX ---
    uint32_t heap_start = (uint32_t)&_bss_end;
    if (heap_start % 16 != 0) heap_start += 16 - (heap_start % 16);
    init_heap(heap_start, 32 * 1024 * 1024);
    
    init_paging();
    init_apic();
    init_timer(50);
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

void kernel_main(void* mboot_ptr) {
    kernel_init_hal(); 
    s_printf("\n[KERNEL] Entry successful.\n");

    extern void gfx_init_hal(void*);
    gfx_init_hal(mboot_ptr);

    extern void pfs32_init_handles();
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

    if (sys_fs_mount() != 0) s_printf("[KERNEL] FS Mount Failed.\n");
    else sys_print("[OK] Filesystem Mounted.\n");

    sys_print("Booting...\n");
    sys_print("\n--- Hardware Enumeration ---\n");
    pci_init();
    sys_print("----------------------------\n");

    // NETWORK INITIALIZATION WITH DEBUG (after PCI scan)
    s_printf("[KERNEL] Initializing network...\n");
    net_init();

    // Test RTL8139 basic functionality
    rtl8139_test_loopback();

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

    // Update legacy global variables
    extern void net_update_globals();
    net_update_globals();

    s_printf("[KERNEL] Network configured for QEMU\n");
    s_printf("  IP:      10.0.2.15\n");
    s_printf("  Gateway: 10.0.2.2\n");
    s_printf("  DNS:     10.0.2.3\n");

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

    // === FIX: Pre-resolve gateway ARP before DNS test ===
    s_printf("[KERNEL] Pre-resolving gateway ARP...\n");
    extern int arp_resolve(uint32_t ip, uint8_t* mac_out);
    uint8_t gw_mac[6];
    int arp_ok = arp_resolve(ip_parse("10.0.2.2"), gw_mac);  // Gateway IP
    if (arp_ok == 0) {
        s_printf("[KERNEL] Gateway ARP resolved: ");
        for(int i=0; i<6; i++) {
            char hex[3];
            hex[0] = "0123456789ABCDEF"[gw_mac[i] >> 4];
            hex[1] = "0123456789ABCDEF"[gw_mac[i] & 0xF];
            hex[2] = 0;
            s_printf(hex);
            if(i<5) s_printf(":");
        }
        s_printf("\n");
    } else {
        s_printf("[KERNEL] WARNING: Gateway ARP not resolved, using static\n");
    }

    // === ICMP Ping Test to Gateway ===
    s_printf("[KERNEL] Testing ICMP ping to gateway (10.0.2.2)...\n");
    extern int ping(uint32_t ip, uint32_t timeout);
    int ping_ok = ping(ip_parse("10.0.2.2"), 200);  // Gateway IP, 200 tick timeout
    if (ping_ok == 0) {
        s_printf("[KERNEL] ✓ Gateway ping successful\n");
    } else {
        s_printf("[KERNEL] ✗ Gateway ping failed\n");
    }

    // Test network with retries
    char ip_str[16];
    int dns_ok = -1;
    for(int retry = 0; retry < 3; retry++) {
        if(retry > 0) {
            s_printf("[KERNEL] DNS retry ");
            char buf[4];
            extern void int_to_str(int, char*);
            int_to_str(retry, buf);
            s_printf(buf);
            s_printf("/2...\n");
            
            // Poll network between retries
            for(int p=0; p<50; p++) {
                rtl8139_poll();
                for(volatile int d=0; d<10000; d++) asm volatile("pause");
            }
        }
        
        dns_ok = dns_resolve("example.com", ip_str, sizeof(ip_str));
        if(dns_ok == 0) break;
    }
    
    if(dns_ok == 0) {
        s_printf("[KERNEL] ✓ Network test passed: ");
        s_printf(ip_str);
        s_printf("\n");
        
        // Test TCP connection to example.com
        s_printf("[KERNEL] Testing TCP connection to example.com:80...\n");
        extern void* tcp_connect_with_ptr(uint32_t remote_ip, uint16_t remote_port);
        extern int tcp_conn_is_established(void* conn);
        extern uint32_t ip_parse(const char* str);
        
        uint32_t example_ip = ip_parse(ip_str);
        void* conn = tcp_connect_with_ptr(example_ip, 80);
        if (conn) {
            uint32_t start = timer_get_ticks();
            int established = 0;
            while (timer_get_ticks() - start < 5000) {
                rtl8139_poll();
                if (tcp_conn_is_established(conn)) {
                    established = 1;
                    break;
                }
                asm volatile("pause");
            }
            if (established) {
                s_printf("[KERNEL] ✓ TCP connection established!\n");
            } else {
                s_printf("[KERNEL] ✗ TCP connection timeout\n");
            }
        } else {
            s_printf("[KERNEL] ✗ Failed to create TCP connection\n");
        }
    } else {
        s_printf("[KERNEL] ✗ Network test failed after retries\n");
        // Run diagnostic - TODO: implement net_diagnostic
    }

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
            
            // Other event processing...
            
            asm("hlt");  // Halt until next interrupt
        }
    }

    while(1) asm("hlt");
}