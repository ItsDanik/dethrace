// Audio output through the Dethrace FPGA core.
//
// The core plays 44.1kHz stereo frames from a 16384 frame ring in shared memory
// and publishes how far it has fetched. This thread stays LEAD_FRAMES ahead of
// that pointer, so the FPGA's sample clock paces the whole audio pipeline.

#define _GNU_SOURCE
#include "mister_audio.h"
#include "mister_fpga.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RING_FRAMES 16384
#define LEAD_FRAMES 1024 // ~23ms plus up to 128 frames in the core's FIFO
#define CHUNK_FRAMES 256
#define POLL_US 2000

static pthread_t audio_thread;
static volatile int audio_running;
static tMiSTer_audio_render audio_render;
static int underruns;
static volatile float sound_volume = 1.0f;
static volatile float music_volume = 1.0f;

static void write_ring(volatile br_uint_32* ring, br_uint_32 pos, const br_uint_32* frames, int count) {
    int i;
    for (i = 0; i < count; i++) {
        ring[(pos + i) % RING_FRAMES] = frames[i];
    }
}

static void* audio_thread_main(void* arg) {
    volatile br_uint_32* ring = MiSTer_FPGA_AudioRing();
    br_uint_32 chunk[CHUNK_FRAMES];
    br_uint_32 fetch;
    br_uint_32 wr;
    struct timespec ts = { 0, POLL_US * 1000 };

    int primed = 0;

    wr = MiSTer_FPGA_AudioFetchPointer() + LEAD_FRAMES;
    while (audio_running) {
        if (!MiSTer_FPGA_IsOpen()) {
            break;
        }
        fetch = MiSTer_FPGA_AudioFetchPointer();
        if ((br_int_32)(wr - fetch) < CHUNK_FRAMES / 2 || (br_int_32)(wr - fetch) > RING_FRAMES / 2) {
            // fell behind (or the core restarted its pointer): skip ahead
            if (primed) {
                underruns++;
                printf("mister: audio underrun (lead %d frames)\n", (br_int_32)(wr - fetch));
            }
            wr = fetch + LEAD_FRAMES;
        }
        while ((br_int_32)(wr - fetch) < LEAD_FRAMES && audio_running) {
            audio_render((short*)chunk, CHUNK_FRAMES);
            if (!MiSTer_FPGA_IsOpen()) {
                break;
            }
            write_ring(ring, wr, chunk, CHUNK_FRAMES);
            wr += CHUNK_FRAMES;
            // the core starts fetching at the vblank after audio is enabled
            primed = primed || fetch != 0;
        }
        nanosleep(&ts, NULL);
    }
    return NULL;
}

int MiSTer_Audio_Available(void) {
    return MiSTer_FPGA_IsOpen();
}

int MiSTer_Audio_Start(tMiSTer_audio_render render) {
    volatile br_uint_32* ring = MiSTer_FPGA_AudioRing();
    pthread_attr_t attr;
    struct sched_param param;
    int i;

    if (!MiSTer_FPGA_IsOpen() || audio_running) {
        return 0;
    }
    for (i = 0; i < RING_FRAMES; i++) {
        ring[i] = 0;
    }
    audio_render = render;
    audio_running = 1;

    // Real-time priority so mixing is not starved by the game loop on the same CPU
    pthread_attr_init(&attr);
    pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
    param.sched_priority = 50;
    pthread_attr_setschedparam(&attr, &param);
    {
        // CPU1 when the launcher allowed it (the game thread is pinned to CPU0)
        cpu_set_t cpu1;
        CPU_ZERO(&cpu1);
        CPU_SET(1, &cpu1);
        if (gMiSTer_cpu1_allowed) {
            pthread_attr_setaffinity_np(&attr, sizeof(cpu1), &cpu1);
        }
    }
    if (pthread_create(&audio_thread, &attr, audio_thread_main, NULL) != 0) {
        fprintf(stderr, "mister: no real-time audio thread, using normal priority\n");
        if (pthread_create(&audio_thread, NULL, audio_thread_main, NULL) != 0) {
            audio_running = 0;
            pthread_attr_destroy(&attr);
            return 0;
        }
    }
    pthread_attr_destroy(&attr);
    MiSTer_FPGA_SetAudioEnabled(1);
    printf("mister: audio %dHz via FPGA core\n", MISTER_AUDIO_RATE);
    return 1;
}

void MiSTer_Audio_Stop(void) {
    if (!audio_running) {
        return;
    }
    audio_running = 0;
    pthread_join(audio_thread, NULL);
    MiSTer_FPGA_SetAudioEnabled(0);
    if (underruns) {
        printf("mister: %d audio underruns\n", underruns);
    }
}

void MiSTer_Audio_SetVolumes(float sound, float music) {
    sound_volume = sound;
    music_volume = music;
}

float MiSTer_Audio_SoundVolume(void) {
    return sound_volume;
}

float MiSTer_Audio_MusicVolume(void) {
    return music_volume;
}
