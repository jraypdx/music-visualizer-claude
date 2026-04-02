#pragma once
#include <atomic>
#include <chrono>
#include <complex>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

class AudioCapture {
public:
    static constexpr int NUM_BANDS = 32;
    static constexpr int FFT_SIZE  = 2048;   // must be a power of two

    // Drum detection result (published each FFT frame)
    struct DrumInfo {
        bool kickPulse;   // one-shot: true for the single FFT frame a kick fires
        bool snarePulse;  // one-shot: true for the single FFT frame a snare fires
    };

    ~AudioCapture();

    // Start WASAPI loopback capture on the given render device ID.
    // Returns true once the capture thread confirms it is running.
    bool Start(const std::wstring& deviceId);
    void Stop();
    bool IsRunning() const { return m_running.load(std::memory_order_relaxed); }

    // Thread-safe accessors
    void GetBands(float out[NUM_BANDS]) const;
    void GetDrumInfo(DrumInfo& out) const;

private:
    void CaptureLoop(std::wstring deviceId);
    void ProcessFFT(const float* samples, int sampleRate);

    std::thread       m_thread;
    std::atomic<bool> m_running  { false };
    std::atomic<bool> m_stopFlag { false };

    // Capture-thread-only state
    std::vector<float>               m_accumBuf;
    std::vector<std::complex<float>> m_fftBuf;
    float                            m_smoothed[NUM_BANDS] {};

    // ── Drum detection (capture thread only) ─────────────────────────────────
    static constexpr int kKickBands      = 6;   // bands 0-5:  ~40-130 Hz (kick thump)
    static constexpr int kSnareStart     = 7;   // first snare band
    static constexpr int kSnareBands     = 7;   // bands 7-13: ~170-500 Hz (snare body)
    static constexpr int kDrumWindowSize = 43;  // ~1 s of frames for rolling avg

    float m_kickHistory[kDrumWindowSize]  {};
    float m_snareHistory[kDrumWindowSize] {};
    int   m_drumHistHead = 0;
    int   m_drumHistFill = 0;

    std::chrono::steady_clock::time_point m_lastKickClock  {};
    std::chrono::steady_clock::time_point m_lastSnareClock {};
    bool m_lastKickClockSet  = false;
    bool m_lastSnareClockSet = false;

    // ── Shared with main thread ───────────────────────────────────────────────
    mutable std::mutex m_bandMutex;
    float              m_bands[NUM_BANDS] {};

    mutable std::mutex m_drumMutex;
    DrumInfo           m_pubDrumInfo {};
};
