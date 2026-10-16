// Regression test for review point 1, step 2 (2026-09-26): with a multi-
// threaded video decoder, the audio queued right after a seek must be the
// RIGHT audio, starting exactly at the landed frame.
//
// Why: a multi-threaded decoder holds frames in flight, so by the time a
// seek lands, the demuxer has already read -- and, audio being muted during
// the catch-up, discarded -- the audio of the landed frame and the next few.
// Measured before the fix: audio started ~40 ms (2 threads) / ~120 ms (4
// threads) LATER in the content than the picture, for the whole scene after
// every cut. VideoSource now keeps that audio and aligns the queue's first
// sample on the landed frame (see seekKeptAudio_ in VideoSource.h).
//
// Method (no audio device involved): decode the whole clip linearly once,
// single-threaded, to get the reference audio timeline. Then, with 2 and 4
// decoder threads, seek to various frames and check that
//   1. the queued audio's first sample is at the landed frame's time, and
//   2. its content -- that audio and the next ~0.5 s the queue receives --
//      matches the reference at that exact position (best match at lag 0
//      within +-80 samples, correlation > 0.99).
// Clips: assets/test_av_chirp.mp4 -- 160x90, 23.976 fps, B-frames, GOP 48,
// AAC audio of a rising chirp (a frequency that never repeats, so a wrong
// position can't accidentally match) -- plus the same streams remuxed with
// the audio stored in blocks of 10 frames, like Premiere exports:
// test_av_chirp_blocks_vfirst.mp4 (10 frames, then their audio) and
// test_av_chirp_blocks_afirst.mp4 (the audio, then its 10 frames).
// Checked with 1, 2 and 4 decoder threads (the alignment applies to all
// since Cyril's cabinet test, see VideoSource.h). Before that, the
// single-threaded decoder measured +13 / -160 / +224 ms here.

#include "video/VideoSource.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static bool g_allPass = true;

static void check(bool cond, const std::string& what) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) g_allPass = false;
}

static std::vector<int16_t> toSamples(const std::vector<uint8_t>& bytes) {
    std::vector<int16_t> s(bytes.size() / 2);
    if (!s.empty()) std::memcpy(s.data(), bytes.data(), s.size() * 2);
    return s;
}

// Normalized correlation of a[0..n) against b[off..off+n), stereo interleaved.
static double correlation(const std::vector<int16_t>& a, const std::vector<int16_t>& b, long long off, size_t n) {
    if (off < 0 || static_cast<size_t>(off) + n > b.size() || n > a.size()) return -2.0;
    double ab = 0, aa = 0, bb = 0;
    for (size_t i = 0; i < n; ++i) {
        const double x = a[i], y = b[static_cast<size_t>(off) + i];
        ab += x * y; aa += x * x; bb += y * y;
    }
    return (aa > 0 && bb > 0) ? ab / std::sqrt(aa * bb) : -2.0;
}

static void testClip(const char* path);

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <clip.mp4>...\n", argv[0]);
        return 1;
    }
    for (int i = 1; i < argc; ++i) testClip(argv[i]);
    std::printf(g_allPass ? "\nALL PASS\n" : "\nSOME FAILED\n");
    return g_allPass ? 0 : 1;
}

static void testClip(const char* path) {
    constexpr int kRate = 48000, kCh = 2;
    std::string clip = path;
    clip = clip.substr(clip.find_last_of("/\\") + 1);
    std::printf("== %s\n", clip.c_str());

    // Reference: straight-through decode, 1 thread, no seeks.
    std::vector<int16_t> ref;
    double refStart = 0.0;
    double fps = 0.0;
    int64_t frames = 0;
    {
        dirk::VideoSource v;
        check(v.open(path), clip + " reference: clip opens");
        fps = v.fps();
        frames = v.frameCount();
        bool first = true;
        while (v.stepForward()) {
            if (v.pendingAudioBytes() == 0) continue;
            if (first) { refStart = v.pendingAudioStartSeconds(); first = false; }
            const auto s = toSamples(v.takePendingAudio());
            ref.insert(ref.end(), s.begin(), s.end());
        }
        check(ref.size() > static_cast<size_t>(8 * kRate * kCh), clip + " reference: whole audio track decoded (" +
              std::to_string(ref.size() / kCh / kRate) + " s)");
    }

    const int targets[] = {30, 75, 130, 171, 60, 200, 97, 12};
    for (int threads : {1, 2, 4}) {
        const std::string tag = clip + " [" + std::to_string(threads) + " thread(s)] ";
        dirk::VideoSource v;
        v.setDecodeThreads(threads);
        v.setAudioLeadSeconds(0.12);
        if (!v.open(path)) { check(false, tag + "opens"); continue; }
        int aligned = 0, matched = 0, tested = 0;
        double worstStartErrMs = 0.0;
        for (int t : targets) {
            if (t >= frames) continue;
            ++tested;
            if (!v.seekToFrame(t) || v.pendingAudioBytes() == 0) {
                std::printf("  seek %d: no audio queued\n", t);
                continue;
            }
            const double landed = t / fps;
            const double start = v.pendingAudioStartSeconds();
            const double errMs = (start - landed) * 1000.0;
            if (std::fabs(errMs) > std::fabs(worstStartErrMs)) worstStartErrMs = errMs;
            if (std::fabs(errMs) <= 1.0) ++aligned;

            // Content check: skip the first 2048 sample frames (the AAC
            // decoder restarts from a flush at every seek, so its very first
            // output differs from a continuous decode), then compare 4096.
            // The queue right after a seek only holds ~the audio lead
            // (~120 ms); keep playing a few frames and append what they
            // queue -- the queue is one contiguous stream, so this is
            // simply the continuation of the same audio.
            auto got = toSamples(v.takePendingAudio());
            for (int k = 0; k < 12 && v.stepForward(); ++k) {
                const auto more = toSamples(v.takePendingAudio());
                got.insert(got.end(), more.begin(), more.end());
            }
            const size_t skip = 2048 * kCh, n = 8192 * kCh;
            if (got.size() < skip + n) { std::printf("  seek %d: too little audio queued (%zu)\n", t, got.size()); continue; }
            const std::vector<int16_t> window(got.begin() + static_cast<std::ptrdiff_t>(skip),
                                              got.begin() + static_cast<std::ptrdiff_t>(skip + n));
            const long long base = static_cast<long long>(std::llround((start - refStart) * kRate)) * kCh + static_cast<long long>(skip);
            long long bestLag = 0;
            double best = -3.0;
            for (long long lag = -80; lag <= 80; ++lag) {
                const double c = correlation(window, ref, base + lag * kCh, n);
                if (c > best) { best = c; bestLag = lag; }
            }
            if (bestLag == 0 && best > 0.99) ++matched;
            else std::printf("  seek %d: best match at lag %lld samples (corr %.4f)\n", t, bestLag, best);
        }
        check(aligned == tested, tag + std::to_string(aligned) + "/" + std::to_string(tested) +
              " seeks: queued audio starts at the landed frame (worst " + std::to_string(worstStartErrMs) + " ms)");
        check(matched == tested, tag + std::to_string(matched) + "/" + std::to_string(tested) +
              " seeks: queued audio is the reference audio at that exact position (lag 0, corr > 0.99)");
    }
}
