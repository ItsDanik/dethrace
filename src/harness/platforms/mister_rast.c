// The FPGA rasteriser of the Dethrace core as backend of BRender's pentprim
// driver: commands go into a ring in shared DDR3 memory, textures and buffers
// are copied to and from the rasteriser's region of it.
//
// See core/rtl/dethrace_rast_top.sv for the control words and the ring, and
// drivers/pentprim/fpgarast.h for the commands and the rest of the region.
// The memory is mapped uncached: every 32 bit write takes about 60ns.

#define _GNU_SOURCE
#include "mister_rast.h"

#include "brender.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define RAST_PHYS 0x31000000
#define RAST_SIZE 0x1000000
#define RING_OFFSET 0x80000
#define RING_WORDS 0x20000
#define CTRL_MAGIC 0x54534152   // "RAST"
#define STATUS_MAGIC 0x4B4F5352 // "RSOK"

// control and status words (32 bit halves of the 64 bit words)
#define REG_MAGIC 0
#define REG_PRODUCED 1
#define REG_FETCHED 2
#define REG_COMPLETED 3
#define REG_ANSWER 4
#define REG_VERSION 5

// BRender pentprim (drivers/pentprim/fpgarast.h)
typedef struct {
    void (*submit)(const br_uint_32* w, int words);
    void (*wait)(void);
    void (*upload)(br_uint_32 addr, const void* host, br_uint_32 size);
    void (*download)(br_uint_32 addr, void* host, br_uint_32 size);
} tFpgaRast_backend;
extern void FpgaRast_SetBackend(const tFpgaRast_backend* backend);
extern int gPentprim_fpga;

static int mem_fd = -1;
static volatile br_uint_8* shm;
static volatile br_uint_32* reg;
static volatile br_uint_32* ring;
static br_uint_32 produced;
static br_uint_32 fetched; // last value read back

static double now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e3 + t.tv_nsec / 1e6;
}

static void sleep_ms(int ms) {
    struct timespec ts = { 0, ms * 1000000 };
    nanosleep(&ts, NULL);
}

static void rast_failed(const char* what) {
    fprintf(stderr, "mister: FPGA rasteriser %s (written %u, fetched %u, completed %u)\n", what, produced, reg[REG_FETCHED], reg[REG_COMPLETED]);
    reg[REG_MAGIC] = 0;
    abort();
}

static void rast_submit(const br_uint_32* w, int words) {
    int i;

    // room for the command and its padding? The core fetches far faster than
    // commands are written, so this is rare
    if (produced + words + 1 - fetched > RING_WORDS) {
        const double t0 = now_ms();
        while (produced + words + 1 - (fetched = reg[REG_FETCHED]) > RING_WORDS) {
            if (now_ms() - t0 > 2000) {
                rast_failed("does not fetch commands");
            }
        }
    }
    for (i = 0; i < words; i++) {
        ring[produced++ % RING_WORDS] = w[i];
    }
    if (words & 1) {
        // commands are fetched as 64 bit words
        ring[produced++ % RING_WORDS] = 0x00000100;
    }
    reg[REG_PRODUCED] = produced;
}

static void rast_wait(void) {
    const double t0 = now_ms();
    int i;

    for (;;) {
        for (i = 0; i < 1000; i++) {
            if (reg[REG_COMPLETED] == produced) {
                return;
            }
        }
        if (now_ms() - t0 > 2000) {
            rast_failed("does not finish");
        }
    }
}

static void rast_upload(br_uint_32 addr, const void* host, br_uint_32 size) {
    memcpy((void*)(shm + addr), host, size);
}

static void rast_download(br_uint_32 addr, void* host, br_uint_32 size) {
    memcpy(host, (void*)(shm + addr), size);
}

static const tFpgaRast_backend rast_backend = { rast_submit, rast_wait, rast_upload, rast_download };

int MiSTer_Rast_Open(void) {
    int i;

    mem_fd = open("/dev/mem", O_RDWR | O_SYNC);
    if (mem_fd < 0) {
        return 0;
    }
    shm = mmap(NULL, RAST_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, mem_fd, RAST_PHYS);
    if (shm == MAP_FAILED) {
        close(mem_fd);
        mem_fd = -1;
        shm = NULL;
        return 0;
    }
    reg = (volatile br_uint_32*)shm;
    ring = (volatile br_uint_32*)(shm + RING_OFFSET);

    // a new session: the core resets its counters and answers
    reg[REG_MAGIC] = 0;
    sleep_ms(5);
    for (i = 1; i < 6; i++) {
        reg[i] = 0;
    }
    reg[REG_MAGIC] = CTRL_MAGIC;
    for (i = 0; i < 100 && reg[REG_ANSWER] != STATUS_MAGIC; i++) {
        sleep_ms(1);
    }
    if (reg[REG_ANSWER] != STATUS_MAGIC) {
        fprintf(stderr, "mister: the core has no rasteriser\n");
        reg[REG_MAGIC] = 0;
        munmap((void*)shm, RAST_SIZE);
        close(mem_fd);
        mem_fd = -1;
        shm = NULL;
        return 0;
    }
    produced = 0;
    fetched = 0;
    printf("mister: FPGA rasteriser version %u\n", reg[REG_VERSION]);
    gPentprim_fpga = 1;
    FpgaRast_SetBackend(&rast_backend);
    return 1;
}

void MiSTer_Rast_Close(void) {
    if (shm == NULL) {
        return;
    }
    // takes the buffers back first
    FpgaRast_SetBackend(NULL);
    gPentprim_fpga = 0;
    reg[REG_MAGIC] = 0;
    munmap((void*)shm, RAST_SIZE);
    close(mem_fd);
    mem_fd = -1;
    shm = NULL;
}
