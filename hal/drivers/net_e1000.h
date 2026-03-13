// hal/drivers/net_e1000.h - Intel e1000 Gigabit Ethernet Driver Header
#ifndef NET_E1000_H
#define NET_E1000_H

#include <types.h>
#include "../../core/net.h"

// Initialize all e1000 devices
void e1000_init_all(void);

// Poll all e1000 devices for received packets
void e1000_poll_all(void);

// Get device count
int e1000_get_device_count(void);

// Get link status
int e1000_get_link_status(int device_index);

// Get link speed (10, 100, or 1000)
int e1000_get_link_speed(int device_index);

// Get link duplex (0=half, 1=full)
int e1000_get_link_duplex(int device_index);

#endif // NET_E1000_H
