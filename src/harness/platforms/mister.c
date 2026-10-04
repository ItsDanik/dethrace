// MiSTer FPGA platform (HPS side of the Dethrace hybrid core).
//
// Frames and palettes go to the Dethrace FPGA core through shared memory
// (mister_fpga.c), input comes back the same way (mister_input.c). Without the
// core the driver runs headless, which together with the scripted input,
// frame statistics, screenshots and SIGPROF sampling profiler below is used
// for benchmarking.
//
// Environment variables:
//   DETHRACE_MISTER_HEADLESS  do not use the FPGA core even if it is loaded
//   DETHRACE_MISTER_SCRIPT    input/control script (see load_script)
//   DETHRACE_MISTER_OUT       directory for screenshots/profile/stats (default ".")
//   DETHRACE_MISTER_SHOTS     take a screenshot every N ms (0 = off)
//   DETHRACE_MISTER_QUIT_TO_MENU  load the MiSTer menu core when the player quits
//   DETHRACE_MISTER_FIXED_STEP  benchmark: game time advances by this many ms per
//                             frame instead of following the clock, and the random
//                             seed is fixed, so every run renders the same frames.
//                             Sound is off unless DETHRACE_MISTER_FIXED_SOUND is set

#define _GNU_SOURCE
#include "harness.h"
#include "harness/config.h"
#include "harness/hooks.h"
#include "harness/trace.h"
#include "mister_audio.h"
#include "mister_fpga.h"
#include "mister_input.h"

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <ucontext.h>
#include <sys/syscall.h>
#include <unistd.h>

// Callbacks back into original game code
extern void QuitGame(void);
extern int gGraf_spec_index;
extern int gSound_override;

// BRender pentprim (drivers/pentprim/verify.h): 1 = original rasteriser loops
extern int gPentprim_reference;
// 1 = perspective texture mapping by subdivision, not bit-identical
extern int gPentprim_fast;

static void (*gKeyHandler_func)(void);

// 32 bytes, 1 bit per key. Matches dos executable behavior
static br_uint_32 key_state[8];
// keys held by the input script
static br_uint_32 script_keys[8];

static int screen_width = 320;
static int screen_height = 200;
static int mouse_x, mouse_y;
static int mouse_buttons;
static br_int_32 last_mouse_acc_x, last_mouse_acc_y;
static int mouse_initialized;

// set when the game shuts down normally (not on a signal or core switch)
static int game_quit;


static br_uint_64 present_us;
static int present_count;

static br_uint_8 palette_rgb[256][3];
static br_pixelmap* last_screen_src;

static struct timespec start_time;
static struct timespec start_time_coarse;

static const char* out_dir = ".";
static br_uint_32 shot_interval_ms;
static br_uint_32 next_shot_time;
static int shot_requested;
static int shot_count;

// DETHRACE_MISTER_FIXED_STEP: game time per frame and the virtual clock
static br_uint_32 fixed_step_us;
static br_uint_64 fixed_time_us;
static br_uint_32 fixed_reads;

static br_uint_32 mister_get_ticks(void) {
    struct timespec now;
    if (fixed_step_us != 0) {
        // The clock stands still within a frame, so the frames do not depend on
        // how often it is read (sound on or off). It only creeps forward once a
        // loop is clearly waiting for it.
        if (++fixed_reads > 2000) {
            fixed_time_us += 20;
        }
        return fixed_time_us / 1000;
    }
    // The game asks for the time very often. The Cortex-A9 has no user readable
    // timer, so CLOCK_MONOTONIC is a syscall; the coarse clock (1ms with HZ=1000)
    // is answered from the vDSO.
    clock_gettime(CLOCK_MONOTONIC_COARSE, &now);
    return (now.tv_sec - start_time_coarse.tv_sec) * 1000 + (now.tv_nsec - start_time_coarse.tv_nsec) / 1000000;
}

static br_uint_64 mister_get_micros(void) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (br_uint_64)(now.tv_sec - start_time.tv_sec) * 1000000 + (now.tv_nsec - start_time.tv_nsec) / 1000;
}

// Sleep until an absolute time, resuming after signals (the profiler's
// 1kHz SIGPROF would otherwise cut every sleep short)
static void sleep_us(br_uint_64 microseconds) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += microseconds / 1000000;
    deadline.tv_nsec += (microseconds % 1000000) * 1000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL) == EINTR) {
    }
}

static void mister_sleep(br_uint_32 milliseconds) {
    if (fixed_step_us != 0) {
        fixed_time_us += (br_uint_64)milliseconds * 1000;
        return;
    }
    sleep_us((br_uint_64)milliseconds * 1000);
}

//
// Sampling profiler. Records (pc, lr) pairs on SIGPROF; symbolized offline.
//

#define PROF_MAX_SAMPLES (1 << 20)

static br_uint_32 (*prof_samples)[2];
static volatile int prof_count;
static int prof_running;

static void prof_handler(int sig, siginfo_t* info, void* ctx) {
    ucontext_t* uc = ctx;
    int i = prof_count;
    if (i < PROF_MAX_SAMPLES) {
#if defined(__arm__)
        prof_samples[i][0] = uc->uc_mcontext.arm_pc;
        prof_samples[i][1] = uc->uc_mcontext.arm_lr;
#elif defined(__x86_64__)
        // host builds (renderer verification): no link register
        prof_samples[i][0] = (br_uint_32)uc->uc_mcontext.gregs[REG_RIP];
        prof_samples[i][1] = 0;
#endif
        prof_count = i + 1;
    }
}

// A CLOCK_MONOTONIC hrtimer gives real 1kHz sampling (ITIMER_PROF only has jiffy
// resolution). Being wall-clock based, time spent sleeping shows up as samples in libc.
static timer_t prof_timer;

static void prof_set_timer(int usec) {
    struct itimerspec ts;
    ts.it_interval.tv_sec = 0;
    ts.it_interval.tv_nsec = usec * 1000;
    ts.it_value = ts.it_interval;
    timer_settime(prof_timer, 0, &ts, NULL);
}

static void prof_start(void) {
    struct sigaction sa;
    struct sigevent sev;
    if (prof_samples == NULL) {
        prof_samples = malloc(sizeof(*prof_samples) * PROF_MAX_SAMPLES);
        memset(&sa, 0, sizeof(sa));
        sa.sa_sigaction = prof_handler;
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGPROF, &sa, NULL);
        memset(&sev, 0, sizeof(sev));
        // sample the game thread only, not e.g. the audio thread sleeping
        sev.sigev_notify = SIGEV_THREAD_ID;
        sev.sigev_signo = SIGPROF;
        sev._sigev_un._tid = syscall(SYS_gettid);
        timer_create(CLOCK_MONOTONIC, &sev, &prof_timer);
    }
    prof_running = 1;
    prof_set_timer(1000);
    printf("mister: profiler started\n");
}

static void prof_stop(void) {
    if (prof_running) {
        prof_set_timer(0);
        prof_running = 0;
        printf("mister: profiler stopped (%d samples)\n", prof_count);
    }
}

static void prof_dump(void) {
    char path[MAX_PATH];
    FILE* f;
    prof_stop();
    if (prof_count == 0) {
        return;
    }
    snprintf(path, sizeof(path), "%s/profile.bin", out_dir);
    f = fopen(path, "wb");
    if (f != NULL) {
        fwrite(prof_samples, sizeof(*prof_samples), prof_count, f);
        fclose(f);

        printf("mister: wrote %d profile samples to %s\n", prof_count, path);
        // shared library load addresses for symbolizing
        snprintf(path, sizeof(path), "cp /proc/%d/maps %s/maps.txt", getpid(), out_dir);
        if (system(path) != 0) {
            fprintf(stderr, "mister: could not save memory map\n");
        }
    }
}

//
// Frame statistics
//

#define STATS_MAX_FRAMES 100000

static br_uint_32* frame_times_us;
static int frame_count;
static br_uint_64 last_swap_us;
static br_uint_64 interval_start_us;
static int interval_frames;
static br_uint_32 interval_max_us;
static int stats_enabled;

static void stats_reset(void) {
    if (frame_times_us == NULL) {
        frame_times_us = malloc(sizeof(*frame_times_us) * STATS_MAX_FRAMES);
    }
    frame_count = 0;
    stats_enabled = 1;
    last_swap_us = 0;
    printf("mister: stats reset\n");
}

static int compare_u32(const void* a, const void* b) {
    br_uint_32 x = *(const br_uint_32*)a;
    br_uint_32 y = *(const br_uint_32*)b;
    return (x > y) - (x < y);
}

static void stats_dump(void) {
    char path[MAX_PATH];
    br_uint_64 total = 0;
    br_uint_32* sorted;
    FILE* f;
    int i;

    if (!stats_enabled || frame_count == 0) {
        return;
    }
    snprintf(path, sizeof(path), "%s/frametimes.txt", out_dir);
    f = fopen(path, "w");
    if (f != NULL) {
        for (i = 0; i < frame_count; i++) {
            fprintf(f, "%u\n", frame_times_us[i]);
        }
        fclose(f);
    }
    sorted = malloc(sizeof(*sorted) * frame_count);
    memcpy(sorted, frame_times_us, sizeof(*sorted) * frame_count);
    qsort(sorted, frame_count, sizeof(*sorted), compare_u32);
    for (i = 0; i < frame_count; i++) {
        total += sorted[i];
    }
    printf("mister: SUMMARY frames=%d avg=%.2fms (%.1f fps) median=%.2fms p95=%.2fms p99=%.2fms max=%.2fms\n",
        frame_count,
        total / 1000.0 / frame_count,
        1000000.0 * frame_count / total,
        sorted[frame_count / 2] / 1000.0,
        sorted[frame_count * 95 / 100] / 1000.0,
        sorted[frame_count * 99 / 100] / 1000.0,
        sorted[frame_count - 1] / 1000.0);
    free(sorted);
}

static void stats_frame(void) {
    br_uint_64 now = mister_get_micros();
    br_uint_32 dt;

    if (last_swap_us != 0) {
        dt = (br_uint_32)(now - last_swap_us);
        if (stats_enabled && frame_count < STATS_MAX_FRAMES) {
            frame_times_us[frame_count++] = dt;
        }
        if (dt > interval_max_us) {
            interval_max_us = dt;
        }
        interval_frames++;
    }
    last_swap_us = now;

    if (interval_start_us == 0) {
        interval_start_us = now;
    } else if (now - interval_start_us >= 2000000) {
        printf("mister: t=%.1fs %d frames %.1f fps (worst %.1fms, present %.2fms)\n",
            now / 1000000.0,
            interval_frames,
            interval_frames * 1000000.0 / (now - interval_start_us),
            interval_max_us / 1000.0,
            present_count ? present_us / 1000.0 / present_count : 0.0);
        present_us = 0;
        present_count = 0;
        fflush(stdout);
        interval_start_us = now;
        interval_frames = 0;
        interval_max_us = 0;
    }
}

//
// Screenshots (binary PPM)
//

static void write_screenshot(br_pixelmap* src) {
    char path[MAX_PATH];
    br_uint_8* row;
    FILE* f;
    int x, y;

    snprintf(path, sizeof(path), "%s/shot_%04d.ppm", out_dir, shot_count++);
    f = fopen(path, "wb");
    if (f == NULL) {
        return;
    }
    fprintf(f, "P6\n%d %d\n255\n", src->width, src->height);
    for (y = 0; y < src->height; y++) {
        row = (br_uint_8*)src->pixels + y * src->row_bytes;
        for (x = 0; x < src->width; x++) {
            fwrite(palette_rgb[row[x]], 3, 1, f);
        }
    }
    fclose(f);
    printf("mister: screenshot %s (t=%u)\n", path, mister_get_ticks());
}

//
// Input/control script.
//
// One command per line, times in ms since startup, '#' starts a comment:
//   <time> key <scancode> <hold_ms>   press and release a key (scancode hex or decimal)
//   <time> down <scancode>            press and hold
//   <time> up <scancode>              release
//   <time> shot                       screenshot of next frame
//   <time> stats                      reset frame statistics
//   <time> prof_on | prof_off         control the sampling profiler
//   <time> quit                       dump results and exit
//

typedef enum {
    eCmd_down,
    eCmd_up,
    eCmd_shot,
    eCmd_stats,
    eCmd_prof_on,
    eCmd_prof_off,
    eCmd_quit,
} tScript_cmd;

typedef struct {
    br_uint_32 time;
    tScript_cmd cmd;
    int arg;
} tScript_event;

#define SCRIPT_MAX_EVENTS 4096

static tScript_event script[SCRIPT_MAX_EVENTS];
static int script_count;
static int script_pos;

static void add_event(br_uint_32 time, tScript_cmd cmd, int arg) {
    int i;
    if (script_count == SCRIPT_MAX_EVENTS) {
        return;
    }
    // keep sorted by time (insertion after equal times preserves file order)
    for (i = script_count; i > 0 && script[i - 1].time > time; i--) {
        script[i] = script[i - 1];
    }
    script[i].time = time;
    script[i].cmd = cmd;
    script[i].arg = arg;
    script_count++;
}

static void load_script(const char* path) {
    char line[256];
    char cmd[32];
    unsigned int time, arg1, arg2;
    int n;
    FILE* f;

    f = fopen(path, "r");
    if (f == NULL) {
        LOG_PANIC2("mister: cannot open script %s", path);
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (line[0] == '#' || line[0] == '\n') {
            continue;
        }
        n = sscanf(line, "%u %31s %i %i", &time, cmd, &arg1, &arg2);
        if (n < 2) {
            continue;
        }
        if (strcmp(cmd, "key") == 0 && n == 4) {
            add_event(time, eCmd_down, arg1);
            add_event(time + arg2, eCmd_up, arg1);
        } else if (strcmp(cmd, "down") == 0 && n >= 3) {
            add_event(time, eCmd_down, arg1);
        } else if (strcmp(cmd, "up") == 0 && n >= 3) {
            add_event(time, eCmd_up, arg1);
        } else if (strcmp(cmd, "shot") == 0) {
            add_event(time, eCmd_shot, 0);
        } else if (strcmp(cmd, "stats") == 0) {
            add_event(time, eCmd_stats, 0);
        } else if (strcmp(cmd, "prof_on") == 0) {
            add_event(time, eCmd_prof_on, 0);
        } else if (strcmp(cmd, "prof_off") == 0) {
            add_event(time, eCmd_prof_off, 0);
        } else if (strcmp(cmd, "quit") == 0) {
            add_event(time, eCmd_quit, 0);
        } else {
            LOG_WARN2("mister: bad script line: %s", line);
        }
    }
    fclose(f);
    printf("mister: loaded %d script events from %s\n", script_count, path);
}

static void update_input(void);

// OSD volume entries are 100%, 90%, .. 0%. Squared for a roughly even loudness
// step: 50% is -12dB.
static float osd_volume(int index) {
    float v = index > 10 ? 0.0f : (10 - index) / 10.0f;
    return v * v;
}

// OSD "Lock to 30 FPS"
static int lock_30fps;

// Applies the OSD options whenever they change
static void apply_osd_options(br_uint_64 osd) {
    static const char* const renderer_names[] = { "optimized", "fast", "original", "optimized" };
    static int applied;
    static br_uint_64 last;
    static int renderer_from_env;
    static int renderer = -1;

    if (applied && osd == last) {
        return;
    }
    if (!applied) {
        // benchmark/verification environment variables take precedence
        renderer_from_env = getenv("PENTPRIM_REFERENCE") != NULL || getenv("PENTPRIM_VERIFY") != NULL
            || getenv("PENTPRIM_TIMING") != NULL || getenv("PENTPRIM_FAST") != NULL;
    }
    MiSTer_Audio_SetVolumes(osd_volume(MISTER_OSD_SOUND_VOLUME(osd)), osd_volume(MISTER_OSD_MUSIC_VOLUME(osd)));
    if (!renderer_from_env && MISTER_OSD_RENDERER(osd) != renderer) {
        renderer = MISTER_OSD_RENDERER(osd);
        gPentprim_reference = renderer == 2;
        gPentprim_fast = renderer == 1;
        printf("mister: %s renderer\n", renderer_names[renderer]);
    }
    if (MISTER_OSD_LOCK_30FPS(osd) != lock_30fps) {
        lock_30fps = MISTER_OSD_LOCK_30FPS(osd);
        printf("mister: %s\n", lock_30fps ? "locked to 30 fps" : "frame rate not locked");
    }
    applied = 1;
    last = osd;
}

static void set_key(int scancode, int down) {
    if (down) {
        script_keys[scancode >> 5] |= (1u << (scancode & 0x1F));
    } else {
        script_keys[scancode >> 5] &= ~(1u << (scancode & 0x1F));
    }
    update_input();
}

// Combine script and MiSTer keyboard/joystick state; notify the game on change
static void update_input(void) {
    tMiSTer_input input;
    br_uint_32 keys[8];
    int dx, dy;

    memcpy(keys, script_keys, sizeof(keys));
    if (MiSTer_FPGA_IsOpen() && !MiSTer_FPGA_CheckAlive()) {
        printf("mister: Dethrace core unloaded, exiting\n");
        exit(0);
    }
    if (MiSTer_FPGA_IsOpen()) {
        MiSTer_FPGA_ReadInput(&input);
        MiSTer_Input_Map(&input, keys);
        apply_osd_options(input.osd_status);

        if (!mouse_initialized) {
            mouse_initialized = 1;
            last_mouse_acc_x = input.mouse_x;
            last_mouse_acc_y = input.mouse_y;
            mouse_x = screen_width / 2;
            mouse_y = screen_height / 2;
        }
        dx = input.mouse_x - last_mouse_acc_x;
        dy = input.mouse_y - last_mouse_acc_y;
        last_mouse_acc_x = input.mouse_x;
        last_mouse_acc_y = input.mouse_y;
        mouse_x += dx * screen_width / 320;
        mouse_y -= dy * screen_height / 200;
        mouse_x = mouse_x < 0 ? 0 : (mouse_x >= screen_width ? screen_width - 1 : mouse_x);
        mouse_y = mouse_y < 0 ? 0 : (mouse_y >= screen_height ? screen_height - 1 : mouse_y);
        mouse_buttons = input.mouse_buttons;
    }
    if (memcmp(keys, key_state, sizeof(keys)) != 0) {
        memcpy(key_state, keys, sizeof(keys));
        if (gKeyHandler_func != NULL) {
            gKeyHandler_func();
        }
    }
}

static void run_script(void) {
    br_uint_32 now = mister_get_ticks();
    tScript_event* ev;

    while (script_pos < script_count && script[script_pos].time <= now) {
        ev = &script[script_pos++];
        switch (ev->cmd) {
        case eCmd_down:
            set_key(ev->arg & 0xff, 1);
            break;
        case eCmd_up:
            set_key(ev->arg & 0xff, 0);
            break;
        case eCmd_shot:
            shot_requested = 1;
            break;
        case eCmd_stats:
            stats_reset();
            break;
        case eCmd_prof_on:
            prof_start();
            break;
        case eCmd_prof_off:
            prof_stop();
            break;
        case eCmd_quit:
            printf("mister: quit requested by script\n");
            exit(0);
        }
    }
}

static void mister_at_exit(void) {
    int back_to_menu = game_quit && MiSTer_FPGA_IsOpen() && getenv("DETHRACE_MISTER_QUIT_TO_MENU") != NULL;
    FILE* f;

    MiSTer_Audio_Stop();
    MiSTer_FPGA_Close();
    if (back_to_menu) {
        // otherwise the hybrid core launcher just restarts the game
        f = fopen("/dev/MiSTer_cmd", "w");
        if (f != NULL) {
            fputs("load_core /media/fat/menu.rbf\n", f);
            fclose(f);
        }
    }
    prof_dump();
    stats_dump();
    fflush(stdout);
}

//
// Platform callbacks
//

static void mister_create_window(const char* title, int width, int height, tHarness_window_type window_type) {
    printf("mister: framebuffer %dx%d\n", width, height);
    screen_width = width;
    screen_height = height;
    MiSTer_FPGA_SetMode(width, height);
}

static void mister_process_window_messages(void) {
    run_script();
    update_input();
}

// Same frame limiter as the SDL platforms (harness FPSLimit, default 60)
static void limit_fps(void) {
    static br_uint_64 last_frame_us;
    br_uint_64 now = mister_get_micros();
    br_uint_64 frame_us = 1000000 / harness_game_config.fps;

    if (last_frame_us != 0 && now - last_frame_us < frame_us) {
        sleep_us(frame_us - (now - last_frame_us));
    }
    last_frame_us = mister_get_micros();
}

// "Lock to 30 FPS": every frame is shown for two fields of the core's 59.6Hz
// video, which is steadier than a frame rate that floats between 30 and 60.
// A frame that takes longer goes out as soon as it is done.
static void wait_two_fields(void) {
    static br_uint_32 last_field;
    int i;

    // 100ms at most, in case the core stops counting
    for (i = 0; i < 200 && (br_uint_32)(MiSTer_FPGA_FieldCounter() - last_field) < 2; i++) {
        sleep_us(500);
    }
    last_field = MiSTer_FPGA_FieldCounter();
}

static void mister_swap(br_pixelmap* back_buffer) {
    static int fixed_seeded;
    br_uint_32 now;

    last_screen_src = back_buffer;
    if (fixed_step_us != 0) {
        if (!fixed_seeded) {
            // after the game seeded from the wall clock
            fixed_seeded = 1;
            srand(1);
        }
        fixed_time_us += fixed_step_us;
        fixed_reads = 0;
    } else if (lock_30fps && MiSTer_FPGA_IsOpen()) {
        wait_two_fields();
    } else if (harness_game_config.fps != 0) {
        limit_fps();
    }
    if (MiSTer_FPGA_IsOpen()) {
        br_uint_64 t0 = mister_get_micros();
        MiSTer_FPGA_Present(back_buffer);
        present_us += mister_get_micros() - t0;
        present_count++;
    }
    stats_frame();

    now = mister_get_ticks();
    if (shot_interval_ms != 0 && now >= next_shot_time) {
        next_shot_time = now + shot_interval_ms;
        shot_requested = 1;
    }
    if (shot_requested) {
        shot_requested = 0;
        write_screenshot(back_buffer);
    }
    run_script();
    update_input();
}

static void mister_palette_changed(br_colour entries[256]) {
    int i;
    MiSTer_FPGA_SetPalette(entries);
    for (i = 0; i < 256; i++) {
        palette_rgb[i][0] = BR_RED(entries[i]);
        palette_rgb[i][1] = BR_GRN(entries[i]);
        palette_rgb[i][2] = BR_BLU(entries[i]);
    }
}

static void mister_set_key_handler(void (*handler_func)(void)) {
    gKeyHandler_func = handler_func;
}

static void mister_get_keyboard_state(br_uint_32* buffer) {
    memcpy(buffer, key_state, sizeof(key_state));
}

static int mister_get_mouse_buttons(int* pButton1, int* pButton2) {
    *pButton1 = mouse_buttons & 1;
    *pButton2 = (mouse_buttons >> 1) & 1;
    return 0;
}

static int mister_get_mouse_position(int* pX, int* pY) {
    *pX = mouse_x;
    *pY = mouse_y;
    return 0;
}

static int mister_show_cursor(int show) {
    return 0;
}

static int mister_set_window_pos(void* hWnd, int x, int y, int nWidth, int nHeight) {
    printf("mister: resize %dx%d\n", nWidth, nHeight);
    return 0;
}

static void mister_destroy_window(void) {
    game_quit = 1;
}

static int mister_show_error_message(char* title, char* message) {
    fprintf(stderr, "%s: %s\n", title, message);
    return 0;
}

static void mister_get_viewport(int* x, int* y, float* width_multiplier, float* height_multiplier) {
    *x = 0;
    *y = 0;
    *width_multiplier = 1.0f;
    *height_multiplier = 1.0f;
}

static int mister_platform_init(tHarness_platform* platform) {
    const char* env;

    clock_gettime(CLOCK_MONOTONIC, &start_time);
    clock_gettime(CLOCK_MONOTONIC_COARSE, &start_time_coarse);

    env = getenv("DETHRACE_MISTER_OUT");
    if (env != NULL) {
        out_dir = env;
    }
    env = getenv("DETHRACE_MISTER_SHOTS");
    if (env != NULL) {
        shot_interval_ms = atoi(env);
    }
    env = getenv("DETHRACE_MISTER_SCRIPT");
    if (env != NULL) {
        load_script(env);
    }
    env = getenv("DETHRACE_MISTER_FIXED_STEP");
    if (env != NULL) {
        fixed_step_us = atoi(env) * 1000;
        // the audio backend runs in real time, which makes runs differ a little:
        // DETHRACE_MISTER_FIXED_SOUND=1 keeps it on to measure what sound costs
        if (getenv("DETHRACE_MISTER_FIXED_SOUND") == NULL) {
            gSound_override = 1;
        }
        printf("mister: fixed time step %s ms\n", env);
    }
    // Render on CPU0: Main_MiSTer owns CPU1, and CPU0 has the better DDR3
    // bandwidth. Helper threads (audio) go to CPU1 if the process may use it.
    {
        cpu_set_t cpus;
        if (sched_getaffinity(0, sizeof(cpus), &cpus) == 0 && CPU_ISSET(1, &cpus)) {
            gMiSTer_cpu1_allowed = 1;
        }
        CPU_ZERO(&cpus);
        CPU_SET(0, &cpus);
        sched_setaffinity(0, sizeof(cpus), &cpus);
    }
    if (getenv("DETHRACE_MISTER_HEADLESS") == NULL) {
        MiSTer_FPGA_Open();
    }
    // The core only does 320x200 (640x480 is too slow for the CPU); override a Hires=1 ini
    gGraf_spec_index = 0;
    atexit(mister_at_exit);

    platform->ProcessWindowMessages = mister_process_window_messages;
    platform->Sleep = mister_sleep;
    platform->GetTicks = mister_get_ticks;
    platform->ShowCursor = mister_show_cursor;
    platform->SetWindowPos = mister_set_window_pos;
    platform->DestroyWindow = mister_destroy_window;
    platform->SetKeyHandler = mister_set_key_handler;
    platform->GetKeyboardState = mister_get_keyboard_state;
    platform->GetMousePosition = mister_get_mouse_position;
    platform->GetMouseButtons = mister_get_mouse_buttons;
    platform->ShowErrorMessage = mister_show_error_message;

    platform->CreateWindow_ = mister_create_window;
    platform->Swap = mister_swap;
    platform->PaletteChanged = mister_palette_changed;
    platform->GetViewport = mister_get_viewport;
    return 0;
}

const tPlatform_bootstrap MISTER_bootstrap = {
    "mister",
    "MiSTer FPGA hybrid core",
    ePlatform_cap_software,
    mister_platform_init,
};
