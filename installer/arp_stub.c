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

// TCP maintenance stubs.
// hal/cpu/timer.c now calls these from the timer IRQ to keep the browser's
// connections alive during long fetches. The installer shares timer.o with
// the kernel build but does NOT link core/tcp.o (it has no networking),
// so we provide no-op stubs here. The installer's timer_callback will
// happily call them every ~200ms with no effect.
void tcp_retransmit_check(void) {
    // Stub - installer has no TCP stack
}

void tcp_process_listeners(void) {
    // Stub - installer has no TCP listeners
}
