// hal/cpu/apic.h
#ifndef APIC_H
#define APIC_H

#include "../../include/types.h"

void init_apic();
void apic_send_eoi();
void ioapic_set_gsi_redirect(uint8_t gsi, uint8_t vector, uint8_t cpu_apic_id, int active_low, int level_trigger);

// Register access for Timer
void lapic_write_timer(uint32_t reg, uint32_t value);
uint32_t lapic_read_timer(uint32_t reg);

#endif