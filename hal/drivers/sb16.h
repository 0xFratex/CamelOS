// hal/drivers/sb16.h - Sound Blaster 16 Driver
// Provides low-level SB16 DSP programming for PCM audio output.

#ifndef SB16_H
#define SB16_H

#include "../../include/types.h"

// Initialize the SB16 DSP (reset and verify)
// Returns 1 if SB16 detected, 0 if not found
int sb16_init(void);

// Play raw 8-bit unsigned PCM data directly through the SB16 DAC.
// This is a blocking call — it spins until all samples are written.
// Data length is clamped to 65535 bytes for safety.
void sb16_play_direct(uint8_t* data, uint32_t len);

#endif // SB16_H
