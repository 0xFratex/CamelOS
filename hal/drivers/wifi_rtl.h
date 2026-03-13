#ifndef WIFI_RTL_H
#define WIFI_RTL_H

#include "usb.h"

// Realtek Register Offsets (8188CUS)
#define RTL_REG_MACID    0x0000
#define RTL_REG_SYS_CFG  0x0002
#define RTL_REG_GPU_CFG  0x000D // GPIO
#define RTL_REG_TX_DMA   0x0040

void wifi_rtl8188_probe(void* dev);

#endif