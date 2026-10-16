// Why this file exists: every attempt this project made at compensating
// for audio output hardware latency by GUESSING a constant (tried 0.06,
// 0.15, 0.30, 0.45 seconds, in that order, each tuned against one specific
// developer machine's speakers) ran into the same wall -- it's not a
// property of Dirk, it's a property of the PLAYER'S sound card and driver,
// which is different for every single install. A number tuned to work on
// one PC is just as likely to under- or over-shoot on the next one. That's
// not a fixable-by-tuning-harder problem; it needed a different mechanism
// entirely: ask the operating system what its own real number is, on each
// machine, automatically, instead of shipping a guess.
//
// WASAPI (Windows' native low-level audio API, underneath SDL2's simpler
// SDL_QueueAudio abstraction) can answer this directly: IAudioClient::
// GetStreamLatency() reports the shared-mode audio engine's own round-trip
// latency for the current default output device. SDL2's own audio API has
// no equivalent call -- it deliberately hides this level of detail -- so
// this file talks to WASAPI directly, in parallel with (not instead of)
// the normal SDL2 audio device main.cpp actually plays through. This is a
// one-shot query at startup, not a replacement for SDL2 playback.

#include "audio/AudioLatency.h"

#ifdef _WIN32
#define INITGUID // gives CLSID_MMDeviceEnumerator/IID_* real storage in
                 // this translation unit, so linking doesn't need a
                 // separate GUID library (uuid.lib and its MinGW
                 // equivalents aren't consistently available/named across
                 // MSYS2 toolchain versions -- this sidesteps that
                 // entirely for the one file that needs these symbols).
#include <objbase.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#endif

#include <cstdio>

namespace dirk {

#ifdef _WIN32
namespace {
template <typename T>
void releaseIfSet(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}
} // namespace

double queryAudioOutputLatencySeconds(double fallbackSeconds) {
    // COM may already be initialized on this thread by SDL2 or something
    // else -- RPC_E_CHANGED_MODE just means "already initialized with a
    // different concurrency model", which is fine, we're borrowing it for
    // one query, not asserting ownership of it.
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool needsUninit = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE) {
        std::fprintf(stderr,
            "[dirk] WASAPI latency query: CoInitializeEx failed (0x%08lx) -- "
            "using fallback of %.1fms\n",
            static_cast<unsigned long>(hr), fallbackSeconds * 1000.0);
        return fallbackSeconds;
    }

    double result = fallbackSeconds;
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;

    hr = CoCreateInstance(CLSID_MMDeviceEnumerator, nullptr, CLSCTX_ALL,
                           IID_IMMDeviceEnumerator, reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr)) {
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    }
    if (SUCCEEDED(hr)) {
        hr = device->Activate(IID_IAudioClient, CLSCTX_ALL, nullptr,
                               reinterpret_cast<void**>(&audioClient));
    }
    if (SUCCEEDED(hr)) {
        hr = audioClient->GetMixFormat(&mixFormat);
    }
    if (SUCCEEDED(hr)) {
        // Shared mode, default (system-chosen) buffer duration -- this is
        // what SDL2's own WASAPI backend uses too, so the number this
        // reports is the same engine period the real playback path (opened
        // separately, by main.cpp, through SDL_OpenAudioDevice) actually
        // runs on.
        hr = audioClient->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 0, 0, mixFormat, nullptr);
    }
    if (SUCCEEDED(hr)) {
        // GetStreamLatency() alone turns out not to be trustworthy: on a
        // real machine (confirmed by hardware testing on this project) it
        // came back as exactly 0.0ms, which no real sound card actually
        // has. This is a known, widely-reported WASAPI quirk, not a bug
        // specific to this code -- a lot of driver/APO stacks just never
        // fill in the "additional device latency" figure GetStreamLatency
        // is documented to report, especially when Initialize() was asked
        // for the default (0) buffer duration, and silently leave it at
        // zero instead of erroring out. So this doesn't rely on that call
        // alone: it also asks how big a buffer the audio engine actually
        // allocated for this stream (GetBufferSize(), only valid to call
        // after Initialize()) -- that buffer has to drain before anything
        // submitted to it is audible, which is real latency regardless of
        // whether the driver also reports additional latency on top of it.
        // The two are summed: GetBufferSize()'s figure is latency we know
        // is real (it's *our* buffer), GetStreamLatency()'s figure is
        // whatever extra the driver claims beyond that -- adding them errs
        // conservative (slightly too much hold) rather than repeating the
        // 0.0ms failure (nowhere near enough), which is the safer side to
        // be wrong on given this project's whole tuning history has been
        // "not enough compensation", never "too much".
        UINT32 bufferFrames = 0;
        HRESULT bufHr = audioClient->GetBufferSize(&bufferFrames);
        double bufferSeconds = 0.0;
        if (SUCCEEDED(bufHr) && mixFormat->nSamplesPerSec > 0) {
            bufferSeconds = static_cast<double>(bufferFrames) / mixFormat->nSamplesPerSec;
        }

        REFERENCE_TIME latency = 0; // 100-nanosecond units
        hr = audioClient->GetStreamLatency(&latency);
        double streamLatencySeconds = SUCCEEDED(hr) ? static_cast<double>(latency) / 10000000.0 : 0.0;

        // Diagnostic only, not used in the result -- if this ever needs
        // debugging again on someone else's machine, having both raw
        // figures on record (rather than just the combined total) saves a
        // round trip.
        REFERENCE_TIME defaultPeriod = 0, minPeriod = 0;
        HRESULT periodHr = audioClient->GetDevicePeriod(&defaultPeriod, &minPeriod);

        // GetStreamLatency() failing outright still falls through to the
        // caller's fallback below (hr stays FAILED); but if it merely
        // *succeeded with a bogus 0*, that's not a failure this code can
        // detect via HRESULT -- it just means streamLatencySeconds is 0.0
        // here and the buffer-size figure alone is carrying the result,
        // which is exactly the situation this whole rewrite is for.
        if (SUCCEEDED(hr) || SUCCEEDED(bufHr)) {
            hr = S_OK; // at least one of the two figures is usable
            result = bufferSeconds + streamLatencySeconds;
            std::fprintf(stderr,
                "[dirk] WASAPI audio latency: buffer=%.1fms + stream=%.1fms = %.1fms"
                " (device period: default=%.1fms min=%.1fms)\n",
                bufferSeconds * 1000.0, streamLatencySeconds * 1000.0, result * 1000.0,
                SUCCEEDED(periodHr) ? defaultPeriod / 10000.0 : -1.0,
                SUCCEEDED(periodHr) ? minPeriod / 10000.0 : -1.0);
        }
    }
    if (FAILED(hr)) {
        std::fprintf(stderr,
            "[dirk] WASAPI latency query failed (0x%08lx) -- using fallback of %.1fms\n",
            static_cast<unsigned long>(hr), fallbackSeconds * 1000.0);
    }

    if (mixFormat) CoTaskMemFree(mixFormat);
    releaseIfSet(audioClient);
    releaseIfSet(device);
    releaseIfSet(enumerator);
    if (needsUninit) CoUninitialize();

    return result;
}
#else
double queryAudioOutputLatencySeconds(double fallbackSeconds) {
    // No equivalent measurement wired up for this platform yet -- the
    // caller's guess is all we have here.
    return fallbackSeconds;
}
#endif

} // namespace dirk
