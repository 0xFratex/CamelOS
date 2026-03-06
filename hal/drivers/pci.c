#include "pci.h"
#include "vga.h"
#include "serial.h"
#include "../../core/string.h"
#include "../../core/memory.h"

extern void rtl8139_init(pci_device_t* dev);
extern void rtl8169_init(pci_device_t* dev);
extern void xhci_controller_init(pci_device_t* dev);
extern void wifi_rtl8188_probe(void* dev); // Mock

uint8_t rtl8139_irq_line = 0xFF;
uint8_t rtl8169_irq_line = 0xFF;

uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_write_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (uint32_t)((bus << 16) | (slot << 11) | (func << 8) | (offset & 0xFC) | 0x80000000);
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, value);
}

void pci_enable_bus_master(pci_device_t* dev) {
    uint32_t cmd = pci_read_config_dword(dev->bus, dev->slot, dev->func, 0x04);
    cmd |= 0x07; 
    cmd &= ~(1 << 10); // Unmask INTx
    pci_write_config_dword(dev->bus, dev->slot, dev->func, 0x04, cmd);
}

void pci_check_function(uint8_t bus, uint8_t device, uint8_t function) {
    uint32_t vendor_did = pci_read_config_dword(bus, device, function, 0x00);
    if ((vendor_did & 0xFFFF) == 0xFFFF) return;

    pci_device_t dev;
    memset(&dev, 0, sizeof(pci_device_t));
    
    dev.bus = bus; dev.slot = device; dev.func = function;
    dev.vendor_id = vendor_did & 0xFFFF;
    dev.device_id = (vendor_did >> 16) & 0xFFFF;

    uint32_t class_rev = pci_read_config_dword(bus, device, function, 0x08);
    dev.class_id = (class_rev >> 24) & 0xFF;
    dev.subclass_id = (class_rev >> 16) & 0xFF;
    dev.prog_if = (class_rev >> 8) & 0xFF;

    // Parse BARs
    for(int i=0; i<6; i++) {
        uint32_t bar_val = pci_read_config_dword(bus, device, function, 0x10 + (i*4));
        if(bar_val == 0) continue;
        if(bar_val & 1) { 
            dev.bar_type[i] = 1; dev.bar[i] = bar_val & 0xFFFFFFFC;
        } else { 
            dev.bar_type[i] = 0; dev.bar[i] = bar_val & 0xFFFFFFF0;
        }
    }

    uint32_t intr = pci_read_config_dword(bus, device, function, 0x3C);
    dev.irq_line = intr & 0xFF;

    // Device Identification
    char name[64] = "Unknown Device";

    if(dev.class_id == 0x03) strcpy(name, "VGA Controller");
    else if(dev.class_id == 0x02) strcpy(name, "Network Controller");
    else if(dev.class_id == 0x06) strcpy(name, "Bridge Device");
    else if(dev.class_id == 0x01) strcpy(name, "Storage Controller");
    else if(dev.class_id == 0x0C) strcpy(name, "Serial Bus (USB)");

    // Specific Overrides
    if(dev.vendor_id == 0x10EC && dev.device_id == 0x8139) strcpy(name, "Realtek RTL8139 Fast Ethernet");
    if(dev.vendor_id == 0x10EC && dev.device_id == 0x8169) strcpy(name, "Realtek RTL8169 Gigabit Ethernet");
    if(dev.vendor_id == 0x10EC && dev.device_id == 0x8168) strcpy(name, "Realtek RTL8111/8168 Gigabit Ethernet");
    if(dev.vendor_id == 0x10EC && dev.device_id == 0x8136) strcpy(name, "Realtek RTL8101E Fast Ethernet");
    if(dev.vendor_id == 0x8086 && dev.device_id == 0x7000) strcpy(name, "Intel PIIX3 ISA");
    if(dev.vendor_id == 0x8086 && dev.device_id == 0x7113) strcpy(name, "Intel PIIX4 ACPI");

    // Print formatted
    s_printf("[PCI] ");
    char buf[8];
    extern void int_to_str(int, char*);
    int_to_str(dev.bus, buf); s_printf(buf); s_printf(":");
    int_to_str(dev.slot, buf); s_printf(buf); s_printf(".0  ");
    s_printf(name);
    s_printf("\n");

    // Initialize Drivers
    if(dev.vendor_id == 0x10EC && dev.device_id == 0x8139) {
        rtl8139_irq_line = dev.irq_line;
        rtl8139_init(&dev);
    }
    
    // RTL8169/RTL8111/RTL8168 Family (PCIe Gigabit Ethernet)
    // Device IDs: 0x8168 (RTL8111/8168), 0x8169 (RTL8169), 0x8136 (RTL8101E)
    if(dev.vendor_id == 0x10EC && 
       (dev.device_id == 0x8169 || dev.device_id == 0x8168 || dev.device_id == 0x8136)) {
        s_printf("[PCI] Found Realtek RTL8169 family device\n");
        rtl8169_irq_line = dev.irq_line;
        rtl8169_init(&dev);
    }

    // Trigger Mock Wifi if we see a USB controller (often present)
    if(dev.class_id == 0x0C) {
        wifi_rtl8188_probe(0);
    }
}

void pci_init() {
    s_printf("\n[PCI] Scanning Bus...\n");
    for(uint16_t bus = 0; bus < 256; bus++) {
        for(uint8_t slot = 0; slot < 32; slot++) {
            uint32_t vendor = pci_read_config_dword(bus, slot, 0, 0x00) & 0xFFFF;
            if(vendor != 0xFFFF) {
                pci_check_function(bus, slot, 0);
                uint32_t header = pci_read_config_dword(bus, slot, 0, 0x0C);
                if((header >> 16) & 0x80) {
                    for(int f=1; f<8; f++) {
                        if((pci_read_config_dword(bus, slot, f, 0x00) & 0xFFFF) != 0xFFFF) {
                            pci_check_function(bus, slot, f);
                        }
                    }
                }
            }
        }
    }
    s_printf("[PCI] Scan Complete.\n");
}