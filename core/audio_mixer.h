// core/audio_mixer.h - Software Audio Mixer for CamelOS
// Provides multi-channel PCM audio mixing, volume control,
// and system sound event playback.

#ifndef AUDIO_MIXER_H
#define AUDIO_MIXER_H

#include "../../include/types.h"

// ============================================================================
// Configuration
// ============================================================================
#define AUDIO_MIXER_CHANNELS    8       // Max simultaneous channels
#define AUDIO_SAMPLE_RATE       22050   // Hz (matches SB16/QEMU output)
#define AUDIO_BUFFER_SIZE       4096    // Per-channel PCM buffer (bytes)

// Channel types
#define AUDIO_CHAN_SYSTEM       0       // System sounds (startup, click, etc.)
#define AUDIO_CHAN_APP          1       // Application sounds
#define AUDIO_CHAN_MUSIC        2       // Background music
#define AUDIO_CHAN_NOTIFICATION 3       // Notification sounds

// System sound IDs
#define SND_STARTUP             0
#define SND_NOTIFICATION        1
#define SND_ERROR               2
#define SND_CLICK               3
#define SND_VOLUME_CHANGE       4
#define SND_COUNT               5

// ============================================================================
// Types
// ============================================================================

typedef struct {
    uint8_t*    pcm_data;       // PCM sample data (8-bit unsigned mono)
    uint32_t    pcm_length;     // Length of pcm_data in bytes
    uint32_t    position;       // Current playback position
    int         playing;        // 1 = actively playing
    int         looping;        // 1 = loop when done
    int         type;           // AUDIO_CHAN_*
    uint8_t     volume;         // Per-channel volume (0-255)
} audio_channel_t;

typedef struct {
    audio_channel_t channels[AUDIO_MIXER_CHANNELS];
    uint8_t         master_volume;     // Master volume (0-255)
    int             initialized;
    // Pre-synthesized system sound data
    uint8_t*        system_sounds[SND_COUNT];
    uint32_t        system_sound_len[SND_COUNT];
} audio_mixer_t;

// ============================================================================
// API
// ============================================================================

// Initialize the audio mixer subsystem
void audio_mixer_init(void);

// Play a system sound by ID (SND_STARTUP, SND_NOTIFICATION, etc.)
// Returns the channel index used, or -1 if no channel available.
int audio_play_system_sound(int sound_id);

// Play raw PCM data on a free channel.
// `data` must remain valid for the duration of playback.
// `type` is AUDIO_CHAN_* for volume grouping.
// Returns channel index or -1 on failure.
int audio_play_pcm(const uint8_t* data, uint32_t length, int type, int looping);

// Stop playback on a specific channel
void audio_channel_stop(int channel);

// Set per-channel volume (0-255)
void audio_channel_set_volume(int channel, uint8_t volume);

// Set master volume (0-255)
void audio_mixer_set_master_volume(uint8_t volume);

// Get master volume
uint8_t audio_mixer_get_master_volume(void);

// Mix all active channels into a single output buffer.
// Called by the audio driver when it needs more samples.
// `out` must be at least `frames` bytes.  Each frame is one 8-bit sample.
void audio_mixer_mix(uint8_t* out, uint32_t frames);

// Check if any channel is playing
int audio_mixer_is_playing(void);

// Stop all channels
void audio_mixer_stop_all(void);

// Synthesize a simple sine-wave tone into a PCM buffer.
// Returns a kmalloc'd buffer; caller must free.
// `frequency` in Hz, `duration_ms` in milliseconds, `volume` 0-255.
uint8_t* audio_synth_sine(int frequency, int duration_ms, uint8_t volume, uint32_t* out_len);

#endif // AUDIO_MIXER_H
