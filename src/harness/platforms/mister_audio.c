// Audio output through the Dethrace FPGA core.
//
// The core plays 44.1kHz stereo frames from a ring in shared memory
// (MH_AudioWrite() of the hybrid core library). This thread stays LEAD_FRAMES
// ahead of the core, so the FPGA's sample clock paces the whole audio
// pipeline.

#define _GNU_SOURCE
#include "mister_audio.h"
#include "mister_fpga.h"

#include <mister_hybrid.h>

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define LEAD_FRAMES 1024 // ~23ms plus up to 128 frames in the core's FIFO
#define CHUNK_FRAMES 256

static pthread_t audio_thread;
static volatile int audio_running;
static tMiSTer_audio_render audio_render;
static volatile float sound_volume = 1.0f;
static volatile float music_volume = 1.0f;

static void* audio_thread_main(void* arg) {
    br_uint_32 chunk[CHUNK_FRAMES];

    while (audio_running && MiSTer_FPGA_IsOpen()) {
        audio_render((short*)chunk, CHUNK_FRAMES);
        // waits until the core has less than LEAD_FRAMES to play
        MH_AudioWrite(chunk, CHUNK_FRAMES, LEAD_FRAMES);
    }
    return NULL;
}

int MiSTer_Audio_Available(void) {
    return MiSTer_FPGA_IsOpen();
}

int MiSTer_Audio_Start(tMiSTer_audio_render render) {
    pthread_attr_t attr;
    struct sched_param param;

    if (!MiSTer_FPGA_IsOpen() || audio_running) {
        return 0;
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
    MH_AudioEnable(1);
    printf("mister: audio %dHz via FPGA core\n", MISTER_AUDIO_RATE);
    return 1;
}

void MiSTer_Audio_Stop(void) {
    if (!audio_running) {
        return;
    }
    audio_running = 0;
    pthread_join(audio_thread, NULL);
    MH_AudioEnable(0);
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
