#include "R26USB.h"
#include "../shared/RingBuffer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>

R26SharedAudio *g_shm = NULL;
volatile bool g_running = true;

static int shm_fd = -1;

static void signal_handler(int sig) {
    (void)sig;
    g_running = false;
}

static int setup_shared_memory(void) {
    // Remove stale shared memory
    shm_unlink(R26_SHM_NAME);

    shm_fd = shm_open(R26_SHM_NAME, O_CREAT | O_RDWR, 0666);
    if (shm_fd < 0) {
        perror("r26d: shm_open failed");
        return -1;
    }

    if (ftruncate(shm_fd, R26_SHM_SIZE) < 0) {
        perror("r26d: ftruncate failed");
        close(shm_fd);
        shm_unlink(R26_SHM_NAME);
        return -1;
    }

    g_shm = mmap(NULL, R26_SHM_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (g_shm == MAP_FAILED) {
        perror("r26d: mmap failed");
        g_shm = NULL;
        close(shm_fd);
        shm_unlink(R26_SHM_NAME);
        return -1;
    }

    // Initialize shared memory
    memset(g_shm, 0, R26_SHM_SIZE);
    atomic_store(&g_shm->status, R26_STATUS_STOPPED);
    atomic_store(&g_shm->channels, 2);
    atomic_store(&g_shm->sample_rate, 48000);

    printf("r26d: Shared memory created (%s, %.1f MB)\n",
           R26_SHM_NAME, (double)R26_SHM_SIZE / (1024 * 1024));
    return 0;
}

static void cleanup_shared_memory(void) {
    if (g_shm) {
        atomic_store(&g_shm->status, R26_STATUS_STOPPED);
        munmap(g_shm, R26_SHM_SIZE);
        g_shm = NULL;
    }
    if (shm_fd >= 0) {
        close(shm_fd);
        shm_fd = -1;
    }
    shm_unlink(R26_SHM_NAME);
}

static void print_usage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("Roland R-26 USB Audio Bridge Daemon\n\n");
    printf("Options:\n");
    printf("  --probe       Dump USB descriptors and exit\n");
    printf("  --rate RATE   Set sample rate (32000|44100|48000|88200|96000|192000)\n");
    printf("  --help        Show this help\n");
    printf("\nThe daemon captures audio from the Roland R-26 via USB and\n");
    printf("exposes it through shared memory for the CoreAudio driver plugin.\n");
}

int main(int argc, char *argv[]) {
    bool probe_only = false;
    uint32_t requested_rate = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--probe") == 0) {
            probe_only = true;
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            requested_rate = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    printf("r26d: Roland R-26 USB Audio Bridge v1.0\n");

    // Open the R-26
    R26Device dev;
    if (r26_open(&dev) < 0) {
        return 1;
    }

    // Always dump descriptors first (useful for debugging)
    r26_dump_descriptors(&dev);

    if (probe_only) {
        r26_close(&dev);
        return 0;
    }

    // Probe for audio endpoints
    if (r26_probe(&dev) < 0) {
        r26_close(&dev);
        return 1;
    }

    // Set requested sample rate
    if (requested_rate > 0) {
        r26_set_sample_rate(&dev, requested_rate);
    }

    // Set up shared memory
    if (setup_shared_memory() < 0) {
        r26_close(&dev);
        return 1;
    }

    // Install signal handlers
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    printf("r26d: Starting audio capture (Ctrl+C to stop)\n");
    printf("r26d: Format: %d Hz, %d ch, %d-bit\n",
           dev.sample_rate, dev.channels, dev.bit_depth);

    // Start capture (blocks until signal)
    r26_start_capture(&dev);

    printf("r26d: Shutting down...\n");
    cleanup_shared_memory();
    r26_close(&dev);
    printf("r26d: Done.\n");

    return 0;
}
