// Resolves physical gamepad buttons to Dethrace core buttons through
// Main_MiSTer's mapping files, see mister_joymap.h.

#include "mister_joymap.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef CONFIG_DIR
#define CONFIG_DIR "/media/fat/config/"
#endif
#define INPUTS_DIR CONFIG_DIR "inputs/"
#define CORE_NAME "Dethrace"
#define RESCAN_SECONDS 2

// Main_MiSTer map layout (input.h): uint32 per entry, low 16 bits = Linux input
// code, high 16 bits = alternative button
#define MAP_ENTRIES 32
#define SYS_BTN_A 4 // A, B, X, Y, L, R, Select, Start follow
#define SYS_BTN_B 5
#define SYS_BTN_MENU_FUNC 23 // low 16 bits = menu OK, high = menu Back
#define NUM_NAMED 8

// core buttons: bit 4.. in the order of the core's "J1," list
#define CORE_FIRST 4
#define CORE_RACE_LAST 11 // Accelerate .. Pause; Menu OK/Back have no default
#define CORE_PRATCAM 14   // after Menu OK/Back, no default either

// Default mapping of the core buttons to named buttons (index into A, B, X, Y,
// L, R, Select, Start), must match "jn," in core/Dethrace.sv
static const int core_default[CORE_RACE_LAST - CORE_FIRST + 1] = {
    1, // Accelerate  B
    3, // Brake       Y
    0, // Handbrake   A
    2, // Change View X
    4, // Repair      L
    6, // Recover     Select
    5, // Map         R
    7, // Pause       Start
};

// Linux gamepad codes of A, B, X, Y, L, R, Select, Start (MiSTer's names are
// SNES positions: A east, B south, X north, Y west), used when a controller
// has no menu mapping file
static const br_uint_16 standard_codes[NUM_NAMED] = { 0x131, 0x130, 0x133, 0x134, 0x136, 0x137, 0x13a, 0x13b };

#define MAX_DEVICES 8

typedef struct {
    unsigned vid, pid;
} tDevice;

static int last_ok = -1, last_back = -1;
static time_t last_scan;
static br_uint_32 ok_result, back_result;

// Connected joysticks from /proc/bus/input/devices (entries with a js handler)
static int list_joysticks(tDevice* devices, int max) {
    FILE* f = fopen("/proc/bus/input/devices", "r");
    char line[512];
    unsigned vid = 0, pid = 0;
    int count = 0;
    int i, dup;

    if (f == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "I:", 2) == 0) {
            char* v = strstr(line, "Vendor=");
            char* p = strstr(line, "Product=");
            vid = v ? (unsigned)strtoul(v + 7, NULL, 16) : 0;
            pid = p ? (unsigned)strtoul(p + 8, NULL, 16) : 0;
        } else if (strncmp(line, "H:", 2) == 0 && (strstr(line, "=js") != NULL || strstr(line, " js") != NULL) && count < max) {
            dup = 0;
            for (i = 0; i < count; i++) {
                dup |= devices[i].vid == vid && devices[i].pid == pid;
            }
            if (!dup) {
                devices[count].vid = vid;
                devices[count].pid = pid;
                count++;
            }
        }
    }
    fclose(f);
    return count;
}

static int read_map(const char* path, br_uint_32 map[MAP_ENTRIES]) {
    FILE* f = fopen(path, "rb");
    size_t n;

    if (f == NULL) {
        return 0;
    }
    memset(map, 0, MAP_ENTRIES * sizeof(br_uint_32));
    n = fread(map, 1, MAP_ENTRIES * sizeof(br_uint_32), f);
    fclose(f);
    return n >= (SYS_BTN_MENU_FUNC + 1) * sizeof(br_uint_32);
}

// Loads <prefix>input_<vid>_<pid>[_<variant>]_v3.map like Main_MiSTer: from
// config/inputs/ or, for old setups, config/
static int load_map(const char* prefix, const tDevice* dev, br_uint_32 map[MAP_ENTRIES]) {
    char name[128], path[384];
    size_t len;
    DIR* dir;
    struct dirent* ent;
    int found = 0;

    snprintf(name, sizeof(name), "%sinput_%04x_%04x_v3.map", prefix, dev->vid, dev->pid);
    snprintf(path, sizeof(path), INPUTS_DIR "%s", name);
    if (read_map(path, map)) {
        return 1;
    }
    snprintf(path, sizeof(path), CONFIG_DIR "%s", name);
    if (read_map(path, map)) {
        return 1;
    }
    // unique mapping or name hash variants: <id>_<suffix>_v3.map, but not the
    // "_m" modifier maps
    snprintf(name, sizeof(name), "%sinput_%04x_%04x_", prefix, dev->vid, dev->pid);
    len = strlen(name);
    dir = opendir(INPUTS_DIR);
    if (dir == NULL) {
        return 0;
    }
    while (!found && (ent = readdir(dir)) != NULL) {
        size_t n = strlen(ent->d_name);
        if (strncmp(ent->d_name, name, len) == 0 && n > len + 7 && strcmp(ent->d_name + n - 7, "_v3.map") == 0
            && !(n >= 9 && strcmp(ent->d_name + n - 9, "_m_v3.map") == 0)) {
            snprintf(path, sizeof(path), INPUTS_DIR "%.255s", ent->d_name);
            found = read_map(path, map);
        }
    }
    closedir(dir);
    return found;
}

// Race buttons driven by the physical button with Linux code `code`
static br_uint_32 core_bits_for_code(const br_uint_32 core[MAP_ENTRIES], br_uint_32 code) {
    br_uint_32 bits = 0;
    int i;

    if (code == 0) {
        return 0;
    }
    for (i = CORE_FIRST; i <= CORE_PRATCAM; i++) {
        if ((i <= CORE_RACE_LAST || i == CORE_PRATCAM) && ((core[i] & 0xFFFF) == code || (core[i] >> 16) == code)) {
            bits |= 1u << i;
        }
    }
    return bits;
}

static br_uint_32 selected_code(const br_uint_32 menu[MAP_ENTRIES], int select, int back) {
    br_uint_32 func;

    if (select > 0 && select <= NUM_NAMED) {
        return menu[SYS_BTN_A + select - 1] & 0xFFFF;
    }
    // MiSTer: the menu's OK/Back buttons, A/B when not set (as Main_MiSTer does)
    func = back ? menu[SYS_BTN_MENU_FUNC] >> 16 : menu[SYS_BTN_MENU_FUNC] & 0xFFFF;
    return func ? func : menu[back ? SYS_BTN_B : SYS_BTN_A] & 0xFFFF;
}

static void resolve(int ok_select, int back_select) {
    tDevice devices[MAX_DEVICES];
    br_uint_32 menu[MAX_DEVICES][MAP_ENTRIES];
    br_uint_32 core[MAX_DEVICES][MAP_ENTRIES];
    int score[MAX_DEVICES];
    int count, best = 0;
    int i, k;

    ok_result = 0;
    back_result = 0;
    count = list_joysticks(devices, MAX_DEVICES);
    for (i = 0; i < count; i++) {
        int has_menu = load_map("", &devices[i], menu[i]);
        int has_core = load_map(CORE_NAME "_", &devices[i], core[i]);

        if (!has_menu) {
            memset(menu[i], 0, sizeof(menu[i]));
            for (k = 0; k < NUM_NAMED; k++) {
                menu[i][SYS_BTN_A + k] = standard_codes[k];
            }
        }
        if (!has_core) {
            // what Main_MiSTer derives from the core's default ("jn") mapping
            memset(core[i], 0, sizeof(core[i]));
            for (k = CORE_FIRST; k <= CORE_RACE_LAST; k++) {
                core[i][k] = menu[i][SYS_BTN_A + core_default[k - CORE_FIRST]];
            }
        }
        // Controllers set up for Dethrace or the menu are the ones in use,
        // not e.g. virtual joysticks of other tools
        score[i] = has_core * 2 + has_menu;
        if (score[i] > best) {
            best = score[i];
        }
    }
    for (i = 0; i < count; i++) {
        if (score[i] == best) {
            ok_result |= core_bits_for_code(core[i], selected_code(menu[i], ok_select, 0));
            back_result |= core_bits_for_code(core[i], selected_code(menu[i], back_select, 1));
        }
    }
}

void MiSTer_JoyMap_MenuButtons(int ok_select, int back_select, br_uint_32* ok_bits, br_uint_32* back_bits) {
    time_t now = time(NULL);

    if (ok_select != last_ok || back_select != last_back || now - last_scan >= RESCAN_SECONDS) {
        if (ok_select != last_ok || back_select != last_back) {
            printf("mister: menu buttons OK %d, Back %d\n", ok_select, back_select);
        }
        last_ok = ok_select;
        last_back = back_select;
        last_scan = now;
        resolve(ok_select, back_select);
    }
    *ok_bits = ok_result;
    *back_bits = back_result;
}
