#include "audio_pw_v3.h"
#include "config.h"

#include <iostream>
#include <iomanip>
#include <signal.h>
#include <atomic>
#include <chrono>
#include <thread>
#include <unistd.h>
#include <libgen.h>
#include <linux/limits.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

// Forward declarations from audio_pw_v3.cpp
struct AudioDriverV3;
extern "C" {
    AudioDriverV3* audio_v3_create();
    bool           audio_v3_init(AudioDriverV3* d, float streamVol, float sfxVol, const char* device);
    void           audio_v3_stop(AudioDriverV3* d);
    void           audio_v3_destroy(AudioDriverV3* d);
    AudioStats*    audio_v3_get_stats(AudioDriverV3* d);
}

static std::atomic<bool> running(true);
static void signalHandler(int sig) {
    std::cout << "\n[AUDIO_MAIN] Signal " << sig << " — shutting down..." << std::endl;
    running = false;
}

static const char* pwStateShort(int32_t s) {
    switch (s) {
    case -1: return "ERROR";
    case  0: return "UNCONNECTED";
    case  1: return "CONNECTING";
    case  2: return "PAUSED";
    case  3: return "STREAMING";
    default: return "?";
    }
}

static void printStats(AudioStats* s) {
    if (!s) return;
    float sv   = s->streamVolumeX1000.load() / 1000.f;
    float sfxv = s->sfxVolumeX1000.load()    / 1000.f;
    int32_t pw = s->pwState.load();
    std::cout << "\r[AUDIO] PW:" << std::setw(11) << pwStateShort(pw)
              << " | cb=" << std::setw(8) << s->callbacksTotal.load()
              << " | ring=" << std::setw(3) << s->streamFillPct.load() << "%"
              << " | consumed=" << std::setw(10) << s->samplesConsumed.load()
              << " | sfx=" << s->sfxActive.load()
              << " | underruns=" << s->underruns.load()
              << " | vol s=" << std::fixed << std::setprecision(2) << sv
              << " x=" << sfxv
              << "  " << std::flush;
}

int main(int argc, char** argv) {
    signal(SIGINT,  signalHandler);
    signal(SIGTERM, signalHandler);

    std::cout << R"(
╔═══════════════════════════════════════════════╗
║   Audio Driver V3 — PipeWire Edition          ║
║   Shared-Memory PCM Ring + SFX Mixer          ║
╚═══════════════════════════════════════════════╝
)" << std::endl;

    // Locate and load settings.conf in the same directory as this binary
    Config config;
    char exePath[PATH_MAX];
    ssize_t len = readlink("/proc/self/exe", exePath, sizeof(exePath) - 1);
    if (len > 0) {
        exePath[len] = '\0';
        std::string cfgPath = std::string(dirname(exePath)) + "/settings.conf";
        std::cout << "[AUDIO_MAIN] Config: " << cfgPath << std::endl;
        if (!config.load(cfgPath))
            std::cout << "[AUDIO_MAIN] Using defaults (settings.conf not found)" << std::endl;
    }

    float       streamVol  = config.getFloat("audio_stream_volume", 1.0f);
    float       sfxVol     = config.getFloat("audio_sfx_volume",    1.0f);
    bool        showStats  = config.getBool("audio_show_stats",     true);
    int         statsMs    = config.getInt("audio_stats_interval",  500);
    bool        verbose    = config.getBool("verbose",              false);
    std::string audioDevice = config.getString("audio_device",     "");
    bool        daemonMode = false;

    // Command-line overrides
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--daemon" || arg == "-d") {
            daemonMode = true;
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--no-stats") {
            showStats = false;
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [OPTIONS]\n"
                      << "  --daemon, -d    Run in background (no terminal output)\n"
                      << "  --verbose, -v   Enable verbose logging\n"
                      << "  --no-stats      Suppress live statistics\n"
                      << "  --help, -h      Show this help\n"
                      << "\nShared memory created:\n"
                      << "  " << AUDIO_V3_PCM_SHM   << "  — F32 stereo 48kHz ring (apps write PCM here)\n"
                      << "  " << AUDIO_V3_CMD_SHM   << "  — command queue (play WAV, beep, volume)\n"
                      << "  " << AUDIO_V3_STATS_SHM << "  — driver status (read-only for apps)\n"
                      << "\nDevice targeting (Pi aux jack):\n"
                      << "  Run: pactl list sinks short\n"
                      << "  Add to settings.conf:  audio_device = <sink name>\n"
                      << "  Example: audio_device = alsa_output.platform-bcm2835_audio.analog-stereo\n";
            return 0;
        }
    }

    if (verbose) {
        std::cout << "[AUDIO_MAIN] audio_stream_volume = " << streamVol  << "\n"
                  << "[AUDIO_MAIN] audio_sfx_volume    = " << sfxVol     << "\n"
                  << "[AUDIO_MAIN] audio_device        = "
                  << (audioDevice.empty() ? "(default)" : audioDevice)  << "\n";
    }

    // Daemonise
    if (daemonMode) {
        std::cout << "[AUDIO_MAIN] Forking to daemon..." << std::endl;
        pid_t pid = fork();
        if (pid < 0)  { std::cerr << "[AUDIO_MAIN] fork() failed" << std::endl; return 1; }
        if (pid > 0)  {
            std::cout << "[AUDIO_MAIN] Daemon PID: " << pid << "  (kill to stop)" << std::endl;
            return 0;
        }
        setsid();
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); close(devnull); }
        showStats = false;
    }

    AudioDriverV3* drv = audio_v3_create();
    if (!audio_v3_init(drv, streamVol, sfxVol,
                       audioDevice.empty() ? nullptr : audioDevice.c_str())) {
        std::cerr << "[AUDIO_MAIN] Failed to initialise audio driver" << std::endl;
        audio_v3_destroy(drv);
        return 1;
    }

    std::cout << "[AUDIO_MAIN] Driver running.  Press Ctrl+C to stop." << std::endl;
    if (showStats) std::cout << "[AUDIO_MAIN] Live stats:\n";

    auto lastStats = std::chrono::steady_clock::now();
    AudioStats* s  = audio_v3_get_stats(drv);

    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (showStats) {
            auto now = std::chrono::steady_clock::now();
            if (std::chrono::duration_cast<std::chrono::milliseconds>(now - lastStats).count() >= statsMs) {
                printStats(s);
                lastStats = now;
            }
        }
    }

    std::cout << std::endl;
    audio_v3_stop(drv);
    audio_v3_destroy(drv);
    std::cout << "[AUDIO_MAIN] Clean exit." << std::endl;
    return 0;
}
