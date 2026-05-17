// core/audio_mixer.c - Software Audio Mixer for CamelOS
// Provides multi-channel PCM audio mixing, volume control,
// and system sound event playback with sine-wave synthesis.

#include "audio_mixer.h"
#include "memory.h"
#include "string.h"
#include "../hal/drivers/serial.h"
#include "../hal/drivers/sb16.h"
#include "../sys/api.h"

// ============================================================================
// State
// ============================================================================
static audio_mixer_t mixer;

// ============================================================================
// Sine Wave Synthesis
// ============================================================================

// Simple fixed-point sine approximation using Taylor series (3 terms)
// Good enough for 8-bit audio — no FPU required.
static int32_t sine_approx(int32_t x) {
    // Normalize x to [0, 2*pi) in 16.16 fixed-point
    // 2*pi ≈ 411775 (16.16)
    const int32_t TWO_PI = 411775;
    while (x < 0) x += TWO_PI;
    while (x >= TWO_PI) x -= TWO_PI;

    // Taylor series: sin(x) ≈ x - x³/6 + x⁵/120
    // We work in 16.16 fixed-point
    // But for 8-bit audio we can use a simpler table-less approach
    // using a parabolic approximation:
    // sin(x) ≈ 4*x*(pi - x) / pi²  for x in [0, pi]
    
    const int32_t PI = 205887;  // pi in 16.16
    const int32_t PI2 = 102944; // pi/2 in 16.16

    int32_t sign = 1;
    if (x > PI) {
        x -= PI;
        sign = -1;
    }
    if (x > PI2) {
        x = PI - x;
    }
    
    // 4*x*(pi - x) / pi²  in 16.16 fixed-point
    // = (4 * x * (PI - x)) >> 16 / (PI*PI >> 16)
    int32_t term = (int32_t)(((long long)4 * x * (PI - x)) >> 16);
    int32_t denom = (int32_t)(((long long)PI * PI) >> 16);
    if (denom == 0) denom = 1;
    int32_t result = term / denom;
    
    return result * sign;
}

uint8_t* audio_synth_sine(int frequency, int duration_ms, uint8_t volume, uint32_t* out_len) {
    if (frequency <= 0 || duration_ms <= 0) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    
    uint32_t num_samples = (uint32_t)((long long)AUDIO_SAMPLE_RATE * duration_ms / 1000);
    if (num_samples == 0) num_samples = 1;
    if (num_samples > 65536) num_samples = 65536;  // Safety cap
    
    uint8_t* buf = (uint8_t*)kmalloc(num_samples);
    if (!buf) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    
    // Angular velocity per sample: 2*pi*freq / sample_rate
    // In 16.16 fixed-point
    int32_t step = (int32_t)(((long long)411775 * frequency) / AUDIO_SAMPLE_RATE);
    int32_t phase = 0;
    
    for (uint32_t i = 0; i < num_samples; i++) {
        int32_t s = sine_approx(phase);
        // s is in [-32768, 32767] approximately, scale to [0, 255]
        // Center at 128, scale by volume
        int32_t sample = 128 + ((s * (int32_t)volume) >> 8);
        if (sample < 0) sample = 0;
        if (sample > 255) sample = 255;
        buf[i] = (uint8_t)sample;
        phase += step;
    }
    
    if (out_len) *out_len = num_samples;
    return buf;
}

// Synthesize a short descending chime (two notes)
static uint8_t* audio_synth_chime(int freq1, int freq2, int note_dur_ms, 
                                   uint8_t volume, uint32_t* out_len) {
    uint32_t len1, len2;
    uint8_t* note1 = audio_synth_sine(freq1, note_dur_ms, volume, &len1);
    uint8_t* note2 = audio_synth_sine(freq2, note_dur_ms, volume, &len2);
    
    if (!note1 || !note2) {
        if (note1) kfree(note1);
        if (note2) kfree(note2);
        if (out_len) *out_len = 0;
        return NULL;
    }
    
    // Add a short gap between notes (20ms silence)
    uint32_t gap_samples = AUDIO_SAMPLE_RATE * 20 / 1000;
    uint32_t total = len1 + gap_samples + len2;
    uint8_t* buf = (uint8_t*)kmalloc(total);
    if (!buf) {
        kfree(note1);
        kfree(note2);
        if (out_len) *out_len = 0;
        return NULL;
    }
    
    memcpy(buf, note1, len1);
    memset(buf + len1, 128, gap_samples);  // Silence (center value)
    memcpy(buf + len1 + gap_samples, note2, len2);
    
    kfree(note1);
    kfree(note2);
    
    if (out_len) *out_len = total;
    return buf;
}

// Synthesize a short click/pop sound
static uint8_t* audio_synth_click(uint8_t volume, uint32_t* out_len) {
    // Very short sine burst at 1000Hz, 30ms
    return audio_synth_sine(1000, 30, volume, out_len);
}

// Synthesize error buzzer
static uint8_t* audio_synth_error(uint8_t volume, uint32_t* out_len) {
    // Low harsh tone: 200Hz for 200ms
    return audio_synth_sine(200, 200, volume, out_len);
}

// Synthesize volume change tick
static uint8_t* audio_synth_vol_tick(uint8_t volume, uint32_t* out_len) {
    // Very short high-pitched tick: 2000Hz, 15ms
    return audio_synth_sine(2000, 15, volume, out_len);
}

// ============================================================================
// Initialization
// ============================================================================

void audio_mixer_init(void) {
    memset(&mixer, 0, sizeof(mixer));
    mixer.master_volume = 200;  // ~78% by default
    mixer.initialized = 1;
    
    // Pre-synthesize system sounds
    uint8_t vol = 180;
    
    // Startup: ascending chime (A5 → C#6)
    mixer.system_sounds[SND_STARTUP] = 
        audio_synth_chime(880, 1100, 120, vol, &mixer.system_sound_len[SND_STARTUP]);
    
    // Notification: gentle two-tone (C5 → E5)
    mixer.system_sounds[SND_NOTIFICATION] = 
        audio_synth_chime(523, 659, 80, vol, &mixer.system_sound_len[SND_NOTIFICATION]);
    
    // Error: low buzzer
    mixer.system_sounds[SND_ERROR] = 
        audio_synth_error(vol, &mixer.system_sound_len[SND_ERROR]);
    
    // Click: short tick
    mixer.system_sounds[SND_CLICK] = 
        audio_synth_click(vol, &mixer.system_sound_len[SND_CLICK]);
    
    // Volume change: tiny tick
    mixer.system_sounds[SND_VOLUME_CHANGE] = 
        audio_synth_vol_tick(vol, &mixer.system_sound_len[SND_VOLUME_CHANGE]);
    
    s_printf("[AUDIO] Mixer initialized (%d channels, %dHz, master vol=%d)\n",
             AUDIO_MIXER_CHANNELS, AUDIO_SAMPLE_RATE, mixer.master_volume);
}

// ============================================================================
// Playback
// ============================================================================

int audio_play_system_sound(int sound_id) {
    if (sound_id < 0 || sound_id >= SND_COUNT) return -1;
    if (!mixer.system_sounds[sound_id]) return -1;
    
    // Find a free system channel (prefer channel 0-1 for system sounds)
    int ch = -1;
    for (int i = 0; i < 2 && i < AUDIO_MIXER_CHANNELS; i++) {
        if (!mixer.channels[i].playing) {
            ch = i;
            break;
        }
    }
    if (ch < 0) {
        // No free system channel, try any free channel
        for (int i = 2; i < AUDIO_MIXER_CHANNELS; i++) {
            if (!mixer.channels[i].playing) {
                ch = i;
                break;
            }
        }
    }
    if (ch < 0) {
        // Steal the oldest system channel
        ch = 0;
    }
    
    // Stop current playback on this channel
    mixer.channels[ch].pcm_data = mixer.system_sounds[sound_id];
    mixer.channels[ch].pcm_length = mixer.system_sound_len[sound_id];
    mixer.channels[ch].position = 0;
    mixer.channels[ch].playing = 1;
    mixer.channels[ch].looping = 0;
    mixer.channels[ch].type = AUDIO_CHAN_SYSTEM;
    mixer.channels[ch].volume = 255;
    
    // Play via SB16 if available
    sb16_play_direct(mixer.system_sounds[sound_id], mixer.system_sound_len[sound_id]);
    
    return ch;
}

int audio_play_pcm(const uint8_t* data, uint32_t length, int type, int looping) {
    if (!data || length == 0) return -1;
    
    // Find a free channel
    int ch = -1;
    for (int i = 0; i < AUDIO_MIXER_CHANNELS; i++) {
        if (!mixer.channels[i].playing) {
            ch = i;
            break;
        }
    }
    if (ch < 0) return -1;  // All channels busy
    
    mixer.channels[ch].pcm_data = (uint8_t*)data;
    mixer.channels[ch].pcm_length = length;
    mixer.channels[ch].position = 0;
    mixer.channels[ch].playing = 1;
    mixer.channels[ch].looping = looping;
    mixer.channels[ch].type = type;
    mixer.channels[ch].volume = 255;
    
    return ch;
}

void audio_channel_stop(int channel) {
    if (channel < 0 || channel >= AUDIO_MIXER_CHANNELS) return;
    mixer.channels[channel].playing = 0;
    mixer.channels[channel].position = 0;
}

void audio_channel_set_volume(int channel, uint8_t volume) {
    if (channel < 0 || channel >= AUDIO_MIXER_CHANNELS) return;
    mixer.channels[channel].volume = volume;
}

void audio_mixer_set_master_volume(uint8_t volume) {
    mixer.master_volume = volume;
}

uint8_t audio_mixer_get_master_volume(void) {
    return mixer.master_volume;
}

// ============================================================================
// Mixing
// ============================================================================

void audio_mixer_mix(uint8_t* out, uint32_t frames) {
    if (!out || frames == 0) return;
    
    // Start with silence (128 = center for unsigned 8-bit)
    memset(out, 128, frames);
    
    int has_audio = 0;
    
    for (int ch = 0; ch < AUDIO_MIXER_CHANNELS; ch++) {
        audio_channel_t* c = &mixer.channels[ch];
        if (!c->playing || !c->pcm_data) continue;
        
        has_audio = 1;
        
        // Compute effective volume: channel * master / 255
        uint32_t eff_vol = ((uint32_t)c->volume * mixer.master_volume) / 255;
        
        for (uint32_t i = 0; i < frames; i++) {
            if (c->position >= c->pcm_length) {
                if (c->looping) {
                    c->position = 0;
                } else {
                    c->playing = 0;
                    break;
                }
            }
            
            // Mix: convert from unsigned to signed, apply volume, add, convert back
            int32_t existing = (int32_t)out[i] - 128;  // Signed
            int32_t sample = (int32_t)c->pcm_data[c->position] - 128;  // Signed
            sample = (sample * (int32_t)eff_vol) / 255;  // Apply volume
            
            // Simple additive mixing with soft clipping
            int32_t mixed = existing + sample;
            if (mixed > 127) mixed = 127;
            if (mixed < -128) mixed = -128;
            
            out[i] = (uint8_t)(mixed + 128);
            c->position++;
        }
    }
    
    // If no audio was mixed, output silence
    if (!has_audio) {
        memset(out, 128, frames);
    }
}

int audio_mixer_is_playing(void) {
    for (int i = 0; i < AUDIO_MIXER_CHANNELS; i++) {
        if (mixer.channels[i].playing) return 1;
    }
    return 0;
}

void audio_mixer_stop_all(void) {
    for (int i = 0; i < AUDIO_MIXER_CHANNELS; i++) {
        mixer.channels[i].playing = 0;
        mixer.channels[i].position = 0;
    }
}
