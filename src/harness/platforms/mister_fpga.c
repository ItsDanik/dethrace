// Bridge to the Dethrace FPGA core through shared DDR3 memory.
//
// The FPGA side (core/rtl/dethrace_host.sv) scans out one of three 8bpp
// framebuffers every frame, reloads the palette when its sequence number
// changes and publishes input state once per vblank.
//
// The shared memory is mapped uncached, which makes copying a frame into it
// take ~1ms. A present thread on CPU1 (when the launcher allows it) does that
// copy so the game thread only copies into a cached staging buffer.

#define _GNU_SOURCE
#include "mister_fpga.h"
#include "harness/trace.h"

#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define SHM_PHYS 0x30000000
#define SHM_SIZE 0x400000

#define CTRL_OFFSET 0x0
#define STATUS_OFFSET 0x40
#define AUDIO_PTR_OFFSET 0xC0
#define AUDIO_RING_OFFSET 0x10000
#define PALETTE_OFFSET 0x1000
#define PALETTE_SLOT_SIZE 0x400
#define FB_OFFSET 0x100000
#define FB_SIZE 0x100000
#define FB_COUNT 3

#define CTRL_MAGIC 0x48544544   // "DETH"
#define STATUS_MAGIC 0x53485444 // "DTHS"

static int mem_fd = -1;
static volatile br_uint_8* shm;
static volatile br_uint_32* ctrl;
static volatile br_uint_32* status;

static int fb_width;
static int fb_height;
static int fb_current;
static int palette_slot;
static br_uint_32 palette_seq;
static int ctrl_enabled;
static int audio_enabled;
// set once the core is gone: another core may own the memory now, never write to it again
static volatile int detached;

// present thread: FIFO of up to 2 staged frames
static pthread_t present_thread;
static int present_threaded;
int gMiSTer_cpu1_allowed;
static int present_running;
static pthread_mutex_t present_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t present_cond = PTHREAD_COND_INITIALIZER;
static br_uint_8* staging[2];
static int staging_next;
static int queue[2];
static int queue_head;
static int queue_count;
// serialises control block updates between the game and present threads
static pthread_mutex_t ctrl_lock = PTHREAD_MUTEX_INITIALIZER;

static void start_present_thread(void);
static void stop_present_thread(void);

static br_uint_32 alive_frame;
static struct timespec alive_time;

static void write_ctrl(void) {
    // Palette/framebuffer writes must land before the FPGA can see the new control block
    pthread_mutex_lock(&ctrl_lock);
    __sync_synchronize();
    ctrl[2] = palette_seq;
    ctrl[3] = 0;
    ctrl[1] = fb_current | (palette_slot << 16) | (audio_enabled << 24);
    ctrl[0] = ctrl_enabled ? CTRL_MAGIC : 0;
    __sync_synchronize();
    pthread_mutex_unlock(&ctrl_lock);
}

static void sleep_ms(int ms) {
    struct timespec ts = { 0, ms * 1000000 };
    nanosleep(&ts, NULL);
}

int MiSTer_FPGA_Open(void) {
    br_uint_32 frame;
    int i;

    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        fprintf(stderr, "mister: cannot open /dev/mem, running headless\n");
        return 0;
    }
    shm = mmap(NULL, SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, SHM_PHYS);
    if (shm == MAP_FAILED) {
        fprintf(stderr, "mister: cannot map shared memory, running headless\n");
        close(mem_fd);
        mem_fd = -1;
        shm = NULL;
        return 0;
    }
    ctrl = (volatile br_uint_32*)(shm + CTRL_OFFSET);
    status = (volatile br_uint_32*)(shm + STATUS_OFFSET);

    // The core rewrites the status block every vblank; a stale block from an
    // earlier session has a frozen frame counter.
    frame = status[1];
    for (i = 0; i < 10 && status[1] == frame; i++) {
        sleep_ms(10);
    }
    if (status[0] != STATUS_MAGIC || status[1] == frame) {
        fprintf(stderr, "mister: Dethrace core not running, running headless\n");
        MiSTer_FPGA_Close();
        return 0;
    }
    printf("mister: Dethrace core found (version %u)\n", status[3]);
    return 1;
}

void MiSTer_FPGA_Close(void) {
    stop_present_thread();
    if (shm != NULL) {
        if (!detached) {
            // back to the core's test pattern, audio off
            ctrl_enabled = 0;
            audio_enabled = 0;
            write_ctrl();
        }
        munmap((void*)shm, SHM_SIZE);
        shm = NULL;
    }
    if (mem_fd >= 0) {
        close(mem_fd);
        mem_fd = -1;
    }
}

int MiSTer_FPGA_IsOpen(void) {
    return shm != NULL && !detached;
}

void MiSTer_FPGA_SetMode(int width, int height) {
    int i;

    if (!MiSTer_FPGA_IsOpen()) {
        return;
    }
    if (width != 320 || height != 200) {
        fprintf(stderr, "mister: unsupported resolution %dx%d\n", width, height);
        abort();
    }
    fb_width = width;
    fb_height = height;
    for (i = 0; i < FB_COUNT; i++) {
        memset((void*)(shm + FB_OFFSET + i * FB_SIZE), 0, width * height);
    }
    // The control block is published with the first palette so the test
    // pattern stays up until the game shows something.
    write_ctrl();
    if (!present_threaded) {
        start_present_thread();
    }
}

// Copy a frame into the next framebuffer and flip to it
static void present_to_fpga(const br_uint_8* pixels, int row_bytes) {
    volatile br_uint_8* dst;
    int next;
    int y;
    int i;

    // Triple buffering. Never write into the buffer that is being scanned out:
    // with two frames submitted within one vblank the FPGA may still show it.
    next = (fb_current + 1) % FB_COUNT;
    for (i = 0; i < 50 && (status[2] & 0xff) == (br_uint_32)next; i++) {
        sleep_ms(1);
    }

    dst = shm + FB_OFFSET + next * FB_SIZE;
    if (row_bytes == fb_width) {
        memcpy((void*)dst, pixels, fb_width * fb_height);
    } else {
        for (y = 0; y < fb_height; y++) {
            memcpy((void*)(dst + y * fb_width), pixels + y * row_bytes, fb_width);
        }
    }
    fb_current = next;
    write_ctrl();
}

static void* present_thread_main(void* arg) {
    int slot;

    pthread_mutex_lock(&present_lock);
    for (;;) {
        while (queue_count == 0 && present_running) {
            pthread_cond_wait(&present_cond, &present_lock);
        }
        if (queue_count == 0) {
            break;
        }
        slot = queue[queue_head];
        pthread_mutex_unlock(&present_lock);

        if (MiSTer_FPGA_IsOpen()) {
            present_to_fpga(staging[slot], fb_width);
        }

        pthread_mutex_lock(&present_lock);
        queue_head = (queue_head + 1) % 2;
        queue_count--;
        pthread_cond_broadcast(&present_cond);
    }
    pthread_mutex_unlock(&present_lock);
    return NULL;
}

static void start_present_thread(void) {
    cpu_set_t cpu1;
    pthread_attr_t attr;

    // only worth it with a second CPU to run on
    if (!gMiSTer_cpu1_allowed) {
        return;
    }
    CPU_ZERO(&cpu1);
    CPU_SET(1, &cpu1);
    staging[0] = malloc(fb_width * fb_height);
    staging[1] = malloc(fb_width * fb_height);
    pthread_attr_init(&attr);
    pthread_attr_setaffinity_np(&attr, sizeof(cpu1), &cpu1);
    present_running = 1;
    if (pthread_create(&present_thread, &attr, present_thread_main, NULL) == 0) {
        present_threaded = 1;
        printf("mister: frame copy on CPU1\n");
    } else {
        present_running = 0;
    }
    pthread_attr_destroy(&attr);
}

// Wait until all staged frames reached the FPGA
static void present_flush(void) {
    if (!present_threaded) {
        return;
    }
    pthread_mutex_lock(&present_lock);
    while (queue_count > 0) {
        pthread_cond_wait(&present_cond, &present_lock);
    }
    pthread_mutex_unlock(&present_lock);
}

static void stop_present_thread(void) {
    if (!present_threaded) {
        return;
    }
    pthread_mutex_lock(&present_lock);
    present_running = 0;
    pthread_cond_broadcast(&present_cond);
    pthread_mutex_unlock(&present_lock);
    pthread_join(present_thread, NULL);
    present_threaded = 0;
}

void MiSTer_FPGA_Present(br_pixelmap* src) {
    br_uint_8* dst;
    int slot;
    int y;

    if (!MiSTer_FPGA_IsOpen()) {
        return;
    }
    if (!present_threaded) {
        present_to_fpga(src->pixels, src->row_bytes);
        return;
    }

    pthread_mutex_lock(&present_lock);
    while (queue_count == 2) {
        pthread_cond_wait(&present_cond, &present_lock);
    }
    slot = staging_next;
    staging_next ^= 1;
    pthread_mutex_unlock(&present_lock);

    dst = staging[slot];
    if (src->row_bytes == fb_width) {
        memcpy(dst, src->pixels, fb_width * fb_height);
    } else {
        for (y = 0; y < fb_height; y++) {
            memcpy(dst + y * fb_width, (br_uint_8*)src->pixels + y * src->row_bytes, fb_width);
        }
    }

    pthread_mutex_lock(&present_lock);
    queue[(queue_head + queue_count) % 2] = slot;
    queue_count++;
    pthread_cond_signal(&present_cond);
    pthread_mutex_unlock(&present_lock);
}

br_uint_32 MiSTer_FPGA_FieldCounter(void) {
    return status[1];
}

void MiSTer_FPGA_SetPalette(br_colour* entries) {
    volatile br_uint_32* pal;
    int i;

    if (!MiSTer_FPGA_IsOpen()) {
        return;
    }
    // Frames queued before this palette change must reach the FPGA first
    present_flush();

    // Two palette slots: fill the one the FPGA is not using, then switch
    palette_slot ^= 1;
    pal = (volatile br_uint_32*)(shm + PALETTE_OFFSET + palette_slot * PALETTE_SLOT_SIZE);
    for (i = 0; i < 256; i++) {
        pal[i] = entries[i] & 0xffffff;
    }
    palette_seq++;
    ctrl_enabled = 1;
    write_ctrl();
}

static long elapsed_ms(const struct timespec* since) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - since->tv_sec) * 1000 + (now.tv_nsec - since->tv_nsec) / 1000000;
}

static int corename_is_dethrace(void) {
    char name[64] = { 0 };
    FILE* f = fopen("/tmp/CORENAME", "r");
    if (f == NULL) {
        return 1;
    }
    if (fgets(name, sizeof(name), f) == NULL) {
        name[0] = 0;
    }
    fclose(f);
    return strncmp(name, "Dethrace", 8) == 0;
}

int MiSTer_FPGA_CheckAlive(void) {
    if (!MiSTer_FPGA_IsOpen()) {
        return !detached;
    }
    if (alive_time.tv_sec == 0) {
        alive_frame = status[1];
        clock_gettime(CLOCK_MONOTONIC, &alive_time);
        return 1;
    }
    if (elapsed_ms(&alive_time) < 500) {
        return 1;
    }
    // Another core may use the same memory: stop touching it as soon as ours is gone
    if (status[0] != STATUS_MAGIC || status[1] == alive_frame || !corename_is_dethrace()) {
        detached = 1;
        return 0;
    }
    alive_frame = status[1];
    clock_gettime(CLOCK_MONOTONIC, &alive_time);
    return 1;
}

void MiSTer_FPGA_ReadInput(tMiSTer_input* input) {
    int i;

    memset(input, 0, sizeof(*input));
    if (!MiSTer_FPGA_IsOpen()) {
        return;
    }
    input->frame = status[1];
    input->joystick[0] = status[4];
    input->joystick[1] = status[5];
    input->analog_x[0] = (br_int_8)(status[6] & 0xff);
    input->analog_y[0] = (br_int_8)((status[6] >> 8) & 0xff);
    input->analog_x[1] = (br_int_8)(status[7] & 0xff);
    input->analog_y[1] = (br_int_8)((status[7] >> 8) & 0xff);
    input->osd_status = status[8] | ((br_uint_64)status[9] << 32);
    input->mouse_x = (br_int_32)status[10];
    input->mouse_y = (br_int_32)status[11];
    input->mouse_buttons = status[12] & 7;
    for (i = 0; i < 16; i++) {
        input->keys[i] = status[16 + i];
    }
}

volatile br_uint_32* MiSTer_FPGA_AudioRing(void) {
    return (volatile br_uint_32*)(shm + AUDIO_RING_OFFSET);
}

br_uint_32 MiSTer_FPGA_AudioFetchPointer(void) {
    return *(volatile br_uint_32*)(shm + AUDIO_PTR_OFFSET);
}

void MiSTer_FPGA_SetAudioEnabled(int enabled) {
    if (!MiSTer_FPGA_IsOpen()) {
        return;
    }
    audio_enabled = enabled;
    write_ctrl();
}
