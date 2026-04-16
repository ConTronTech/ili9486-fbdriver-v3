#include <iostream>
#include <cstring>
#include <signal.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <libgen.h>
#include <limits.h>
#include <cstdio>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/imgutils.h>
#include <libavutil/rational.h>
#include <libavutil/channel_layout.h>
}

#include "../../Drivers/audio_pw_v3.h"

#include <vector>

#define FB_WIDTH   320
#define FB_HEIGHT  480
#define FB_V3_SHM_NAME_0 "/ili9486_fb_v3_0"
#define FB_V3_SHM_NAME_1 "/ili9486_fb_v3_1"
#define FB_V3_STATS_SHM_NAME "/ili9486_fb_v3_stats"

// Stats structure — MUST match driver definition in fb_ili9486_v3.h exactly.
struct FramebufferStats {
    std::atomic<uint64_t> totalFrames;
    std::atomic<uint64_t> droppedFrames;
    std::atomic<uint32_t> currentFps;
    std::atomic<uint32_t> dirtyPixelCount;
    std::atomic<uint32_t> avgUpdateTimeUs;
    std::atomic<bool> driverActive;
    std::atomic<bool> frameInProgress;
    std::atomic<uint32_t> activeBufferIndex;
    std::atomic<bool> buffer0Done;
    std::atomic<bool> buffer1Done;
    std::atomic<uint32_t> cpuTempMilliC;
    std::atomic<uint32_t> thermalState;
};

enum ScaleMode {
    FIT,      // Scale to fit inside display (maintain aspect ratio, letterbox)
    FILL      // Scale to fill display (maintain aspect ratio, crop edges)
};

static const char* PID_FILE = "/tmp/video_player_v3.pid";

static pid_t checkExistingInstance() {
    FILE* f = fopen(PID_FILE, "r");
    if (!f) return 0;
    pid_t pid = 0;
    bool ok = (fscanf(f, "%d", &pid) == 1);
    fclose(f);
    if (ok && pid > 0 && kill(pid, 0) == 0) return pid;
    unlink(PID_FILE);
    return 0;
}

static void writePidFile() {
    FILE* f = fopen(PID_FILE, "w");
    if (f) { fprintf(f, "%d\n", (int)getpid()); fclose(f); }
}

static std::atomic<bool> running{true};
static void handleSignal(int) { unlink(PID_FILE); running.store(false); }

uint16_t* mapBackBuffer() {
    int stats_fd = shm_open(FB_V3_STATS_SHM_NAME, O_RDONLY, 0666);
    if (stats_fd < 0) return nullptr;

    FramebufferStats* stats = (FramebufferStats*)mmap(nullptr, sizeof(FramebufferStats),
                                                        PROT_READ, MAP_SHARED, stats_fd, 0);
    close(stats_fd);
    if (stats == MAP_FAILED) return nullptr;

    uint32_t activeIdx = stats->activeBufferIndex.load();
    munmap(stats, sizeof(FramebufferStats));

    const char* shmName = (activeIdx == 0) ? FB_V3_SHM_NAME_1 : FB_V3_SHM_NAME_0;
    int fd = shm_open(shmName, O_RDWR, 0666);
    if (fd < 0) return nullptr;

    void* addr = mmap(nullptr, FB_WIDTH * FB_HEIGHT * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return (addr == MAP_FAILED) ? nullptr : (uint16_t*)addr;
}

void unmapFramebuffer(uint16_t* fb) {
    if (fb) munmap(fb, FB_WIDTH * FB_HEIGHT * 2);
}

FramebufferStats* mapStats() {
    int fd = shm_open(FB_V3_STATS_SHM_NAME, O_RDWR, 0666);
    if (fd < 0) return nullptr;
    void* addr = mmap(nullptr, sizeof(FramebufferStats), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    return (addr == MAP_FAILED) ? nullptr : (FramebufferStats*)addr;
}

void unmapStats(FramebufferStats* stats) {
    if (stats) munmap(stats, sizeof(FramebufferStats));
}

void swapBuffers() {
    FramebufferStats* stats = mapStats();
    if (stats) {
        uint32_t current = stats->activeBufferIndex.load();
        stats->activeBufferIndex.store(1 - current);
        unmapStats(stats);
    }
}

// The ILI9486 panel has a BGR physical subpixel layout but MADCTL BGR=0 is set in the
// driver. The net effect is that R and B are physically swapped on the panel. This
// function compensates by placing blue data in bits 15-11 and red data in bits 4-0,
// so the panel displays the correct colours.
inline uint16_t rgb24_to_bgr565(uint8_t r, uint8_t g, uint8_t b) {
    return ((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3);
}

// Parse configuration file and return orientation value.
// Searches relative to the binary's own location so it works from any CWD.
int parseConfigFile(const char* configPath = nullptr) {
    char exePath[PATH_MAX];
    char dirBuf[PATH_MAX];
    char resolvedPath[PATH_MAX];

    const char* path = configPath;
    if (!path) {
        ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
        if (len > 0) {
            exePath[len] = '\0';
            strncpy(dirBuf, exePath, sizeof(dirBuf));
            const char* dir = dirname(dirBuf);
            snprintf(resolvedPath, sizeof(resolvedPath),
                     "%s/../../Drivers/settings.conf", dir);
            path = resolvedPath;
        } else {
            path = "../../Drivers/settings.conf";
        }
    }

    std::ifstream configFile(path);
    if (!configFile.is_open()) {
        std::cerr << "[VIDEO] Warning: Could not open config file: " << path << std::endl;
        std::cerr << "[VIDEO] Using default orientation: 0 (landscape)" << std::endl;
        return 0;
    }

    int orientation = 0;
    std::string line;

    while (std::getline(configFile, line)) {
        if (line.empty() || line[0] == '#') continue;

        std::istringstream iss(line);
        std::string key;
        char equals;
        int value;

        if (iss >> key >> equals >> value && equals == '=') {
            if (key == "orientation") {
                orientation = value;
                std::cout << "[VIDEO] Config: orientation = " << orientation << "°" << std::endl;
                break;
            }
        }
    }

    configFile.close();
    return orientation;
}

void clearBuffer(uint16_t* fb) {
    if (fb) {
        memset(fb, 0, FB_WIDTH * FB_HEIGHT * 2);
    }
}

// ── Audio background decode thread ──────────────────────────────────────────
// Runs independently of the video render loop so PTS sleeps never starve the ring.

struct AudioPktQueue {
    std::deque<AVPacket*>   pkts;
    std::mutex              mtx;
    std::condition_variable cv;
    std::atomic<bool>       eof{false};
    static constexpr int    MAX_PKTS = 256;  // ~6 seconds of pre-buffered packets
};

static void audioWorker(AudioPktQueue& q,
                        AVCodecContext* ctx, SwrContext* swr, AudioPcmRing* ring,
                        std::atomic<uint64_t>& totalPushed,
                        std::atomic<int>&      pktsRecv) {
    AVFrame* af = av_frame_alloc();

    while (true) {
        AVPacket* pkt = nullptr;
        {
            std::unique_lock<std::mutex> lk(q.mtx);
            q.cv.wait(lk, [&]{ return !q.pkts.empty() || q.eof.load() || !running.load(); });
            if (q.pkts.empty()) break;
            pkt = q.pkts.front();
            q.pkts.pop_front();
        }
        q.cv.notify_all();   // wake main thread if it was waiting for queue space

        if (avcodec_send_packet(ctx, pkt) >= 0) {
            while (avcodec_receive_frame(ctx, af) >= 0) {
                int outSamples = (int)av_rescale_rnd(
                    swr_get_delay(swr, af->sample_rate) + af->nb_samples,
                    (int64_t)AUDIO_SAMPLE_RATE, af->sample_rate, AV_ROUND_UP);

                std::vector<float> tmp((size_t)outSamples * AUDIO_CHANNELS);
                uint8_t* op = reinterpret_cast<uint8_t*>(tmp.data());
                int got = swr_convert(swr, &op, outSamples,
                                      (const uint8_t**)af->extended_data, af->nb_samples);
                if (got > 0) {
                    // Retry loop: if ring is full, wait for PipeWire to drain some
                    uint32_t toSend = (uint32_t)got * AUDIO_CHANNELS;
                    uint32_t sent   = 0;
                    while (sent < toSend && running.load()) {
                        uint32_t n = audio_v3::pushPcm(ring, tmp.data() + sent, toSend - sent);
                        if (n) { sent += n; }
                        else   { std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
                    }
                    totalPushed += sent;

                    int pn = ++pktsRecv;
                    if (pn <= 3) {
                        uint32_t fill = (ring->writeHead.load() - ring->readHead.load()
                                         + AUDIO_RING_SAMPLES) % AUDIO_RING_SAMPLES;
                        std::cout << "[AUDIO] pkt#" << pn
                                  << " decoded=" << got << " frames"
                                  << " pushed=" << sent << " samples"
                                  << " ring=" << fill * 100 / AUDIO_RING_SAMPLES << "%"
                                  << std::endl;
                    }
                }
                av_frame_unref(af);
            }
        }
        av_packet_free(&pkt);
    }

    av_frame_free(&af);
}
// ────────────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    pid_t existing = checkExistingInstance();
    if (existing) {
        std::cerr << "[VIDEO] Already running (PID " << existing << "). Kill it first:  kill " << existing << std::endl;
        return 1;
    }
    writePidFile();

    std::cout << R"(
╔═══════════════════════════════════════════════╗
║   Video Player V3 - FFmpeg Edition            ║
║   MP4/AVI/MKV with Intelligent Rotation       ║
╚═══════════════════════════════════════════════╝
)" << std::endl;

    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <video_file> [options]" << std::endl;
        std::cerr << "Options:" << std::endl;
        std::cerr << "  fit            Scale to fit (default, letterbox with black borders)" << std::endl;
        std::cerr << "  fill           Scale to fill (crop edges to fill screen)" << std::endl;
        std::cerr << "  -fps NUM       Override frame rate (default: auto-detect from video)" << std::endl;
        std::cerr << "  0 | 90 | 180 | 270    Display orientation (default: from settings.conf)" << std::endl;
        std::cerr << "\nNotes:" << std::endl;
        std::cerr << "  - Orientation is read from ../../Drivers/settings.conf by default" << std::endl;
        std::cerr << "  - Command-line orientation arguments override the config file" << std::endl;
        std::cerr << "\nExamples:" << std::endl;
        std::cerr << "  " << argv[0] << " video.mp4              # Fit mode, config orientation" << std::endl;
        std::cerr << "  " << argv[0] << " video.mp4 fill         # Fill mode (crop to fit)" << std::endl;
        std::cerr << "  " << argv[0] << " video.mp4 -fps 30      # Force 30 FPS" << std::endl;
        std::cerr << "  " << argv[0] << " video.mp4 fill 0       # Fill mode, landscape" << std::endl;
        std::cerr << "  " << argv[0] << " video.mp4 -fps 15 90   # 15 FPS, portrait (90°)" << std::endl;
        return 1;
    }

    const char* filename = argv[1];

    int orientation = parseConfigFile();
    double customFps = -1.0;
    ScaleMode scaleMode = FIT;

    for (int i = 2; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "fit") {
            scaleMode = FIT;
        } else if (arg == "fill") {
            scaleMode = FILL;
        } else if (arg == "-fps" && i + 1 < argc) {
            customFps = std::stod(argv[++i]);
        } else if (arg == "0" || arg == "90" || arg == "180" || arg == "270") {
            orientation = std::stoi(arg);
            std::cout << "[VIDEO] Orientation override: " << orientation << "°" << std::endl;
        }
    }

    std::cout << "[VIDEO] Initializing FFmpeg..." << std::endl;

    AVFormatContext* formatCtx = nullptr;
    if (avformat_open_input(&formatCtx, filename, nullptr, nullptr) < 0) {
        std::cerr << "[VIDEO] Failed to open: " << filename << std::endl;
        return 1;
    }

    if (avformat_find_stream_info(formatCtx, nullptr) < 0) {
        std::cerr << "[VIDEO] Failed to find stream info" << std::endl;
        avformat_close_input(&formatCtx);
        return 1;
    }

    int videoStream = -1;
    int audioStream = -1;
    AVCodecParameters* codecParams = nullptr;
    AVCodecParameters* audioParams = nullptr;
    for (unsigned i = 0; i < formatCtx->nb_streams; i++) {
        auto type = formatCtx->streams[i]->codecpar->codec_type;
        if (type == AVMEDIA_TYPE_VIDEO && videoStream == -1) {
            videoStream = i;
            codecParams = formatCtx->streams[i]->codecpar;
        } else if (type == AVMEDIA_TYPE_AUDIO && audioStream == -1) {
            audioStream = i;
            audioParams = formatCtx->streams[i]->codecpar;
        }
    }

    if (videoStream == -1) {
        std::cerr << "[VIDEO] No video stream found" << std::endl;
        avformat_close_input(&formatCtx);
        return 1;
    }

    const AVCodec* codec = avcodec_find_decoder(codecParams->codec_id);
    if (!codec) {
        std::cerr << "[VIDEO] Codec not found" << std::endl;
        avformat_close_input(&formatCtx);
        return 1;
    }

    AVCodecContext* codecCtx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codecCtx, codecParams);
    if (avcodec_open2(codecCtx, codec, nullptr) < 0) {
        std::cerr << "[VIDEO] Failed to open codec" << std::endl;
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return 1;
    }

    // ── Audio: connect to audio driver shared memory ─────────────────────────
    AVCodecContext*  audioCtx  = nullptr;
    SwrContext*      swrCtx    = nullptr;
    AudioPcmRing*    audioRing = nullptr;
    bool             hasAudio  = false;

    if (audioStream >= 0 && audioParams) {
        std::cout << "[AUDIO] Connecting to audio driver (spawning if needed)..." << std::endl;
        audioRing = audio_v3::ensureAudio(3000);
        if (!audioRing) {
            std::cerr << "[AUDIO] Could not start audio driver — audio disabled" << std::endl;
        } else {
            const AVCodec* acodec = avcodec_find_decoder(audioParams->codec_id);
            if (acodec) {
                audioCtx = avcodec_alloc_context3(acodec);
                avcodec_parameters_to_context(audioCtx, audioParams);
                if (avcodec_open2(audioCtx, acodec, nullptr) >= 0) {
                    AVChannelLayout outLayout{};
                    { AVChannelLayout tmp = AV_CHANNEL_LAYOUT_STEREO; outLayout = tmp; }

                    int ret = swr_alloc_set_opts2(&swrCtx,
                        &outLayout,            AV_SAMPLE_FMT_FLT, (int)AUDIO_SAMPLE_RATE,
                        &audioCtx->ch_layout,  audioCtx->sample_fmt, audioCtx->sample_rate,
                        0, nullptr);

                    if (ret >= 0 && swr_init(swrCtx) >= 0) {
                        hasAudio = true;
                        std::cout << "[AUDIO] " << acodec->name
                                  << " → F32 stereo " << AUDIO_SAMPLE_RATE << "Hz"
                                  << " via " << AUDIO_V3_PCM_SHM << std::endl;
                    } else {
                        std::cerr << "[AUDIO] swresample init failed — no audio" << std::endl;
                        if (swrCtx) { swr_free(&swrCtx); swrCtx = nullptr; }
                        avcodec_free_context(&audioCtx);
                        audio_v3::unmapPcmRing(audioRing); audioRing = nullptr;
                    }
                } else {
                    std::cerr << "[AUDIO] Codec open failed — no audio" << std::endl;
                    avcodec_free_context(&audioCtx);
                    audio_v3::unmapPcmRing(audioRing); audioRing = nullptr;
                }
            } else {
                std::cerr << "[AUDIO] Codec not found — no audio" << std::endl;
                audio_v3::unmapPcmRing(audioRing); audioRing = nullptr;
            }
        }
    } else {
        std::cout << "[AUDIO] No audio stream in file" << std::endl;
    }
    // ─────────────────────────────────────────────────────────────────────────

    bool displayIsLandscape = (orientation == 0 || orientation == 180);
    int actualLcdWidth  = displayIsLandscape ? 480 : 320;
    int actualLcdHeight = displayIsLandscape ? 320 : 480;

    float videoAspect   = (float)codecCtx->width / codecCtx->height;
    float displayAspect = (float)actualLcdWidth / actualLcdHeight;

    int targetWidth, targetHeight;
    int drawWidth, drawHeight;
    int offsetX = 0, offsetY = 0;

    if (scaleMode == FIT) {
        float scaleX = (float)actualLcdWidth  / codecCtx->width;
        float scaleY = (float)actualLcdHeight / codecCtx->height;
        float scale  = std::min(scaleX, scaleY);
        drawWidth  = (int)(codecCtx->width  * scale);
        drawHeight = (int)(codecCtx->height * scale);
        offsetX = (actualLcdWidth  - drawWidth)  / 2;
        offsetY = (actualLcdHeight - drawHeight) / 2;
        targetWidth  = drawWidth;
        targetHeight = drawHeight;
    } else {
        float scaleX = (float)actualLcdWidth  / codecCtx->width;
        float scaleY = (float)actualLcdHeight / codecCtx->height;
        float scale  = std::max(scaleX, scaleY);
        drawWidth  = (int)(codecCtx->width  * scale);
        drawHeight = (int)(codecCtx->height * scale);
        offsetX = (actualLcdWidth  - drawWidth)  / 2;
        offsetY = (actualLcdHeight - drawHeight) / 2;
        targetWidth  = drawWidth;
        targetHeight = drawHeight;
    }

    std::cout << "[VIDEO] Video: " << codecCtx->width << "x" << codecCtx->height
              << " (aspect: " << videoAspect << ")" << std::endl;
    std::cout << "[VIDEO] Display orientation: " << orientation << "°" << std::endl;
    std::cout << "[VIDEO] Display mode: " << (displayIsLandscape ? "landscape" : "portrait")
              << " (" << actualLcdWidth << "x" << actualLcdHeight << ", aspect: " << displayAspect << ")" << std::endl;
    std::cout << "[VIDEO] Scale mode: " << (scaleMode == FIT ? "FIT (letterbox)" : "FILL (crop)") << std::endl;
    std::cout << "[VIDEO] Draw size: " << drawWidth << "x" << drawHeight
              << " at offset (" << offsetX << "," << offsetY << ")" << std::endl;
    std::cout << "[VIDEO] Scaling video to: " << targetWidth << "x" << targetHeight << std::endl;

    SwsContext* swsCtx = sws_getContext(
        codecCtx->width, codecCtx->height, codecCtx->pix_fmt,
        targetWidth, targetHeight, AV_PIX_FMT_RGB24,
        SWS_BILINEAR, nullptr, nullptr, nullptr
    );

    if (!swsCtx) {
        std::cerr << "[VIDEO] Failed to create scaler" << std::endl;
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return 1;
    }

    // Map all three shared memory regions ONCE for the lifetime of the program.
    // This eliminates 12 kernel syscalls per frame from the per-frame hot path.
    FramebufferStats* stats = mapStats();
    if (!stats) {
        std::cerr << "[VIDEO] Warning: Could not map stats (may flicker)" << std::endl;
    }

    int fd0 = shm_open(FB_V3_SHM_NAME_0, O_RDWR, 0666);
    if (fd0 < 0) {
        std::cerr << "[VIDEO] Failed to open framebuffer 0" << std::endl;
        if (stats) unmapStats(stats);
        sws_freeContext(swsCtx);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return 1;
    }
    uint16_t* fb0 = (uint16_t*)mmap(nullptr, FB_WIDTH * FB_HEIGHT * 2,
                                    PROT_READ | PROT_WRITE, MAP_SHARED, fd0, 0);
    close(fd0);
    if (fb0 == MAP_FAILED) {
        std::cerr << "[VIDEO] Failed to mmap framebuffer 0" << std::endl;
        if (stats) unmapStats(stats);
        sws_freeContext(swsCtx);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return 1;
    }

    int fd1 = shm_open(FB_V3_SHM_NAME_1, O_RDWR, 0666);
    if (fd1 < 0) {
        std::cerr << "[VIDEO] Failed to open framebuffer 1" << std::endl;
        munmap(fb0, FB_WIDTH * FB_HEIGHT * 2);
        if (stats) unmapStats(stats);
        sws_freeContext(swsCtx);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return 1;
    }
    uint16_t* fb1 = (uint16_t*)mmap(nullptr, FB_WIDTH * FB_HEIGHT * 2,
                                    PROT_READ | PROT_WRITE, MAP_SHARED, fd1, 0);
    close(fd1);
    if (fb1 == MAP_FAILED) {
        std::cerr << "[VIDEO] Failed to mmap framebuffer 1" << std::endl;
        munmap(fb0, FB_WIDTH * FB_HEIGHT * 2);
        if (stats) unmapStats(stats);
        sws_freeContext(swsCtx);
        avcodec_free_context(&codecCtx);
        avformat_close_input(&formatCtx);
        return 1;
    }

    std::cout << "[VIDEO] Clearing buffers..." << std::endl;

    if (stats) {
        int waitCount = 0;
        while (stats->frameInProgress.load() && waitCount < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            waitCount++;
        }
        std::cout << "[VIDEO] Driver idle, ready to clear" << std::endl;
    }

    // Determine back buffer using persistent stats pointer and clear it.
    uint16_t* fb = (stats && stats->activeBufferIndex.load() == 0) ? fb1 : fb0;
    clearBuffer(fb);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (stats) stats->activeBufferIndex.store(1 - stats->activeBufferIndex.load());

    if (stats) {
        uint64_t currentFrames = stats->totalFrames.load();
        int waitCount = 0;
        while (stats->totalFrames.load() == currentFrames && waitCount < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            waitCount++;
        }
    }

    // Now the other buffer is the back buffer — clear it too.
    fb = (stats && stats->activeBufferIndex.load() == 0) ? fb1 : fb0;
    clearBuffer(fb);

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    if (stats) stats->activeBufferIndex.store(1 - stats->activeBufferIndex.load());

    if (stats) {
        uint64_t currentFrames = stats->totalFrames.load();
        int waitCount = 0;
        while (stats->totalFrames.load() == currentFrames && waitCount < 100) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            waitCount++;
        }
    }

    std::cout << "[VIDEO] Both buffers cleared and stabilized" << std::endl;

    // Set fb to the current back buffer, ready for rendering.
    fb = (stats && stats->activeBufferIndex.load() == 0) ? fb1 : fb0;
    std::cout << "[VIDEO] Ready to render - back buffer mapped" << std::endl;

    AVFrame* frame    = av_frame_alloc();
    AVFrame* frameRGB = av_frame_alloc();

    int numBytes = av_image_get_buffer_size(AV_PIX_FMT_RGB24, targetWidth, targetHeight, 1);
    uint8_t* buffer = (uint8_t*)av_malloc(numBytes);
    av_image_fill_arrays(frameRGB->data, frameRGB->linesize, buffer,
                         AV_PIX_FMT_RGB24, targetWidth, targetHeight, 1);

    std::cout << "[VIDEO] Starting playback... Press Ctrl+C to stop" << std::endl;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    AVPacket packet;
    int frameCount = 0;

    AVRational frameRate = av_guess_frame_rate(formatCtx, formatCtx->streams[videoStream], nullptr);
    double fps = (customFps > 0) ? customFps : av_q2d(frameRate);
    if (fps <= 0) fps = 30.0;
    auto targetFrameTime = std::chrono::microseconds((int)(1000000.0 / fps));

    if (customFps > 0) {
        std::cout << "[VIDEO] FPS Override: " << fps << " (original: " << av_q2d(frameRate) << ")" << std::endl;
    } else {
        std::cout << "[VIDEO] Auto-detected FPS: " << fps << std::endl;
    }

    using Clock = std::chrono::steady_clock;
    Clock::time_point wallStart{};
    double            ptsBasis  = 0.0;
    bool              syncReady = false;
    AVRational        vTimeBase = formatCtx->streams[videoStream]->time_base;

    // Audio thread is started after the first video frame renders to avoid
    // audio running ahead of video during the startup/buffer-clear sequence.
    AudioPktQueue             audioPktQ;
    std::atomic<uint64_t>     totalSamplesPushed{0};
    std::atomic<int>          audioPacketsRecv{0};
    std::thread               audioThread;
    bool                      audioThreadStarted = false;

    // Track driver frame counter — used for wait-for-driver and display FPS.
    uint64_t initialDriverFrame = stats ? stats->totalFrames.load() : 0;


    while (running && av_read_frame(formatCtx, &packet) >= 0) {
        // ── Audio packet ──────────────────────────────────────────────────────
        if (hasAudio && packet.stream_index == audioStream) {
            if (!audioThreadStarted) {
                // Drop audio until the first video frame is on screen — this
                // prevents audio from running ahead during startup.
                av_packet_unref(&packet);
                continue;
            }
            AVPacket* pkt = av_packet_alloc();
            av_packet_move_ref(pkt, &packet);
            {
                std::unique_lock<std::mutex> lk(audioPktQ.mtx);
                audioPktQ.cv.wait(lk, [&]{
                    return (int)audioPktQ.pkts.size() < AudioPktQueue::MAX_PKTS || !running.load();
                });
                audioPktQ.pkts.push_back(pkt);
            }
            audioPktQ.cv.notify_all();
            continue;
        }

        // ── Video packet ─────────────────────────────────────────────────────
        if (packet.stream_index != videoStream) {
            av_packet_unref(&packet);
            continue;
        }

        if (avcodec_send_packet(codecCtx, &packet) >= 0) {
            while (avcodec_receive_frame(codecCtx, frame) >= 0) {
                if (frameCount == 0 && stats) {
                    int waitCount = 0;
                    while (stats->frameInProgress.load() && waitCount < 100) {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1));
                        waitCount++;
                    }
                    std::cout << "[VIDEO] First frame - driver confirmed idle" << std::endl;
                }

                // ── PTS tracking ──────────────────────────────────────────
                double framePts = (frame->pts != AV_NOPTS_VALUE)
                    ? frame->pts * av_q2d(vTimeBase) : -1.0;

                if (!syncReady && framePts >= 0.0) {
                    wallStart = Clock::now();
                    ptsBasis  = framePts;
                    syncReady = true;
                }

                // ── Frame dropping ────────────────────────────────────────
                // If this frame's PTS is more than 1.5 frame-periods in the past
                // (we're behind), skip rendering it to keep wall-clock speed correct.
                // We still decoded it (required by FFmpeg), just don't display it.
                if (syncReady && framePts >= 0.0) {
                    double nowSec    = std::chrono::duration<double>(Clock::now() - wallStart).count();
                    double targetSec = framePts - ptsBasis;
                    if (targetSec < nowSec - (1.5 / fps)) {
                        av_frame_unref(frame);
                        continue;  // skip — too old
                    }
                }

                // ── PTS sleep ─────────────────────────────────────────────
                if (syncReady && framePts >= 0.0) {
                    double targetSec  = framePts - ptsBasis;
                    auto   targetTime = wallStart + std::chrono::duration_cast<Clock::duration>(
                        std::chrono::duration<double>(targetSec));
                    if (targetTime > Clock::now())
                        std::this_thread::sleep_until(targetTime);
                }
                // ─────────────────────────────────────────────────────────

                auto frameStart = Clock::now();

                if (stats) {
                    stats->frameInProgress.store(true, std::memory_order_seq_cst);
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                }

                memset(fb, 0, FB_WIDTH * FB_HEIGHT * sizeof(uint16_t));

                sws_scale(swsCtx, frame->data, frame->linesize, 0, codecCtx->height,
                         frameRGB->data, frameRGB->linesize);

                for (int lcdY = 0; lcdY < actualLcdHeight; lcdY++) {
                    for (int lcdX = 0; lcdX < actualLcdWidth; lcdX++) {
                        int fbX, fbY;
                        if (orientation == 0) {
                            fbX = lcdY;
                            fbY = 479 - lcdX;
                        } else if (orientation == 90) {
                            fbX = lcdX;
                            fbY = lcdY;
                        } else if (orientation == 180) {
                            fbX = 319 - lcdY;
                            fbY = lcdX;
                        } else {
                            fbX = 319 - lcdX;
                            fbY = 479 - lcdY;
                        }

                        int videoX = lcdX - offsetX;
                        int videoY = lcdY - offsetY;
                        if (videoX < 0 || videoX >= drawWidth || videoY < 0 || videoY >= drawHeight)
                            continue;

                        int rgbIdx = (videoY * frameRGB->linesize[0]) + (videoX * 3);
                        uint8_t r = frameRGB->data[0][rgbIdx];
                        uint8_t g = frameRGB->data[0][rgbIdx + 1];
                        uint8_t b = frameRGB->data[0][rgbIdx + 2];
                        fb[fbY * FB_WIDTH + fbX] = rgb24_to_bgr565(r, g, b);
                    }
                }

                if (stats) {
                    std::atomic_thread_fence(std::memory_order_seq_cst);
                    stats->frameInProgress.store(false, std::memory_order_seq_cst);
                }

                // Inline buffer swap: toggle activeBufferIndex atomically.
                if (stats) stats->activeBufferIndex.store(1 - stats->activeBufferIndex.load());
                frameCount++;

                // ── Start audio thread in sync with first video frame ─────
                // Audio packets before this point were dropped, so audio and
                // video begin at the same wall-clock moment.
                if (!audioThreadStarted && hasAudio) {
                    audioThread = std::thread(audioWorker,
                                              std::ref(audioPktQ),
                                              audioCtx, swrCtx, audioRing,
                                              std::ref(totalSamplesPushed),
                                              std::ref(audioPacketsRecv));
                    audioThreadStarted = true;
                    std::cout << "[AUDIO] Thread started — synced to frame 1" << std::endl;
                }
                // ─────────────────────────────────────────────────────────

                // Select back buffer using persistent pointers — zero syscalls.
                fb = (stats && stats->activeBufferIndex.load() == 0) ? fb1 : fb0;


                if (!syncReady) {
                    auto frameDuration = std::chrono::duration_cast<std::chrono::microseconds>(
                        Clock::now() - frameStart);
                    if (frameDuration < targetFrameTime)
                        std::this_thread::sleep_for(targetFrameTime - frameDuration);
                }

                if (frameCount % 30 == 0) {
                    if (syncReady && stats) {
                        double el = std::chrono::duration<double>(Clock::now() - wallStart).count();
                        uint64_t displayedFrames = stats->totalFrames.load() - initialDriverFrame;
                        double displayFps = (el > 0) ? (displayedFrames / el) : 0.0;
                        std::cout << "[VIDEO] Frame " << frameCount
                                  << " | display=" << displayedFrames
                                  << " | FPS: " << std::fixed << std::setprecision(1) << displayFps;
                    } else {
                        std::cout << "[VIDEO] Frame " << frameCount;
                    }
                    if (hasAudio && audioRing) {
                        uint32_t fill = (audioRing->writeHead.load() - audioRing->readHead.load()
                                         + AUDIO_RING_SAMPLES) % AUDIO_RING_SAMPLES;
                        std::cout << " | audio pushed=" << totalSamplesPushed.load()
                                  << " ring=" << fill * 100 / AUDIO_RING_SAMPLES << "%"
                                  << " qlen=" << [&]{ std::unique_lock<std::mutex> lk(audioPktQ.mtx);
                                                      return audioPktQ.pkts.size(); }();
                    } else if (!hasAudio) {
                        std::cout << " | audio=disabled";
                    }
                    std::cout << std::endl;
                }
            }
        }
        av_packet_unref(&packet);
    }

    std::cout << "\n[VIDEO] Playback finished. Total frames: " << frameCount << std::endl;

    // ── Shut down audio thread cleanly ────────────────────────────────────────
    if (audioThreadStarted && audioThread.joinable()) {
        {
            std::unique_lock<std::mutex> lk(audioPktQ.mtx);
            audioPktQ.eof.store(true);
        }
        audioPktQ.cv.notify_all();
        audioThread.join();
        std::cout << "[AUDIO] Background thread joined" << std::endl;
    }
    // ─────────────────────────────────────────────────────────────────────────

    if (swrCtx)    swr_free(&swrCtx);
    if (audioCtx)  avcodec_free_context(&audioCtx);
    if (audioRing) audio_v3::unmapPcmRing(audioRing);

    av_free(buffer);
    av_frame_free(&frameRGB);
    av_frame_free(&frame);
    // Unmap all three persistent shared memory regions once at exit.
    munmap(fb0, FB_WIDTH * FB_HEIGHT * 2);
    munmap(fb1, FB_WIDTH * FB_HEIGHT * 2);
    if (stats) unmapStats(stats);
    sws_freeContext(swsCtx);
    avcodec_free_context(&codecCtx);
    avformat_close_input(&formatCtx);

    unlink(PID_FILE);
    return 0;
}
