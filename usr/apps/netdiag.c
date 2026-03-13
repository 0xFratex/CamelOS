// usr/apps/netdiag.cdl - Network Diagnostic Tool
#include "../../sys/cdl_defs.h"

static kernel_api_t* sys = 0;

typedef struct {
    char name[32];
    int (*test_func)(void);
    char description[128];
} net_test_t;

// Test functions
int test_arp(void) {
    sys->print("[TEST] ARP Test\n");

    // Test gateway ARP - we need to call the kernel function
    // For now, just check if we can ping the gateway
    char result[128];
    int status = sys->ping("10.0.2.2", result, 128);

    if(status >= 0) {
        sys->print("[TEST] ✓ Gateway reachable: ");
        sys->print(result);
        sys->print("\n");
        return 1;
    } else {
        sys->print("[TEST] ✗ Gateway not reachable\n");
        return 0;
    }
}

int test_dns(void) {
    sys->print("[TEST] DNS Test\n");

    char ip[32];
    int result = sys->dns_resolve("example.com", ip, 32);

    if(result == 0) {
        sys->print("[TEST] ✓ DNS resolved: ");
        sys->print(ip);
        sys->print("\n");
        return 1;
    } else {
        sys->print("[TEST] ✗ DNS failed\n");
        return 0;
    }
}

int test_ping(void) {
    sys->print("[TEST] Ping Test\n");

    char result[128];
    int status = sys->ping("8.8.8.8", result, 128);

    if(status >= 0) {
        sys->print("[TEST] ✓ Ping successful: ");
        sys->print(result);
        sys->print("\n");
        return 1;
    } else {
        sys->print("[TEST] ✗ Ping failed\n");
        return 0;
    }
}

int test_interface(void) {
    sys->print("[TEST] Interface Test\n");

    char ip[16], mac[20];
    if(sys->net_get_interface_info("eth0", ip, mac) == 0) {
        sys->print("[TEST] ✓ Interface eth0:\n");
        sys->print("  IP:  "); sys->print(ip); sys->print("\n");
        sys->print("  MAC: "); sys->print(mac); sys->print("\n");
        return 1;
    } else {
        sys->print("[TEST] ✗ Interface not found\n");
        return 0;
    }
}

// Diagnostic UI
void on_paint(int x, int y, int w, int h) {
    sys->draw_rect(x, y, w, h, 0xFF1E1E1E);

    sys->draw_text(x + 10, y + 20, "Network Diagnostics", 0xFF00FFFF);
    sys->draw_rect(x + 10, y + 40, w - 20, 1, 0xFF444444);

    // Test results would be displayed here
    sys->draw_text(x + 20, y + 60, "Press SPACE to run tests", 0xFFCCCCCC);
    sys->draw_text(x + 20, y + 80, "Press R to reset network", 0xFFCCCCCC);
    sys->draw_text(x + 20, y + 100, "Press P to start packet capture", 0xFFCCCCCC);
}

void on_input(int key) {
    if(key == ' ') {
        // Run all tests
        sys->print("\n=== NETWORK DIAGNOSTICS ===\n");

        int passed = 0;
        passed += test_interface();
        passed += test_arp();
        passed += test_dns();
        passed += test_ping();

        sys->print("\n=== RESULTS: ");
        char buf[16];
        sys->itoa(passed, buf);
        sys->print(buf);
        sys->print("/4 tests passed ===\n");
    }
    else if(key == 'r' || key == 'R') {
        sys->print("[DIAG] Reset network functionality not implemented\n");
    }
    else if(key == 'p' || key == 'P') {
        sys->print("[DIAG] Packet capture functionality not implemented\n");
    }
}

cdl_exports_t* cdl_main(kernel_api_t* api) {
    sys = api;

    sys->print("[NETDIAG] Network Diagnostics v1.0\n");
    sys->print("  Run tests with SPACE\n");
    sys->print("  Reset network with R\n");
    sys->print("  Capture packets with P\n");

    void* win = sys->create_window("Network Diagnostics", 400, 300, on_paint, on_input, 0);
    return 0;
}