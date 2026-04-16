#ifndef AUDIO_PW_V3_H
#define AUDIO_PW_V3_H

// Audio driver shared-memory API — apps include this header and link with -lrt only.
// No PipeWire headers required by client apps.

#include <cstdint>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libgen.h>
#include <limits.h>
#include <cstdio>
#include <chrono>
#include <thread>

// ── Fixed output format ────────────────────────────────────────────────────
// All PCM data in the ring must be F32 interleaved stereo at this rate.
// Apps use swresample (or any resampler) to produce this format.
#define AUDIO_SAMPLE_RATE   48000u
#define AUDIO_CHANNELS      2u

// PCM ring: 2 seconds = 48000 * 2 * 2 = 192 000 F32 samples (~768 KB in shm)
#define AUDIO_RING_FRAMES   (AUDIO_SAMPLE_RATE * 2u)
#define AUDIO_RING_SAMPLES  (AUDIO_RING_FRAMES * AUDIO_CHANNELS)

// SFX command queue capacity
#define AUDIO_CMD_QUEUE_LEN  32u
#define AUDIO_CMD_MAX_PATH   256u

// Shared memory names
#define AUDIO_V3_PCM_SHM    "/audio_v3_pcm"
#define AUDIO_V3_CMD_SHM    "/audio_v3_cmd"
#define AUDIO_V3_STATS_SHM  "/audio_v3_stats"

// ── PCM streaming ring ─────────────────────────────────────────────────────
// Single-producer (app) / single-consumer (driver) ring.
// Apps write via audio_v3::pushPcm().  Driver reads in PipeWire RT callback.
struct AudioPcmRing {
    std::atomic<uint32_t> writeHead;   // app advances after writing
    std::atomic<uint32_t> readHead;    // driver advances after reading
    float data[AUDIO_RING_SAMPLES];
};

// ── Command queue (app → driver) ──────────────────────────────────────────
enum AudioCmdType : uint32_t {
    AUDIO_CMD_NONE          = 0,
    AUDIO_CMD_PLAY_WAV      = 1,   // play a WAV file (path in entry)
    AUDIO_CMD_BEEP          = 2,   // synthesize a sine-wave beep
    AUDIO_CMD_STOP_SFX      = 3,   // stop all active SFX immediately
    AUDIO_CMD_SFX_VOLUME    = 4,   // set SFX master volume (0.0–1.0)
    AUDIO_CMD_STREAM_VOLUME = 5,   // set streaming ring volume (0.0–1.0)
};

struct AudioCmdEntry {
    AudioCmdType type;
    char         path[AUDIO_CMD_MAX_PATH];  // PLAY_WAV: file path
    float        volume;                    // PLAY_WAV / SFX_VOLUME / STREAM_VOLUME
    float        frequency;                 // BEEP: Hz
    float        duration;                  // BEEP: seconds
    uint32_t     id;                        // sequence number — echoed in stats.lastCmdId
};

struct AudioCmdQueue {
    std::atomic<uint32_t> head;    // driver reads and advances
    std::atomic<uint32_t> tail;    // app writes and advances
    AudioCmdEntry entries[AUDIO_CMD_QUEUE_LEN];
};

// PipeWire stream state values (mirrors pw_stream_state enum)
enum AudioPwState : int32_t {
    AUDIO_PW_ERROR        = -1,
    AUDIO_PW_UNCONNECTED  =  0,
    AUDIO_PW_CONNECTING   =  1,
    AUDIO_PW_PAUSED       =  2,
    AUDIO_PW_STREAMING    =  3,
};

// ── Stats (driver → apps, read-only for apps) ─────────────────────────────
struct AudioStats {
    std::atomic<bool>     driverActive;
    std::atomic<uint32_t> sampleRate;
    std::atomic<uint32_t> channels;
    std::atomic<uint32_t> streamFillPct;  // 0–100: streaming ring fill level
    std::atomic<uint32_t> sfxActive;      // number of active SFX slots
    std::atomic<uint32_t> underruns;      // streaming ring underrun count
    std::atomic<uint32_t> lastCmdId;      // ID of most recently processed cmd
    // Volumes exposed so apps can read back current values
    std::atomic<uint32_t> streamVolumeX1000;  // streamVolume * 1000 (avoids atomic<float>)
    std::atomic<uint32_t> sfxVolumeX1000;     // sfxVolume * 1000
    // PipeWire connection diagnostics
    std::atomic<int32_t>  pwState;        // AudioPwState: see if stream reached STREAMING
    std::atomic<uint32_t> callbacksTotal; // total onProcess() calls — 0 means PW never fired
    std::atomic<uint32_t> samplesConsumed;// total samples drained from ring (wraps at 4G)
};

// ==========================================================================
//  Client API — header-only, zero PipeWire dependency
//  Link: -lrt    (for shm_open / mmap)
// ==========================================================================
namespace audio_v3 {

// ── Map / unmap helpers ───────────────────────────────────────────────────

inline AudioPcmRing* mapPcmRing() {
    int fd = shm_open(AUDIO_V3_PCM_SHM, O_RDWR, 0666);
    if (fd < 0) return nullptr;
    void* p = mmap(nullptr, sizeof(AudioPcmRing), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return (p == MAP_FAILED) ? nullptr : static_cast<AudioPcmRing*>(p);
}
inline void unmapPcmRing(AudioPcmRing* r) { if (r) munmap(r, sizeof(AudioPcmRing)); }

inline AudioCmdQueue* mapCmdQueue() {
    int fd = shm_open(AUDIO_V3_CMD_SHM, O_RDWR, 0666);
    if (fd < 0) return nullptr;
    void* p = mmap(nullptr, sizeof(AudioCmdQueue), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return (p == MAP_FAILED) ? nullptr : static_cast<AudioCmdQueue*>(p);
}
inline void unmapCmdQueue(AudioCmdQueue* q) { if (q) munmap(q, sizeof(AudioCmdQueue)); }

inline AudioStats* mapStats() {
    int fd = shm_open(AUDIO_V3_STATS_SHM, O_RDONLY, 0666);
    if (fd < 0) return nullptr;
    void* p = mmap(nullptr, sizeof(AudioStats), PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    return (p == MAP_FAILED) ? nullptr : static_cast<AudioStats*>(p);
}
inline void unmapStats(AudioStats* s) { if (s) munmap(s, sizeof(AudioStats)); }

// ── PCM streaming ─────────────────────────────────────────────────────────

// Push F32 interleaved stereo samples at AUDIO_SAMPLE_RATE into the ring.
// 'count' is the total number of samples (frames * AUDIO_CHANNELS).
// Returns number of samples actually written; 0 if ring is full or driver absent.
inline uint32_t pushPcm(AudioPcmRing* ring, const float* samples, uint32_t count) {
    if (!ring || !samples || !count) return 0;

    constexpr uint32_t cap = AUDIO_RING_SAMPLES;
    uint32_t wh  = ring->writeHead.load(std::memory_order_relaxed);
    uint32_t rh  = ring->readHead.load(std::memory_order_acquire);
    uint32_t occ = (wh - rh + cap) % cap;
    uint32_t avail = cap - 1u - occ;

    uint32_t n = (count < avail) ? count : avail;
    if (!n) return 0;

    uint32_t seg1 = cap - wh;
    if (seg1 > n) seg1 = n;
    uint32_t seg2 = n - seg1;

    memcpy(ring->data + wh, samples,        seg1 * sizeof(float));
    if (seg2)
        memcpy(ring->data,      samples + seg1, seg2 * sizeof(float));

    ring->writeHead.store((wh + n) % cap, std::memory_order_release);
    return n;
}

// ── Command helpers ───────────────────────────────────────────────────────

// Internal: push one entry onto the tail of the queue.
inline bool _enqueueCmd(AudioCmdQueue* q, const AudioCmdEntry& e) {
    if (!q) return false;
    uint32_t tail = q->tail.load(std::memory_order_relaxed);
    uint32_t head = q->head.load(std::memory_order_acquire);
    uint32_t next = (tail + 1u) % AUDIO_CMD_QUEUE_LEN;
    if (next == head) return false;  // queue full
    q->entries[tail] = e;
    q->tail.store(next, std::memory_order_release);
    return true;
}

// Play a WAV file.  volume 0.0–1.0.  id echoed in stats.lastCmdId.
inline bool playSfxWav(AudioCmdQueue* q, const char* path,
                       float volume = 1.0f, uint32_t id = 0) {
    AudioCmdEntry e{};
    e.type   = AUDIO_CMD_PLAY_WAV;
    e.volume = volume;
    e.id     = id;
    strncpy(e.path, path, AUDIO_CMD_MAX_PATH - 1);
    return _enqueueCmd(q, e);
}

// Synthesize a sine-wave beep.
inline bool playSfxBeep(AudioCmdQueue* q, float frequency, float duration,
                        float volume = 0.5f, uint32_t id = 0) {
    AudioCmdEntry e{};
    e.type      = AUDIO_CMD_BEEP;
    e.frequency = frequency;
    e.duration  = duration;
    e.volume    = volume;
    e.id        = id;
    return _enqueueCmd(q, e);
}

// Stop all active SFX immediately.
inline bool stopAllSfx(AudioCmdQueue* q) {
    AudioCmdEntry e{};
    e.type = AUDIO_CMD_STOP_SFX;
    return _enqueueCmd(q, e);
}

// Set SFX master volume (0.0–1.0).
inline bool setSfxVolume(AudioCmdQueue* q, float volume) {
    AudioCmdEntry e{};
    e.type   = AUDIO_CMD_SFX_VOLUME;
    e.volume = volume;
    return _enqueueCmd(q, e);
}

// Set streaming ring volume (0.0–1.0).
inline bool setStreamVolume(AudioCmdQueue* q, float volume) {
    AudioCmdEntry e{};
    e.type   = AUDIO_CMD_STREAM_VOLUME;
    e.volume = volume;
    return _enqueueCmd(q, e);
}

// ── On-demand driver launch helpers ──────────────────────────────────────────

// Find the audio_main_v3 binary.
// Search order:
//   1. /usr/local/bin/audio_main_v3  (system install, preferred)
//   2. ../../Drivers/audio_main_v3   (relative to calling app, for dev builds)
// Returns a pointer to a static buffer, or nullptr if not found.
inline const char* findAudioDriver() {
    if (access("/usr/local/bin/audio_main_v3", X_OK) == 0)
        return "/usr/local/bin/audio_main_v3";

    static char path[PATH_MAX];
    char exePath[PATH_MAX];
    char dirBuf[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) {
        exePath[len] = '\0';
        strncpy(dirBuf, exePath, sizeof(dirBuf));
        const char* dir = dirname(dirBuf);
        snprintf(path, sizeof(path), "%s/../../Drivers/audio_main_v3", dir);
        if (access(path, X_OK) == 0)
            return path;
    }
    return nullptr;
}

// Spawn audio_main_v3 as a fully detached daemon (survives the calling app).
// Returns true if fork succeeded (does not wait for driver to be ready).
inline bool spawnDriver(const char* path) {
    if (!path || access(path, X_OK) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) return false;   // fork failed
    if (pid > 0) return true;    // parent: done

    // Child: detach completely so it outlives the caller
    setsid();
    int devnull = open("/dev/null", O_RDWR);
    if (devnull >= 0) {
        dup2(devnull, STDIN_FILENO);
        dup2(devnull, STDOUT_FILENO);
        dup2(devnull, STDERR_FILENO);
        close(devnull);
    }
    execl(path, path, "--no-stats", nullptr);
    _exit(1);  // execl failed
}

// Ensure the audio driver is running and return a mapped PCM ring.
// If the driver is not running, it is located and spawned automatically.
// Polls until the driver signals driverActive=true or timeoutMs elapses.
// Returns nullptr if the driver cannot be found, spawned, or timed out.
inline AudioPcmRing* ensureAudio(int timeoutMs = 3000) {
    // Already running?
    AudioPcmRing* ring = mapPcmRing();
    if (ring) return ring;

    // Find and spawn
    const char* driverPath = findAudioDriver();
    if (!driverPath) return nullptr;
    if (!spawnDriver(driverPath)) return nullptr;

    // Wait for driver to create shm and reach driverActive=true
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        ring = mapPcmRing();
        if (ring) {
            AudioStats* s = mapStats();
            if (s) {
                bool active = s->driverActive.load();
                unmapStats(s);
                if (active) return ring;
            }
            unmapPcmRing(ring);
            ring = nullptr;
        }
    }
    return nullptr;  // timed out
}

} // namespace audio_v3

#endif // AUDIO_PW_V3_H
