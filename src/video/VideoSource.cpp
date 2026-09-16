#include "video/VideoSource.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

namespace dirk {

namespace {
constexpr char kIndexMagic[8] = {'D', 'I', 'R', 'K', 'I', 'D', 'X', '1'};
}

VideoSource::~VideoSource() {
    close();
}

std::string VideoSource::indexCachePath(const std::string& videoPath) {
    return videoPath + ".dirkindex";
}

bool VideoSource::open(const std::string& path) {
    close();

    if (avformat_open_input(&formatCtx_, path.c_str(), nullptr, nullptr) != 0) {
        std::fprintf(stderr, "[dirk] VideoSource: could not open '%s'\n", path.c_str());
        return false;
    }
    if (avformat_find_stream_info(formatCtx_, nullptr) < 0) {
        std::fprintf(stderr, "[dirk] VideoSource: could not find stream info in '%s'\n", path.c_str());
        close();
        return false;
    }

    const AVCodec* decoder = nullptr;
    videoStreamIndex_ = av_find_best_stream(formatCtx_, AVMEDIA_TYPE_VIDEO, -1, -1, &decoder, 0);
    if (videoStreamIndex_ < 0 || !decoder) {
        std::fprintf(stderr, "[dirk] VideoSource: no video stream in '%s'\n", path.c_str());
        close();
        return false;
    }

    AVStream* stream = formatCtx_->streams[videoStreamIndex_];

    codecCtx_ = avcodec_alloc_context3(decoder);
    if (!codecCtx_ || avcodec_parameters_to_context(codecCtx_, stream->codecpar) < 0) {
        std::fprintf(stderr, "[dirk] VideoSource: could not set up codec context\n");
        close();
        return false;
    }
    if (avcodec_open2(codecCtx_, decoder, nullptr) < 0) {
        std::fprintf(stderr, "[dirk] VideoSource: could not open codec\n");
        close();
        return false;
    }

    width_ = codecCtx_->width;
    height_ = codecCtx_->height;

    AVRational fps = av_guess_frame_rate(formatCtx_, stream, nullptr);
    if (fps.num <= 0 || fps.den <= 0) {
        // Fall back to a sane NTSC-film default rather than dividing by zero;
        // real assets should always have a valid rate.
        fps = AVRational{24000, 1001};
    }
    fpsNum_ = fps.num;
    fpsDen_ = fps.den;
    tbNum_ = stream->time_base.num;
    tbDen_ = stream->time_base.den;

    decodeFrame_ = av_frame_alloc();
    rgbFrame_ = av_frame_alloc();
    packet_ = av_packet_alloc();
    if (!decodeFrame_ || !rgbFrame_ || !packet_) {
        close();
        return false;
    }

    // We keep the decoded picture as YUV420P (matches what SDL_UpdateYUVTexture
    // wants directly, so the renderer never pays for a CPU colour convert on
    // top of this one) regardless of the source pixel format.
    rgbFrame_->format = AV_PIX_FMT_YUV420P;
    rgbFrame_->width = width_;
    rgbFrame_->height = height_;
    if (av_frame_get_buffer(rgbFrame_, 32) < 0) {
        std::fprintf(stderr, "[dirk] VideoSource: could not allocate picture buffer\n");
        close();
        return false;
    }

    if (!buildOrLoadIndex(path)) {
        std::fprintf(stderr, "[dirk] VideoSource: could not build frame index for '%s'\n", path.c_str());
        close();
        return false;
    }

    currentFrame_ = -1;
    return true;
}

void VideoSource::close() {
    if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
    if (packet_) { av_packet_free(&packet_); }
    if (decodeFrame_) { av_frame_free(&decodeFrame_); }
    if (rgbFrame_) { av_frame_free(&rgbFrame_); }
    if (codecCtx_) { avcodec_free_context(&codecCtx_); }
    if (formatCtx_) { avformat_close_input(&formatCtx_); }
    videoStreamIndex_ = -1;
    width_ = height_ = 0;
    index_.clear();
    currentFrame_ = -1;
}

int64_t VideoSource::ptsToFrameNumber(int64_t pts) const {
    // frame = pts * time_base * fps, done in integer-safe order:
    //   frame = pts * tbNum * fpsNum / (tbDen * fpsDen)
    // Uses double for simplicity in this first pass; fine for movie-length
    // (tens of thousands of frames) precision, revisit if it ever matters.
    const double timeBase = static_cast<double>(tbNum_) / static_cast<double>(tbDen_);
    const double fps = static_cast<double>(fpsNum_) / static_cast<double>(fpsDen_);
    return static_cast<int64_t>(std::llround(pts * timeBase * fps));
}

bool VideoSource::buildOrLoadIndex(const std::string& path) {
    const std::string cachePath = indexCachePath(path);
    if (loadIndexCache(cachePath)) {
        return true;
    }
    if (!buildIndex()) {
        return false;
    }
    saveIndexCache(cachePath); // best-effort; a failed cache write isn't fatal
    return true;
}

bool VideoSource::buildIndex() {
    // First-launch parse: walk every packet in the video stream once,
    // recording its presentation timestamp and keyframe flag. This mirrors
    // what Singe itself does (see feasibility study section 7) -- we pay
    // this cost once per video, ever.
    index_.clear();

    AVPacket* pkt = av_packet_alloc();
    if (!pkt) return false;

    while (av_read_frame(formatCtx_, pkt) >= 0) {
        if (pkt->stream_index == videoStreamIndex_) {
            FrameIndexEntry entry;
            entry.pts = (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts;
            entry.isKeyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;
            index_.push_back(entry);
        }
        av_packet_unref(pkt);
    }
    av_packet_free(&pkt);

    // Packets aren't guaranteed to arrive in presentation order (B-frames),
    // so sort the index by pts and keep it that way -- discSkipToFrame
    // addresses frames by *display* order, which is what scripts expect.
    std::sort(index_.begin(), index_.end(),
              [](const FrameIndexEntry& a, const FrameIndexEntry& b) { return a.pts < b.pts; });

    // Rewind for normal decoding.
    if (av_seek_frame(formatCtx_, videoStreamIndex_, 0, AVSEEK_FLAG_BACKWARD) < 0) {
        return false;
    }
    avcodec_flush_buffers(codecCtx_);

    return !index_.empty();
}

bool VideoSource::loadIndexCache(const std::string& cachePath) {
    std::ifstream in(cachePath, std::ios::binary);
    if (!in) return false;

    char magic[8];
    in.read(magic, sizeof(magic));
    if (!in || std::memcmp(magic, kIndexMagic, sizeof(magic)) != 0) return false;

    int64_t count = 0;
    int32_t w = 0, h = 0, fn = 0, fd = 0, tn = 0, td = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    in.read(reinterpret_cast<char*>(&w), sizeof(w));
    in.read(reinterpret_cast<char*>(&h), sizeof(h));
    in.read(reinterpret_cast<char*>(&fn), sizeof(fn));
    in.read(reinterpret_cast<char*>(&fd), sizeof(fd));
    in.read(reinterpret_cast<char*>(&tn), sizeof(tn));
    in.read(reinterpret_cast<char*>(&td), sizeof(td));
    if (!in || count < 0 || w != width_ || h != height_) return false;

    std::vector<FrameIndexEntry> loaded(static_cast<size_t>(count));
    for (auto& e : loaded) {
        int64_t pts = 0;
        uint8_t keyframe = 0;
        in.read(reinterpret_cast<char*>(&pts), sizeof(pts));
        in.read(reinterpret_cast<char*>(&keyframe), sizeof(keyframe));
        if (!in) return false;
        e.pts = pts;
        e.isKeyframe = keyframe != 0;
    }

    index_ = std::move(loaded);
    fpsNum_ = fn; fpsDen_ = fd; tbNum_ = tn; tbDen_ = td;
    return true;
}

bool VideoSource::saveIndexCache(const std::string& cachePath) const {
    std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    const int64_t count = static_cast<int64_t>(index_.size());
    const int32_t w = width_, h = height_;
    const int32_t fn = fpsNum_, fd = fpsDen_, tn = tbNum_, td = tbDen_;

    out.write(kIndexMagic, sizeof(kIndexMagic));
    out.write(reinterpret_cast<const char*>(&count), sizeof(count));
    out.write(reinterpret_cast<const char*>(&w), sizeof(w));
    out.write(reinterpret_cast<const char*>(&h), sizeof(h));
    out.write(reinterpret_cast<const char*>(&fn), sizeof(fn));
    out.write(reinterpret_cast<const char*>(&fd), sizeof(fd));
    out.write(reinterpret_cast<const char*>(&tn), sizeof(tn));
    out.write(reinterpret_cast<const char*>(&td), sizeof(td));

    for (const auto& e : index_) {
        const uint8_t keyframe = e.isKeyframe ? 1 : 0;
        out.write(reinterpret_cast<const char*>(&e.pts), sizeof(e.pts));
        out.write(reinterpret_cast<const char*>(&keyframe), sizeof(keyframe));
    }
    return static_cast<bool>(out);
}

bool VideoSource::decodeOneFrame() {
    for (;;) {
        int recvStatus = avcodec_receive_frame(codecCtx_, decodeFrame_);
        if (recvStatus == 0) {
            if (!swsCtx_) {
                swsCtx_ = sws_getContext(
                    width_, height_, static_cast<AVPixelFormat>(decodeFrame_->format),
                    width_, height_, AV_PIX_FMT_YUV420P,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
            }
            if (swsCtx_) {
                sws_scale(swsCtx_, decodeFrame_->data, decodeFrame_->linesize, 0, height_,
                          rgbFrame_->data, rgbFrame_->linesize);
            }
            const int64_t pts = (decodeFrame_->pts != AV_NOPTS_VALUE) ? decodeFrame_->pts
                                                                       : decodeFrame_->pkt_dts;
            currentFrame_ = ptsToFrameNumber(pts);
            return true;
        }
        if (recvStatus != AVERROR(EAGAIN)) {
            return false; // AVERROR_EOF or a real decode error
        }

        // Decoder wants more input.
        int readStatus = av_read_frame(formatCtx_, packet_);
        if (readStatus < 0) {
            avcodec_send_packet(codecCtx_, nullptr); // flush
            continue;
        }
        if (packet_->stream_index == videoStreamIndex_) {
            avcodec_send_packet(codecCtx_, packet_);
        }
        av_packet_unref(packet_);
    }
}

bool VideoSource::stepForward() {
    return decodeOneFrame();
}

bool VideoSource::seekToFrame(int64_t frame) {
    if (frame < 0 || frame >= frameCount()) return false;

    // Find the nearest keyframe at or before `frame` -- this is the entire
    // point of building the index: one av_seek_frame to a known-good
    // position instead of scanning from zero every time.
    int64_t keyframeIdx = frame;
    while (keyframeIdx > 0 && !index_[keyframeIdx].isKeyframe) {
        --keyframeIdx;
    }

    // Already positioned at-or-before the target and no backward jump
    // needed? Just decode forward from where we are.
    const bool needSeek = (currentFrame_ < 0) || (currentFrame_ > frame) ||
                           (keyframeIdx > 0 && currentFrame_ < keyframeIdx);
    if (needSeek) {
        const int64_t targetPts = index_[keyframeIdx].pts;
        if (av_seek_frame(formatCtx_, videoStreamIndex_, targetPts, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(codecCtx_);
        currentFrame_ = -1;
    }

    // Decode forward, discarding frames, until we land exactly on `frame`.
    while (currentFrame_ < frame) {
        if (!decodeOneFrame()) return false;
    }
    return currentFrame_ == frame;
}

} // namespace dirk
