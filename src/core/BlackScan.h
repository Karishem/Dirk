#pragma once

// AUTO CREATE LEVELS / AUTO CREATE DEATHS in Wizard (2026-10-15, Cyril):
// the author marks the first frame of the first level (or death) and the
// last frame of the last one; this finds where the movie separates them.
// Tested first on DLe and Nimh (cut-down copies of both, against their
// sheets): what separates levels and deaths on these discs is a run of
// black frames that is also silent -- not picture cuts, which also happen
// inside every scene. Silence matters: Nimh has two dark passages inside
// levels where something can still be heard; black alone would have
// split those levels in two.
//
// A separator = at least kMinRunFrames frames in a row whose picture is
// black (mean luma, 0-255 scale, below kBlackLuma) and, over the run, a
// median sound level below kSilentDb (a movie without sound: black alone).
// The pieces between separators are the result; pieces shorter than
// kMinPieceFrames are dropped. A fade into / out of black is cut where the
// picture becomes black, so a piece can differ by a few frames from what
// an author would mark by hand -- corrected with FRAMES.
//
// Runs on its own thread with its own VideoSource on the same file (the
// game's own movie keeps playing untouched); Wizard polls it, see
// dirkScanStart() and the other dirkScan* functions in lua/api_singe.cpp.

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace dirk {

struct ScanPiece {
    int64_t a = 0;
    int64_t b = 0;
};

class BlackScan {
public:
    static constexpr int kMinRunFrames = 8;
    static constexpr double kBlackLuma = 3.0;
    static constexpr double kSilentDb = -60.0;
    static constexpr int kMinPieceFrames = 3;

    ~BlackScan();

    // Starts scanning frames a..b of `videoPath` (a <= b, swapped if not).
    // False when a scan is already running.
    bool start(const std::string& videoPath, int64_t a, int64_t b, int decodeThreads);
    // Stops a running scan (its result is dropped); waits for the thread.
    void cancel();

    enum class State { Idle, Running, Done, Failed };
    State state() const { return state_.load(); }
    double progress() const { return progress_.load(); }
    std::string error() const;
    std::vector<ScanPiece> pieces() const; // when Done

    // The splitting rule alone (also used by the tests): `black` and `db`
    // hold one value per frame from a to b.
    static std::vector<ScanPiece> split(int64_t a, const std::vector<uint8_t>& black, const std::vector<float>& db);

private:
    void run(std::string path, int64_t a, int64_t b, int threads);

    std::thread worker_;
    std::atomic<State> state_{State::Idle};
    std::atomic<double> progress_{0.0};
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    std::string error_;
    std::vector<ScanPiece> pieces_;
};

} // namespace dirk
