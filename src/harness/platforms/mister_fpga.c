// Bridge to the Dethrace FPGA core: the hybrid core library of Hybrid_MiSTer
// (hybrid/hps/mister_hybrid.h), which owns the shared memory, the present
// thread on CPU1, the palette slots and the audio ring. This file keeps the
// interface the rest of the MiSTer platform uses (mister_fpga.h).

#define _GNU_SOURCE
#include "mister_fpga.h"

#include <mister_hybrid.h>
#include <stdio.h>
#include <stdlib.h>

int gMiSTer_cpu1_allowed;

int MiSTer_FPGA_Open(void) {
    if (!MH_Open()) {
        fprintf(stderr, "mister: Dethrace core not running, running headless\n");
        return 0;
    }
    return 1;
}

void MiSTer_FPGA_Close(void) {
    MH_Close();
}

int MiSTer_FPGA_IsOpen(void) {
    return MH_IsOpen();
}

int MiSTer_FPGA_Has240(void) {
    return MH_IsOpen() && MH_ModeAvailable(MH_MODE_320x240);
}

void MiSTer_FPGA_SetMode(int width, int height) {
    static int format_set;

    if (!MH_IsOpen()) {
        return;
    }
    if (width != 320 || (height != 200 && height != 240) || (height == 240 && !MiSTer_FPGA_Has240())) {
        fprintf(stderr, "mister: unsupported resolution %dx%d\n", width, height);
        abort();
    }
    if (!format_set) {
        MH_SetFormat(MH_FORMAT_INDEX8);
        format_set = 1;
    }
    MH_SetMode(height == 240 ? MH_MODE_320x240 : MH_MODE_320x200);
}

void MiSTer_FPGA_Present(br_pixelmap* src) {
    MH_Present(src->pixels, src->row_bytes);
}

br_uint_32 MiSTer_FPGA_FieldCounter(void) {
    return MH_FieldCounter();
}

void MiSTer_FPGA_SetPalette(br_colour* entries) {
    uint32_t rgb[256];
    int i;

    for (i = 0; i < 256; i++) {
        rgb[i] = entries[i] & 0xffffff;
    }
    MH_SetPalette(rgb);
}

int MiSTer_FPGA_CheckAlive(void) {
    return MH_CheckAlive();
}

void MiSTer_FPGA_ReadInput(tMiSTer_input* input) {
    mh_input in;
    int i;

    MH_ReadInput(&in);
    input->frame = in.frame;
    for (i = 0; i < 2; i++) {
        input->joystick[i] = in.joystick[i];
        input->analog_x[i] = in.analog_lx[i];
        input->analog_y[i] = in.analog_ly[i];
    }
    input->osd_status = in.osd_status;
    input->mouse_x = in.mouse_x;
    input->mouse_y = in.mouse_y;
    input->mouse_buttons = in.mouse_buttons;
    for (i = 0; i < 16; i++) {
        input->keys[i] = in.keys[i];
    }
}
