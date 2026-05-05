// installer/arp_stub.c - Stubs for functions not included in installer build
// The installer doesn't need full VMM/TSS/scheduler, but some shared objects
// reference these functions.

void arp_cleanup(void) {
    // Stub - installer doesn't need ARP
}

// VMM stubs - installer uses simple paging, not full VMM
typedef unsigned int uint32_t;
typedef struct { int dummy; } address_space_t;

void vmm_switch_address_space(address_space_t* space) {
    (void)space;
    // Stub - installer doesn't use address space switching
}

int vmm_handle_page_fault(uint32_t fault_addr, uint32_t error_code) {
    (void)fault_addr; (void)error_code;
    // Stub - installer uses simple page fault handler
    return -1;
}

// TSS stub - installer doesn't do user-mode task switching
void tss_set_kernel_stack(uint32_t esp0) {
    (void)esp0;
    // Stub - installer runs entirely in kernel mode
}
