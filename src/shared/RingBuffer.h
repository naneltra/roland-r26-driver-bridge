#ifndef R26_RING_BUFFER_H
#define R26_RING_BUFFER_H

#include <stdint.h>
#include <stdatomic.h>

// Shared memory layout for audio data between r26d daemon and CoreAudio plugin.
// Uses a lock-free single-producer single-consumer ring buffer.

#define R26_SHM_NAME        "/r26audio"
#define R26_MAX_CHANNELS    2
#define R26_RING_FRAMES     (48000 * 2)  // 2 seconds at 48kHz, enough for any rate
#define R26_SAMPLE_SIZE     sizeof(float) // 32-bit float PCM

// Status flags
#define R26_STATUS_STOPPED   0
#define R26_STATUS_RUNNING   1
#define R26_STATUS_ERROR     2

// A single-producer single-consumer ring buffer for one direction of audio
typedef struct {
    _Atomic uint64_t    write_pos;          // in frames, wraps via modulo
    _Atomic uint64_t    read_pos;           // in frames, wraps via modulo
    uint8_t             _pad[48];           // pad to cache line
    float               data[R26_RING_FRAMES * R26_MAX_CHANNELS];
} R26RingBuffer;

typedef struct {
    // Header - written by daemon, read by plugin
    _Atomic uint32_t    status;             // R26_STATUS_*
    _Atomic uint32_t    sample_rate;        // e.g. 44100, 48000, 96000
    _Atomic uint32_t    channels;           // 1 or 2
    _Atomic uint32_t    bits_per_sample;    // original bits (16, 24, 32) for info
    _Atomic uint64_t    frames_written;     // total frames written (monotonic)

    uint8_t             _pad[64];

    // Input: daemon writes, plugin reads (device → host)
    R26RingBuffer       input;

    // Output: plugin writes, daemon reads (host → device)
    R26RingBuffer       output;
} R26SharedAudio;

#define R26_SHM_SIZE sizeof(R26SharedAudio)

// Inline helpers for ring buffer operations

static inline uint64_t r26_rb_available(const R26RingBuffer *rb) {
    uint64_t w = atomic_load_explicit(&rb->write_pos, memory_order_acquire);
    uint64_t r = atomic_load_explicit(&rb->read_pos, memory_order_acquire);
    return w - r;
}

static inline uint64_t r26_rb_free(const R26RingBuffer *rb) {
    return R26_RING_FRAMES - r26_rb_available(rb);
}

static inline uint64_t r26_rb_write(R26RingBuffer *rb, uint32_t ch, const float *src, uint64_t nframes) {
    uint64_t free_frames = r26_rb_free(rb);
    if (nframes > free_frames) nframes = free_frames;
    if (nframes == 0) return 0;

    uint64_t wp = atomic_load_explicit(&rb->write_pos, memory_order_relaxed);
    uint64_t ring_wp = wp % R26_RING_FRAMES;

    uint64_t first = R26_RING_FRAMES - ring_wp;
    if (first > nframes) first = nframes;

    for (uint64_t i = 0; i < first * ch; i++) {
        rb->data[ring_wp * ch + i] = src[i];
    }

    uint64_t second = nframes - first;
    if (second > 0) {
        for (uint64_t i = 0; i < second * ch; i++) {
            rb->data[i] = src[first * ch + i];
        }
    }

    atomic_store_explicit(&rb->write_pos, wp + nframes, memory_order_release);
    return nframes;
}

static inline uint64_t r26_rb_read(R26RingBuffer *rb, uint32_t ch, float *dst, uint64_t nframes) {
    uint64_t avail = r26_rb_available(rb);
    if (nframes > avail) nframes = avail;
    if (nframes == 0) return 0;

    uint64_t rp = atomic_load_explicit(&rb->read_pos, memory_order_relaxed);
    uint64_t ring_rp = rp % R26_RING_FRAMES;

    uint64_t first = R26_RING_FRAMES - ring_rp;
    if (first > nframes) first = nframes;

    for (uint64_t i = 0; i < first * ch; i++) {
        dst[i] = rb->data[ring_rp * ch + i];
    }

    uint64_t second = nframes - first;
    if (second > 0) {
        for (uint64_t i = 0; i < second * ch; i++) {
            dst[first * ch + i] = rb->data[i];
        }
    }

    atomic_store_explicit(&rb->read_pos, rp + nframes, memory_order_release);
    return nframes;
}

// Convenience wrappers using the shared audio struct
static inline uint64_t r26_ring_write(R26SharedAudio *shm, const float *src, uint64_t nframes) {
    uint32_t ch = atomic_load_explicit(&shm->channels, memory_order_relaxed);
    if (ch == 0) ch = 2;
    return r26_rb_write(&shm->input, ch, src, nframes);
}

static inline uint64_t r26_ring_read(R26SharedAudio *shm, float *dst, uint64_t nframes) {
    uint32_t ch = atomic_load_explicit(&shm->channels, memory_order_relaxed);
    if (ch == 0) ch = 2;
    return r26_rb_read(&shm->input, ch, dst, nframes);
}

#endif // R26_RING_BUFFER_H
