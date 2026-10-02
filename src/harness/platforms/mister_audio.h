#ifndef MISTER_AUDIO_H
#define MISTER_AUDIO_H

// Audio output through the Dethrace FPGA core: a thread pulls mixed audio from
// the render callback and keeps the core's DDR ring buffer filled.

#define MISTER_AUDIO_RATE 44100
#define MISTER_AUDIO_CHANNELS 2

// Fills `frames` interleaved stereo 16-bit frames
typedef void (*tMiSTer_audio_render)(short* out, int frames);

// Returns 1 if audio can go to the FPGA core
int MiSTer_Audio_Available(void);
int MiSTer_Audio_Start(tMiSTer_audio_render render);
void MiSTer_Audio_Stop(void);

// Master volumes from the OSD, 0.0 - 1.0 linear gain, applied by the render callback
void MiSTer_Audio_SetVolumes(float sound, float music);
float MiSTer_Audio_SoundVolume(void);
float MiSTer_Audio_MusicVolume(void);

#endif
