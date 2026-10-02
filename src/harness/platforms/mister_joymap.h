#ifndef MISTER_JOYMAP_H
#define MISTER_JOYMAP_H

#include "brender.h"

// Which core joystick buttons a physical gamepad button is mapped to.
//
// Main_MiSTer refuses to map one physical button to two core buttons, so the
// core's own Menu OK/Back buttons can only use buttons without a race
// function. For buttons that do have one, the OSD names the button (A, B, X,
// Y, L, R, Select, Start, or "MiSTer" for the MiSTer menu's OK/Back) and this
// module finds the core buttons it drives by reading Main_MiSTer's mapping
// files: the controller's menu mapping (input_<vid>_<pid>_v3.map) and the
// Dethrace mapping (Dethrace_input_<vid>_<pid>_v3.map, or the core's default
// "jn" mapping when there is none).

// OSD selection: 0 = MiSTer menu OK/Back, 1.. = A, B, X, Y, L, R, Select, Start
// Returns the core joystick bits (race buttons only) of the selected buttons.
// Cheap to call every frame; mapping files are re-read every few seconds.
void MiSTer_JoyMap_MenuButtons(int ok_select, int back_select, br_uint_32* ok_bits, br_uint_32* back_bits);

#endif
