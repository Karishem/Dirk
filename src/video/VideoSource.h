#pragma once
// VideoSource: frame-accurate video decode + seek, backing discSkipToFrame /
// discGetFrame / discPause / discPlay / discStop for Dirk.
//
// Design notes (see the Dirk feasibility study, section 7, for the full
// reasoning):
//   - Singe's own approach is to parse the video once on first launch and
//     cache a frame index. We do the same, but with our own index format --
//     there is no need to bit-match Singe's undocumented frame_XXXX.txt,
//     since that file is never touched by Lua scripts; only the *behavior*
//     of discSkipToFrame needs to match.
//   - Most video codecs can only start decoding from a keyframe and must
//     decode forward sequentially to reach an arbitrary target frame. The
//     index lets us jump to the *nearest preceding keyframe* in one seek,
//     then decode-and-discard only the handful of frames between that
//     keyframe and the target, rather than re-scanning the whole file.
//   - Frame numbers are 0-based decode-order indices, matching how
//     KimmyScript's discSkipToFrame(frame) is used (a plain integer frame
//     count into the movie).

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
}

namespace dirk {

struct FrameIndexEntry {
    int64_t pts = 0;        // presentation timestamp, in the stream's time_base
    bool isKeyframe = false;
};

class VideoSource {
public:
    VideoSource() = default;
    ~VideoSource();

    VideoSource(const VideoSource&) = delete;
    VideoSource& operator=(const VideoSource&) = delete;

    // Opens `path`. Builds (or loads a cached) frame index. Returns false on
    // failure (bad path, no video stream, unsupported codec, ...).
    bool open(const std::string& path);
    void close();

    bool isOpen() const { return formatCtx_ != nullptr; }

    // Total number of frames the index knows about.
    int64_t frameCount() const { return static_cast<int64_t>(index_.size()); }

    int width() const { return width_; }
    int height() const { return height_; }

    // discSkipToFrame(frame): seeks to the nearest preceding keyframe, then
    // decodes forward until `frame` is reached. Leaves the decoded picture
    // in currentFrame(). Returns false if `frame` is out of range or a
    // decode error occurs.
    bool seekToFrame(int64_t frame);

    // discGetFrame(): the frame number currently decoded/displayed.
    int64_t currentFrameNumber() const { return currentFrame_; }

    // Decodes exactly one frame forward from the current position (used by
    // discPlay's per-tick advance). Returns false at end of stream.
    bool stepForward();

    // Raw decoded picture (YUV420P from swscale-normalized output), or
    // nullptr if nothing has been decoded yet. Owned by VideoSource.
    const AVFrame* currentPicture() const { return rgbFrame_; }

private:
    bool buildOrLoadIndex(const std::string& path);
    bool buildIndex();
    bool loadIndexCache(const std::string& cachePath);
    bool saveIndexCache(const std::string& cachePath) const;
    static std::string indexCachePath(const std::string& videoPath);

    // Converts a stream PTS to a 0-based frame number using the stream's
    // average frame rate (constant-frame-rate assumption -- true for every
    // KimmyScript asset observed so far; see feasibility study section 7).
    int64_t ptsToFrameNumber(int64_t pts) const;

    bool decodeOneFrame(); // pulls + decodes the next packet belonging to the video stream

    AVFormatContext* formatCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* decodeFrame_ = nullptr; // raw decoder output (native pixel format)
    AVFrame* rgbFrame_ = nullptr;    // swscale-converted output for rendering
    AVPacket* packet_ = nullptr;
    SwsContext* swsCtx_ = nullptr;

    int videoStreamIndex_ = -1;
    int width_ = 0;
    int height_ = 0;

    std::vector<FrameIndexEntry> index_;
    int64_t currentFrame_ = -1;

    // Reduced fraction avg_frame_rate = fpsNum_/fpsDen_, and the stream's
    // time_base, cached at open() time for pts<->frame conversion.
    int fpsNum_ = 0;
    int fpsDen_ = 1;
    int tbNum_ = 0;
    int tbDen_ = 1;
};

} // namespace dirk
