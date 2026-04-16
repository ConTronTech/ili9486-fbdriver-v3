#include "audio_pw_v3.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/utils/result.h>
#include <spa/utils/hook.h>

#include <iostream>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <fstream>

// ── SFX slot ──────────────────────────────────────────────────────────────
// Pre-allocated pool.  Loader thread fills a free slot then sets active=true.
// RT callback mixes from active slots and sets active=false when done.
// No locks needed: each slot is written by exactly one thread at a time.
static constexpr int MAX_SFX_SLOTS = 8;

struct SfxSlot {
    std::vector<float> pcm;      // F32 interleaved stereo at AUDIO_SAMPLE_RATE
    size_t             cursor;
    float              volume;
    std::atomic<bool>  active{false};
};

// ── WAV parser ────────────────────────────────────────────────────────────

struct WavHeader {
    uint32_t audioFormat;   // 1 = PCM, 3 = IEEE float
    uint32_t numChannels;
    uint32_t sampleRate;
    uint32_t bitsPerSample;
};

static bool readWavHeader(std::ifstream& f, WavHeader& hdr, uint32_t& dataBytes) {
    auto r32 = [&](uint32_t& v) {
        return (bool)f.read(reinterpret_cast<char*>(&v), 4);
    };
    auto r16 = [&](uint32_t& v) {
        uint16_t x = 0;
        bool ok = (bool)f.read(reinterpret_cast<char*>(&x), 2);
        v = x;
        return ok;
    };

    // RIFF header
    char tag[5]{};
    if (!f.read(tag, 4)) return false;
    if (memcmp(tag, "RIFF", 4) != 0) return false;
    uint32_t riffSize; r32(riffSize);
    if (!f.read(tag, 4)) return false;
    if (memcmp(tag, "WAVE", 4) != 0) return false;

    bool gotFmt = false;
    dataBytes = 0;

    while (f.read(tag, 4)) {
        tag[4] = '\0';
        uint32_t chunkSize; r32(chunkSize);
        std::streampos chunkStart = f.tellg();

        if (memcmp(tag, "fmt ", 4) == 0) {
            r16(hdr.audioFormat);
            r16(hdr.numChannels);
            r32(hdr.sampleRate);
            uint32_t byteRate; r32(byteRate);
            uint32_t blockAlign; r16(blockAlign);
            r16(hdr.bitsPerSample);
            gotFmt = true;
        } else if (memcmp(tag, "data", 4) == 0) {
            dataBytes = chunkSize;
            return gotFmt;  // data chunk begins here — leave file at its start
        }

        // Skip to next chunk (align to 2-byte boundary)
        uint32_t skip = chunkSize + (chunkSize & 1);
        f.seekg(chunkStart + (std::streamoff)skip);
    }
    return false;
}

// Decode WAV file → F32 interleaved stereo at AUDIO_SAMPLE_RATE.
// Returns empty vector on failure.
static std::vector<float> decodeWav(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "[AUDIO] Cannot open WAV: " << path << std::endl;
        return {};
    }

    WavHeader hdr{};
    uint32_t dataBytes = 0;
    if (!readWavHeader(f, hdr, dataBytes)) {
        std::cerr << "[AUDIO] Bad WAV header: " << path << std::endl;
        return {};
    }

    if (hdr.numChannels < 1 || hdr.numChannels > 8) {
        std::cerr << "[AUDIO] Unsupported channel count: " << hdr.numChannels << std::endl;
        return {};
    }
    if (hdr.bitsPerSample != 8 && hdr.bitsPerSample != 16 &&
        hdr.bitsPerSample != 24 && hdr.bitsPerSample != 32) {
        std::cerr << "[AUDIO] Unsupported bits/sample: " << hdr.bitsPerSample << std::endl;
        return {};
    }

    uint32_t bytesPerSample = hdr.bitsPerSample / 8;
    uint32_t totalSamples   = dataBytes / bytesPerSample;
    uint32_t inFrames       = totalSamples / hdr.numChannels;

    // Read raw bytes
    std::vector<uint8_t> raw(dataBytes);
    if (!f.read(reinterpret_cast<char*>(raw.data()), dataBytes)) {
        std::cerr << "[AUDIO] Short read: " << path << std::endl;
        return {};
    }

    // Convert to F32 mono/stereo intermediate (keep up to 2ch; downmix extras)
    std::vector<float> fStereo(inFrames * 2);

    for (uint32_t fr = 0; fr < inFrames; fr++) {
        float ch[2] = {0.f, 0.f};
        for (uint32_t c = 0; c < hdr.numChannels; c++) {
            uint32_t byteOff = (fr * hdr.numChannels + c) * bytesPerSample;
            float s = 0.f;
            if (hdr.audioFormat == 3 && hdr.bitsPerSample == 32) {
                // IEEE float
                memcpy(&s, raw.data() + byteOff, 4);
            } else if (hdr.bitsPerSample == 8) {
                s = (raw[byteOff] - 128) / 128.0f;
            } else if (hdr.bitsPerSample == 16) {
                int16_t v;
                memcpy(&v, raw.data() + byteOff, 2);
                s = v / 32768.0f;
            } else if (hdr.bitsPerSample == 24) {
                int32_t v = (int8_t)raw[byteOff + 2];
                v = (v << 8) | raw[byteOff + 1];
                v = (v << 8) | raw[byteOff + 0];
                s = v / 8388608.0f;
            } else { // 32-bit PCM signed
                int32_t v;
                memcpy(&v, raw.data() + byteOff, 4);
                s = v / 2147483648.0f;
            }
            if (c < 2) ch[c] = s;
        }
        // Mono → stereo upmix
        if (hdr.numChannels == 1) ch[1] = ch[0];
        fStereo[fr * 2]     = ch[0];
        fStereo[fr * 2 + 1] = ch[1];
    }

    // Resample to AUDIO_SAMPLE_RATE if needed (linear interpolation — fine for SFX)
    if (hdr.sampleRate == AUDIO_SAMPLE_RATE) return fStereo;

    uint32_t outFrames = (uint32_t)((double)inFrames * AUDIO_SAMPLE_RATE / hdr.sampleRate + 0.5);
    std::vector<float> out(outFrames * 2);
    for (uint32_t i = 0; i < outFrames; i++) {
        double pos  = (double)i * hdr.sampleRate / AUDIO_SAMPLE_RATE;
        uint32_t i0 = (uint32_t)pos;
        uint32_t i1 = std::min(i0 + 1u, inFrames - 1u);
        float frac  = (float)(pos - i0);
        out[i*2]   = fStereo[i0*2]   + frac * (fStereo[i1*2]   - fStereo[i0*2]);
        out[i*2+1] = fStereo[i0*2+1] + frac * (fStereo[i1*2+1] - fStereo[i0*2+1]);
    }
    return out;
}

// Generate a sine-wave beep, F32 stereo at AUDIO_SAMPLE_RATE with 10ms fade in/out.
static std::vector<float> generateBeep(float frequency, float duration, float volume) {
    uint32_t frames = (uint32_t)(AUDIO_SAMPLE_RATE * duration);
    std::vector<float> out(frames * 2);
    uint32_t fade = std::min(frames / 2u, (uint32_t)(AUDIO_SAMPLE_RATE * 0.01f)); // 10ms

    for (uint32_t i = 0; i < frames; i++) {
        float env = 1.0f;
        if (i < fade)             env = (float)i / fade;
        else if (i >= frames - fade) env = (float)(frames - i) / fade;

        float s = volume * env * sinf(2.0f * (float)M_PI * frequency * i / AUDIO_SAMPLE_RATE);
        out[i*2]     = s;
        out[i*2 + 1] = s;
    }
    return out;
}

// ── AudioDriverV3 ─────────────────────────────────────────────────────────

class AudioDriverV3 {
public:
    AudioDriverV3() = default;
    ~AudioDriverV3() { stop(); destroySharedMemory(); }

    bool init(float streamVolume = 1.0f, float sfxVolume = 1.0f,
              const std::string& targetDevice = "");
    void stop();

    AudioStats* getStats() { return stats; }

private:
    // Shared memory
    int pcmShmFd   = -1;
    int cmdShmFd   = -1;
    int statsShmFd = -1;
    AudioPcmRing*  pcmRing  = nullptr;
    AudioCmdQueue* cmdQueue = nullptr;
    AudioStats*    stats    = nullptr;

    bool createSharedMemory();
    void destroySharedMemory();

    // PipeWire
    struct pw_thread_loop* pwLoop   = nullptr;
    struct pw_stream*      pwStream = nullptr;
    struct spa_hook        pwListener{};

    static void onProcess(void* data);
    static void onStateChanged(void* data, enum pw_stream_state old,
                               enum pw_stream_state state, const char* error);
    static const struct pw_stream_events streamEvents;

    // SFX
    SfxSlot sfxSlots[MAX_SFX_SLOTS];
    std::atomic<float> sfxVol{1.0f};
    std::atomic<float> streamVol{1.0f};
    uint32_t underrunCount = 0;

    // SFX command-processing thread (non-RT)
    std::thread   cmdThread;
    std::atomic<bool> running{false};
    void cmdLoop();

    // Find a free SFX slot; returns index or -1 if all busy
    int findFreeSlot() {
        for (int i = 0; i < MAX_SFX_SLOTS; i++)
            if (!sfxSlots[i].active.load(std::memory_order_acquire))
                return i;
        return -1;
    }
};

const struct pw_stream_events AudioDriverV3::streamEvents = {
    .version       = PW_VERSION_STREAM_EVENTS,
    .state_changed = AudioDriverV3::onStateChanged,
    .process       = AudioDriverV3::onProcess,
};

static const char* pwStateName(enum pw_stream_state s) {
    switch (s) {
    case PW_STREAM_STATE_ERROR:        return "ERROR";
    case PW_STREAM_STATE_UNCONNECTED:  return "UNCONNECTED";
    case PW_STREAM_STATE_CONNECTING:   return "CONNECTING";
    case PW_STREAM_STATE_PAUSED:       return "PAUSED";
    case PW_STREAM_STATE_STREAMING:    return "STREAMING";
    default:                           return "UNKNOWN";
    }
}

void AudioDriverV3::onStateChanged(void* data, enum pw_stream_state /*old*/,
                                   enum pw_stream_state state, const char* error) {
    auto* d = static_cast<AudioDriverV3*>(data);

    // Write state into stats so the main thread can display it
    if (d->stats) d->stats->pwState.store((int32_t)state, std::memory_order_relaxed);

    // Can't use std::cout from RT context safely, but state_changed fires from
    // the pw_thread_loop thread, NOT the RT process thread, so it's fine here.
    std::cout << "\n[AUDIO] PipeWire state: " << pwStateName(state);
    if (state == PW_STREAM_STATE_ERROR && error)
        std::cout << "  error='" << error << "'";
    std::cout << std::endl;

    if (state == PW_STREAM_STATE_STREAMING) {
        // Log which node we actually connected to
        uint32_t nodeId = pw_stream_get_node_id(d->pwStream);
        std::cout << "[AUDIO] Connected to PipeWire node id=" << nodeId
                  << " — onProcess() will now fire\n";
        std::cout << "[AUDIO] If no sound: check 'pactl info | grep Default Sink'" << std::endl;
    }
}

// RT callback: read ring → mix SFX → fill PipeWire buffer
void AudioDriverV3::onProcess(void* data) {
    auto* d = static_cast<AudioDriverV3*>(data);

    struct pw_buffer* pwbuf = pw_stream_dequeue_buffer(d->pwStream);
    if (!pwbuf) return;

    struct spa_buffer* spabuf = pwbuf->buffer;
    float* dst = static_cast<float*>(spabuf->datas[0].data);
    if (!dst) { pw_stream_queue_buffer(d->pwStream, pwbuf); return; }

    const uint32_t stride    = sizeof(float) * AUDIO_CHANNELS;
    const uint32_t maxFrames = spabuf->datas[0].maxsize / stride;
    uint32_t nFrames = pwbuf->requested ? (uint32_t)pwbuf->requested : maxFrames;
    if (nFrames > maxFrames) nFrames = maxFrames;
    const uint32_t nSamples = nFrames * AUDIO_CHANNELS;

    memset(dst, 0, nSamples * sizeof(float));

    // ── Streaming ring ──────────────────────────────────────────────────
    AudioPcmRing* ring = d->pcmRing;
    float svol = d->streamVol.load(std::memory_order_relaxed);

    constexpr uint32_t cap = AUDIO_RING_SAMPLES;
    uint32_t rh  = ring->readHead.load(std::memory_order_acquire);
    uint32_t wh  = ring->writeHead.load(std::memory_order_acquire);
    uint32_t occ = (wh - rh + cap) % cap;
    uint32_t toRead = std::min(occ, nSamples);

    if (toRead < nSamples) d->underrunCount++;

    uint32_t seg1 = std::min(cap - rh, toRead);
    uint32_t seg2 = toRead - seg1;
    for (uint32_t i = 0; i < seg1; i++) dst[i]        += ring->data[rh + i] * svol;
    for (uint32_t i = 0; i < seg2; i++) dst[seg1 + i] += ring->data[i]      * svol;
    ring->readHead.store((rh + toRead) % cap, std::memory_order_release);

    // ── SFX mix ─────────────────────────────────────────────────────────
    float sfxvol = d->sfxVol.load(std::memory_order_relaxed);
    uint32_t activeSfx = 0;
    for (int i = 0; i < MAX_SFX_SLOTS; i++) {
        SfxSlot& slot = d->sfxSlots[i];
        if (!slot.active.load(std::memory_order_acquire)) continue;
        activeSfx++;

        float vol = slot.volume * sfxvol;
        uint32_t avail = (uint32_t)(slot.pcm.size() - slot.cursor);
        uint32_t toMix = std::min(avail, nSamples);
        for (uint32_t s = 0; s < toMix; s++)
            dst[s] += slot.pcm[slot.cursor + s] * vol;
        slot.cursor += toMix;
        if (slot.cursor >= slot.pcm.size())
            slot.active.store(false, std::memory_order_release);
    }

    // ── Stats ────────────────────────────────────────────────────────────
    if (d->stats) {
        d->stats->sfxActive.store(activeSfx,        std::memory_order_relaxed);
        d->stats->underruns.store(d->underrunCount,  std::memory_order_relaxed);
        uint32_t occ2 = (ring->writeHead.load() - ring->readHead.load() + cap) % cap;
        d->stats->streamFillPct.store(occ2 * 100u / cap, std::memory_order_relaxed);
        d->stats->callbacksTotal.fetch_add(1,        std::memory_order_relaxed);
        d->stats->samplesConsumed.fetch_add(toRead,  std::memory_order_relaxed);
    }

    spabuf->datas[0].chunk->offset = 0;
    spabuf->datas[0].chunk->stride = (int32_t)stride;
    spabuf->datas[0].chunk->size   = nSamples * sizeof(float);
    pw_stream_queue_buffer(d->pwStream, pwbuf);
}

// Non-RT command loop: decode WAV/BEEP, populate SFX slots
void AudioDriverV3::cmdLoop() {
    while (running.load()) {
        uint32_t head = cmdQueue->head.load(std::memory_order_acquire);
        uint32_t tail = cmdQueue->tail.load(std::memory_order_acquire);

        if (head == tail) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        const AudioCmdEntry& e = cmdQueue->entries[head];

        switch (e.type) {
        case AUDIO_CMD_PLAY_WAV: {
            auto pcm = decodeWav(e.path);
            if (!pcm.empty()) {
                int slot = findFreeSlot();
                if (slot >= 0) {
                    sfxSlots[slot].pcm    = std::move(pcm);
                    sfxSlots[slot].cursor = 0;
                    sfxSlots[slot].volume = e.volume;
                    sfxSlots[slot].active.store(true, std::memory_order_release);
                } else {
                    std::cerr << "[AUDIO] All SFX slots busy — dropping: " << e.path << std::endl;
                }
            }
            break;
        }
        case AUDIO_CMD_BEEP: {
            int slot = findFreeSlot();
            if (slot >= 0) {
                sfxSlots[slot].pcm    = generateBeep(e.frequency, e.duration, 1.0f);
                sfxSlots[slot].cursor = 0;
                sfxSlots[slot].volume = e.volume;
                sfxSlots[slot].active.store(true, std::memory_order_release);
            }
            break;
        }
        case AUDIO_CMD_STOP_SFX:
            for (int i = 0; i < MAX_SFX_SLOTS; i++)
                sfxSlots[i].active.store(false, std::memory_order_release);
            break;
        case AUDIO_CMD_SFX_VOLUME:
            sfxVol.store(std::max(0.f, std::min(1.f, e.volume)));
            if (stats) stats->sfxVolumeX1000.store((uint32_t)(e.volume * 1000.f));
            break;
        case AUDIO_CMD_STREAM_VOLUME:
            streamVol.store(std::max(0.f, std::min(1.f, e.volume)));
            if (stats) stats->streamVolumeX1000.store((uint32_t)(e.volume * 1000.f));
            break;
        default:
            break;
        }

        if (stats) stats->lastCmdId.store(e.id);
        cmdQueue->head.store((head + 1u) % AUDIO_CMD_QUEUE_LEN, std::memory_order_release);
    }
}

bool AudioDriverV3::createSharedMemory() {
    auto makeShm = [](const char* name, size_t size, int& fdOut, void*& ptrOut) {
        shm_unlink(name);  // clean up any stale segment
        int fd = shm_open(name, O_CREAT | O_RDWR, 0666);
        if (fd < 0) { std::cerr << "[AUDIO] shm_open failed: " << name << std::endl; return false; }
        if (ftruncate(fd, (off_t)size) < 0) {
            std::cerr << "[AUDIO] ftruncate failed: " << name << std::endl;
            close(fd); return false;
        }
        void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p == MAP_FAILED) {
            std::cerr << "[AUDIO] mmap failed: " << name << std::endl;
            close(fd); return false;
        }
        memset(p, 0, size);
        fdOut  = fd;
        ptrOut = p;
        return true;
    };

    void* p0 = nullptr, *p1 = nullptr, *p2 = nullptr;
    if (!makeShm(AUDIO_V3_PCM_SHM,   sizeof(AudioPcmRing),  pcmShmFd,   p0)) return false;
    if (!makeShm(AUDIO_V3_CMD_SHM,   sizeof(AudioCmdQueue), cmdShmFd,   p1)) return false;
    if (!makeShm(AUDIO_V3_STATS_SHM, sizeof(AudioStats),    statsShmFd, p2)) return false;

    pcmRing  = static_cast<AudioPcmRing*>(p0);
    cmdQueue = static_cast<AudioCmdQueue*>(p1);
    stats    = static_cast<AudioStats*>(p2);
    return true;
}

void AudioDriverV3::destroySharedMemory() {
    if (pcmRing)  { munmap(pcmRing,  sizeof(AudioPcmRing));  pcmRing  = nullptr; }
    if (cmdQueue) { munmap(cmdQueue, sizeof(AudioCmdQueue)); cmdQueue = nullptr; }
    if (stats)    { munmap(stats,    sizeof(AudioStats));    stats    = nullptr; }
    if (pcmShmFd   >= 0) { close(pcmShmFd);   shm_unlink(AUDIO_V3_PCM_SHM);   pcmShmFd   = -1; }
    if (cmdShmFd   >= 0) { close(cmdShmFd);   shm_unlink(AUDIO_V3_CMD_SHM);   cmdShmFd   = -1; }
    if (statsShmFd >= 0) { close(statsShmFd); shm_unlink(AUDIO_V3_STATS_SHM); statsShmFd = -1; }
}

bool AudioDriverV3::init(float streamVolume, float sfxVolume,
                         const std::string& targetDevice) {
    if (!createSharedMemory()) return false;

    streamVol.store(streamVolume);
    sfxVol.store(sfxVolume);
    stats->streamVolumeX1000.store((uint32_t)(streamVolume * 1000.f));
    stats->sfxVolumeX1000.store((uint32_t)(sfxVolume * 1000.f));
    stats->sampleRate.store(AUDIO_SAMPLE_RATE);
    stats->channels.store(AUDIO_CHANNELS);
    stats->pwState.store((int32_t)PW_STREAM_STATE_UNCONNECTED);

    pw_init(nullptr, nullptr);
    std::cout << "[AUDIO] PipeWire version: " << pw_get_library_version() << std::endl;

    pwLoop = pw_thread_loop_new("audio-driver", nullptr);
    if (!pwLoop) { std::cerr << "[AUDIO] Failed to create PipeWire thread loop" << std::endl; return false; }

    struct pw_properties* props = pw_properties_new(
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE,     "Music",
        nullptr
    );

    // Force a specific output device (e.g. bcm2835 headphones on Pi aux jack).
    // Set audio_device in settings.conf.  Leave blank to use PipeWire default.
    if (!targetDevice.empty()) {
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, targetDevice.c_str());
        std::cout << "[AUDIO] Target device: " << targetDevice << std::endl;
    } else {
        std::cout << "[AUDIO] Target device: (PipeWire default — may be HDMI on Pi)" << std::endl;
        std::cout << "[AUDIO] To force aux jack, set audio_device in settings.conf" << std::endl;
        std::cout << "[AUDIO]   Run: pactl list sinks short   to see available sinks" << std::endl;
    }
    pwStream = pw_stream_new_simple(
        pw_thread_loop_get_loop(pwLoop),
        "audio-driver-v3",
        props,
        &streamEvents,
        this
    );
    if (!pwStream) {
        std::cerr << "[AUDIO] Failed to create PipeWire stream" << std::endl;
        pw_thread_loop_destroy(pwLoop); pwLoop = nullptr;
        return false;
    }

    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    struct spa_audio_info_raw info{};
    info.format      = SPA_AUDIO_FORMAT_F32;
    info.rate        = AUDIO_SAMPLE_RATE;
    info.channels    = AUDIO_CHANNELS;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;

    const struct spa_pod* params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    pw_stream_add_listener(pwStream, &pwListener, &streamEvents, this);

    int ret = pw_stream_connect(
        pwStream,
        PW_DIRECTION_OUTPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(
            PW_STREAM_FLAG_AUTOCONNECT |
            PW_STREAM_FLAG_MAP_BUFFERS |
            PW_STREAM_FLAG_RT_PROCESS),
        params, 1
    );
    if (ret < 0) {
        std::cerr << "[AUDIO] pw_stream_connect: " << spa_strerror(ret) << std::endl;
        pw_stream_destroy(pwStream); pwStream = nullptr;
        pw_thread_loop_destroy(pwLoop); pwLoop = nullptr;
        return false;
    }

    running.store(true);
    cmdThread = std::thread(&AudioDriverV3::cmdLoop, this);
    pw_thread_loop_start(pwLoop);

    stats->driverActive.store(true);
    std::cout << "[AUDIO] Driver started — " << AUDIO_SAMPLE_RATE << "Hz stereo F32" << std::endl;
    std::cout << "[AUDIO] PCM ring:  " << AUDIO_V3_PCM_SHM << " (" << sizeof(AudioPcmRing)/1024 << "KB)" << std::endl;
    std::cout << "[AUDIO] Cmd queue: " << AUDIO_V3_CMD_SHM  << std::endl;
    std::cout << "[AUDIO] Stats:     " << AUDIO_V3_STATS_SHM << std::endl;
    return true;
}

void AudioDriverV3::stop() {
    if (stats) stats->driverActive.store(false);

    running.store(false);
    if (cmdThread.joinable()) cmdThread.join();

    if (pwLoop)   pw_thread_loop_stop(pwLoop);
    if (pwStream) { pw_stream_destroy(pwStream); pwStream = nullptr; }
    if (pwLoop)   { pw_thread_loop_destroy(pwLoop); pwLoop = nullptr; }
    pw_deinit();
}

// ── Public factory (used by audio_main_v3.cpp) ────────────────────────────
AudioDriverV3* g_audioDriver = nullptr;

extern "C" {
    AudioDriverV3* audio_v3_create()                               { return new AudioDriverV3(); }
    bool           audio_v3_init(AudioDriverV3* d, float sv,
                                 float sfxv, const char* dev)      { return d->init(sv, sfxv, dev ? dev : ""); }
    void           audio_v3_stop(AudioDriverV3* d)                 { d->stop(); }
    void           audio_v3_destroy(AudioDriverV3* d)              { delete d; }
    AudioStats*    audio_v3_get_stats(AudioDriverV3* d)            { return d->getStats(); }
}
