// installer/arp_stub.c - Stub for ARP functions in installer
// The installer doesn't need full networking, but timer.c calls arp_cleanup()

void arp_cleanup(void) {
    // Stub - installer doesn't need ARP
}
