#ifndef SOUND_H
#define SOUND_H

void beep(int duration_ms);

void play_startup_chime(); // New function

// System sound playback via the audio mixer
// Sound IDs: SND_STARTUP, SND_NOTIFICATION, SND_ERROR, SND_CLICK, SND_VOLUME_CHANGE
int audio_play_system_sound(int sound_id);

// System sound IDs
#define SND_STARTUP         0
#define SND_NOTIFICATION    1
#define SND_ERROR           2
#define SND_CLICK           3
#define SND_VOLUME_CHANGE   4

#endif
