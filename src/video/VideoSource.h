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
#include <deque>
#include <string>
#include <vector>

extern "C" {
struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwsContext;
struct SwrContext;
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

    // Split form of open(), for a caller (main.cpp) that wants to show
    // something on screen while a slow first-time index build runs, but
    // not for every launch (loading the cached index back is already fast
    // -- see indexCachePath() in the .cpp). The sequence is:
    //   openStream(path)         -- format/codec/dimensions/audio; always
    //                                fast, never does the frame-by-frame
    //                                scan below.
    //   hasCachedIndexFile(path) -- cheap check (the index file's first 24
    //                                bytes + one stat of the movie) that a
    //                                USABLE index exists -- right format,
    //                                built from this exact movie file (same
    //                                size + date, see kIndexMagic in the
    //                                .cpp) -- so the caller can decide
    //                                whether the next call is likely to be
    //                                slow BEFORE paying for it, e.g. to
    //                                skip drawing a loading screen when
    //                                it's about to return almost instantly
    //                                anyway.
    //   finishOpening()          -- loads the cached index if one exists
    //                                and matches, otherwise does the full
    //                                packet scan (the potentially-slow
    //                                part) and writes a fresh cache.
    // open(path) above is just these three calls in sequence, unchanged in
    // behavior -- existing callers (VideoResource's secondary video
    // streams, tools/seek_test.cpp) that don't care about a loading screen
    // keep using it exactly as before.
    bool openStream(const std::string& path);
    bool finishOpening();
    static bool hasCachedIndexFile(const std::string& path);

    bool isOpen() const { return formatCtx_ != nullptr; }
    // The file openStream() opened (empty when closed).
    const std::string& path() const { return openPath_; }

    // Total number of frames the index knows about.
    int64_t frameCount() const { return static_cast<int64_t>(index_.size()); }

    int width() const { return width_; }
    int height() const { return height_; }

    // Shape the picture is meant to be shown at (width / height), from the
    // file's pixel shape (sample aspect ratio): e.g. a 720x480 MPEG-2 disc
    // rip with 8:9 pixels is 4:3 (1.333), not 3:2. Most movies have square
    // pixels, so this is width() / height(). Same rule as VLC and mpv.
    double displayAspect() const { return displayAspect_; }
    // width a picture of height() pixels takes on screen at that shape
    // (720x480 with 8:9 pixels -> 640; square pixels -> width()).
    int displayWidth() const;
    // The pixel shape found in the file, "1:1" when square or unknown.
    std::string pixelShape() const;

    // How this movie's YUV pixels must be turned back into RGB (review
    // point 8, 2026-09-26). Read per file in openStream(), so every movie
    // is shown with its own encoding, whoever made it:
    //   - matrix: the file's own colour-matrix tag when it has one
    //     (BT.709 / BT.601); when it has none, the usual player convention
    //     (mpv/VLC): width >= 1280 or height > 576 -> BT.709 (HD), else
    //     BT.601 (SD). BT.2020 tags map to BT.709 on purpose: SDL's
    //     renderers don't all draw BT.2020 (the software one renders it
    //     solid black -- measured), and BT.2020 is in practice HDR content
    //     this engine doesn't target anyway.
    //   - range: whether the pixel values handed to the texture are FULL
    //     range (0-255) or the usual LIMITED (16-235). Measured, not
    //     assumed: decoders that signal full range with a "J" pixel format
    //     (H.264: yuvj420p) get converted to limited by this class's own
    //     swscale step, but decoders that tag a plain yuv420p frame as
    //     full range (VP9, ...) pass the full-range values straight
    //     through -- shown as limited, those would clip the whites and wash
    //     out the blacks.
    // Before this, every texture used SDL3's default (BT.601 limited):
    // measured on a BT.709 file, pure red displayed as RGB(233,0,2) and
    // pure green as (21,255,10) instead of (255,0,0)/(0,255,0).
    enum class YuvMatrix { BT601, BT709 };
    YuvMatrix yuvMatrix() const { return yuvMatrix_; }
    bool yuvFullRange() const { return yuvFullRange_; }
    bool yuvMatrixTagged() const { return yuvMatrixTagged_; } // false = guessed from the resolution

    // The stream's average frame rate, e.g. for a secondary video (menu.singe's
    // attract-preview clips) that needs to pace its own playback against real
    // time rather than the main disc's discFps/discPlay loop.
    double fps() const { return fpsDen_ > 0 ? static_cast<double>(fpsNum_) / fpsDen_ : 0.0; }

    // discSkipToFrame(frame): seeks to the nearest preceding keyframe, then
    // decodes forward until `frame` is reached. Leaves the decoded picture
    // in currentFrame(). Returns false if `frame` is out of range or a
    // decode error occurs.
    bool seekToFrame(int64_t frame);

    // On by default. Only exists so tests/benchmarks can compare both
    // modes on the same file (tools/seek_skip_test.cpp) -- see
    // seekTargetPts_ in the private section for what it does.
    void setSkipNonRefDuringSeek(bool on) { skipNonRefDuringSeek_ = on; }

    // Video decoder threads (review point 1, step 2 -- 2026-09-26). Takes
    // effect at the next openStream()/open(). 1 (this class's default) =
    // single-threaded; dirk itself defaults to "auto" (see
    // LaunchSettings::decodeThreads). More threads decode faster
    // (measured on 1440x1080 H.264, 2 threads: seeks 473 -> 310 ms on top
    // of the non-reference skip, plain playback decode 19 -> 10 ms/frame)
    // but make the decoder hold a few frames in flight, which reads the
    // movie -- and its interleaved audio -- further ahead. After a seek
    // that used to leave the queued audio starting 40-120 ms late relative
    // to the picture; seekToFrame() now keeps and aligns that audio, for
    // every thread count (see seekKeptAudio_).
    void setDecodeThreads(int threads) { decodeThreads_ = threads < 1 ? 1 : threads; }
    // Threads the video decoder actually got (after opening).
    int decodeThreads() const;

    // discGetFrame(): the frame number currently decoded/displayed.
    int64_t currentFrameNumber() const { return currentFrame_; }

    // Position of `frame` in the movie file, in seconds from its first
    // frame (the stream's start time subtracted) -- what a subtitle
    // editor shows for that picture. -1 if `frame` is out of range.
    // Used by the .srt subtitles (core/Subtitles.h).
    double frameTimeSeconds(int64_t frame) const {
        if (frame < 0 || frame >= static_cast<int64_t>(index_.size()) || tbDen_ <= 0) return -1.0;
        return index_[static_cast<size_t>(frame)].pts * (static_cast<double>(tbNum_) / tbDen_) - videoStartTimeSeconds_;
    }

    // Decodes exactly one frame forward from the current position (used by
    // discPlay's per-tick advance). Returns false at end of stream.
    bool stepForward();

    // Raw decoded picture (YUV420P from swscale-normalized output), or
    // nullptr if nothing has been decoded yet. Owned by VideoSource.
    const AVFrame* currentPicture() const { return rgbFrame_; }

    // Wizard's black/silence scan (core/BlackScan.h, 2026-10-15): set
    // before open(). The decoder skips its loop filter and the picture is
    // not converted for display -- only its luma is looked at -- which
    // makes reading a long passage noticeably faster. scanPicture() is the
    // decoder's own picture when it is planar YUV (luma in data[0]),
    // else the converted one.
    void setScanOnly(bool on) { scanOnly_ = on; }
    const AVFrame* scanPicture() const { return scanRaw_ ? decodeFrame_ : rgbFrame_; }

    // -- Movie audio (the embedded audio track, e.g. dialogue/music on
    // lair.mp4) -----------------------------------------------------------
    // Decoded opportunistically as a side effect of stepForward()/
    // seekToFrame() pulling packets off the shared demuxer -- there is no
    // separate "decode audio now" call. Output is always resampled to a
    // fixed S16 stereo format (see kAudioOutRate/kAudioOutChannels in the
    // .cpp) so callers don't need to care what the source file's native
    // audio format is.
    bool hasAudio() const { return audioStreamIndex_ >= 0; }

    // -- Audio tracks (2026-09-27): a movie can carry several soundtracks
    // (languages) -- an .mp4/.mkv with more than one audio stream, or a
    // Hypseus game's <name>-fr.ogg alternates merged in by HypseusRemux.
    // Tracks are numbered 0..audioTrackCount()-1 in file order.
    int audioTrackCount() const { return static_cast<int>(audioTrackStreams_.size()); }
    // Menu label: the language's English name ("French"), see
    // dirk::audioStreamLabel() -- "Original" for a first track with no
    // language, "Unknown" for a code Dirk doesn't know; made unique
    // ("French (2)").
    std::string audioTrackLabel(int track) const;
    // Track currently decoded, -1 if none.
    int currentAudioTrack() const { return currentAudioTrack_; }
    // Track to open with at the next openStream()/open(); -1 (default) =
    // the file's own default (av_find_best_stream, the behavior before).
    void setPreferredAudioTrack(int track) { preferredAudioTrack_ = track; }
    // Switches the decoded soundtrack. Only reopens the audio decoder: the
    // caller then re-seeks to the frame on screen (seekToFrame()), which
    // realigns the new track exactly like any jump. False (nothing
    // changed) if `track` is out of range or its codec can't be opened --
    // then the previous track is reopened.
    bool selectAudioTrack(int track);
    int audioSampleRate() const;
    int audioChannels() const;

    // Returns and clears whatever PCM has been decoded since the last call.
    std::vector<uint8_t> takePendingAudio();

    // Diagnostic-only peek (does NOT consume, unlike takePendingAudio()) --
    // lets a caller check how much audio is sitting buffered right now
    // without disturbing it. See main.cpp's startup-priming diagnostic.
    size_t pendingAudioBytes() const { return pendingAudio_.size(); }
    // Diagnostic/test only: content time (seconds, same timeline as the
    // video frames) of the first sample still in the pending audio queue.
    // Meaningful only while pendingAudioBytes() > 0. Used by
    // tools/seek_audio_align_test.cpp to check audio lines up with the
    // landed frame after a seek.
    double pendingAudioStartSeconds() const { return pendingAudioFirstSampleSeconds_; }
    double audioDecodedSeconds() const { return audioDecodedSeconds_; }

    // Caps how much undelivered audio is sitting in pendingAudio_, dropping
    // any excess off the END (the furthest-ahead-in-time samples) while
    // keeping the front (the soonest-to-play samples) completely intact --
    // this never creates an audible gap or skip, it just gives back
    // over-buffered lead. Exists specifically for main.cpp to call right
    // after a generation change/seek: a real capture this session showed
    // pendingAudioBytes() landing around 300-370ms right after a cut (SDL
    // queue backlog measured the same way via F7, independently, by the
    // user), roughly 3x audioLeadSeconds_'s intended ~120ms margin, and
    // that inflated backlog is exactly what makes the FIRST audio the
    // player hears after a cut already several frames stale -- the queue
    // has to drain all the way down before anything decoded AT the cut
    // becomes audible, which is a fixed phase shift for the rest of the
    // scene, not a one-tick glitch. This is a blunt, unconditional cap
    // rather than a fix for whichever exact call overshoot the target --
    // it guarantees the result regardless of which path produced the
    // excess. No-op if pendingAudioBytes() is already at or under the cap.
    void trimPendingAudioToSeconds(double maxSeconds);

    // Bumped every time seekToFrame() performs a jump (see .cpp). A caller
    // that's feeding takePendingAudio() into a real audio device should
    // compare this against the value it last saw and, if it changed, drop
    // whatever it already queued there -- otherwise a discSkipToFrame jump
    // plays a burst of now-stale audio before the new position's audio
    // catches up.
    int audioGeneration() const { return audioGeneration_; }

    // How far ahead of the current video frame's own timestamp
    // decodeOneFrame()'s audio catch-up loop tries to keep audio decoded
    // (see decodeOneFrame() in the .cpp for the full mechanism, and
    // AudioLatency.h for why this is set from a measured value at runtime
    // rather than a fixed constant: audio output hardware latency is a
    // property of the PLAYER's sound card, not something one number can be
    // right for on every machine). Safe to call before or after open();
    // takes effect on the next decodeOneFrame() call either way. The
    // default here is only a reasonable starting margin against the queue
    // running dry (see the catch-up loop's own comments) for a caller that
    // never calls this at all -- not a real answer to the hardware-latency
    // question, which is main.cpp's job to set from an actual measurement.
    void setAudioLeadSeconds(double seconds) { audioLeadSeconds_ = seconds; }
    double audioLeadSeconds() const { return audioLeadSeconds_; }

private:
    bool buildOrLoadIndex(const std::string& path);
    bool buildIndex();
    bool loadIndexCache(const std::string& cachePath);
    bool saveIndexCache(const std::string& cachePath) const;
    static std::string indexCachePath(const std::string& videoPath);

    // Converts a stream PTS to a 0-based frame number -- via a binary
    // search of index_ (see the 2026-09-22 comment on the .cpp definition
    // for why this is no longer done via a guessed constant frame rate).
    // The result is always consistent, by construction, with frameCount(),
    // with seekToFrame()'s own `frame` parameter, and with this file's own
    // top-of-file documentation: frame N is the Nth entry of index_ in
    // decode order, a purely positional fact -- never a time computation.
    int64_t ptsToFrameNumber(int64_t pts) const;

    bool decodeOneFrame(); // pulls + decodes the next packet belonging to the video or audio stream
    void decodeAudioPacket(AVPacket* pkt); // helper for decodeOneFrame's audio branch

    // Shared by decodeOneFrame()'s own catch-up lookahead and seekToFrame()
    // (see seekToFrame()'s comment on why it needs this too): reads and
    // decodes audio packets -- setting aside any video/other packets read
    // along the way into pendingVideoPackets_ rather than decoding them --
    // until audioDecodedSeconds_ reaches targetSeconds or the file runs out.
    // Never touches currentFrame_/the decoded picture; this only ever
    // produces MORE queued audio.
    void catchUpAudio(double targetSeconds);

    AVFormatContext* formatCtx_ = nullptr;
    AVCodecContext* codecCtx_ = nullptr;
    AVFrame* decodeFrame_ = nullptr; // raw decoder output (native pixel format)
    AVFrame* rgbFrame_ = nullptr;    // swscale-converted output for rendering
    AVPacket* packet_ = nullptr;
    SwsContext* swsCtx_ = nullptr;

    int videoStreamIndex_ = -1;
    int width_ = 0;
    int height_ = 0;
    int sarNum_ = 1;
    int sarDen_ = 1;
    double displayAspect_ = 4.0 / 3.0;
    YuvMatrix yuvMatrix_ = YuvMatrix::BT601; // see yuvMatrix()
    bool yuvFullRange_ = false;
    bool yuvMatrixTagged_ = false;

    // Set by openStream(), read by finishOpening()/hasCachedIndexFile() --
    // see the split-open() comment above the public declarations.
    std::string openPath_;

    // Audio side -- entirely optional; a video with no audio stream (or an
    // unsupported one) just leaves audioStreamIndex_ at -1 and hasAudio()
    // false, everything else becomes a no-op.
    int audioStreamIndex_ = -1;
    std::vector<int> audioTrackStreams_; // stream index of each audio track, file order
    std::vector<std::string> audioTrackLabels_; // menu label of each, same order
    int currentAudioTrack_ = -1;
    int preferredAudioTrack_ = -1;
    bool openAudioStream(int streamIndex); // decoder + resampler for that stream; see selectAudioTrack()
    void closeAudioDecoder();
    AVCodecContext* audioCodecCtx_ = nullptr;
    AVFrame* audioFrame_ = nullptr;
    SwrContext* swrCtx_ = nullptr;
    std::vector<uint8_t> pendingAudio_;
    int audioGeneration_ = 0;
    // True while seekToFrame() is decoding-and-discarding frames between the
    // nearest keyframe and the actual target: we don't want to hear the
    // audio for frames we're jumping past, only for frames we actually land
    // on and play forward from.
    bool suppressAudio_ = false;

    // Seek speed-up (review point 1, step 1 -- 2026-09-26). While
    // seekToFrame() decodes forward from the keyframe to its target, every
    // frame BEFORE the target is decoded only to be thrown away. Frames
    // that no other frame depends on ("non-reference" -- most B-frames in
    // a normal H.264 encode) don't even need decoding: decodeOneFrame()
    // tells the decoder to skip them (AVDISCARD_NONREF) for any video
    // packet whose pts is before seekTargetPts_. Reference frames are
    // always decoded (later frames need them), the target itself and
    // everything after it too, and outside a seek seekTargetPts_ is
    // INT64_MIN so normal playback is never affected. Audio isn't touched
    // at all: it's suppressed and discarded during this catch-up anyway.
    int64_t seekTargetPts_ = INT64_MIN;
    bool skipNonRefDuringSeek_ = true;
    bool scanOnly_ = false; // see setScanOnly()
    bool scanRaw_ = false;  // the last picture was left unconverted (scanOnly_ and planar YUV)
    int decodeThreads_ = 1; // see setDecodeThreads()

    // Audio alignment after a seek (2026-09-26, review point 1 step 2 --
    // extended to the single-threaded decoder the same day, see the end of
    // this comment). Measured problem: a multi-
    // threaded decoder holds frames in flight, so by the time seekToFrame()
    // lands on its target, the demuxer has already read -- and, audio being
    // suppressed during the catch-up, thrown away -- the audio that belongs
    // to the landed frame and the few after it. The audio queued after the
    // cut then starts that much LATER in the content than the picture:
    // measured ~40 ms ahead of the picture with 2 threads, ~120 ms with 4,
    // for the whole scene (every cut). Fix: while seeking, keep (in
    // seekKeptAudio_) the decoded audio from the landed frame's time on
    // instead of discarding it, then line the queued audio's first sample
    // up with the landed frame exactly (a little silence if audio starts
    // late, a little trimmed if it starts early).
    // Single-threaded decoder: first left untouched (tuned on the cabinet),
    // but measured to have the same kind of error on movies whose audio is
    // stored in blocks (Premiere exports): after a seek the queued audio
    // started 160 ms early (audio stored after its 10 frames) or 224 ms
    // late (audio stored before them), 13 ms on a finely interleaved file.
    // Cyril then found sync at cuts "perfect" on the cabinet with the
    // aligned (multi-threaded) path, so it now applies to every thread
    // count (seek_audio_align_test checks 1, 2 and 4 threads, both block
    // layouts).
    double seekKeepFromSeconds_ = 0.0;        // landed frame's time, while a seek is decoding
    std::vector<uint8_t> seekKeptAudio_;      // decoded audio at/after seekKeepFromSeconds_
    double seekKeptStartSeconds_ = 0.0;       // content time of seekKeptAudio_'s first sample
    double pendingAudioFirstSampleSeconds_ = 0.0; // content time of pendingAudio_[0] (valid when non-empty)

    // See setAudioLeadSeconds() above. 0.15s until a caller sets a real,
    // measured value -- comfortably enough margin against queue starvation
    // on its own (see decodeOneFrame()'s catch-up loop), just not a real
    // answer to hardware output latency.
    double audioLeadSeconds_ = 0.15;

    // Diagnostic only (DIRK_DEBUG_TICK) -- running total of how much every
    // catchUpAudio() call has overshot its own target by, in seconds.
    // Packets decode in fixed atomic chunks (a real capture this session
    // showed ~21.3ms per AAC packet), so a call almost always ends up
    // slightly past whatever target it was aiming for, never short -- this
    // tracks that drift over time so catchUpAudio() can log only when it
    // actually adds up to something (a coarse-grained running total),
    // instead of printing on literally every video frame, which was heavy
    // enough on its own to be a real suspect for a genuine underrun seen
    // while capturing it.
    double debugCumulativeCatchUpOvershootSeconds_ = 0.0;

    // Running total of audio TIME decoded so far (in seconds, same absolute
    // timeline as a frame's PTS*time_base) -- lets decodeOneFrame() tell
    // whether audio decode has actually caught up to the frame it just
    // returned, and lets seekToFrame() re-anchor it after a jump. See
    // decodeOneFrame()'s catch-up loop for why this exists: without it,
    // audio trails the picture by however coarse the file's own MP4
    // interleaving is (a real, confirmed bug -- several frames' worth of
    // lag on a real exported movie, not the unrelated main-loop-latency bug
    // Singe has in the opposite direction).
    double audioDecodedSeconds_ = 0.0;
    // Video (or other non-audio) packets the catch-up loop read past while
    // looking for audio, queued here instead of submitted to the video
    // decoder (submitting them there without immediately receive()'ing them
    // back would risk the decoder returning EAGAIN on a full internal
    // reorder buffer, and calling receive() again there would risk silently
    // skipping a frame). Drained one at a time, in order, by ordinary
    // decodeOneFrame() calls -- each still decodes exactly one frame, same
    // as before this existed, just sourcing its packet from here first
    // instead of always reading a fresh one. A real exported movie (see
    // decodeOneFrame()'s catch-up loop) can interleave a whole run of video
    // packets -- ten, in one confirmed case -- before the audio chunk that
    // covers all of them, so a single-packet lookahead isn't enough; this
    // has to be able to read through the whole run to reach it.
    std::deque<AVPacket*> pendingVideoPackets_;

    std::vector<FrameIndexEntry> index_;
    int64_t currentFrame_ = -1;

    // Reduced fraction avg_frame_rate = fpsNum_/fpsDen_, and the stream's
    // time_base, cached at open() time for pts<->frame conversion.
    int fpsNum_ = 0;
    int fpsDen_ = 1;
    int tbNum_ = 0;
    int tbDen_ = 1;

    // The AUDIO stream's own time_base -- deliberately separate from
    // tbNum_/tbDen_ above (the VIDEO stream's), and NOT assumed to be the
    // same fraction. A container is free to give each stream its own
    // time_base, and real-world audio tracks very often have their own
    // fixed startup offset relative to the video track's own t=0 (AAC
    // encoder priming/delay is the common case, typically ~20-45ms, but an
    // NLE export -- Premiere, in this project's case -- can bake in a much
    // larger one). decodeAudioPacket() uses this to re-anchor
    // audioDecodedSeconds_ to each packet's own real PTS rather than just
    // counting decoded samples from an assumed zero -- see that function
    // for the full story of the bug this fixes.
    int audioTbNum_ = 0;
    int audioTbDen_ = 1;

    // Each stream's own AVStream::start_time (its first packet's PTS),
    // converted to seconds in ITS OWN time_base, cached at open() time.
    // Confirmed necessary by a direct, controlled test (same PC, same
    // speakers, same file: perfectly in sync in VLC, a rock-solid ~400ms/10
    // frames of audio lag in Dirk, completely unmoved by every runtime
    // fix tried -- hardware latency compensation, decode-ahead margin,
    // queue depth, none of it touched this number even slightly). Raw PTS
    // values from two different streams in the same container are NOT
    // guaranteed to share a common zero point -- each stream can (and,
    // per that test, apparently does here) have its own fixed startup
    // offset baked in by the encoder/muxer (AAC priming delay, an edit
    // list, whatever an NLE export like this project's Premiere-produced
    // movies adds). decodeOneFrame()'s catch-up-loop target and
    // decodeAudioPacket()'s own PTS-anchor (see audioTbNum_/audioTbDen_
    // above) both subtract the relevant one of these before comparing
    // video "real time" against audio "real time", so a fixed per-stream
    // offset like that can no longer show up as a fixed, unmovable AV-sync
    // error the way it evidently was. Deliberately NOT used by
    // ptsToFrameNumber() or the frame index -- KimmyScript's own frame
    // numbering (discSkipToFrame/discGetFrame) stays exactly as validated
    // before; this only corrects the audio-vs-video REAL TIME comparison,
    // not what a given frame NUMBER means.
    double videoStartTimeSeconds_ = 0.0;
    double audioStartTimeSeconds_ = 0.0;
};

} // namespace dirk
