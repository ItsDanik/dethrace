#ifndef MISTER_FPGA_H
#define MISTER_FPGA_H

#include "brender.h"

// Bridge to the Dethrace FPGA core, on the hybrid core library of
// Hybrid_MiSTer (hybrid/hps/mister_hybrid.h, which the game links): video,
// palette, input and the field counter here; audio is MH_AudioWrite()
// (mister_audio.c), messages and the game list MH_UI_Message() and
// MH_PickGame() (mister.c).

typedef struct tMiSTer_input {
    br_uint_32 frame;          // FPGA field counter, increments every vblank
    br_uint_32 joystick[2];    // MiSTer joystick bits: 0 R, 1 L, 2 D, 3 U, 4.. buttons
    br_int_8 analog_x[2];      // left stick
    br_int_8 analog_y[2];
    br_uint_64 osd_status;     // OSD option bits [63:0]
    br_int_32 mouse_x;         // accumulated PS/2 mouse movement (y positive up)
    br_int_32 mouse_y;
    int mouse_buttons;         // bit 0 left, bit 1 right, bit 2 middle
    br_uint_32 keys[16];       // PS/2 set 2 key bitmap, bit index = extended << 8 | code
} tMiSTer_input;

// OSD options (CONF_STR in core/Dethrace.sv). The first entry of each option
// is 0, the default: volumes are listed 100%, 90%, .. 0%.
#define MISTER_OSD_SOUND_VOLUME(s) ((int)(((s) >> 7) & 0xF))   // 0 = 100%, 10 = 0%
#define MISTER_OSD_MUSIC_VOLUME(s) ((int)(((s) >> 11) & 0xF))  // 0 = 100%, 10 = 0%
// 0 MiSTer menu OK/Back, 1.. A, B, X, Y, L, R, Select, Start (MH_MenuButtons())
#define MISTER_OSD_MENU_OK(s) ((int)(((s) >> 16) & 0xF))
#define MISTER_OSD_MENU_BACK(s) ((int)(((s) >> 20) & 0xF))
#define MISTER_OSD_RENDERER(s) ((int)(((s) >> 24) & 3))   // 0 Optimized, 1 Fast, 2 Original
#define MISTER_OSD_LOCK_30FPS(s) ((int)(((s) >> 26) & 1))
#define MISTER_OSD_RES_240(s) ((int)(((s) >> 27) & 1))       // 0 320x200, 1 320x240
#define MISTER_OSD_NO_CUTSCENES(s) ((int)(((s) >> 28) & 1))  // 0 On, 1 Off
// Cheats page: Invulnerability (0 Off, 1 On); the triggers T[56] Instant
// Repair, T[57] Extra Credits, T[58] Finish Lap are MH_OSDTriggers() bits 0..2
#define MISTER_OSD_INVULNERABLE(s) ((int)(((s) >> 29) & 1))
#define MISTER_OSD_NO_RUMBLE(s) ((int)(((s) >> 30) & 1))      // 0 On, 1 Off

// Set at startup, before the game thread pins itself to CPU0: whether the
// launcher allowed CPU1, where helper threads (audio, frame copy) then run
extern int gMiSTer_cpu1_allowed;

// Returns 1 if the Dethrace core is loaded and running
int MiSTer_FPGA_Open(void);
void MiSTer_FPGA_Close(void);
int MiSTer_FPGA_IsOpen(void);
// Returns 0 (and detaches) once the Dethrace core is no longer loaded
int MiSTer_FPGA_CheckAlive(void);

// 320x200 or 320x240 (both 15kHz progressive). The mode can be changed while
// the game runs; the screen is black until the next frame then
void MiSTer_FPGA_SetMode(int width, int height);
// 1 if the core loaded has 320x240 (hybrid_host version 6)
int MiSTer_FPGA_Has240(void);
void MiSTer_FPGA_Present(br_pixelmap* src);
void MiSTer_FPGA_SetPalette(br_colour* entries);
void MiSTer_FPGA_ReadInput(tMiSTer_input* input);
// The core's field counter (59.6Hz), increments every vblank
br_uint_32 MiSTer_FPGA_FieldCounter(void);

#endif
