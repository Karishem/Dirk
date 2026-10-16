#include "video/VideoSource.h"
#include "core/Languages.h"

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/pixdesc.h>
#include <libavutil/channel_layout.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}

namespace dirk {

namespace {
// .dirkindex format. Version 2 (2026-09-26, review point 7) adds the movie
// file's own size + last-write time to the header, right after the magic.
// Version 1 only recorded the movie's width/height, so a movie re-encoded
// at the SAME resolution (e.g. with shorter keyframe spacing to make seeks
// cheaper -- review point 1) silently reused the old index: wrong keyframe
// flags and timestamps, i.e. seeks landing on the wrong frame or failing.
// A v1 file is now treated as stale and rebuilt once (with the INDEXING
// screen, see hasCachedIndexFile()), then saved as v2.
constexpr char kIndexMagic[8] = {'D', 'I', 'R', 'K', 'I', 'D', 'X', '2'};

// Size + last-write time of the movie file itself, the "is this still the
// same file the index was built from?" stamp stored in a v2 header. Cheap
// (one stat), no reading of the movie. A copy that preserves the date
// (normal Windows copy) keeps the index valid; anything else just costs
// one rebuild.
bool movieFileStamp(const std::string& videoPath, uint64_t& size, int64_t& mtime) {
    std::error_code ec;
    const auto sz = std::filesystem::file_size(videoPath, ec);
    if (ec) return false;
    const auto wt = std::filesystem::last_write_time(videoPath, ec);
    if (ec) return false;
    size = static_cast<uint64_t>(sz);
    mtime = static_cast<int64_t>(wt.time_since_epoch().count());
    return true;
}

// Reads and checks just the magic + stamp at the start of an index file.
// Leaves `in` positioned right after them on success.
bool readAndCheckIndexHeader(std::ifstream& in, const std::string& videoPath) {
    char magic[8];
    in.read(magic, sizeof(magic));
    if (!in || std::memcmp(magic, kIndexMagic, sizeof(magic)) != 0) return false; // missing, corrupt, or v1
    uint64_t storedSize = 0;
    int64_t storedMtime = 0;
    in.read(reinterpret_cast<char*>(&storedSize), sizeof(storedSize));
    in.read(reinterpret_cast<char*>(&storedMtime), sizeof(storedMtime));
    if (!in) return false;
    uint64_t size = 0;
    int64_t mtime = 0;
    if (!movieFileStamp(videoPath, size, mtime)) return false;
    return size == storedSize && mtime == storedMtime;
}

// Fixed output format every VideoSource resamples its audio to, regardless
// of the source file's native format -- callers (main.cpp's SDL audio
// device) only ever have to deal with one format. 48kHz/stereo/S16 is a
// safe, universally-supported target for SDL_OpenAudioDevice.
constexpr int kAudioOutRate = 48000;
constexpr int kAudioOutChannels = 2;
constexpr AVSampleFormat kAudioOutFormat = AV_SAMPLE_FMT_S16;
constexpr int kAudioOutBytesPerSample = 2; // S16

// See VideoSource::setAudioLeadSeconds() in the header for what this value
// (now a runtime-settable member, audioLeadSeconds_, rather than a fixed
// constant) controls and why it's no longer hardcoded here. Short version
// of the history for whoever reads this next: earlier versions of this file
// had a `kAudioLeadSeconds` constant that went through several tuning
// passes (0.06 -> 0.15 -> 0.45) on the theory that raising it was directly
// compensating for fixed audio *output hardware* latency. That theory was
// wrong in an important way -- SDL's queue plays out in real time
// regardless of how deep it is, so a bigger lead mostly just made the
// queue's own backlog deeper without touching the actual output-hardware
// delay -- and more fundamentally, hardware output latency is a property of
// the PLAYER's sound card and driver, not something any single constant
// baked into the source could be correct for on every machine Dirk runs on.
// See src/audio/AudioLatency.h/.cpp and README's AV-sync section for the
// real fix: main.cpp now measures this machine's actual audio output
// latency at startup (via WASAPI on Windows) and calls
// setAudioLeadSeconds() with a value derived from that real number.
}

int VideoSource::audioSampleRate() const { return kAudioOutRate; }
int VideoSource::decodeThreads() const { return codecCtx_ ? codecCtx_->thread_count : decodeThreads_; }
int VideoSource::audioChannels() const { return kAudioOutChannels; }

VideoSource::~VideoSource() {
    close();
}

std::string VideoSource::indexCachePath(const std::string& videoPath) {
    return videoPath + ".dirkindex";
}

bool VideoSource::open(const std::string& path) {
    return openStream(path) && finishOpening();
}

bool VideoSource::hasCachedIndexFile(const std::string& path) {
    // Not just "does the file exist" any more (review point 7): a stale
    // index (v1 format, or built from a different version of this movie)
    // is about to be rebuilt by finishOpening(), and main.cpp relies on
    // this answer to decide whether to show the INDEXING screen during
    // that build -- saying "cached" here for a stale file would run the
    // full rebuild behind a frozen window instead. Still cheap: reads only
    // the first 24 bytes of the index plus one stat of the movie.
    std::ifstream in(indexCachePath(path), std::ios::binary);
    return in && readAndCheckIndexHeader(in, path);
}

bool VideoSource::openStream(const std::string& path) {
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
    // Multi-core video decoding (review point 1, step 2 -- 2026-09-26),
    // OFF unless the caller asked for it (see setDecodeThreads()). Must be
    // set before avcodec_open2. 1 = ffmpeg's own default single thread,
    // i.e. exactly the behavior before this option existed.
    codecCtx_->thread_count = decodeThreads_ > 1 ? decodeThreads_ : 1;
    if (scanOnly_) codecCtx_->skip_loop_filter = AVDISCARD_ALL; // see setScanOnly()
    if (avcodec_open2(codecCtx_, decoder, nullptr) < 0) {
        std::fprintf(stderr, "[dirk] VideoSource: could not open codec\n");
        close();
        return false;
    }

    width_ = codecCtx_->width;
    height_ = codecCtx_->height;
    {
        // Pixel shape: container's, else the stream's (av_guess picks the
        // right one, like ffplay). Ignored when absent or absurd (some
        // files carry 0:1 or 1:0).
        AVRational sar = av_guess_sample_aspect_ratio(formatCtx_, stream, nullptr);
        sarNum_ = sarDen_ = 1;
        if (sar.num > 0 && sar.den > 0) {
            const double v = static_cast<double>(sar.num) / sar.den;
            if (v > 0.25 && v < 4.0) {
                av_reduce(&sarNum_, &sarDen_, sar.num, sar.den, 1 << 20);
            }
        }
        displayAspect_ = height_ > 0 ? (static_cast<double>(width_) * sarNum_) / (static_cast<double>(height_) * sarDen_)
                                     : 4.0 / 3.0;
    }

    // Colour matrix + range (review point 8) -- see yuvMatrix() in the
    // header for the rules and the measurements behind them.
    {
        const AVCodecParameters* par = stream->codecpar;
        switch (par->color_space) {
            case AVCOL_SPC_BT709:
            case AVCOL_SPC_SMPTE240M:  // near-identical to BT.709
            case AVCOL_SPC_BT2020_NCL: // not drawable by every SDL renderer -- closest safe choice
            case AVCOL_SPC_BT2020_CL:
                yuvMatrix_ = YuvMatrix::BT709;
                yuvMatrixTagged_ = true;
                break;
            case AVCOL_SPC_BT470BG:
            case AVCOL_SPC_SMPTE170M:
            case AVCOL_SPC_FCC:
                yuvMatrix_ = YuvMatrix::BT601;
                yuvMatrixTagged_ = true;
                break;
            default: // unspecified (very common), or something with no YUV meaning here
                yuvMatrix_ = (width_ >= 1280 || height_ > 576) ? YuvMatrix::BT709 : YuvMatrix::BT601;
                yuvMatrixTagged_ = false;
                break;
        }
        // "J" formats are full range by definition, but decodeOneFrame()'s
        // swscale step (target AV_PIX_FMT_YUV420P) already converts those
        // to limited -- measured. Only a plain YUV format tagged full range
        // reaches the texture as full-range values.
        const AVPixelFormat fmt = static_cast<AVPixelFormat>(par->format);
        const bool jFormat = fmt == AV_PIX_FMT_YUVJ420P || fmt == AV_PIX_FMT_YUVJ422P ||
                             fmt == AV_PIX_FMT_YUVJ444P || fmt == AV_PIX_FMT_YUVJ440P ||
                             fmt == AV_PIX_FMT_YUVJ411P;
        yuvFullRange_ = par->color_range == AVCOL_RANGE_JPEG && !jFormat;
    }

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
    if (stream->start_time != AV_NOPTS_VALUE && tbDen_ > 0) {
        videoStartTimeSeconds_ = stream->start_time * (static_cast<double>(tbNum_) / tbDen_);
    }

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

    // Audio is optional -- a missing/unsupported audio stream just leaves
    // hasAudio() false rather than failing the whole open().
    audioTrackStreams_.clear();
    currentAudioTrack_ = -1;
    for (unsigned i = 0; i < formatCtx_->nb_streams; ++i) {
        if (formatCtx_->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) {
            audioTrackStreams_.push_back(static_cast<int>(i));
        }
    }
    // Menu labels, once per open (2026-09-28: real language names).
    audioTrackLabels_.clear();
    for (size_t t = 0; t < audioTrackStreams_.size(); ++t) {
        const AVDictionary* md = formatCtx_->streams[audioTrackStreams_[t]]->metadata;
        const AVDictionaryEntry* title = av_dict_get(md, "title", nullptr, 0);
        const AVDictionaryEntry* lang = av_dict_get(md, "language", nullptr, 0);
        audioTrackLabels_.push_back(dirk::audioStreamLabel(title ? title->value : "", lang ? lang->value : "", t == 0));
    }
    dirk::makeLabelsUnique(audioTrackLabels_);
    int wanted = -1;
    if (preferredAudioTrack_ >= 0 && preferredAudioTrack_ < audioTrackCount()) {
        wanted = audioTrackStreams_[preferredAudioTrack_];
    } else {
        // The file's default track: the one flagged "default" if any (the
        // original soundtrack of a HypseusRemux .mkv, the main language of
        // most multi-language files), else ffmpeg's own choice.
        for (int s : audioTrackStreams_) {
            if (formatCtx_->streams[s]->disposition & AV_DISPOSITION_DEFAULT) { wanted = s; break; }
        }
        if (wanted < 0) wanted = av_find_best_stream(formatCtx_, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    }
    if (wanted >= 0 && !openAudioStream(wanted)) {
        std::fprintf(stderr, "[dirk] VideoSource: warning: could not open the audio of '%s' -- movie will be silent\n", path.c_str());
    }

    openPath_ = path;
    return true;
}

void VideoSource::closeAudioDecoder() {
    if (swrCtx_) { swr_free(&swrCtx_); swrCtx_ = nullptr; }
    if (audioFrame_) { av_frame_free(&audioFrame_); audioFrame_ = nullptr; }
    if (audioCodecCtx_) { avcodec_free_context(&audioCodecCtx_); }
    audioStreamIndex_ = -1;
    currentAudioTrack_ = -1;
}

bool VideoSource::openAudioStream(int streamIndex) {
    closeAudioDecoder();
    if (!formatCtx_ || streamIndex < 0 || streamIndex >= static_cast<int>(formatCtx_->nb_streams)) return false;
    AVStream* audioStream = formatCtx_->streams[streamIndex];
    const AVCodec* audioDecoder = avcodec_find_decoder(audioStream->codecpar->codec_id);
    if (!audioDecoder) return false;

    audioTbNum_ = audioStream->time_base.num;
    audioTbDen_ = audioStream->time_base.den;
    audioStartTimeSeconds_ = 0.0;
    if (audioStream->start_time != AV_NOPTS_VALUE && audioTbDen_ > 0) {
        audioStartTimeSeconds_ = audioStream->start_time * (static_cast<double>(audioTbNum_) / audioTbDen_);
    }
    // Routine/informational only -- gated behind DIRK_VERBOSE (see
    // main.cpp's dirkVerbose()) so a normal launch doesn't print this.
    if (std::getenv("DIRK_VERBOSE")) {
        std::fprintf(stderr,
            "[dirk] VideoSource: stream start_time -- video=%.1fms audio=%.1fms (delta %.1fms)\n",
            videoStartTimeSeconds_ * 1000.0, audioStartTimeSeconds_ * 1000.0,
            (audioStartTimeSeconds_ - videoStartTimeSeconds_) * 1000.0);
    }
    audioCodecCtx_ = avcodec_alloc_context3(audioDecoder);
    if (!audioCodecCtx_ || avcodec_parameters_to_context(audioCodecCtx_, audioStream->codecpar) < 0 ||
        avcodec_open2(audioCodecCtx_, audioDecoder, nullptr) < 0) {
        closeAudioDecoder();
        return false;
    }
    audioFrame_ = av_frame_alloc();

    AVChannelLayout outLayout = AV_CHANNEL_LAYOUT_STEREO;
    AVChannelLayout inLayout = audioCodecCtx_->ch_layout;
    if (inLayout.nb_channels <= 0) {
        // Some containers/codecs leave ch_layout unset; fall back to
        // a plain stereo default rather than reading the deprecated
        // AVCodecContext::channels field.
        av_channel_layout_default(&inLayout, 2);
    }
    int swrErr = swr_alloc_set_opts2(&swrCtx_,
                                      &outLayout, kAudioOutFormat, kAudioOutRate,
                                      &inLayout, audioCodecCtx_->sample_fmt, audioCodecCtx_->sample_rate,
                                      0, nullptr);
    if (swrErr < 0 || !swrCtx_ || !audioFrame_ || swr_init(swrCtx_) < 0) {
        closeAudioDecoder();
        return false;
    }
    audioStreamIndex_ = streamIndex;
    for (int t = 0; t < audioTrackCount(); ++t) {
        if (audioTrackStreams_[t] == streamIndex) currentAudioTrack_ = t;
        // The other soundtracks' packets are dropped by the demuxer itself:
        // no cost for carrying several languages.
        formatCtx_->streams[audioTrackStreams_[t]]->discard =
            audioTrackStreams_[t] == streamIndex ? AVDISCARD_DEFAULT : AVDISCARD_ALL;
    }
    if (std::getenv("DIRK_VERBOSE")) {
        std::fprintf(stderr, "[dirk] VideoSource: audio track %d (%s) found in '%s' (%d Hz, %d ch -> %d Hz stereo)\n",
                     currentAudioTrack_ + 1, audioTrackLabel(currentAudioTrack_).c_str(), openPath_.empty() ? "movie" : openPath_.c_str(),
                     audioCodecCtx_->sample_rate, inLayout.nb_channels, kAudioOutRate);
    }
    return true;
}

std::string VideoSource::audioTrackLabel(int track) const {
    if (track < 0 || track >= static_cast<int>(audioTrackLabels_.size())) return "";
    return audioTrackLabels_[track];
}

bool VideoSource::selectAudioTrack(int track) {
    if (track < 0 || track >= audioTrackCount()) return false;
    if (track == currentAudioTrack_) return true;
    const int previous = audioStreamIndex_;
    if (openAudioStream(audioTrackStreams_[track])) {
        pendingAudio_.clear(); // anything already decoded belongs to the old track
        return true;
    }
    std::fprintf(stderr, "[dirk] VideoSource: warning: could not switch to audio track %d -- keeping the previous one\n", track + 1);
    if (previous >= 0) openAudioStream(previous);
    return false;
}

bool VideoSource::finishOpening() {
    if (!buildOrLoadIndex(openPath_)) {
        std::fprintf(stderr, "[dirk] VideoSource: could not build frame index for '%s'\n", openPath_.c_str());
        close();
        return false;
    }
    currentFrame_ = -1;
    return true;
}

void VideoSource::close() {
    if (swsCtx_) { sws_freeContext(swsCtx_); swsCtx_ = nullptr; }
    if (packet_) { av_packet_free(&packet_); }
    for (AVPacket* p : pendingVideoPackets_) { av_packet_free(&p); }
    pendingVideoPackets_.clear();
    if (decodeFrame_) { av_frame_free(&decodeFrame_); }
    if (rgbFrame_) { av_frame_free(&rgbFrame_); }
    if (codecCtx_) { avcodec_free_context(&codecCtx_); }
    closeAudioDecoder();
    audioTrackStreams_.clear();
    audioTrackLabels_.clear();
    if (formatCtx_) { avformat_close_input(&formatCtx_); }
    videoStreamIndex_ = -1;
    audioStreamIndex_ = -1;
    width_ = height_ = 0;
    sarNum_ = sarDen_ = 1;
    displayAspect_ = 4.0 / 3.0;
    yuvMatrix_ = YuvMatrix::BT601;
    yuvFullRange_ = false;
    yuvMatrixTagged_ = false;
    openPath_.clear();
    index_.clear();
    currentFrame_ = -1;
    pendingAudio_.clear();
    audioGeneration_ = 0;
    suppressAudio_ = false;
    audioDecodedSeconds_ = 0.0;
}

std::vector<uint8_t> VideoSource::takePendingAudio() {
    std::vector<uint8_t> out;
    out.swap(pendingAudio_);
    return out;
}

void VideoSource::trimPendingAudioToSeconds(double maxSeconds) {
    const double bytesPerSecond = static_cast<double>(audioSampleRate()) * audioChannels() * 2.0; // S16
    if (bytesPerSecond <= 0.0 || maxSeconds < 0.0) return;

    const size_t bytesPerFrame = static_cast<size_t>(audioChannels()) * 2; // one sample, all channels
    if (bytesPerFrame == 0) return;

    size_t maxBytes = static_cast<size_t>(maxSeconds * bytesPerSecond);
    maxBytes -= maxBytes % bytesPerFrame; // keep sample-frame alignment

    if (pendingAudio_.size() <= maxBytes) return; // already under the cap, nothing to do

    const size_t droppedBytes = pendingAudio_.size() - maxBytes;
    pendingAudio_.resize(maxBytes); // keep the FRONT (soonest-to-play) samples, drop the tail

    // audioDecodedSeconds_ claimed "decoded up to" the pre-trim end of this
    // buffer -- leaving it there after silently dropping the tail would make
    // the next catchUpAudio() call believe that dropped range is already
    // covered and skip decoding it, producing real silence once playback
    // reaches it. Winding it back by exactly what was dropped means the very
    // next decodeAudioPacket() call (which re-anchors this to the real
    // packet PTS regardless of what it's set to beforehand -- see that
    // function) simply resumes reading forward from here: continuous audio,
    // no gap, just a smaller lead buffer than catchUpAudio() had produced.
    const double droppedSeconds = droppedBytes / bytesPerSecond;
    audioDecodedSeconds_ -= droppedSeconds;
}

void VideoSource::decodeAudioPacket(AVPacket* pkt) {
    if (!audioCodecCtx_ || !swrCtx_ || !audioFrame_) return;

    // Re-anchor audioDecodedSeconds_ to this packet's own real PTS, in the
    // AUDIO stream's own time_base -- NOT the running sample-count total
    // this used to be left to drift from. Why this matters: before this
    // fix, audioDecodedSeconds_ started at a hardcoded 0.0 (see the header)
    // and only ever advanced by dividing decoded sample counts by the
    // output rate -- i.e. it silently assumed the very first audio sample
    // ever decoded corresponds to video time zero. That's often false: AAC
    // encoders commonly emit a few thousand samples of priming/encoder
    // delay before the audio that's meant to align with frame 0 (the
    // stream's own first packet PTS reflects this directly, often small
    // but sometimes not -- an NLE export, like the Premiere-produced movies
    // this project actually ships, can bake in a considerably larger fixed
    // offset between the video and audio tracks' own start points), and
    // nothing before this fix ever looked at that number. The effect: a
    // fixed, unchanging offset between what Dirk thinks "audio time" is and
    // what it actually is, present from frame 0 and never growing (so it
    // doesn't look like a decode-rate/drift bug), invisible to this
    // engine's own avOffsetMs diagnostic (which only ever compares Dirk's
    // own bookkeeping against itself, not against the file's real
    // timestamps), and completely undetectable by anything about hardware
    // output latency -- matching, in hindsight, exactly the profile of the
    // AV-sync bug this project spent so long chasing down the audio-
    // hardware-latency path instead. Re-deriving this from each packet's
    // own real PTS instead of a running count fixes the anchor at open()
    // (and re-fixes it, for free, on the very first audio packet decoded
    // after any seek) and keeps it self-correcting against drift for the
    // rest of playback, rather than trusting one running total for the
    // whole session.
    if (pkt->pts != AV_NOPTS_VALUE && audioTbDen_ > 0) {
        const double audioTimeBase = static_cast<double>(audioTbNum_) / audioTbDen_;
        // audioStartTimeSeconds_: see the header -- puts this on "real
        // seconds since the AUDIO stream's own true start" rather than raw
        // PTS, matching the basis decodeOneFrame() now uses for
        // frameTimeSeconds (video's own start-time-corrected real time).
        audioDecodedSeconds_ = pkt->pts * audioTimeBase - audioStartTimeSeconds_;
    }

    if (avcodec_send_packet(audioCodecCtx_, pkt) < 0) return;

    for (;;) {
        int recvStatus = avcodec_receive_frame(audioCodecCtx_, audioFrame_);
        if (recvStatus != 0) break; // EAGAIN (need more input) or EOF/error -- either way, done for this packet

        int outSamples = swr_get_out_samples(swrCtx_, audioFrame_->nb_samples);
        if (outSamples <= 0) continue;

        std::vector<uint8_t> buf(static_cast<size_t>(outSamples) * kAudioOutChannels * kAudioOutBytesPerSample);
        uint8_t* outPtr = buf.data();
        int converted = swr_convert(swrCtx_, &outPtr, outSamples,
                                     const_cast<const uint8_t**>(audioFrame_->data), audioFrame_->nb_samples);
        if (converted <= 0) continue;

        // Counts even while suppressed -- audioDecodedSeconds_ tracks how
        // far the audio timeline has actually progressed, which is true
        // regardless of whether we're throwing the samples away during a
        // seek's catch-up decode (see seekToFrame()/the header comment).
        const double chunkStartSeconds = audioDecodedSeconds_;
        audioDecodedSeconds_ += static_cast<double>(converted) / kAudioOutRate;
        constexpr size_t kFrameBytes = kAudioOutChannels * kAudioOutBytesPerSample;

        if (suppressAudio_) {
            // mid-seek catch-up: decode to keep the resampler's state
            // consistent, but discard the output -- except the part at/after
            // the landed frame (see seekKeptAudio_ in the header for why
            // that matters).
            if (seekTargetPts_ == INT64_MIN) continue;
            if (audioDecodedSeconds_ <= seekKeepFromSeconds_) continue; // entirely before the landed frame
            size_t skip = 0;
            if (chunkStartSeconds < seekKeepFromSeconds_) {
                skip = static_cast<size_t>((seekKeepFromSeconds_ - chunkStartSeconds) * kAudioOutRate + 0.5);
            }
            if (skip >= static_cast<size_t>(converted)) continue;
            if (seekKeptAudio_.empty()) {
                seekKeptStartSeconds_ = chunkStartSeconds + static_cast<double>(skip) / kAudioOutRate;
            }
            seekKeptAudio_.insert(seekKeptAudio_.end(), buf.begin() + static_cast<std::ptrdiff_t>(skip * kFrameBytes),
                                  buf.begin() + static_cast<std::ptrdiff_t>(static_cast<size_t>(converted) * kFrameBytes));
            continue;
        }

        if (pendingAudio_.empty()) pendingAudioFirstSampleSeconds_ = chunkStartSeconds;
        size_t bytes = static_cast<size_t>(converted) * kFrameBytes;
        pendingAudio_.insert(pendingAudio_.end(), buf.begin(), buf.begin() + bytes);
    }
}

void VideoSource::catchUpAudio(double targetSeconds) {
    // Diagnostic only (DIRK_DEBUG_TICK) -- confirmed this session that
    // packets decode in fixed atomic chunks (~21.3ms per AAC packet on a
    // real capture), so an ordinary, un-bursted catch-up call routinely
    // overshoots its own target by a small amount (~1ms), every single
    // video frame, forever -- it can never come back UNDER target, only
    // land past it. First cut of this diagnostic printed that on every
    // single call, which turned out to be heavy enough to be a real
    // suspect for a genuine underrun seen while capturing it. Now
    // accumulated into a running total instead (see
    // debugCumulativeCatchUpOvershootSeconds_'s declaration) and only
    // printed when it's actually added up to something, or on a single
    // call big enough to matter on its own (a seek's own post-land catch-up,
    // which reads a whole run of interleaved packets at once).
    const bool debugTick = std::getenv("DIRK_DEBUG_TICK") != nullptr;
    const double startAudioDecodedSeconds = audioDecodedSeconds_;
    int audioPacketsRead = 0;
    int videoPacketsRead = 0;

    int lookaheadBudget = 4096; // generous -- a real muxer's worst-case interleave run is a handful of GOPs at most, and each iteration here is a cheap read+maybe-decode
    while (audioStreamIndex_ >= 0 && !suppressAudio_ &&
           audioDecodedSeconds_ < targetSeconds && lookaheadBudget-- > 0) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) break;
        if (av_read_frame(formatCtx_, pkt) < 0) {
            av_packet_free(&pkt);
            break; // EOF -- nothing left to catch up with
        }
        if (pkt->stream_index == audioStreamIndex_) {
            decodeAudioPacket(pkt);
            ++audioPacketsRead;
            av_packet_unref(pkt);
            av_packet_free(&pkt);
        } else {
            // Video (or some other stream) -- queue it rather than submit
            // it to the video decoder here (see the header comment on
            // pendingVideoPackets_ for why) and keep going: unlike a
            // single-packet lookahead, this has to be able to read through
            // an entire run of video packets to reach the audio chunk that
            // covers them.
            pendingVideoPackets_.push_back(pkt);
            ++videoPacketsRead;
        }
    }

    if (debugTick && audioPacketsRead > 0) {
        double decodedSeconds = audioDecodedSeconds_ - startAudioDecodedSeconds;
        double targetGapSeconds = targetSeconds - startAudioDecodedSeconds;
        double overshootSeconds = decodedSeconds - targetGapSeconds;
        debugCumulativeCatchUpOvershootSeconds_ += overshootSeconds;
        // 20ms: big enough that it's almost certainly a seek's own
        // multi-packet post-land catch-up (see seekToFrame()), not an
        // ordinary single-frame call -- worth its own line regardless of
        // the running total. 50ms: the running total has quietly built up
        // to something that would actually matter for AV sync if it kept
        // going -- print AND reset it here so the NEXT print again means
        // "50ms accumulated since last time", not a number that just keeps
        // growing forever in the log too.
        // Both branches below are routine tuning diagnostics, not warnings
        // -- gated behind DIRK_VERBOSE like every other informational print
        // (see main.cpp's dirkVerbose()) so a normal launch stays quiet.
        if (overshootSeconds * 1000.0 > 20.0) {
            if (std::getenv("DIRK_VERBOSE")) {
                std::fprintf(stderr,
                    "[dirk] catchUpAudio: target was %.1fms ahead, read %d audio + %d video packet(s), "
                    "decoded %.1fms (overshoot vs target: %.1fms, this call)\n",
                    targetGapSeconds * 1000.0, audioPacketsRead, videoPacketsRead,
                    decodedSeconds * 1000.0, overshootSeconds * 1000.0);
            }
        } else if (debugCumulativeCatchUpOvershootSeconds_ * 1000.0 > 50.0) {
            if (std::getenv("DIRK_VERBOSE")) {
                std::fprintf(stderr,
                    "[dirk] catchUpAudio: accumulated %.1fms of small per-call overshoot since last "
                    "report (ordinary ~%.1fms-per-call AAC packet granularity, not a single big call)\n",
                    debugCumulativeCatchUpOvershootSeconds_ * 1000.0, overshootSeconds * 1000.0);
            }
            debugCumulativeCatchUpOvershootSeconds_ = 0.0;
        }
    }
}

int64_t VideoSource::ptsToFrameNumber(int64_t pts) const {
    // 2026-09-22 correction -- this used to compute
    // round(pts * time_base * fps), reconstructing a frame number from raw
    // time rather than looking one up. That formula silently assumes the
    // file has a perfectly constant, exactly-known frame duration and that
    // fpsNum_/fpsDen_ (guessed once at open time by av_guess_frame_rate())
    // exactly matches it. Real assets don't reliably satisfy either: any
    // container whose muxer normalizes/coarsens the stream time_base
    // (confirmed directly -- writing a 90kHz-timebase video into a .mkv on
    // this very build, the matroska muxer silently rewrote the stream's
    // time_base to 1/1000 during avformat_write_header(), regardless of
    // what was requested beforehand) or whose source has even one frame
    // held slightly longer than its neighbours (ordinary for
    // telecined/pulldown-sourced NTSC film content, and for anything
    // remuxed from an NLE export, both plausible histories for a real
    // LaserDisc capture) breaks the assumption, and once it's broken this
    // formula doesn't just get one frame number a little bit wrong -- see
    // below, it can be off by tens of thousands.
    //
    // Reproduced directly, not guessed: a synthetic file built for this
    // fix (real mpeg2-style 90kHz timing, a genuine single-frame timing
    // irregularity every 733 frames -- the same *kind* of thing real
    // pulldown/NLE-sourced content does, not a global rate error) was
    // opened and decoded through this exact class, unmodified. The old
    // formula's reconstructed frame number diverged from the real,
    // positional frame number on 19916 of 20000 frames (av_guess_frame_rate()
    // guessed 11.99fps for content that's actually ~23.976fps -- half the
    // real rate, thrown off by the muxer's own time_base rewrite above),
    // and seekToFrame() returned false on every single target adjacent to
    // one of the irregular frames: 81 failures out of 81 tested. That is
    // exactly the mechanism behind the "disc stays paused forever" freeze:
    // discSkipToFrame() (l_discSkipToFrame in api_singe.cpp) only clears
    // discPaused when seekToFrame() returns true, and setupClip() (the
    // KimmyScript helper toolbox.singe defines for scripted "land on this
    // frame and keep playing" moments -- Get Ready, Game Over, and others)
    // never calls discPause() itself; it relies entirely on that auto-
    // resume firing. When seekToFrame() can't land exactly because this
    // formula computed the wrong number, discPaused simply never clears,
    // and main.singe's branch08/branch09 state machine -- which only
    // advances past these scenes on `currentFrame == iFrameEnd` -- waits
    // forever for a frame count that will never arrive.
    //
    // The actual, authoritative definition of "frame number" was already
    // sitting right here the whole time (see the top-of-file comment:
    // "Frame numbers are 0-based decode-order indices") -- it's index_'s
    // own array position, built once by buildIndex() and never recomputed
    // from a guess afterward. So: look the position up instead of
    // reconstructing it. index_ is sorted ascending by pts (buildIndex()
    // sorts it explicitly), so a binary search is enough, and it costs
    // nothing that matters (~15 comparisons per decoded frame either way).
    if (index_.empty()) {
        return -1;
    }
    auto it = std::lower_bound(
        index_.begin(), index_.end(), pts,
        [](const FrameIndexEntry& entry, int64_t value) { return entry.pts < value; });
    if (it == index_.end()) {
        // Shouldn't happen for a pts that genuinely came from this file's
        // own packets, but land on the last real entry rather than one
        // past the end.
        return static_cast<int64_t>(index_.size()) - 1;
    }
    int64_t position = static_cast<int64_t>(it - index_.begin());
    // The decoded frame's pts should equal this entry's pts exactly -- both
    // ultimately come from the same underlying packet metadata. If it
    // doesn't (a genuinely off-by-a-few-ticks pts, e.g. a dts fallback used
    // on one side but not the other), prefer whichever neighbouring entry
    // is actually closer rather than always the one at-or-after pts.
    if (it->pts != pts && it != index_.begin()) {
        const auto prev = it - 1;
        const int64_t afterDelta = it->pts - pts;
        const int64_t beforeDelta = pts - prev->pts;
        if (beforeDelta < afterDelta) {
            position -= 1;
        }
    }
    return position;
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

    // Magic + "same movie file?" stamp (see kIndexMagic). openPath_ is the
    // movie this VideoSource just opened (set by openStream()).
    if (!readAndCheckIndexHeader(in, openPath_)) return false;

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
    // Stamp first: if the movie can't even be stat'ed, don't write an index
    // that could never validate anyway.
    uint64_t fileSize = 0;
    int64_t fileMtime = 0;
    if (!movieFileStamp(openPath_, fileSize, fileMtime)) return false;

    std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
    if (!out) return false;

    const int64_t count = static_cast<int64_t>(index_.size());
    const int32_t w = width_, h = height_;
    const int32_t fn = fpsNum_, fd = fpsDen_, tn = tbNum_, td = tbDen_;

    out.write(kIndexMagic, sizeof(kIndexMagic));
    out.write(reinterpret_cast<const char*>(&fileSize), sizeof(fileSize));
    out.write(reinterpret_cast<const char*>(&fileMtime), sizeof(fileMtime));
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
            scanRaw_ = false;
            if (scanOnly_) {
                const AVPixFmtDescriptor* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(decodeFrame_->format));
                scanRaw_ = desc && (desc->flags & AV_PIX_FMT_FLAG_PLANAR) && !(desc->flags & AV_PIX_FMT_FLAG_RGB) &&
                           desc->nb_components >= 3;
            }
            if (!scanRaw_ && !swsCtx_) {
                swsCtx_ = sws_getContext(
                    width_, height_, static_cast<AVPixelFormat>(decodeFrame_->format),
                    width_, height_, AV_PIX_FMT_YUV420P,
                    SWS_BILINEAR, nullptr, nullptr, nullptr);
            }
            if (!scanRaw_ && swsCtx_) {
                sws_scale(swsCtx_, decodeFrame_->data, decodeFrame_->linesize, 0, height_,
                          rgbFrame_->data, rgbFrame_->linesize);
            }
            const int64_t pts = (decodeFrame_->pts != AV_NOPTS_VALUE) ? decodeFrame_->pts
                                                                       : decodeFrame_->pkt_dts;
            currentFrame_ = ptsToFrameNumber(pts);

            // Audio catch-up: without this, takePendingAudio() (main.cpp,
            // called once per render tick right after stepForward()) only
            // ever has whatever audio happened to already be decoded from
            // PAST calls -- the loop below stopped as soon as it got this
            // one video frame, so any audio the container's own interleaving
            // places even slightly further ahead in the file than this
            // frame's packet doesn't get read until some LATER call.
            // Confirmed against a real exported movie (DLe's lair.mp4):
            // its muxer writes ten consecutive video packets, THEN the
            // ~0.4s audio chunk covering all ten -- so with a naive one-
            // packet lookahead, frames 0-9 would display in dead silence
            // and then the whole batch's audio arrives in one burst right
            // as frame 10 comes up: an audible, visible ~10-frame lag,
            // matching what actually got reported. Not the unrelated main-
            // loop-latency bug Singe has, which lags video instead of
            // audio. Keep reading ahead -- decoding audio immediately, and
            // queuing (not submitting) every video/other packet hit along
            // the way instead of stopping at the first one -- until decoded
            // audio covers at least this frame's own PTS, so the
            // takePendingAudio() call right after this one already has
            // "now"'s audio in hand rather than it trickling in several
            // stepForward() calls later. Skipped entirely during a seek's
            // catch-up decode (suppressAudio_) since pendingAudio_ gets
            // thrown away afterward regardless -- see seekToFrame().
            const double timeBase = tbDen_ > 0 ? static_cast<double>(tbNum_) / tbDen_ : 0.0;
            // Subtract videoStartTimeSeconds_ so this is real seconds since
            // the VIDEO stream's own true start, on the same basis
            // decodeAudioPacket() now puts audioDecodedSeconds_ on (see
            // videoStartTimeSeconds_/audioStartTimeSeconds_ in the header)
            // -- comparing the two without this correction is exactly what
            // let a fixed per-stream startup offset look like an unmovable
            // AV-sync bug no amount of buffering/latency tuning could touch.
            const double frameTimeSeconds = pts * timeBase - videoStartTimeSeconds_;
            catchUpAudio(frameTimeSeconds + audioLeadSeconds_);
            return true;
        }
        if (recvStatus != AVERROR(EAGAIN)) {
            return false; // AVERROR_EOF or a real decode error
        }

        // Decoder wants more input -- a packet queued by a past call's
        // catch-up lookahead first (in the order it was queued), or a fresh
        // one otherwise. Either way this decodes exactly one packet's worth
        // per iteration here, same as before pendingVideoPackets_ existed.
        int readStatus;
        if (!pendingVideoPackets_.empty()) {
            AVPacket* queued = pendingVideoPackets_.front();
            pendingVideoPackets_.pop_front();
            av_packet_unref(packet_);
            av_packet_move_ref(packet_, queued);
            av_packet_free(&queued);
            readStatus = 0;
        } else {
            readStatus = av_read_frame(formatCtx_, packet_);
        }
        if (readStatus < 0) {
            avcodec_send_packet(codecCtx_, nullptr); // flush
            continue;
        }
        if (packet_->stream_index == videoStreamIndex_) {
            // Seek catch-up only (see seekTargetPts_ in the header): a
            // frame before the target that nothing depends on doesn't need
            // decoding at all. Decided per packet, so the target and every
            // frame after it are always decoded normally; INT64_MIN (not
            // seeking) always means AVDISCARD_DEFAULT. A packet without a
            // pts is never skipped.
            const bool skippable = skipNonRefDuringSeek_ && seekTargetPts_ != INT64_MIN &&
                                   packet_->pts != AV_NOPTS_VALUE && packet_->pts < seekTargetPts_;
            codecCtx_->skip_frame = skippable ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
            avcodec_send_packet(codecCtx_, packet_);
        } else if (packet_->stream_index == audioStreamIndex_) {
            decodeAudioPacket(packet_);
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
        if (audioCodecCtx_) avcodec_flush_buffers(audioCodecCtx_);
        currentFrame_ = -1;

        // Anything queued by decodeOneFrame()'s audio catch-up lookahead
        // (see VideoSource.h) was read from the OLD demuxer position -- the
        // seek just invalidated it, so it must not be fed back in as if it
        // were freshly read from the new one.
        for (AVPacket* p : pendingVideoPackets_) { av_packet_free(&p); }
        pendingVideoPackets_.clear();

        // A hard jump (discSkipToFrame) shouldn't play the audio for
        // whatever we're jumping past, and any audio a caller already
        // queued for the *old* position is now stale -- drop it and let
        // audioGeneration() tell the caller (main.cpp's SDL audio device)
        // to clear its own queue too.
        pendingAudio_.clear();
        ++audioGeneration_;

        // Re-anchor the audio-decoded-so-far clock to the keyframe we just
        // jumped to (its PTS is the closest approximation to the new
        // position available before any audio has actually been decoded
        // from there) -- left at its old value, decodeOneFrame()'s catch-up
        // loop would misjudge how far ahead or behind audio decode actually
        // is relative to the new position, either over-reading pointlessly
        // or wrongly skipping catch-up it does need. Only a rough pre-seed,
        // using the VIDEO stream's own PTS/time_base as a stand-in since no
        // audio has been decoded from the new position yet -- decodeAudioPacket()
        // overwrites this with the real value, from the AUDIO stream's own
        // PTS, the moment the first post-seek audio packet is decoded, so
        // any small mismatch between the two streams' timelines here is
        // self-correcting within one packet and never reaches takePendingAudio().
        const double timeBase = tbDen_ > 0 ? static_cast<double>(tbNum_) / tbDen_ : 0.0;
        audioDecodedSeconds_ = targetPts * timeBase - videoStartTimeSeconds_;
    }

    // Decode forward, discarding frames, until we land exactly on `frame`.
    // Audio decoded along the way (between the keyframe and the real
    // target) is intentionally thrown away -- see decodeAudioPacket().
    suppressAudio_ = true;
    seekTargetPts_ = index_[frame].pts; // see decodeOneFrame(): skip non-reference frames before this
    {
        const double tb = tbDen_ > 0 ? static_cast<double>(tbNum_) / tbDen_ : 0.0;
        seekKeepFromSeconds_ = index_[frame].pts * tb - videoStartTimeSeconds_;
        seekKeptAudio_.clear();
    }
    while (currentFrame_ < frame) {
        if (!decodeOneFrame()) { suppressAudio_ = false; seekTargetPts_ = INT64_MIN; return false; }
    }
    suppressAudio_ = false;
    seekTargetPts_ = INT64_MIN;
    pendingAudio_.clear(); // discard anything decoded during the catch-up above

    // The bug this fixes, found from a completely different direction (real
    // Singe users independently reporting the same "audio takes a moment to
    // resync after a scene cut" symptom across many different encodes, not
    // just this project's one file): while suppressAudio_ was true above,
    // every decodeOneFrame() call still ran its own audio catch-up loop and
    // still advanced audioDecodedSeconds_ for each packet decoded -- it just
    // threw the resulting PCM away instead of appending it to pendingAudio_.
    // By the time this loop lands on `frame`, audioDecodedSeconds_ reads as
    // "already decoded up to (this frame's own time + audioLeadSeconds_)",
    // same as normal steady-state playback -- except NONE of that audio
    // actually exists anywhere for takePendingAudio() to hand to the real
    // device; it was discarded on purpose, on every single one of those
    // suppressed calls. The very next ordinary stepForward() call's catch-up
    // loop only decodes MORE audio when audioDecodedSeconds_ is BEHIND its
    // target -- and this leftover value makes it look like it's already
    // comfortably ahead, so that call (and likely several after it) produce
    // little or no real output. Net effect: real, audible silence after
    // every scene cut, for roughly audioLeadSeconds_ worth of playback
    // (confirmed on a live test to be the whole "10 frames" symptom, not
    // just the small clean gap the queue-clear-on-cut fix was meant to
    // leave behind) before real decode catches back up on its own. The fix:
    // reset this to the landed frame's own real, honest timestamp -- not
    // "+audioLeadSeconds_", the bare frame time -- so the next catch-up
    // loop correctly sees itself as needing to produce a full margin of
    // REAL, non-discarded audio from here, instead of believing that work
    // was already done.
    // See seekKeptAudio_ in the header: the
    // audio decoded during the catch-up from the landed frame on becomes
    // the start of the queue, instead of being lost. audioDecodedSeconds_
    // is then already the true end of that audio -- NOT reset to the frame
    // time below, or the catch-up would decode the same span a second time.
    const bool keptAudio = !seekKeptAudio_.empty();
    if (keptAudio) {
        pendingAudio_.swap(seekKeptAudio_);
        pendingAudioFirstSampleSeconds_ = seekKeptStartSeconds_;
    }
    seekKeptAudio_.clear();

    if (frame >= 0 && frame < static_cast<int64_t>(index_.size())) {
        const double timeBase = tbDen_ > 0 ? static_cast<double>(tbNum_) / tbDen_ : 0.0;
        if (!keptAudio) audioDecodedSeconds_ = index_[frame].pts * timeBase - videoStartTimeSeconds_;

        // The fix above stops the NEXT catch-up loop from wrongly thinking
        // this work is already done -- but between here and whenever that
        // next stepForward() call actually happens (main.cpp's own frame-
        // rate pacing, or -- confirmed on a live capture -- a startup
        // holdback still in effect if this seek landed very early) the
        // caller's takePendingAudio() has nothing real to hand the audio
        // device, so it plays real silence for however long that gap is.
        // Confirmed directly: a game's own startup discSkipToFrame(0) (real
        // KimmyScript games commonly do this, to guarantee playback starts
        // exactly at frame 0) measured a 79ms silence gap this way, closely
        // matching a real ~83ms OBS-measured lag at that exact moment. Since
        // every discSkipToFrame() in a game goes through this exact same
        // path, this isn't just a startup issue -- it's the same mechanism
        // this project already suspected as the main source of the residual
        // lag measured right at ordinary in-game scene cuts too. Fix: decode
        // real audio for the landed position right now, same target
        // (landed frame's own time + audioLeadSeconds_) decodeOneFrame()
        // itself would use, so takePendingAudio() already has real content
        // the moment this call returns instead of waiting on the next
        // stepForward().
        const double landedSeconds = index_[frame].pts * timeBase - videoStartTimeSeconds_;
        const double preCatchUpAudioDecodedSeconds = audioDecodedSeconds_;
        const double catchUpTarget = landedSeconds + audioLeadSeconds_;
        catchUpAudio(catchUpTarget);

        // Make the queued audio's first sample
        // line up with the landed frame exactly (see seekKeptAudio_). Audio
        // starting late (its first samples were read before the landing
        // and not kept) gets that much silence in front; audio starting
        // early (a big interleaved chunk that begins before the frame) gets
        // its early part trimmed. Bounded to 1 s either way -- anything
        // bigger means the file's own audio simply isn't there, and
        // inventing that much silence would be worse than leaving it.
        if (!pendingAudio_.empty()) {
            constexpr size_t kFrameBytes = kAudioOutChannels * kAudioOutBytesPerSample;
            const double diff = pendingAudioFirstSampleSeconds_ - landedSeconds;
            const long long samples = static_cast<long long>(diff * kAudioOutRate + (diff >= 0 ? 0.5 : -0.5));
            if (samples > 0 && samples <= kAudioOutRate) {
                pendingAudio_.insert(pendingAudio_.begin(), static_cast<size_t>(samples) * kFrameBytes, 0);
                pendingAudioFirstSampleSeconds_ = landedSeconds;
            } else if (samples < 0 && -samples <= kAudioOutRate) {
                const size_t drop = std::min(static_cast<size_t>(-samples) * kFrameBytes, pendingAudio_.size());
                pendingAudio_.erase(pendingAudio_.begin(), pendingAudio_.begin() + static_cast<std::ptrdiff_t>(drop));
                pendingAudioFirstSampleSeconds_ = landedSeconds;
            }
        }
        // Diagnostic only: a real capture at an in-game scene cut (not the
        // engine's own startup discSkipToFrame(0)) showed this one call
        // producing ~341ms of audio -- more than 5x the ~60ms this is
        // normally supposed to top up. That much overshoot isn't explained
        // by ordinary AAC packet granularity (~21-23ms each); it needs a
        // real, specific number for what actually happened, to tell apart
        // "the file's own audio/video tracks are locally misaligned at this
        // exact splice point by roughly that much" from some other cause.
        // preCatchUpAudioDecodedSeconds is the freshly re-seeded value
        // (from the VIDEO stream's own PTS -- see above) BEFORE the first
        // real audio packet re-anchors it to the AUDIO stream's own PTS;
        // a big, one-time jump between that seed and what the first
        // decoded packet actually reports would show up directly here.
        if (std::getenv("DIRK_DEBUG_TICK")) {
            std::fprintf(stderr,
                "[dirk] seekToFrame(%lld) post-land catch-up: seed=%.1fms target=%.1fms -> "
                "landed audioDecodedSeconds()=%.1fms (decoded %.1fms of real content)\n",
                static_cast<long long>(frame), preCatchUpAudioDecodedSeconds * 1000.0,
                catchUpTarget * 1000.0, audioDecodedSeconds_ * 1000.0,
                (audioDecodedSeconds_ - preCatchUpAudioDecodedSeconds) * 1000.0);
        }
    }

    return currentFrame_ == frame;
}

int VideoSource::displayWidth() const {
    if (sarNum_ == sarDen_ || height_ <= 0) return width_;
    return static_cast<int>(std::lround(height_ * displayAspect_));
}

std::string VideoSource::pixelShape() const {
    return std::to_string(sarNum_) + ":" + std::to_string(sarDen_);
}

} // namespace dirk
