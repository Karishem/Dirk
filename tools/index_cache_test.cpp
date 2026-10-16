// Regression test for review point 7 (2026-09-26): the .dirkindex cache
// must never be reused for a movie it wasn't built from.
//
// The trap (reproduced for real before this fix, same two clips as here):
// build the index of a movie, then replace the movie with a re-encode at
// the SAME resolution -- e.g. shorter keyframe spacing to make seeks
// cheaper (review point 1). The old v1 index only checked width/height, so
// it was silently reused: 11 of 13 seeks then failed ("seekToFrame
// returned false"), the kind of failure that freezes a game.
//
// Clips (both 64x64, 100 frames, luma of every pixel = frame number, see
// tools/seek_test.cpp for the method):
//   assets/test_pattern.mp4       -- the original (4 keyframes)
//   assets/test_pattern_gop7.mp4  -- same frames re-encoded with ffmpeg
//       -g 7 -video_track_timescale 90000 (15 keyframes, different
//       time base): what "re-encoding the same movie" really produces.
//
// Checks: index built on first open and recognized afterwards (and NOT
// rebuilt on a normal relaunch); replacing the movie invalidates it and
// the rebuilt index seeks correctly (frame number AND decoded pixels); a
// legacy v1 index is rebuilt once as v2; a date change alone invalidates.

#include "video/VideoSource.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static bool g_allPass = true;

static void check(bool cond, const std::string& what) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) g_allPass = false;
}

static std::string magicOf(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    char m[8] = {};
    in.read(m, 8);
    return std::string(m, in ? 8 : 0);
}

// Same frame-number + pixel check as tools/seek_test.cpp, over the same
// out-of-order plan (forward decode, backward jumps, revisits, bounds).
static bool seeksLandCorrectly(dirk::VideoSource& disc, std::string& firstFailure) {
    const int64_t last = disc.frameCount() - 1;
    const std::vector<int64_t> plan = {0, 1, 2, 10, last / 2, last, 0, last / 4, last / 2, last - 1, 3, last, 25, 26};
    for (int64_t f : plan) {
        if (f < 0 || f > last) continue;
        const AVFrame* pic = nullptr;
        if (!disc.seekToFrame(f) || disc.currentFrameNumber() != f || !(pic = disc.currentPicture()) ||
            !pic->data[0] || pic->data[0][0] != static_cast<uint8_t>(f % 256)) {
            firstFailure = "frame " + std::to_string(f);
            return false;
        }
    }
    return true;
}

static void bumpDate(const fs::path& p, int seconds) {
    fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(seconds));
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <test_pattern.mp4> <test_pattern_gop7.mp4>\n", argv[0]);
        return 1;
    }
    const fs::path original = argv[1];
    const fs::path reencoded = argv[2];

    const fs::path dir = fs::temp_directory_path() / "dirk_index_cache_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    const fs::path movie = dir / "movie.mp4";
    const fs::path index = dir / "movie.mp4.dirkindex";
    fs::copy_file(original, movie);

    // 1) First launch: no index yet -> built, saved as v2.
    check(!dirk::VideoSource::hasCachedIndexFile(movie.string()), "no index yet -> reported as needing a build");
    {
        dirk::VideoSource disc;
        check(disc.open(movie.string()), "first open builds the index");
    }
    check(magicOf(index) == "DIRKIDX2", "index saved in the new v2 format");
    check(dirk::VideoSource::hasCachedIndexFile(movie.string()), "index now recognized as usable");

    // 2) Normal relaunch: loaded, not rebuilt (index file left untouched).
    {
        const auto before = fs::last_write_time(index);
        dirk::VideoSource disc;
        check(disc.open(movie.string()), "second open succeeds");
        check(fs::last_write_time(index) == before, "...by LOADING the index, not rebuilding it");
        std::string fail;
        check(seeksLandCorrectly(disc, fail), "seeks correct on the original movie" + (fail.empty() ? "" : " (" + fail + ")"));
    }

    // 3) The trap: movie replaced by a same-resolution re-encode.
    fs::copy_file(reencoded, movie, fs::copy_options::overwrite_existing);
    bumpDate(movie, 5); // don't depend on the filesystem's timestamp granularity
    check(!dirk::VideoSource::hasCachedIndexFile(movie.string()),
          "re-encoded movie (same 64x64 resolution) -> old index reported STALE (INDEXING screen will show)");
    {
        dirk::VideoSource disc;
        check(disc.open(movie.string()), "re-encoded movie opens (index rebuilt)");
        check(dirk::VideoSource::hasCachedIndexFile(movie.string()), "...and the rebuilt index is saved and valid");
        std::string fail;
        check(seeksLandCorrectly(disc, fail),
              "seeks land on the right frames AND pixels after re-encode" + (fail.empty() ? "" : " (failed at " + fail + ")"));
    }

    // 4) Legacy v1 index (what every existing game has today): rebuilt once as v2.
    {
        std::ofstream out(index, std::ios::binary | std::ios::trunc);
        const char v1[8] = {'D', 'I', 'R', 'K', 'I', 'D', 'X', '1'};
        const int64_t count = 100;
        const int32_t w = 64, h = 64, zero = 0;
        out.write(v1, 8);
        out.write(reinterpret_cast<const char*>(&count), sizeof(count));
        out.write(reinterpret_cast<const char*>(&w), sizeof(w));
        out.write(reinterpret_cast<const char*>(&h), sizeof(h));
        for (int i = 0; i < 4; ++i) out.write(reinterpret_cast<const char*>(&zero), sizeof(zero));
        for (int64_t i = 0; i < count; ++i) { // plausible-looking but wrong (v1 had no way to tell)
            const int64_t pts = i * 512;
            const uint8_t key = (i % 25 == 0) ? 1 : 0;
            out.write(reinterpret_cast<const char*>(&pts), sizeof(pts));
            out.write(reinterpret_cast<const char*>(&key), sizeof(key));
        }
    }
    check(!dirk::VideoSource::hasCachedIndexFile(movie.string()), "legacy v1 index -> reported as needing a rebuild");
    {
        dirk::VideoSource disc;
        check(disc.open(movie.string()), "open with a v1 index succeeds (rebuilds)");
        check(magicOf(index) == "DIRKIDX2", "...and upgrades the file to v2");
        std::string fail;
        check(seeksLandCorrectly(disc, fail), "seeks correct after the v1 -> v2 rebuild" + (fail.empty() ? "" : " (" + fail + ")"));
    }

    // 5) Date change alone (same content, e.g. file touched/replaced by a copy
    //    that didn't keep the date) -> one harmless rebuild, never a stale load.
    bumpDate(movie, 60);
    check(!dirk::VideoSource::hasCachedIndexFile(movie.string()), "movie date changed -> index reported stale");

    fs::remove_all(dir, ec);
    std::printf(g_allPass ? "\nALL PASS\n" : "\nSOME FAILED\n");
    return g_allPass ? 0 : 1;
}
