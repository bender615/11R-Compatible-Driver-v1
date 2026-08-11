#include "ERAudioRing.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SWEEP_SECONDS 7.0
#define START_HZ 40.0
#define END_HZ 12000.0
#define LEVEL 0.03
#define CHUNK 256

int main(int argc, char **argv) {
    uint32_t rate = argc > 1 ? (uint32_t)strtoul(argv[1], 0, 10) : 48000;
    ERRing *ring = NULL;
    for (int attempt = 0; attempt < 100 && !ring; ++attempt) {
        ring = er_ring_attach();
        if (ring && (!er_load32(&ring->engineRunning) || er_load32(&ring->sampleRate) != rate)) {
            er_ring_close(ring, 0);
            ring = NULL;
        }
        if (!ring) usleep(20000);
    }
    if (!ring) { fprintf(stderr, "engine ring unavailable\n"); return 1; }

    er_store32(&ring->sampleRate, rate);
    const uint64_t total = (uint64_t)(SWEEP_SECONDS * rate);
    const uint64_t fade = (uint64_t)(0.05 * rate);
    const double ratio = END_HZ / START_HZ;
    double phase = 0.0;
    uint64_t produced = 0;

    printf("%u Hz: 7-second 40 Hz ↗ 12 kHz ↘ 40 Hz sweep, Main L/R at -30.5 dBFS\n", rate);
    fflush(stdout);
    while (produced < total) {
        uint64_t fill = er_load(&ring->outWrite) - er_load(&ring->outRead);
        if (fill > 2048) { usleep(1000); continue; }
        uint32_t count = (uint32_t)((total - produced) > CHUNK ? CHUNK : (total - produced));
        float frames[CHUNK * ER_OUT_CH];
        memset(frames, 0, sizeof(frames));
        for (uint32_t i = 0; i < count; ++i) {
            uint64_t n = produced + i;
            double t = (double)n / (double)total;
            double sweep = t <= 0.5 ? t * 2.0 : (1.0 - t) * 2.0;
            double frequency = START_HZ * pow(ratio, sweep);
            phase += 2.0 * M_PI * frequency / rate;
            if (phase > 2.0 * M_PI) phase = fmod(phase, 2.0 * M_PI);
            double gain = LEVEL;
            if (n < fade) gain *= (double)n / fade;
            if (total - n < fade) gain *= (double)(total - n) / fade;
            float sample = (float)(sin(phase) * gain);
            frames[i * ER_OUT_CH + 0] = sample;
            frames[i * ER_OUT_CH + 1] = sample;
        }
        produced += er_out_write(ring, frames, count);
    }
    while (er_load(&ring->outWrite) != er_load(&ring->outRead)) usleep(1000);
    er_ring_close(ring, 0);
    return 0;
}
