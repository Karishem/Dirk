// Several soundtracks (2026-09-27): Hypseus <name>-fr.ogg / <name>_it.ogg
// alternates found next to the video, merged by HypseusRemux into extra
// audio tracks of the cached .mkv, then listed and switched by
// VideoSource. Real files: a 30 fps test video with three sine
// soundtracks, 440 Hz (original), 880 Hz (-fr) and 1320 Hz (_it), so the
// decoded audio says which track is playing.
#include "video/HypseusRemux.h"
#include "video/VideoSource.h"

extern "C" {
#include <libavformat/avformat.h>
}

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}

// Which of the three test tones (440 / 880 / 1320 Hz) dominates the left
// channel of S16 stereo 48 kHz PCM -- Goertzel power at each. (Plain zero
// crossings under-read: after a seek VideoSource pads the start with a few
// ms of silence to line the audio up with the picture.)
static double frequencyOf(const std::vector<uint8_t>& pcm) {
    const size_t n = pcm.size() / 4;
    if (n < 4800) return 0.0;
    double best = 0.0, bestPower = -1.0, secondPower = -1.0;
    for (double f : {440.0, 880.0, 1320.0}) {
        const double w = 2.0 * M_PI * f / 48000.0, coeff = 2.0 * std::cos(w);
        double s1 = 0.0, s2 = 0.0;
        for (size_t i = 0; i < n; ++i) {
            int16_t l;
            std::memcpy(&l, pcm.data() + i * 4, 2);
            const double s0 = l + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }
        const double power = s1 * s1 + s2 * s2 - coeff * s1 * s2;
        if (power > bestPower) { secondPower = bestPower; bestPower = power; best = f; }
        else if (power > secondPower) secondPower = power;
    }
    // Only one soundtrack audible: nothing of the previous one mixed in.
    return bestPower > 100.0 * secondPower ? best : -best;
}

// Seeks to `frame` and collects about half a second of the audio decoded
// from there on.
static double frequencyAfterSeek(dirk::VideoSource& v, int64_t frame) {
    v.seekToFrame(frame);
    std::vector<uint8_t> pcm = v.takePendingAudio();
    for (int i = 0; i < 15 && pcm.size() < 48000 * 4 / 2; ++i) {
        if (!v.stepForward()) break;
        std::vector<uint8_t> more = v.takePendingAudio();
        pcm.insert(pcm.end(), more.begin(), more.end());
    }
    if (const char* dump = std::getenv("AUDIO_TRACKS_DUMP")) {
        std::ofstream(dump, std::ios::binary | std::ios::app).write(reinterpret_cast<const char*>(pcm.data()), static_cast<std::streamsize>(pcm.size()));
    }
    return frequencyOf(pcm);
}

static bool near(double f, double want) { return f == want; } // negative = two tones mixed

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <assets/hypseus_multiaudio>\n", argv[0]);
        return 2;
    }
    const fs::path src = argv[1];
    const fs::path game = fs::temp_directory_path() / "dirk_audio_tracks_test";
    fs::remove_all(game);
    fs::create_directories(game / "Video");
    for (const char* f : {"test.m2v", "test.ogg", "test-fr.ogg", "test_it.ogg"}) fs::copy_file(src / f, game / "Video" / f);
    fs::copy_file(src / "test-fr.ogg", game / "Video" / "test-toolongtag123.ogg"); // tag > 10 chars: ignored
    fs::copy_file(src / "test-fr.ogg", game / "Video" / "other-fr.ogg");           // another video's: ignored
    fs::copy_file(src / "test-fr.ogg", game / "Video" / "test.fr.bak.ogg");        // "fr.bak" is not a tag: ignored
    std::ofstream(game / "Game.txt") << "Video/\n0 test.m2v\n";

    // 1. Finding the alternates.
    dirk::HypseusSegment seg;
    std::string base;
    check(dirk::findHypseusFrameFile(game.string(), seg, base), "framefile found");
    std::string tags;
    for (const auto& a : seg.altAudio) tags += a.tag + ",";
    check(tags == "fr,it,", "alternate soundtracks: fr (test-fr.ogg), it (test_it.ogg), junk ignored (got " + tags + ")");

    // 2. Signature changes when a soundtrack is added.
    const std::string sig1 = dirk::hypseusAudioSignature(seg);
    fs::copy_file(src / "test-fr.ogg", game / "Video" / "test-de.ogg");
    dirk::HypseusSegment seg2;
    dirk::findHypseusFrameFile(game.string(), seg2, base);
    check(dirk::hypseusAudioSignature(seg2) != sig1 && seg2.altAudio.size() == 3, "adding test-de.ogg changes the signature");
    fs::remove(game / "Video" / "test-de.ogg");

    // 3. The remuxed .mkv: 1 video + 3 audio streams, titled, interleaved.
    const fs::path mkv = game / "Movie" / "Game.mkv";
    check(dirk::remuxHypseusSegment(seg, mkv.string()), "remux with alternate soundtracks");
    AVFormatContext* fc = nullptr;
    int audioStreams = 0, maxRun = 0, run = 0, lastStream = -1;
    std::string titles;
    if (avformat_open_input(&fc, mkv.string().c_str(), nullptr, nullptr) == 0 && avformat_find_stream_info(fc, nullptr) >= 0) {
        for (unsigned i = 0; i < fc->nb_streams; ++i) {
            if (fc->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
            ++audioStreams;
            const AVDictionaryEntry* t = av_dict_get(fc->streams[i]->metadata, "title", nullptr, 0);
            const AVDictionaryEntry* l = av_dict_get(fc->streams[i]->metadata, "language", nullptr, 0);
            titles += std::string(t ? t->value : "-") + "/" + (l ? l->value : "-") + ",";
        }
        AVPacket* p = av_packet_alloc();
        while (av_read_frame(fc, p) >= 0) {
            run = (p->stream_index == lastStream) ? run + 1 : 1;
            if (fc->streams[p->stream_index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO && run > maxRun) maxRun = run;
            lastStream = p->stream_index;
            av_packet_unref(p);
        }
        av_packet_free(&p);
        avformat_close_input(&fc);
    }
    check(audioStreams == 3, "3 audio streams in the .mkv (got " + std::to_string(audioStreams) + ")");
    check(titles == "-/-,FR/fre,IT/ita,", "titles/languages: original untitled, FR/fre, IT/ita (got " + titles + ")");
    check(maxRun > 0 && maxRun < 60, "interleaved: at most " + std::to_string(maxRun) + " video packets in a row");

    // 4. VideoSource: tracks, labels, default, switching.
    {
        dirk::VideoSource v;
        check(v.open(mkv.string()), "VideoSource opens the .mkv");
        check(v.audioTrackCount() == 3, "3 audio tracks");
        check(v.audioTrackLabel(0) == "Original" && v.audioTrackLabel(1) == "French" && v.audioTrackLabel(2) == "Italian",
              "labels Original, French, Italian (got " + v.audioTrackLabel(0) + "," + v.audioTrackLabel(1) + "," + v.audioTrackLabel(2) + ")");
        check(v.currentAudioTrack() == 0, "default track = the original");
        double f = frequencyAfterSeek(v, 60);
        check(near(f, 440), "original plays: " + std::to_string(f) + " Hz");
        check(v.selectAudioTrack(1), "switch to FR");
        f = frequencyAfterSeek(v, 60);
        check(near(f, 880), "after switch + re-seek, FR plays: " + std::to_string(f) + " Hz");
        check(v.selectAudioTrack(2), "switch to IT");
        f = frequencyAfterSeek(v, 150);
        check(near(f, 1320), "IT plays, further in: " + std::to_string(f) + " Hz");
        check(!v.selectAudioTrack(5) && v.currentAudioTrack() == 2, "out of range: refused, IT kept");
        check(v.selectAudioTrack(0), "back to the original");
        f = frequencyAfterSeek(v, 30);
        check(near(f, 440), "original again: " + std::to_string(f) + " Hz");
    }
    {
        dirk::VideoSource v;
        v.setPreferredAudioTrack(1);
        check(v.open(mkv.string()) && v.currentAudioTrack() == 1, "saved choice FR opened directly");
        double f = frequencyAfterSeek(v, 90);
        check(near(f, 880), "FR from the start: " + std::to_string(f) + " Hz");
    }
    {
        dirk::VideoSource v; // a plain single-soundtrack movie: unchanged behaviour
        check(v.open((game / "Video" / "test.m2v").string()) && v.audioTrackCount() == 0, "video without audio: 0 tracks, opens fine");
    }

    fs::remove_all(game);
    std::printf("%s\n", failures == 0 ? "ALL OK" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
