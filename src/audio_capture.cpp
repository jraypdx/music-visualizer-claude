#include "audio_capture.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <mmreg.h>        // WAVEFORMATEXTENSIBLE
#include <audioclient.h>  // IAudioClient, IAudioCaptureClient
#include <mmdeviceapi.h>  // IMMDeviceEnumerator, IMMDevice
#include <wrl/client.h>   // ComPtr

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>

using Microsoft::WRL::ComPtr;

static constexpr float kPI = 3.14159265358979323846f;

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT = {00000003-0000-0010-8000-00AA00389B71}
static const GUID kSubtypeIEEEFloat = {
    0x00000003, 0x0000, 0x0010,
    { 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71 }
};

// ── Radix-2 Cooley-Tukey FFT (in-place, n must be a power of two) ────────────
static void FFTInPlace(std::complex<float>* buf, int n)
{
    // Bit-reversal permutation
    for (int i = 1, j = 0; i < n; ++i) {
        int bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(buf[i], buf[j]);
    }
    // Butterfly passes
    for (int len = 2; len <= n; len <<= 1) {
        float ang = -2.0f * kPI / static_cast<float>(len);
        std::complex<float> wlen(std::cosf(ang), std::sinf(ang));
        for (int i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (int j = 0; j < len / 2; ++j) {
                auto u = buf[i + j];
                auto v = buf[i + j + len / 2] * w;
                buf[i + j]           = u + v;
                buf[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

// ── AudioCapture ──────────────────────────────────────────────────────────────

AudioCapture::~AudioCapture()
{
    Stop();
}

bool AudioCapture::Start(const std::wstring& deviceId)
{
    Stop();

    m_stopFlag.store(false, std::memory_order_relaxed);
    m_accumBuf.clear();
    m_accumBuf.reserve(FFT_SIZE * 2);
    m_fftBuf.resize(FFT_SIZE);
    std::memset(m_smoothed,      0, sizeof(m_smoothed));
    std::memset(m_kickHistory,   0, sizeof(m_kickHistory));
    std::memset(m_snareHistory,  0, sizeof(m_snareHistory));
    m_drumHistHead      = 0;
    m_drumHistFill      = 0;
    m_lastKickClockSet  = false;
    m_lastSnareClockSet = false;
    m_pubDrumInfo       = {};

    m_thread = std::thread(&AudioCapture::CaptureLoop, this, deviceId);

    // Wait up to 500 ms for the thread to confirm it started
    for (int i = 0; i < 50 && !m_running.load(std::memory_order_acquire); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));

    return m_running.load(std::memory_order_relaxed);
}

void AudioCapture::Stop()
{
    m_stopFlag.store(true, std::memory_order_relaxed);
    if (m_thread.joinable())
        m_thread.join();
    m_running.store(false, std::memory_order_relaxed);
}

void AudioCapture::GetBands(float out[NUM_BANDS]) const
{
    std::lock_guard<std::mutex> lock(m_bandMutex);
    std::memcpy(out, m_bands, sizeof(float) * NUM_BANDS);
}

void AudioCapture::GetDrumInfo(DrumInfo& out) const
{
    std::lock_guard<std::mutex> lock(m_drumMutex);
    out = m_pubDrumInfo;
}

// ── Capture loop (runs on its own thread) ─────────────────────────────────────

void AudioCapture::CaptureLoop(std::wstring deviceId)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // ── Open device ──────────────────────────────────────────────────────────
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), &enumerator)))
        { CoUninitialize(); return; }

    ComPtr<IMMDevice> device;
    HRESULT hr = deviceId.empty()
        ? enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device)
        : enumerator->GetDevice(deviceId.c_str(), &device);
    if (FAILED(hr)) { CoUninitialize(); return; }

    // ── Create audio client and query mix format ──────────────────────────────
    ComPtr<IAudioClient> audioClient;
    if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &audioClient)))
        { CoUninitialize(); return; }

    WAVEFORMATEX* pwfx = nullptr;
    if (FAILED(audioClient->GetMixFormat(&pwfx)))
        { CoUninitialize(); return; }

    const int  sampleRate   = static_cast<int>(pwfx->nSamplesPerSec);
    const int  channels     = static_cast<int>(pwfx->nChannels);
    const int  bitsPerSample = static_cast<int>(pwfx->wBitsPerSample);

    bool isFloat = (pwfx->wFormatTag == WAVE_FORMAT_IEEE_FLOAT);
    if (pwfx->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        auto* ext = reinterpret_cast<WAVEFORMATEXTENSIBLE*>(pwfx);
        isFloat = (IsEqualGUID(ext->SubFormat, kSubtypeIEEEFloat) != 0);
    }

    // ── Initialise for loopback capture ───────────────────────────────────────
    constexpr REFERENCE_TIME kBufferDuration = 200000; // 20 ms in 100-ns units
    hr = audioClient->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_LOOPBACK,
        kBufferDuration, 0, pwfx, nullptr);
    CoTaskMemFree(pwfx);
    if (FAILED(hr)) { CoUninitialize(); return; }

    ComPtr<IAudioCaptureClient> captureClient;
    if (FAILED(audioClient->GetService(__uuidof(IAudioCaptureClient), &captureClient)))
        { CoUninitialize(); return; }

    if (FAILED(audioClient->Start()))
        { CoUninitialize(); return; }

    m_running.store(true, std::memory_order_release);

    // ── Packet loop ───────────────────────────────────────────────────────────
    bool ok = true;
    while (ok && !m_stopFlag.load(std::memory_order_relaxed)) {

        UINT32 packetSize = 0;
        if (FAILED(captureClient->GetNextPacketSize(&packetSize))) break;

        if (packetSize == 0) {
            // No audio arriving — push silence so the bars decay to zero
            constexpr int kSilenceFrames = 256;
            m_accumBuf.insert(m_accumBuf.end(), kSilenceFrames, 0.0f);
            Sleep(5);
        }

        while (packetSize > 0 && ok) {
            BYTE*  pData     = nullptr;
            UINT32 numFrames = 0;
            DWORD  flags     = 0;

            if (FAILED(captureClient->GetBuffer(&pData, &numFrames, &flags, nullptr, nullptr))) {
                ok = false;
                break;
            }

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) || !pData) {
                m_accumBuf.insert(m_accumBuf.end(), numFrames, 0.0f);
            } else {
                for (UINT32 f = 0; f < numFrames; ++f) {
                    float mono = 0.0f;
                    for (int c = 0; c < channels; ++c) {
                        if (isFloat) {
                            mono += reinterpret_cast<const float*>(pData)[f * channels + c];
                        } else if (bitsPerSample == 16) {
                            mono += reinterpret_cast<const int16_t*>(pData)[f * channels + c]
                                    / 32768.0f;
                        }
                    }
                    m_accumBuf.push_back(mono / static_cast<float>(channels));
                }
            }

            captureClient->ReleaseBuffer(numFrames);

            if (FAILED(captureClient->GetNextPacketSize(&packetSize)))
                ok = false;
        }

        // Compute FFT whenever a full frame is available; advance by half (50% overlap)
        while (static_cast<int>(m_accumBuf.size()) >= FFT_SIZE) {
            ProcessFFT(m_accumBuf.data(), sampleRate);
            m_accumBuf.erase(m_accumBuf.begin(), m_accumBuf.begin() + FFT_SIZE / 2);
        }
    }

    audioClient->Stop();
    m_running.store(false, std::memory_order_release);
    CoUninitialize();
}

// ── FFT processing (capture thread only) ─────────────────────────────────────

void AudioCapture::ProcessFFT(const float* samples, int sampleRate)
{
    // Apply Hann window to reduce spectral leakage
    for (int i = 0; i < FFT_SIZE; ++i) {
        float w = 0.5f * (1.0f - std::cosf(2.0f * kPI * i / (FFT_SIZE - 1)));
        m_fftBuf[i] = { samples[i] * w, 0.0f };
    }

    FFTInPlace(m_fftBuf.data(), FFT_SIZE);

    // ── Build single-sided magnitude spectrum ────────────────────────────────
    constexpr int   HALF      = FFT_SIZE / 2;
    constexpr float kMinFreq  = 40.0f;
    constexpr float kMaxFreq  = 20000.0f;
    const float     kLogRange = std::logf(kMaxFreq / kMinFreq); // ln(1000)
    const float     binWidth  = static_cast<float>(sampleRate) / FFT_SIZE;

    float mag[HALF];
    for (int b = 0; b < HALF; ++b)
        mag[b] = std::abs(m_fftBuf[b]) * 2.0f / FFT_SIZE;

    // ── Map to 32 log-spaced bands — iterate per band ────────────────────────
    //
    // Iterating bin→band (the previous approach) leaves bands empty whenever
    // the FFT bin width exceeds the band width.  At 96 kHz the bin width is
    // ~47 Hz, wider than every band below ~100 Hz, so those bars never light up.
    //
    // Instead we iterate band→bins.  When no integer bin falls inside a band we
    // linearly interpolate between the two bins that straddle the band centre.
    float newBands[NUM_BANDS] {};
    for (int i = 0; i < NUM_BANDS; ++i) {
        float fLow  = kMinFreq * std::expf(kLogRange *  i      / NUM_BANDS);
        float fHigh = kMinFreq * std::expf(kLogRange * (i + 1) / NUM_BANDS);

        // Fractional bin positions for the band edges
        float bLow  = fLow  / binWidth;
        float bHigh = fHigh / binWidth;

        // Integer bins that fall fully inside this band
        int iLow  = std::max(1,       static_cast<int>(std::ceilf(bLow)));
        int iHigh = std::min(HALF - 1, static_cast<int>(std::floorf(bHigh)));

        if (iLow <= iHigh) {
            // Normal case: one or more bins sit inside the band — average them
            float sum = 0.0f;
            for (int b = iLow; b <= iHigh; ++b) sum += mag[b];
            newBands[i] = sum / static_cast<float>(iHigh - iLow + 1);
        } else {
            // Band is narrower than one bin (common at low frequencies with high
            // sample rates).  Interpolate between the two bins that bracket the
            // band's log-centre frequency.
            float fCenter = std::sqrtf(fLow * fHigh);   // geometric mean = log-scale centre
            float bCenter = fCenter / binWidth;
            int   b0 = std::max(1,       static_cast<int>(bCenter));
            int   b1 = std::min(HALF - 1, b0 + 1);
            float t  = bCenter - static_cast<float>(b0); // 0..1 interpolation weight
            newBands[i] = mag[b0] * (1.0f - t) + mag[b1] * t;
        }
    }

    // ── Frequency-dependent gain (pink noise / 1/f compensation) ────────────
    // Music has a natural ~1/f² energy rolloff, so raw FFT data always looks
    // bass-heavy.  Multiplying each band by (fCenter / fMin)^0.5 applies a
    // +3 dB/octave slope that counteracts this, balancing the bars across the
    // full frequency range before the dB conversion below.
    for (int i = 0; i < NUM_BANDS; ++i) {
        float fCenter = kMinFreq * std::expf(kLogRange * (i + 0.5f) / NUM_BANDS);
        newBands[i]  *= std::sqrtf(fCenter / kMinFreq);
    }

    // Convert to dB and normalise to [0, 1]
    //   -70 dB → 0.0 (silence floor)   -10 dB → 1.0 (loud signal)
    float normed[NUM_BANDS] {};
    for (int i = 0; i < NUM_BANDS; ++i) {
        float dB = 20.0f * std::log10f(newBands[i] + 1e-9f);
        normed[i] = std::clamp((dB + 70.0f) / 60.0f, 0.0f, 1.0f);
    }

    // Exponential smoothing — fast attack, slow release
    for (int i = 0; i < NUM_BANDS; ++i) {
        float alpha  = (normed[i] > m_smoothed[i]) ? 0.70f : 0.12f;
        m_smoothed[i] = alpha * normed[i] + (1.0f - alpha) * m_smoothed[i];
    }

    // ── Drum detection ────────────────────────────────────────────────────────
    // Kick: average normalized energy in bands 0-5 (~40-130 Hz, sub-bass thump)
    float kickEnergy = 0.0f;
    for (int i = 0; i < kKickBands; ++i) kickEnergy += normed[i];
    kickEnergy /= kKickBands;

    // Snare: average normalized energy in bands 7-13 (~170-500 Hz, snare body)
    float snareEnergy = 0.0f;
    for (int i = kSnareStart; i < kSnareStart + kSnareBands; ++i) snareEnergy += normed[i];
    snareEnergy /= kSnareBands;

    // Rolling averages over the last ~1 second of frames
    float kickAvg  = 0.0f;
    float snareAvg = 0.0f;
    {
        int window = std::min(m_drumHistFill, kDrumWindowSize);
        if (window > 0) {
            for (int i = 0; i < window; ++i) {
                int idx = (m_drumHistHead - 1 - i + kDrumWindowSize) % kDrumWindowSize;
                kickAvg  += m_kickHistory[idx];
                snareAvg += m_snareHistory[idx];
            }
            kickAvg  /= static_cast<float>(window);
            snareAvg /= static_cast<float>(window);
        }
    }

    constexpr float  kKickThreshold  = 1.4f;
    constexpr float  kKickMinEnergy  = 0.15f;
    constexpr float  kSnareThreshold = 1.5f;
    constexpr float  kSnareMinEnergy = 0.12f;
    constexpr double kKickMinGap     = 0.10;  // 100 ms between kicks
    constexpr double kSnareMinGap    = 0.10;  // 100 ms between snares

    auto now = std::chrono::steady_clock::now();

    bool isKick = false;
    if (kickEnergy > kKickThreshold * kickAvg && kickEnergy > kKickMinEnergy) {
        double elapsed = m_lastKickClockSet
            ? std::chrono::duration<double>(now - m_lastKickClock).count()
            : 1e9;
        if (elapsed >= kKickMinGap) {
            isKick = true;
            m_lastKickClock    = now;
            m_lastKickClockSet = true;
        }
    }

    bool isSnare = false;
    if (snareEnergy > kSnareThreshold * snareAvg && snareEnergy > kSnareMinEnergy) {
        double elapsed = m_lastSnareClockSet
            ? std::chrono::duration<double>(now - m_lastSnareClock).count()
            : 1e9;
        if (elapsed >= kSnareMinGap) {
            isSnare = true;
            m_lastSnareClock    = now;
            m_lastSnareClockSet = true;
        }
    }

    // Update rolling history
    m_kickHistory[m_drumHistHead]  = kickEnergy;
    m_snareHistory[m_drumHistHead] = snareEnergy;
    m_drumHistHead = (m_drumHistHead + 1) % kDrumWindowSize;
    if (m_drumHistFill < kDrumWindowSize) ++m_drumHistFill;

    // Publish bands to main thread
    {
        std::lock_guard<std::mutex> lock(m_bandMutex);
        std::memcpy(m_bands, m_smoothed, sizeof(m_bands));
    }

    // Publish drum data to main thread
    {
        std::lock_guard<std::mutex> lock(m_drumMutex);
        m_pubDrumInfo = { isKick, isSnare };
    }
}
