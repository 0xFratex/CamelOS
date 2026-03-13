#!/usr/bin/env python3
"""
Generate a raw 8-bit PCM startup sound for Camel OS.
Creates a C-Major arpeggio compatible with Sound Blaster 16 direct mode.
"""

import math
import os

# Configuration for SB16 (Direct Mode)
SAMPLE_RATE = 22050
OUTPUT_FILE = "assets/system_sounds/startup.pcm"

def generate_pcm():
    # Ensure directory exists
    os.makedirs(os.path.dirname(OUTPUT_FILE), exist_ok=True)

    # C Major Chord: C4, E4, G4, C5
    notes = [
        (261.63, 0.15), # C4
        (329.63, 0.15), # E4
        (392.00, 0.15), # G4
        (523.25, 0.60)  # C5 (Longer)
    ]

    pcm_data = bytearray()

    for freq, duration in notes:
        num_samples = int(duration * SAMPLE_RATE)
        for i in range(num_samples):
            t = float(i) / SAMPLE_RATE
            
            # Simple decay envelope (volume drops over time)
            vol = 1.0 - (i / num_samples)
            
            # Generate Sine Wave (0-255, Center 128)
            # 127 is amplitude, 128 is offset for unsigned 8-bit
            sample = 128 + int(100 * vol * math.sin(2 * math.pi * freq * t))
            
            # Clip
            sample = max(0, min(255, sample))
            pcm_data.append(sample)

    # Add a bit of silence at the end
    for _ in range(1000):
        pcm_data.append(128)

    with open(OUTPUT_FILE, "wb") as f:
        f.write(pcm_data)
    
    print(f"[Sound] Generated {OUTPUT_FILE} ({len(pcm_data)} bytes)")

if __name__ == "__main__":
    generate_pcm()