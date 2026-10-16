// Regression test for review point 1, steps 1 and 2 (2026-09-26).
// Step 1: during a seek,
// VideoSource no longer decodes the non-reference frames it jumps past
// (AVDISCARD_NONREF before the target, see seekTargetPts_ in
// VideoSource.h). This must change NOTHING observable:
//   - every seek lands on exactly the requested frame, with exactly the
//     same decoded picture (all Y/U/V bytes) as with the skip turned off;
//   - normal playback after a seek is untouched: stepping forward from a
//     landed frame must give every following frame, none skipped (the
//     skip must be switched back off as soon as the seek is done).
//
// Step 2: optional multi-threaded decoding (VideoSource::setDecodeThreads)
// must give exactly the same pictures too -- checked with 2 and 4 threads,
// combined with step 1, against a single-threaded no-skip reference.
//
// Clip: assets/test_pattern_bframes.mp4 -- same 64x64, 100-frame pattern
// as seek_test (luma of every pixel = frame number) re-encoded with
// B-frames (x264 -bf 3, b-pyramid, fixed GOP of 40: IBBBPBBBP...), i.e.
// with plenty of non-reference frames for the skip to act on.

#include "video/VideoSource.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

static bool g_allPass = true;
static int g_failuresShown = 0;

static void check(bool cond, const std::string& what, bool quietPass = false) {
    if (!cond) {
        g_allPass = false;
        if (g_failuresShown++ < 20) std::printf("FAIL %s\n", what.c_str());
    } else if (!quietPass) {
        std::printf("PASS %s\n", what.c_str());
    }
}

static std::vector<uint8_t> copyPicture(const AVFrame* f, int w, int h) {
    std::vector<uint8_t> out;
    for (int y = 0; y < h; ++y) out.insert(out.end(), f->data[0] + y * f->linesize[0], f->data[0] + y * f->linesize[0] + w);
    for (int p = 1; p <= 2; ++p)
        for (int y = 0; y < h / 2; ++y)
            out.insert(out.end(), f->data[p] + y * f->linesize[p], f->data[p] + y * f->linesize[p] + w / 2);
    return out;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <test_pattern_bframes.mp4>\n", argv[0]);
        return 1;
    }
    // Reference: single-threaded, no skip -- the decode as it always was.
    dirk::VideoSource reference;
    reference.setSkipNonRefDuringSeek(false);
    check(reference.open(argv[1]), "reference decode (1 thread, no skip) opens");
    const int64_t count = reference.frameCount();
    const int w = reference.width(), h = reference.height();
    check(count == 100, "100 frames indexed");

    // Every frame, in a shuffled order (fixed seed): a mix of long backward
    // jumps, forward jumps within a GOP, and jumps across GOPs.
    std::vector<int64_t> order(static_cast<size_t>(count));
    for (int64_t i = 0; i < count; ++i) order[static_cast<size_t>(i)] = i;
    std::mt19937 rng(20260926);
    std::shuffle(order.begin(), order.end(), rng);
    order.insert(order.end(), {0, 99, 0, 41, 40, 39, 80, 79});

    // Step 1 (skip) alone, then combined with step 2 (multi-threaded
    // decoding, 2 and 4 threads): each must land on exactly the same
    // pictures as the reference, and play forward without dropping frames.
    for (int threads : {1, 2, 4}) {
        const std::string tag = "[skip, " + std::to_string(threads) + " thread(s)] ";
        dirk::VideoSource withSkip;
        withSkip.setDecodeThreads(threads);
        if (!withSkip.open(argv[1])) { check(false, tag + "opens"); continue; }
        check(withSkip.decodeThreads() == threads, tag + "decoder really runs with " + std::to_string(threads) + " thread(s)");

        int landed = 0, identical = 0;
        for (int64_t f : order) {
            const bool a = withSkip.seekToFrame(f);
            const bool b = reference.seekToFrame(f);
            const AVFrame* pa = withSkip.currentPicture();
            const AVFrame* pb = reference.currentPicture();
            const bool ok = a && b && pa && pb && withSkip.currentFrameNumber() == f &&
                            reference.currentFrameNumber() == f && pa->data[0][0] == static_cast<uint8_t>(f % 256);
            check(ok, tag + "seek to " + std::to_string(f) + " lands exactly (frame number + pixel)", true);
            if (!ok) continue;
            ++landed;
            const bool same = copyPicture(pa, w, h) == copyPicture(pb, w, h);
            check(same, tag + "seek to " + std::to_string(f) + ": picture identical to the reference decode", true);
            if (same) ++identical;
        }
        check(landed == static_cast<int>(order.size()),
              tag + std::to_string(landed) + "/" + std::to_string(order.size()) + " seeks landed on the exact frame");
        check(identical == static_cast<int>(order.size()),
              tag + std::to_string(identical) + "/" + std::to_string(order.size()) + " landed pictures byte-identical to the reference");

        // Playback after a seek: no frame may be skipped any more.
        int stepped = 0, stepOk = 0;
        for (int64_t start : {1, 5, 42, 57, 83}) {
            if (!withSkip.seekToFrame(start)) { check(false, tag + "seek before playback to " + std::to_string(start)); continue; }
            for (int64_t k = 1; k <= 12 && start + k < count; ++k) {
                ++stepped;
                const bool ok = withSkip.stepForward() && withSkip.currentFrameNumber() == start + k &&
                                withSkip.currentPicture()->data[0][0] == static_cast<uint8_t>((start + k) % 256);
                check(ok, tag + "playback after seek to " + std::to_string(start) + ": frame " + std::to_string(start + k), true);
                if (ok) ++stepOk;
            }
        }
        check(stepOk == stepped, tag + "normal playback after a seek: " + std::to_string(stepOk) + "/" +
                                 std::to_string(stepped) + " consecutive frames, none skipped");

        // Playing through to the end must still reach the last frame (frame
        // threading holds frames in flight: the end-of-file drain must
        // still deliver them).
        bool reachedEnd = withSkip.seekToFrame(90);
        while (reachedEnd && withSkip.currentFrameNumber() < count - 1) reachedEnd = withSkip.stepForward();
        check(reachedEnd && withSkip.currentFrameNumber() == count - 1,
              tag + "playing to the end delivers the very last frame (" + std::to_string(count - 1) + ")");
    }

    std::printf(g_allPass ? "\nALL PASS\n" : "\nSOME FAILED\n");
    return g_allPass ? 0 : 1;
}
