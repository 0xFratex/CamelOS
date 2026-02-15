#ifndef SYS_IO_PORTS_H
#define SYS_IO_PORTS_H

#include "../common/ports.h" // Legacy x86 ports

// MMIO Macros for non-x86 architectures or Memory Mapped devices (like XHCI)
#define MMIO_READ8(addr)  (*((volatile uint8_t  *)(addr)))
#define MMIO_READ16(addr) (*((volatile uint16_t *)(addr)))
#define MMIO_READ32(addr) (*((volatile uint32_t *)(addr)))
#define MMIO_READ64(addr) (*((volatile uint64_t *)(addr)))

#define MMIO_WRITE8(addr, val)  (*((volatile uint8_t  *)(addr)) = (val))
#define MMIO_WRITE16(addr, val) (*((volatile uint16_t *)(addr)) = (val))
#define MMIO_WRITE32(addr, val) (*((volatile uint32_t *)(addr)) = (val))
#define MMIO_WRITE64(addr, val) (*((volatile uint64_t *)(addr)) = (val))

#endif