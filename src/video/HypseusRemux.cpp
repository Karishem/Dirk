#include "video/HypseusRemux.h"
#include "core/Languages.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
}

#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <regex>
#include <vector>
#include <system_error>

namespace dirk {

namespace {

// Trim leading/trailing whitespace (spaces, tabs, CR -- framefiles seen so
// far are plain ASCII, but a CR from a Windows-edited file shouldn't break
// parsing).
std::string trim(const std::string& s) {
    size_t start = s.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return {};
    size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(start, end - start + 1);
}

} // namespace

namespace {

std::string lowerCopy(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// <videoStem><'-'|'_'|'.'><tag>.ogg next to the video (case-insensitive),
// tag = 1-10 letters/digits, sorted by tag. See HypseusAltAudio.
std::vector<HypseusAltAudio> findAltAudio(const std::filesystem::path& videoPath) {
    namespace fs = std::filesystem;
    std::vector<HypseusAltAudio> out;
    const std::string stem = lowerCopy(videoPath.stem().string());
    std::error_code ec;
    for (fs::directory_iterator it(videoPath.parent_path(), ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        if (lowerCopy(it->path().extension().string()) != ".ogg") continue;
        const std::string fileStem = it->path().stem().string();
        const std::string lower = lowerCopy(fileStem);
        if (lower.size() < stem.size() + 2 || lower.compare(0, stem.size(), stem) != 0) continue;
        const char sep = lower[stem.size()];
        if (sep != '-' && sep != '_' && sep != '.') continue;
        const std::string tag = fileStem.substr(stem.size() + 1);
        if (tag.empty() || tag.size() > 10) continue;
        if (!std::all_of(tag.begin(), tag.end(), [](unsigned char c) { return std::isalnum(c) != 0; })) continue;
        out.push_back({tag, it->path().string()});
    }
    std::sort(out.begin(), out.end(), [](const HypseusAltAudio& a, const HypseusAltAudio& b) {
        return lowerCopy(a.tag) < lowerCopy(b.tag);
    });
    return out;
}

// Tries to parse `frameFile` as a Hypseus framefile and, if it is one, fill
// in `outSegment`/`outBaseName`. Returns false (leaving both untouched) for
// anything that isn't -- an unrelated .txt sitting in the same folder,
// which turns out to happen in practice (see the 2026-09-22 comment on
// findHypseusFrameFile() below), should look exactly like "not a framefile"
// here, not like a hard error.
bool tryParseFrameFile(const std::filesystem::path& frameFile, HypseusSegment& outSegment, std::string& outBaseName) {
    namespace fs = std::filesystem;
    std::error_code ec;

    std::ifstream in(frameFile);
    if (!in) return false;

    std::vector<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        std::string trimmed = trim(line);
        if (!trimmed.empty()) lines.push_back(trimmed);
    }
    if (lines.empty()) return false;

    // First non-blank line: the subfolder holding the video/audio files
    // (e.g. "Video/") -- confirmed convention, see HypseusRemux.h.
    std::string subfolder = lines.front();

    // Every following line should be "<frame> <filename>". Several lines =
    // several pieces stitched at their frames (2026-10-14, see the header).
    static const std::regex kSegmentLine(R"(^(\d+)\s+(.+)$)");
    struct Line { long long frame; std::string file; };
    std::vector<Line> entries;
    for (size_t i = 1; i < lines.size(); ++i) {
        std::smatch m;
        if (!std::regex_match(lines[i], m, kSegmentLine)) return false; // not the format we expect -- bail out quietly
        entries.push_back({std::stoll(m[1].str()), trim(m[2].str())});
    }
    if (entries.empty()) return false;
    std::stable_sort(entries.begin(), entries.end(), [](const Line& x, const Line& y) { return x.frame < y.frame; });

    fs::path videoDir = frameFile.parent_path() / subfolder;
    std::vector<HypseusPiece> pieces;
    for (const auto& e : entries) {
        fs::path videoPath = videoDir / e.file;
        // Audio sibling: same base name, ".ogg" extension -- confirmed
        // mandatory Hypseus convention, not guessed (see HypseusRemux.h).
        fs::path audioPath = videoPath;
        audioPath.replace_extension(".ogg");
        if (!fs::is_regular_file(videoPath, ec) || !fs::is_regular_file(audioPath, ec)) {
            // A real framefile with a file missing says so (a stray .txt
            // never gets this far with several lines of "<n> <name>").
            if (entries.size() > 1) {
                std::fprintf(stderr, "[dirk] %s: %s is missing\n", frameFile.filename().string().c_str(),
                             (fs::is_regular_file(videoPath, ec) ? audioPath : videoPath).string().c_str());
            }
            return false;
        }
        pieces.push_back({e.frame, videoPath.string(), audioPath.string(), findAltAudio(videoPath)});
    }
    if (pieces.size() > 1) {
        // An alternate soundtrack only if every piece has it: otherwise the
        // language would change in the middle of the movie.
        std::vector<HypseusAltAudio> common = pieces.front().altAudio;
        for (const auto& piece : pieces) {
            common.erase(std::remove_if(common.begin(), common.end(), [&](const HypseusAltAudio& a) {
                return std::none_of(piece.altAudio.begin(), piece.altAudio.end(),
                                    [&](const HypseusAltAudio& b) { return lowerCopy(a.tag) == lowerCopy(b.tag); });
            }), common.end());
        }
        for (auto& piece : pieces) {
            std::vector<HypseusAltAudio> kept;
            for (const auto& c : common) {
                for (const auto& alt : piece.altAudio) if (lowerCopy(alt.tag) == lowerCopy(c.tag)) kept.push_back(alt);
            }
            for (const auto& alt : piece.altAudio) {
                if (std::none_of(kept.begin(), kept.end(), [&](const HypseusAltAudio& k) { return k.path == alt.path; })) {
                    outSegment.droppedAudio.push_back(fs::path(alt.path).filename().string());
                }
            }
            piece.altAudio = kept;
        }
    }

    outSegment.videoPath = pieces.front().videoPath;
    outSegment.audioPath = pieces.front().audioPath;
    outSegment.altAudio = pieces.front().altAudio;
    outSegment.pieces = pieces.size() > 1 ? pieces : std::vector<HypseusPiece>{};
    outBaseName = frameFile.stem().string();
    return true;
}

} // namespace

std::vector<HypseusFrameFile> findHypseusFrameFiles(const std::string& gameFolder) {
    namespace fs = std::filesystem;
    std::error_code ec;

    // 2026-09-22 (Cyril, real-world bug): this used to require finding
    // EXACTLY ONE .txt anywhere in the game folder and assume it must be
    // the framefile -- reasonable until a completely unrelated .txt shows
    // up there too. Confirmed directly what did it in practice: Dirk's own
    // "Generate Log" feature (see activateGenerateLog() in main.cpp) writes
    // dirk_log.txt into this exact same folder, so as soon as that's been
    // turned on once for this game, this scan started seeing two .txt files
    // and silently gave up on a game that had worked moments before ("no
    // video file found", with nothing about it in the game's own files
    // having changed). Skip Dirk's own log file by name -- it can never be
    // a real framefile regardless -- and, since any OTHER stray .txt could
    // show up here too (this is the game's own top-level folder, not one
    // Dirk fully controls), try parsing every remaining candidate rather
    // than assuming the count alone proves anything: a file that isn't a
    // framefile just fails tryParseFrameFile() and is skipped, exactly like
    // dirk_log.txt now is explicitly. More than one file that really
    // parses as a framefile used to bail out; since 2026-10-14 every real
    // framefile is returned (two versions of a game, see the header) and
    // the caller decides what several mean.
    std::vector<HypseusFrameFile> out;
    std::vector<fs::path> candidates;
    for (const auto& entry : fs::directory_iterator(gameFolder, ec)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".txt") continue;
        if (entry.path().filename() == "dirk_log.txt") continue;
        if (entry.path().filename() == "dirk_error.txt") continue; // Dirk's own error file (2026-10-12), never a framefile either
        candidates.push_back(entry.path());
    }
    std::sort(candidates.begin(), candidates.end());
    for (const auto& path : candidates) {
        HypseusFrameFile ff;
        if (!tryParseFrameFile(path, ff.segment, ff.baseName)) continue;
        ff.path = path.string();
        out.push_back(ff);
    }
    return out;
}

bool findHypseusFrameFile(const std::string& gameFolder, HypseusSegment& outSegment, std::string& outBaseName) {
    const std::vector<HypseusFrameFile> all = findHypseusFrameFiles(gameFolder);
    if (all.size() != 1) return false; // none, or several (two versions: see findHypseusFrameFiles)
    outSegment = all.front().segment;
    outBaseName = all.front().baseName;
    return true;
}

namespace {

// One side (video or audio) of the merge below: its own input context,
// stream index, output stream, and the next packet read from it that hasn't
// been written yet (nullptr once that input has hit EOF).
struct MergeSide {
    AVFormatContext* inCtx = nullptr;
    int streamIndex = -1;
    AVStream* outStream = nullptr;
    const char* label = "";
    AVPacket* pending = nullptr; // owned; null once exhausted
    bool eof = false;
    bool error = false;
};

// Reads the next packet belonging to `side.streamIndex` from `side.inCtx`
// into `side.pending`, skipping anything from another stream in that input
// (shouldn't normally happen for a single-stream .m2v/.ogg, but stay
// defensive same as before). Leaves `side.pending` null and sets eof/error
// as appropriate on end-of-file or a real read error.
void refill(MergeSide& side) {
    if (side.pending || side.eof || side.error) return;
    for (;;) {
        AVPacket* pkt = av_packet_alloc();
        if (!pkt) {
            std::fprintf(stderr, "[dirk] hypseus remux: out of memory allocating a packet for %s\n", side.label);
            side.error = true;
            return;
        }
        int readErr = av_read_frame(side.inCtx, pkt);
        if (readErr == AVERROR_EOF) {
            av_packet_free(&pkt);
            side.eof = true;
            return;
        }
        if (readErr < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(readErr, errbuf, sizeof(errbuf));
            std::fprintf(stderr, "[dirk] hypseus remux: error reading %s: %s\n", side.label, errbuf);
            av_packet_free(&pkt);
            side.error = true;
            return;
        }
        if (pkt->stream_index != side.streamIndex) {
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            continue; // not this input's stream -- keep looking
        }
        side.pending = pkt;
        return;
    }
}

// Merges `video` and `audio` into `outCtx` in true chronological (PTS)
// order, instead of writing one input's packets entirely before the other's.
//
// 2026-09-22 (Cyril, real-world bug): the original version of this file
// called copyStream() once per input, sequentially -- video first, then
// audio -- on the theory (from ffmpeg's own remuxing.c example) that
// av_interleaved_write_frame() would reorder everything into proper
// interleaving regardless of call order. That's only true within its own
// (bounded, a few seconds' worth) reorder buffer -- remuxing.c gets away
// with it because its single input's av_read_frame() already returns
// packets close to their natural interleaved order across streams. Here,
// with two SEPARATE input files read one all the way through before the
// other, that assumption silently broke on anything longer than a few
// seconds: confirmed directly by inspecting a real remuxed file's own
// packet order (ffprobe -show_entries packet=stream_index) -- long
// unbroken runs of one stream, not the near-alternating pattern a properly
// interleaved file has (503 stream transitions across 12254 packets on a
// 3-minute test, instead of thousands). On a real ~23-minute game movie
// this meant Dirk's own audio catch-up logic (VideoSource.cpp, which reads
// packets forward looking for audio to decode) could have to read through
// tens of thousands of consecutive video packets before finding a single
// audio one -- this is what was actually behind the "audio underrun"/"HARD
// RESYNC" spam and the real freeze reported at a scene deep in the file:
// not a decode-speed problem (measured directly and ruled out: the real
// mpeg2 file decodes at 160x realtime with a full ffmpeg on the reporting
// machine), a seek/keyframe-spacing problem (also ruled out: this file's
// average GOP, ~10 frames, is nothing unusual), or a game-script frame-
// numbering problem (also ruled out: the same script's frame numbers work
// fine when the identical content is loaded from an already-interleaved
// .mp4) -- purely a byte-order problem in this remux step's own output.
bool copyInterleaved(std::vector<MergeSide>& sides, AVFormatContext* outCtx) {
    // Generalized 2026-09-27 from exactly two inputs (video + one audio) to
    // video + any number of audio inputs (alternate soundtracks, see
    // HypseusAltAudio): same rule, the chronologically earliest pending
    // packet across ALL inputs is written next.
    while (true) {
        MergeSide* chosen = nullptr;
        double chosenSeconds = 0.0;
        for (MergeSide& side : sides) {
            refill(side);
            if (side.error) return false;
            if (!side.pending) continue;
            // Compare in seconds -- the inputs don't share a time_base.
            const double t = side.pending->pts * av_q2d(side.inCtx->streams[side.streamIndex]->time_base);
            if (!chosen || t < chosenSeconds) {
                chosen = &side;
                chosenSeconds = t;
            }
        }
        if (!chosen) break; // every input exhausted -- done

        AVPacket* pkt = chosen->pending;
        chosen->pending = nullptr; // ownership moves to av_interleaved_write_frame below

        AVStream* inStream = chosen->inCtx->streams[chosen->streamIndex];
        av_packet_rescale_ts(pkt, inStream->time_base, chosen->outStream->time_base);
        pkt->stream_index = chosen->outStream->index;
        pkt->pos = -1;

        int writeErr = av_interleaved_write_frame(outCtx, pkt);
        // av_interleaved_write_frame() takes ownership of pkt's DATA either
        // way (unrefs it internally), but the AVPacket itself, allocated by
        // refill() with av_packet_alloc(), is still ours to free -- it was
        // leaked once per packet before (found by AddressSanitizer,
        // 2026-09-27: ~1300 packets on an 8 s test file).
        av_packet_free(&pkt);
        if (writeErr < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(writeErr, errbuf, sizeof(errbuf));
            std::fprintf(stderr, "[dirk] hypseus remux: error writing %s: %s\n", chosen->label, errbuf);
            return false;
        }
    }
    return true;
}

} // namespace

std::string hypseusAudioSignature(const HypseusSegment& segment) {
    namespace fs = std::filesystem;
    auto one = [](const std::string& label, const std::string& path) {
        std::error_code ec;
        const auto size = fs::file_size(path, ec);
        return label + " " + fs::path(path).filename().string() + " " + (ec ? std::string("?") : std::to_string(size)) + "\n";
    };
    if (segment.pieces.size() > 1) {
        // Several pieces: their frames and video files count too.
        std::string s;
        for (const auto& piece : segment.pieces) {
            s += "piece " + std::to_string(piece.frame) + " " + one("video", piece.videoPath).substr(6);
            s += one("original", piece.audioPath);
            for (const auto& alt : piece.altAudio) s += one(alt.tag, alt.path);
        }
        return s;
    }
    std::string s = one("original", segment.audioPath);
    for (const auto& alt : segment.altAudio) s += one(alt.tag, alt.path);
    return s;
}

namespace {

// Several pieces stitched into one .mkv (2026-10-14, see the header's rules).
// Same stream copy as the one-piece remux below, piece after piece: inside
// each piece every input is merged by timestamp exactly as copyInterleaved()
// does, then shifted to the piece's place. A video frame's place inside its
// piece is counted from the piece's own first frame (its pts against the
// earliest one, see firstFrameSeconds(), at the stream's frame rate): frame k of the piece
// with line "<f> <file>" becomes frame f - f0 + k of the movie (f0 = the
// first line's frame), which is what Dirk's frame numbers (and the sheet)
// then see.
// The earliest timestamp among a file's first packets of `type`, in
// seconds: its first frame's time. Needed because a raw .m2v reports no
// start_time, and its first packet (the I-frame) is not always its first
// frame in display order (B-frames). Opens the file on its own, so the
// remux's inputs stay at their start.
double firstFrameSeconds(const std::string& path, AVMediaType type) {
    AVFormatContext* ctx = avformat_alloc_context();
    if (!ctx) return 0.0;
    ctx->flags |= AVFMT_FLAG_GENPTS;
    if (avformat_open_input(&ctx, path.c_str(), nullptr, nullptr) < 0) return 0.0;
    double best = 0.0;
    bool any = false;
    if (avformat_find_stream_info(ctx, nullptr) >= 0) {
        const int si = av_find_best_stream(ctx, type, -1, -1, nullptr, 0);
        AVPacket* pkt = av_packet_alloc();
        for (int n = 0; si >= 0 && pkt && n < 64 && av_read_frame(ctx, pkt) >= 0; av_packet_unref(pkt)) {
            if (pkt->stream_index != si) continue;
            ++n;
            const int64_t ts = pkt->pts != AV_NOPTS_VALUE ? pkt->pts : pkt->dts;
            if (ts == AV_NOPTS_VALUE) continue;
            const double t = ts * av_q2d(ctx->streams[si]->time_base);
            if (!any || t < best) best = t;
            any = true;
        }
        av_packet_free(&pkt);
    }
    avformat_close_input(&ctx);
    return best;
}

bool remuxHypseusPieces(const HypseusSegment& segment, const std::string& outPath) {
    namespace fs = std::filesystem;
    const auto& pieces = segment.pieces;
    const std::string tmpPath = outPath + ".tmp";
    AVFormatContext* outCtx = nullptr;
    bool ok = false;

    struct Input { AVFormatContext* ctx = nullptr; int stream = -1; };
    std::vector<Input> open; // the current piece's inputs: video first, then each audio
    auto closeInputs = [&]() {
        for (auto& in : open) if (in.ctx) avformat_close_input(&in.ctx);
        open.clear();
    };
    auto finish = [&]() {
        closeInputs();
        if (outCtx) {
            if (!(outCtx->oformat->flags & AVFMT_NOFILE) && outCtx->pb) avio_closep(&outCtx->pb);
            avformat_free_context(outCtx);
            outCtx = nullptr;
        }
        if (!ok) {
            std::error_code rmEc;
            fs::remove(tmpPath, rmEc);
        }
    };
    auto openInput = [&](const std::string& path, AVMediaType type) -> bool {
        Input in;
        in.ctx = avformat_alloc_context();
        if (!in.ctx) return false;
        in.ctx->flags |= AVFMT_FLAG_GENPTS; // a raw .m2v has no timestamps of its own (see below)
        if (avformat_open_input(&in.ctx, path.c_str(), nullptr, nullptr) < 0) {
            std::fprintf(stderr, "[dirk] hypseus remux: failed to open %s\n", path.c_str());
            return false; // avformat_open_input freed the context
        }
        if (avformat_find_stream_info(in.ctx, nullptr) < 0) {
            std::fprintf(stderr, "[dirk] hypseus remux: failed to read stream info: %s\n", path.c_str());
            avformat_close_input(&in.ctx);
            return false;
        }
        in.stream = av_find_best_stream(in.ctx, type, -1, -1, nullptr, 0);
        if (in.stream < 0) {
            std::fprintf(stderr, "[dirk] hypseus remux: no %s stream in %s\n", type == AVMEDIA_TYPE_VIDEO ? "video" : "audio", path.c_str());
            avformat_close_input(&in.ctx);
            return false;
        }
        open.push_back(in);
        return true;
    };
    auto pieceInputs = [&](const HypseusPiece& piece) -> bool {
        closeInputs();
        if (!openInput(piece.videoPath, AVMEDIA_TYPE_VIDEO)) return false;
        if (!openInput(piece.audioPath, AVMEDIA_TYPE_AUDIO)) return false;
        for (const auto& alt : piece.altAudio) if (!openInput(alt.path, AVMEDIA_TYPE_AUDIO)) return false;
        return true;
    };

    // The output streams, from the first piece.
    if (!pieceInputs(pieces.front())) { finish(); return false; }
    AVStream* firstVideo = open[0].ctx->streams[open[0].stream];
    const AVRational rate = av_guess_frame_rate(open[0].ctx, firstVideo, nullptr);
    if (rate.num <= 0 || rate.den <= 0) {
        std::fprintf(stderr, "[dirk] hypseus remux: no frame rate in %s\n", pieces.front().videoPath.c_str());
        finish();
        return false;
    }
    const double fps = av_q2d(rate);
    const AVCodecParameters firstPar = *firstVideo->codecpar; // compared with every other piece's (codec, size)

    fs::path outDir = fs::path(outPath).parent_path();
    std::error_code mkdirEc;
    if (!outDir.empty()) fs::create_directories(outDir, mkdirEc);
    if (avformat_alloc_output_context2(&outCtx, nullptr, "matroska", tmpPath.c_str()) < 0 || !outCtx) {
        std::fprintf(stderr, "[dirk] hypseus remux: failed to allocate matroska output\n");
        finish();
        return false;
    }
    std::vector<AVStream*> outStreams;
    for (size_t i = 0; i < open.size(); ++i) {
        AVStream* in = open[i].ctx->streams[open[i].stream];
        AVStream* out = avformat_new_stream(outCtx, nullptr);
        if (!out || avcodec_parameters_copy(out->codecpar, in->codecpar) < 0) {
            std::fprintf(stderr, "[dirk] hypseus remux: failed to create an output stream\n");
            finish();
            return false;
        }
        out->time_base = in->time_base;
        if (i >= 1) {
            const std::string tag = i == 1 ? std::string() : pieces.front().altAudio[i - 2].tag;
            out->disposition = tag.empty() ? AV_DISPOSITION_DEFAULT : 0;
            if (!tag.empty()) {
                std::string title = tag;
                for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                av_dict_set(&out->metadata, "title", title.c_str(), 0);
                if (const char* lang = dirk::iso639_2Code(tag)) av_dict_set(&out->metadata, "language", lang, 0);
            }
        }
        outStreams.push_back(out);
    }
    if (!(outCtx->oformat->flags & AVFMT_NOFILE) && avio_open(&outCtx->pb, tmpPath.c_str(), AVIO_FLAG_WRITE) < 0) {
        std::fprintf(stderr, "[dirk] hypseus remux: failed to open output file: %s\n", tmpPath.c_str());
        finish();
        return false;
    }
    if (avformat_write_header(outCtx, nullptr) < 0) {
        std::fprintf(stderr, "[dirk] hypseus remux: failed to write matroska header\n");
        finish();
        return false;
    }

    const long long f0 = pieces.front().frame;
    std::vector<int64_t> lastDts(outStreams.size(), AV_NOPTS_VALUE);
    for (size_t p = 0; p < pieces.size(); ++p) {
        const HypseusPiece& piece = pieces[p];
        const std::string name = fs::path(piece.videoPath).filename().string();
        if (p > 0 && !pieceInputs(piece)) { finish(); return false; }
        const AVCodecParameters* par = open[0].ctx->streams[open[0].stream]->codecpar;
        if (par->codec_id != firstPar.codec_id || par->width != firstPar.width || par->height != firstPar.height) {
            std::fprintf(stderr, "[dirk] hypseus remux: %s is not like %s (%dx%d, not %dx%d, or another codec) -- every piece must be the same kind of video\n",
                         name.c_str(), fs::path(pieces.front().videoPath).filename().string().c_str(),
                         par->width, par->height, firstPar.width, firstPar.height);
            finish();
            return false;
        }
        const double placeSeconds = (piece.frame - f0) / fps;
        const long long room = p + 1 < pieces.size() ? pieces[p + 1].frame - piece.frame : -1; // -1: the last piece runs to its end
        const double endSeconds = room >= 0 ? placeSeconds + room / fps : 0.0;

        std::vector<MergeSide> sides;
        std::vector<double> startSeconds;
        std::vector<std::string> inputPaths{piece.videoPath, piece.audioPath};
        for (const auto& alt : piece.altAudio) inputPaths.push_back(alt.path);
        for (size_t i = 0; i < open.size(); ++i) {
            sides.push_back(MergeSide{open[i].ctx, open[i].stream, outStreams[i], i == 0 ? "video" : "audio"});
            startSeconds.push_back(firstFrameSeconds(inputPaths[i], i == 0 ? AVMEDIA_TYPE_VIDEO : AVMEDIA_TYPE_AUDIO));
        }
        long long framesKept = 0, framesCut = 0;
        for (;;) {
            MergeSide* chosen = nullptr;
            size_t chosenIndex = 0;
            double chosenSeconds = 0.0;
            for (size_t i = 0; i < sides.size(); ++i) {
                MergeSide& side = sides[i];
                refill(side);
                if (side.error) { finish(); return false; }
                if (!side.pending) continue;
                const int64_t ts = side.pending->pts != AV_NOPTS_VALUE ? side.pending->pts : side.pending->dts;
                const double t = ts * av_q2d(side.inCtx->streams[side.streamIndex]->time_base) - startSeconds[i];
                if (!chosen || t < chosenSeconds) { chosen = &side; chosenIndex = i; chosenSeconds = t; }
            }
            if (!chosen) break;
            AVPacket* pkt = chosen->pending;
            chosen->pending = nullptr;
            bool keep = true;
            if (chosenIndex == 0) {
                const long long k = std::llround(chosenSeconds * fps);
                if (k < 0) keep = false;                         // before the piece's first frame (an open GOP's leading B-frames)
                else if (room >= 0 && k >= room) { keep = false; ++framesCut; }
                else ++framesKept;
            } else if (room >= 0 && placeSeconds + chosenSeconds >= endSeconds) {
                keep = false;                                    // the next piece's sound from here on
            }
            if (!keep) { av_packet_free(&pkt); continue; }

            AVStream* inStream = chosen->inCtx->streams[chosen->streamIndex];
            const AVRational outTb = chosen->outStream->time_base;
            const int64_t shift = std::llround((placeSeconds - startSeconds[chosenIndex]) / av_q2d(outTb));
            av_packet_rescale_ts(pkt, inStream->time_base, outTb);
            if (pkt->pts != AV_NOPTS_VALUE) pkt->pts += shift;
            if (pkt->dts != AV_NOPTS_VALUE) pkt->dts += shift;
            // Where two pieces meet, a reordered stream's first decode
            // time can fall just before the last one written: keep it
            // increasing, as the muxer requires.
            int64_t& last = lastDts[chosenIndex];
            if (pkt->dts != AV_NOPTS_VALUE) {
                if (last != AV_NOPTS_VALUE && pkt->dts <= last) pkt->dts = last + 1;
                if (pkt->pts != AV_NOPTS_VALUE && pkt->pts < pkt->dts) pkt->pts = pkt->dts;
                last = pkt->dts;
            }
            pkt->stream_index = chosen->outStream->index;
            pkt->pos = -1;
            const int writeErr = av_interleaved_write_frame(outCtx, pkt);
            av_packet_free(&pkt);
            if (writeErr < 0) {
                char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
                av_strerror(writeErr, errbuf, sizeof(errbuf));
                std::fprintf(stderr, "[dirk] hypseus remux: error writing %s: %s\n", name.c_str(), errbuf);
                finish();
                return false;
            }
        }
        if (room >= 0 && framesKept < room) {
            std::fprintf(stderr,
                "[dirk] hypseus remux: %s has %lld frames, but the next file starts %lld frames after it (frame %lld): "
                "frames %lld to %lld would have no picture -- Dirk can't fill a hole, fix the framefile or the files\n",
                name.c_str(), framesKept, room, pieces[p + 1].frame,
                piece.frame + framesKept, pieces[p + 1].frame - 1);
            finish();
            return false;
        }
        std::fprintf(stderr, "[dirk] hypseus remux: %s at frame %lld, %lld frames%s\n", name.c_str(), piece.frame,
                     framesKept, framesCut ? (", " + std::to_string(framesCut) + " cut where the next file starts").c_str() : "");
    }

    if (av_write_trailer(outCtx) < 0) {
        std::fprintf(stderr, "[dirk] hypseus remux: failed to finalize matroska output\n");
        finish();
        return false;
    }
    ok = true;
    finish();
    std::error_code renameEc;
    fs::rename(tmpPath, outPath, renameEc);
    if (renameEc) {
        std::fprintf(stderr, "[dirk] hypseus remux: converted but failed to finalize %s -> %s: %s\n",
                     tmpPath.c_str(), outPath.c_str(), renameEc.message().c_str());
        return false;
    }
    return true;
}

} // namespace

bool remuxHypseusSegment(const HypseusSegment& segment, const std::string& outPath) {
    namespace fs = std::filesystem;
    if (segment.pieces.size() > 1) return remuxHypseusPieces(segment, outPath);

    AVFormatContext* videoIn = nullptr;
    AVFormatContext* outCtx = nullptr;
    bool ok = false;
    std::string tmpPath = outPath + ".tmp";
    std::function<void()> closeAudioInputs = [] {};

    auto cleanup = [&]() {
        if (videoIn) avformat_close_input(&videoIn);
        closeAudioInputs();
        closeAudioInputs = [] {};
        if (outCtx) {
            if (!(outCtx->oformat->flags & AVFMT_NOFILE) && outCtx->pb) {
                avio_closep(&outCtx->pb);
            }
            avformat_free_context(outCtx);
            outCtx = nullptr;
        }
        if (!ok) {
            std::error_code rmEc;
            fs::remove(tmpPath, rmEc); // best-effort -- never leave a half-written file behind on failure
        }
    };

    // -fflags +genpts equivalent: a raw .m2v elementary stream has no
    // presentation timestamps of its own (confirmed directly -- ffmpeg
    // refuses to mux it otherwise, "Can't write packet with unknown
    // timestamp"; see the LISEZMOI this recipe was first validated in).
    // Applied to both inputs for consistency, though it's the video side
    // that actually needs it.
    videoIn = avformat_alloc_context();
    if (!videoIn) { cleanup(); return false; }
    videoIn->flags |= AVFMT_FLAG_GENPTS;
    if (avformat_open_input(&videoIn, segment.videoPath.c_str(), nullptr, nullptr) < 0) {
        std::fprintf(stderr, "[dirk] hypseus remux: failed to open video: %s\n", segment.videoPath.c_str());
        cleanup();
        return false;
    }
    if (avformat_find_stream_info(videoIn, nullptr) < 0) {
        std::fprintf(stderr, "[dirk] hypseus remux: failed to read stream info: %s\n", segment.videoPath.c_str());
        cleanup();
        return false;
    }

    // Every audio input: the original soundtrack first, then each alternate
    // one (see HypseusAltAudio). Opened the same way; any failure aborts the
    // whole conversion rather than silently dropping a language.
    struct AudioInput { std::string path; std::string tag; AVFormatContext* ctx = nullptr; int stream = -1; };
    std::vector<AudioInput> audioInputs;
    audioInputs.push_back({segment.audioPath, ""});
    for (const auto& alt : segment.altAudio) audioInputs.push_back({alt.path, alt.tag});
    closeAudioInputs = [&]() {
        for (auto& in : audioInputs) if (in.ctx) avformat_close_input(&in.ctx);
    };
    for (auto& in : audioInputs) {
        in.ctx = avformat_alloc_context();
        if (!in.ctx) { cleanup(); return false; }
        in.ctx->flags |= AVFMT_FLAG_GENPTS;
        if (avformat_open_input(&in.ctx, in.path.c_str(), nullptr, nullptr) < 0) {
            std::fprintf(stderr, "[dirk] hypseus remux: failed to open audio: %s\n", in.path.c_str());
            cleanup();
            return false;
        }
        if (avformat_find_stream_info(in.ctx, nullptr) < 0) {
            std::fprintf(stderr, "[dirk] hypseus remux: failed to read stream info: %s\n", in.path.c_str());
            cleanup();
            return false;
        }
        in.stream = av_find_best_stream(in.ctx, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (in.stream < 0) {
            std::fprintf(stderr, "[dirk] hypseus remux: no audio stream found in %s\n", in.path.c_str());
            cleanup();
            return false;
        }
    }

    int videoStreamIdx = av_find_best_stream(videoIn, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (videoStreamIdx < 0) {
        std::fprintf(stderr, "[dirk] hypseus remux: no video stream found in %s\n", segment.videoPath.c_str());
        cleanup();
        return false;
    }

    fs::path outDir = fs::path(outPath).parent_path();
    std::error_code mkdirEc;
    if (!outDir.empty()) fs::create_directories(outDir, mkdirEc);

    if (avformat_alloc_output_context2(&outCtx, nullptr, "matroska", tmpPath.c_str()) < 0 || !outCtx) {
        std::fprintf(stderr, "[dirk] hypseus remux: failed to allocate matroska output (muxer not compiled in? see tools/build_ffmpeg_minimal_windows.sh)\n");
        cleanup();
        return false;
    }

    AVStream* outVideoStream = avformat_new_stream(outCtx, nullptr);
    if (!outVideoStream ||
        avcodec_parameters_copy(outVideoStream->codecpar, videoIn->streams[videoStreamIdx]->codecpar) < 0) {
        std::fprintf(stderr, "[dirk] hypseus remux: failed to create the video output stream\n");
        cleanup();
        return false;
    }
    outVideoStream->time_base = videoIn->streams[videoStreamIdx]->time_base;

    std::vector<MergeSide> sides;
    sides.push_back(MergeSide{videoIn, videoStreamIdx, outVideoStream, "video"});
    for (auto& in : audioInputs) {
        AVStream* outAudio = avformat_new_stream(outCtx, nullptr);
        if (!outAudio || avcodec_parameters_copy(outAudio->codecpar, in.ctx->streams[in.stream]->codecpar) < 0) {
            std::fprintf(stderr, "[dirk] hypseus remux: failed to create an audio output stream for %s\n", in.path.c_str());
            cleanup();
            return false;
        }
        outAudio->time_base = in.ctx->streams[in.stream]->time_base;
        // The original soundtrack is the file's default track.
        outAudio->disposition = in.tag.empty() ? AV_DISPOSITION_DEFAULT : 0;
        if (!in.tag.empty()) {
            // Title = the tag as players and Dirk's own menu show it ("FR").
            std::string title = in.tag;
            for (char& c : title) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            av_dict_set(&outAudio->metadata, "title", title.c_str(), 0);
            if (const char* lang = dirk::iso639_2Code(in.tag)) av_dict_set(&outAudio->metadata, "language", lang, 0);
        }
        sides.push_back(MergeSide{in.ctx, in.stream, outAudio, in.tag.empty() ? "audio" : "alternate audio"});
    }

    if (!(outCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&outCtx->pb, tmpPath.c_str(), AVIO_FLAG_WRITE) < 0) {
            std::fprintf(stderr, "[dirk] hypseus remux: failed to open output file: %s\n", tmpPath.c_str());
            cleanup();
            return false;
        }
    }

    if (avformat_write_header(outCtx, nullptr) < 0) {
        std::fprintf(stderr, "[dirk] hypseus remux: failed to write matroska header\n");
        cleanup();
        return false;
    }

    // Merge all inputs by timestamp as they're read -- see copyInterleaved()
    // above for why reading one input entirely before the other (the
    // original approach here) silently produced a non-interleaved file on
    // anything longer than a few seconds.
    if (!copyInterleaved(sides, outCtx)) {
        cleanup();
        return false;
    }

    if (av_write_trailer(outCtx) < 0) {
        std::fprintf(stderr, "[dirk] hypseus remux: failed to finalize matroska output\n");
        cleanup();
        return false;
    }

    ok = true;
    cleanup(); // closes everything; ok=true means the .tmp file is kept

    std::error_code renameEc;
    fs::rename(tmpPath, outPath, renameEc);
    if (renameEc) {
        std::fprintf(stderr, "[dirk] hypseus remux: converted but failed to finalize %s -> %s: %s\n",
                      tmpPath.c_str(), outPath.c_str(), renameEc.message().c_str());
        return false;
    }

    return true;
}

} // namespace dirk
