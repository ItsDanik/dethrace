// Input mapping for the MiSTer platform: PS/2 keyboard (set 2 codes from the
// FPGA core) and MiSTer joysticks to the PC scancodes the game expects.

#include "mister_input.h"
#include "mister_joymap.h"
#include "../dethrace_scancodes.h"

#include <string.h>

// Game state and key configuration (DETHRACE code)
extern int PDIsRacing(void);
extern int gKey_mapping[67];
extern br_uint_8 gScan_code[123][2];

// keymap slots used by the joystick mapping (see KEYMAP_* in DETHRACE/constants.h)
#define SLOT_ESCAPE 0
#define SLOT_REPAIR 44
#define SLOT_COCKPIT 45
#define SLOT_LEFT 46
#define SLOT_RIGHT 47
#define SLOT_ACCELERATE 48
#define SLOT_BRAKE 49
#define SLOT_HANDBRAKE 53
#define SLOT_MAP 56
#define SLOT_RECOVERY 59

// MiSTer joystick bits, buttons as named in the core's "J1," config string
#define JOY_RIGHT (1 << 0)
#define JOY_LEFT (1 << 1)
#define JOY_DOWN (1 << 2)
#define JOY_UP (1 << 3)
#define JOY_ACCELERATE (1 << 4)
#define JOY_BRAKE (1 << 5)
#define JOY_HANDBRAKE (1 << 6)
#define JOY_VIEW (1 << 7)
#define JOY_REPAIR (1 << 8)
#define JOY_RECOVER (1 << 9)
#define JOY_MAP (1 << 10)
#define JOY_PAUSE (1 << 11)
#define JOY_MENU_OK (1 << 12)
#define JOY_MENU_BACK (1 << 13)

#define ANALOG_THRESHOLD 48

// PS/2 set 2 -> set 1 (PC/DirectInput) scancodes
static const br_uint_8 set2_normal[0x84] = {
    [0x76] = SCANCODE_ESCAPE,
    [0x16] = SCANCODE_1,
    [0x1E] = SCANCODE_2,
    [0x26] = SCANCODE_3,
    [0x25] = SCANCODE_4,
    [0x2E] = SCANCODE_5,
    [0x36] = SCANCODE_6,
    [0x3D] = SCANCODE_7,
    [0x3E] = SCANCODE_8,
    [0x46] = SCANCODE_9,
    [0x45] = SCANCODE_0,
    [0x4E] = SCANCODE_MINUS,
    [0x55] = SCANCODE_EQUALS,
    [0x66] = SCANCODE_BACK,
    [0x0D] = SCANCODE_TAB,
    [0x15] = SCANCODE_Q,
    [0x1D] = SCANCODE_W,
    [0x24] = SCANCODE_E,
    [0x2D] = SCANCODE_R,
    [0x2C] = SCANCODE_T,
    [0x35] = SCANCODE_Y,
    [0x3C] = SCANCODE_U,
    [0x43] = SCANCODE_I,
    [0x44] = SCANCODE_O,
    [0x4D] = SCANCODE_P,
    [0x54] = SCANCODE_LBRACKET,
    [0x5B] = SCANCODE_RBRACKET,
    [0x5A] = SCANCODE_RETURN,
    [0x14] = SCANCODE_LCONTROL,
    [0x1C] = SCANCODE_A,
    [0x1B] = SCANCODE_S,
    [0x23] = SCANCODE_D,
    [0x2B] = SCANCODE_F,
    [0x34] = SCANCODE_G,
    [0x33] = SCANCODE_H,
    [0x3B] = SCANCODE_J,
    [0x42] = SCANCODE_K,
    [0x4B] = SCANCODE_L,
    [0x4C] = SCANCODE_SEMICOLON,
    [0x52] = SCANCODE_APOSTROPHE,
    [0x0E] = SCANCODE_GRAVE,
    [0x12] = SCANCODE_LSHIFT,
    [0x5D] = SCANCODE_BACKSLASH,
    [0x1A] = SCANCODE_Z,
    [0x22] = SCANCODE_X,
    [0x21] = SCANCODE_C,
    [0x2A] = SCANCODE_V,
    [0x32] = SCANCODE_B,
    [0x31] = SCANCODE_N,
    [0x3A] = SCANCODE_M,
    [0x41] = SCANCODE_COMMA,
    [0x49] = SCANCODE_PERIOD,
    [0x4A] = SCANCODE_SLASH,
    [0x59] = SCANCODE_RSHIFT,
    [0x7C] = SCANCODE_MULTIPLY,
    [0x11] = SCANCODE_LALT,
    [0x29] = SCANCODE_SPACE,
    [0x58] = SCANCODE_CAPITAL,
    [0x05] = SCANCODE_F1,
    [0x06] = SCANCODE_F2,
    [0x04] = SCANCODE_F3,
    [0x0C] = SCANCODE_F4,
    [0x03] = SCANCODE_F5,
    [0x0B] = SCANCODE_F6,
    [0x83] = SCANCODE_F7,
    [0x0A] = SCANCODE_F8,
    [0x01] = SCANCODE_F9,
    [0x09] = SCANCODE_F10,
    [0x78] = SCANCODE_F11,
    [0x07] = SCANCODE_F12,
    [0x77] = SCANCODE_NUMLOCK,
    [0x7E] = SCANCODE_SCROLL,
    [0x6C] = SCANCODE_NUMPAD7,
    [0x75] = SCANCODE_NUMPAD8,
    [0x7D] = SCANCODE_NUMPAD9,
    [0x7B] = SCANCODE_SUBTRACT,
    [0x6B] = SCANCODE_NUMPAD4,
    [0x73] = SCANCODE_NUMPAD5,
    [0x74] = SCANCODE_NUMPAD6,
    [0x79] = SCANCODE_ADD,
    [0x69] = SCANCODE_NUMPAD1,
    [0x72] = SCANCODE_NUMPAD2,
    [0x7A] = SCANCODE_NUMPAD3,
    [0x70] = SCANCODE_NUMPAD0,
    [0x71] = SCANCODE_DECIMAL,
    [0x61] = SCANCODE_OEM_102,
};

// E0-prefixed PS/2 set 2 codes
static const br_uint_8 set2_extended[0x80] = {
    [0x5A] = SCANCODE_NUMPADENTER,
    [0x14] = SCANCODE_RCONTROL,
    [0x4A] = SCANCODE_DIVIDE,
    [0x11] = SCANCODE_RALT,
    [0x6C] = SCANCODE_HOME,
    [0x75] = SCANCODE_UP,
    [0x7D] = SCANCODE_PGUP,
    [0x6B] = SCANCODE_LEFT,
    [0x74] = SCANCODE_RIGHT,
    [0x69] = SCANCODE_END,
    [0x72] = SCANCODE_DOWN,
    [0x7A] = SCANCODE_PGDN,
    [0x70] = SCANCODE_INSERT,
    [0x71] = SCANCODE_DELETE,
};

static void press(br_uint_32 key_state[8], int scancode) {
    if (scancode > 0 && scancode < 256) {
        key_state[scancode >> 5] |= 1u << (scancode & 0x1f);
    }
}

// Press the key currently bound to a keymap slot
static void press_slot(br_uint_32 key_state[8], int slot) {
    int key = gKey_mapping[slot];
    if (key >= 0 && key < 123) {
        press(key_state, gScan_code[key][0]);
    }
}

void MiSTer_Input_Map(const tMiSTer_input* input, br_uint_32 key_state[8]) {
    br_uint_32 joy;
    int code;
    int i;

    // keyboard
    for (i = 0; i < 512; i++) {
        if (input->keys[i >> 5] & (1u << (i & 0x1f))) {
            code = i & 0xff;
            if (i & 0x100) {
                press(key_state, code < 0x80 ? set2_extended[code] : 0);
            } else {
                press(key_state, code < 0x84 ? set2_normal[code] : 0);
            }
        }
    }

    // joystick 1, analog stick doubles as the d-pad. Default buttons (core "jn"):
    // Accelerate B, Brake Y, Handbrake A, Change View X, Repair L, Map R,
    // Recover Select, Pause Start; Menu OK/Back unmapped (spare buttons)
    joy = input->joystick[0];
    if (input->analog_x[0] < -ANALOG_THRESHOLD) {
        joy |= JOY_LEFT;
    } else if (input->analog_x[0] > ANALOG_THRESHOLD) {
        joy |= JOY_RIGHT;
    }
    if (input->analog_y[0] < -ANALOG_THRESHOLD) {
        joy |= JOY_UP;
    } else if (input->analog_y[0] > ANALOG_THRESHOLD) {
        joy |= JOY_DOWN;
    }
    if (joy == 0) {
        return;
    }

    if (PDIsRacing()) {
        if (joy & JOY_LEFT) {
            press_slot(key_state, SLOT_LEFT);
        }
        if (joy & JOY_RIGHT) {
            press_slot(key_state, SLOT_RIGHT);
        }
        if (joy & JOY_ACCELERATE) {
            press_slot(key_state, SLOT_ACCELERATE);
        }
        if (joy & JOY_BRAKE) {
            press_slot(key_state, SLOT_BRAKE);
        }
        if (joy & JOY_HANDBRAKE) {
            press_slot(key_state, SLOT_HANDBRAKE);
        }
        if (joy & JOY_VIEW) {
            press_slot(key_state, SLOT_COCKPIT);
        }
        if (joy & JOY_REPAIR) {
            press_slot(key_state, SLOT_REPAIR);
        }
        if (joy & JOY_RECOVER) {
            press_slot(key_state, SLOT_RECOVERY);
        }
        if (joy & JOY_MAP) {
            press_slot(key_state, SLOT_MAP);
        }
        if (joy & JOY_PAUSE) {
            press_slot(key_state, SLOT_ESCAPE);
        }
    } else {
        // menus: d-pad = cursor keys, Menu OK = Enter, Menu Back = Escape. The
        // core's Menu OK/Back buttons are for buttons without a race function;
        // the OSD names a button that has one, see mister_joymap.h
        br_uint_32 ok_bits, back_bits;
        MiSTer_JoyMap_MenuButtons(MISTER_OSD_MENU_OK(input->osd_status), MISTER_OSD_MENU_BACK(input->osd_status), &ok_bits, &back_bits);
        if (joy & JOY_LEFT) {
            press(key_state, SCANCODE_LEFT);
        }
        if (joy & JOY_RIGHT) {
            press(key_state, SCANCODE_RIGHT);
        }
        if (joy & JOY_UP) {
            press(key_state, SCANCODE_UP);
        }
        if (joy & JOY_DOWN) {
            press(key_state, SCANCODE_DOWN);
        }
        if (joy & (JOY_MENU_OK | ok_bits)) {
            press(key_state, SCANCODE_RETURN);
        }
        if (joy & (JOY_MENU_BACK | back_bits)) {
            press(key_state, SCANCODE_ESCAPE);
        }
    }
}
