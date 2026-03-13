#ifndef PCI_H
#define PCI_H

#include "../common/ports.h"

typedef struct {
    uint32_t id;          // Internal OS ID
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t class_id;
    uint8_t subclass_id;
    uint8_t prog_if;
    uint8_t rev_id;
    
    uint8_t bus;
    uint8_t slot;
    uint8_t func;
    
    uint32_t bar[6];      // Base Address Registers
    uint32_t size[6];     // Size of memory region
    int bar_type[6];      // 0 = Memory, 1 = IO
    
    uint8_t irq_line;
    uint8_t irq_pin;
} pci_device_t;

// PCI Configuration Macros
#define PCI_CONFIG_ADDR 0xCF8
#define PCI_CONFIG_DATA 0xCFC

// Class Codes
#define PCI_CLASS_NETWORK  0x02
#define PCI_CLASS_SERIAL   0x0C // USB is here

void pci_init(void);
uint32_t pci_read(pci_device_t dev, uint32_t field);
void pci_write(pci_device_t dev, uint32_t field, uint32_t value);
uint32_t pci_read_config_dword(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void pci_enable_bus_master(pci_device_t* dev);

// RTL8139 IRQ line
extern uint8_t rtl8139_irq_line;

// RTL8169 IRQ line
extern uint8_t rtl8169_irq_line;

#endif