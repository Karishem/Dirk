#include "core/BlackScan.h"
#include "video/VideoSource.h"

extern "C" {
#include <libavutil/frame.h>
#include <libavutil/pixfmt.h>
}

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <cstring>

namespace dirk {

BlackScan::~BlackScan() { cancel(); }

bool BlackScan::start(const std::string& videoPath, int64_t a, int64_t b, int decodeThreads) {
    if (state_.load() == State::Running) return false;
    if (worker_.joinable()) worker_.join();
    if (b < a) std::swap(a, b);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        error_.clear();
        pieces_.clear();
    }
    stop_ = false;
    progress_ = 0.0;
    state_ = State::Running;
    worker_ = std::thread(&BlackScan::run, this, videoPath, a, b, decodeThreads);
    return true;
}

void BlackScan::cancel() {
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    if (state_.load() == State::Running) state_ = State::Idle;
}

std::string BlackScan::error() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return error_;
}

std::vector<ScanPiece> BlackScan::pieces() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pieces_;
}

std::vector<ScanPiece> BlackScan::split(int64_t a, const std::vector<uint8_t>& black, const std::vector<float>& db) {
    const int64_t n = static_cast<int64_t>(black.size());
    std::vector<ScanPiece> out;
    int64_t pieceStart = 0;
    auto addPiece = [&](int64_t from, int64_t to) { // relative, inclusive
        if (to - from + 1 >= kMinPieceFrames) out.push_back({a + from, a + to});
    };
    int64_t i = 0;
    while (i < n) {
        if (!black[static_cast<size_t>(i)]) { ++i; continue; }
        int64_t j = i;
        while (j < n && black[static_cast<size_t>(j)]) ++j;
        if (j - i >= kMinRunFrames) {
            std::vector<float> run(db.begin() + i, db.begin() + j);
            std::nth_element(run.begin(), run.begin() + run.size() / 2, run.end());
            if (run[run.size() / 2] < kSilentDb) {
                if (i > pieceStart) addPiece(pieceStart, i - 1);
                pieceStart = j;
            }
        }
        i = j;
    }
    if (pieceStart < n) addPiece(pieceStart, n - 1);
    return out;
}

void BlackScan::run(std::string path, int64_t a, int64_t b, int threads) {
    auto fail = [&](const std::string& why) {
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = why;
        state_ = State::Failed;
    };
    const auto began = std::chrono::steady_clock::now();
    VideoSource src;
    src.setDecodeThreads(threads);
    src.setScanOnly(true);
    if (!src.open(path)) { fail("could not open the movie"); return; }
    const int64_t count = src.frameCount();
    if (count <= 0) { fail("the movie has no frames"); return; }
    a = std::max<int64_t>(0, std::min(a, count - 1));
    b = std::max<int64_t>(a, std::min(b, count - 1));
    if (!src.seekToFrame(a)) { fail("could not reach frame " + std::to_string(a)); return; }

    const size_t n = static_cast<size_t>(b - a + 1);
    std::vector<uint8_t> black(n, 0);
    std::vector<double> sumSq(n, 0.0);
    std::vector<int64_t> samples(n, 0);
    const double fps = src.fps() > 0 ? src.fps() : 29.97;
    const double t0 = src.frameTimeSeconds(a);
    const int rate = src.audioSampleRate();
    const int channels = std::max(1, src.audioChannels());
    // Sound decoded while reaching frame a (seekToFrame() keeps it lined up
    // with the picture): the bins below take only what falls from frame a on.
    const double firstStart = src.pendingAudioStartSeconds();
    std::vector<uint8_t> dropped = src.takePendingAudio();
    auto addAudio = [&](const std::vector<uint8_t>& pcm, double start) {
        const size_t frames = pcm.size() / (2 * static_cast<size_t>(channels));
        const int16_t* s = reinterpret_cast<const int16_t*>(pcm.data());
        for (size_t k = 0; k < frames; ++k) {
            const double t = start + static_cast<double>(k) / rate;
            const int64_t bin = static_cast<int64_t>(std::floor((t - t0) * fps));
            if (bin < 0 || bin >= static_cast<int64_t>(n)) continue;
            double e = 0.0;
            for (int c = 0; c < channels; ++c) {
                const double v = s[k * channels + c] / 32768.0;
                e += v * v;
            }
            sumSq[static_cast<size_t>(bin)] += e / channels;
            ++samples[static_cast<size_t>(bin)];
        }
    };
    if (!dropped.empty()) addAudio(dropped, firstStart);

    for (int64_t cur = a; cur <= b;) {
        if (stop_) return;
        if (cur > a || src.currentFrameNumber() != a) {
            if (!src.stepForward()) break; // the movie ended early: the rest stays "not black"
        }
        cur = src.currentFrameNumber();
        if (cur < a) { cur = a; continue; }
        if (cur > b) break;
        const size_t i = static_cast<size_t>(cur - a);
        ++cur;
        if (const AVFrame* pic = src.scanPicture()) {
            // Mean of the luma plane, every 4th pixel of every 4th line.
            const int w = pic->width, h = pic->height;
            double sum = 0.0;
            int64_t cnt = 0;
            for (int y = 0; y < h; y += 4) {
                const uint8_t* row = pic->data[0] + static_cast<size_t>(y) * pic->linesize[0];
                for (int x = 0; x < w; x += 4) { sum += row[x]; ++cnt; }
            }
            double luma = cnt ? sum / cnt : 0.0;
            const AVPixelFormat f = static_cast<AVPixelFormat>(pic->format);
            const bool full = pic == src.currentPicture() ? src.yuvFullRange()
                              : (pic->color_range == AVCOL_RANGE_JPEG || f == AV_PIX_FMT_YUVJ420P || f == AV_PIX_FMT_YUVJ422P ||
                                 f == AV_PIX_FMT_YUVJ444P || f == AV_PIX_FMT_YUVJ440P || f == AV_PIX_FMT_YUVJ411P);
            if (!full) luma = (luma - 16.0) * 255.0 / 219.0; // limited range: 16 is black
            black[i] = luma < kBlackLuma ? 1 : 0;
        }
        const double start = src.pendingAudioStartSeconds();
        std::vector<uint8_t> pcm = src.takePendingAudio();
        if (!pcm.empty()) addAudio(pcm, start);
        progress_ = static_cast<double>(i + 1) / static_cast<double>(n);
    }

    std::vector<float> db(n, -120.0f);
    if (src.hasAudio()) {
        for (size_t i = 0; i < n; ++i) {
            if (samples[i] > 0) db[i] = static_cast<float>(10.0 * std::log10(sumSq[i] / samples[i] + 1e-12));
        }
    }
    std::vector<ScanPiece> result = split(a, black, db);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pieces_ = result;
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - began).count();
    std::fprintf(stderr, "[dirk] wizard scan %lld-%lld: %zu piece(s) between black and silent runs (%.1f s)\n",
                 static_cast<long long>(a), static_cast<long long>(b), result.size(), secs);
    state_ = State::Done;
}

} // namespace dirk
