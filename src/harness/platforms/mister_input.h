#ifndef MISTER_INPUT_H
#define MISTER_INPUT_H

#include "mister_fpga.h"

// OR the keys held on the MiSTer keyboard/joystick into a 256-bit scancode bitmap
void MiSTer_Input_Map(const tMiSTer_input* input, br_uint_32 key_state[8]);

#endif
