#pragma once
// HypseusRemux: lets dirk play a Hypseus/Singe-1-style game whose video and
// audio ship as two SEPARATE files (Video/<name>.m2v + Video/<name>.ogg --
// the classic Daphne-era LaserDisc-rip convention, still what Hypseus
// requires today) instead of one muxed .mp4 like every KimmyScript game
// seen so far.
//
// Rather than teaching VideoSource itself to read from two demuxers at
// once -- a real rewrite of a class whose single-demuxer assumption runs
// all through its audio/video interleaving and AV-sync logic (see
// VideoSource.h's own comments on audioStartTimeSeconds_/pendingVideoPackets_
// for how much hard-won, delicate behavior lives in there) -- this remuxes
// the pair into a single ordinary .mkv file ONCE, natively (no external
// ffmpeg.exe, just the same libavformat/libavcodec dirk already links),
// and lets VideoSource open THAT exactly like any other movie, completely
// unchanged. See findVideoInMovieFolder() in main.cpp for where this gets
// called (a fallback tier, only tried once Movie/ and Video/ have already
// failed to yield a normal single-file video) and how the result is cached
// -- once a game has been converted, every later launch just finds the
// .mkv directly and skips this whole file.
//
// Several pieces (2026-10-14, Cyril: "le fichier txt d'un Hypseus est
// censé t'indiquer à quel frame placer chaque fichier"): a framefile can
// list several "<frame> <file>.m2v" lines (an old multi-sided-disc
// convention). They are stitched into the one .mkv, each piece at its own
// frame, counted from the first line's (a single-piece framefile has
// always started the movie at its first frame whatever its number, and
// still does). Rules, see remuxHypseusPieces() in the .cpp:
//   - each piece's first frame lands exactly on its line's frame;
//   - a piece longer than the room before the next one is cut there (the
//     next piece wins);
//   - a piece shorter than that room leaves a hole: refused, with the
//     missing frames named -- Dirk numbers the movie's frames in order and
//     can't make pictures (no encoder) to fill it;
//   - each piece's <name>.ogg goes at the same place; an alternate
//     soundtrack (<name>-fr.ogg...) is kept only if every piece has it.
// Still no real multi-piece game to test with: tested on DLe cut in three.
//
// Two framefiles in the same game (two versions, e.g. 4:3 and 16:9) each
// become their own Movie/<framefile>.mkv, and the player picks one at
// launch like any other two movies (see core/MovieChoice.h).

#include <string>
#include <vector>

namespace dirk {

// One alternate soundtrack next to the video (2026-09-27, Cyril: several
// Hypseus games ship other languages as <name>-fr.ogg, <name>-it.ogg --
// what Hypseus plays with "-altaudio -fr"). `tag` is the part after the
// separator ("fr"), `path` the .ogg.
struct HypseusAltAudio {
    std::string tag;
    std::string path;
};

// One "<frame> <file>" line of a multi-piece framefile.
struct HypseusPiece {
    long long frame = 0;   // as written in the framefile
    std::string videoPath;
    std::string audioPath;
    std::vector<HypseusAltAudio> altAudio; // only the tags every piece has
};

// One video+audio pair resolved from a framefile.
struct HypseusSegment {
    std::string videoPath; // e.g. ".../DLe/Video/lair.m2v"
    std::string audioPath; // same basename, ".ogg" -- e.g. ".../DLe/Video/lair.ogg"
    // <basename><sep><tag>.ogg in the same folder, sep = '-', '_' or '.',
    // tag = 1-10 letters/digits; sorted by tag. Each becomes an extra
    // audio track of the remuxed .mkv, titled with the tag in upper case.
    std::vector<HypseusAltAudio> altAudio;
    // Two or more lines: every piece, sorted by frame (the fields above
    // are then the first piece's). Empty for the usual one-line framefile.
    std::vector<HypseusPiece> pieces;
    // Alternate soundtracks left out because another piece lacks them
    // (file names), said when the movie is converted.
    std::vector<std::string> droppedAudio;
};

// A framefile found in a game folder.
struct HypseusFrameFile {
    HypseusSegment segment;
    std::string baseName;  // the framefile's stem: the cached Movie/<baseName>.mkv
    std::string path;      // the .txt itself
};

// Identifies the exact set of audio files a remuxed .mkv was built from
// (names and sizes), so a soundtrack added, removed or replaced later
// triggers a rebuild. Stored next to the .mkv as "<name>.mkv.audio" by
// main.cpp's findVideoInMovieFolder().
std::string hypseusAudioSignature(const HypseusSegment& segment);

// Looks for exactly one ".txt" file directly in `gameFolder` (the Hypseus
// "framefile" convention: <Game>.txt sits right next to <Game>.singe/.dirk)
// and parses it. Returns false (nothing printed -- this is a routine "not
// a Hypseus game" check, not an error) if there isn't exactly one .txt
// file there that parses as a framefile (one piece or several, see
// HypseusSegment::pieces). On success, `outSegment` is filled
// in and `outBaseName` gets the framefile's own stem (e.g. "DLe"), used to
// name the cached .mkv the same way.
bool findHypseusFrameFile(const std::string& gameFolder, HypseusSegment& outSegment, std::string& outBaseName);

// Every framefile in `gameFolder` (sorted by name), same skips and parsing
// as above. findHypseusFrameFile() is true when there is exactly one.
std::vector<HypseusFrameFile> findHypseusFrameFiles(const std::string& gameFolder);

// Remuxes one segment's video+audio into a single Matroska file at
// `outPath` -- stream copy only (no re-encoding: lossless, and dirk's own
// minimal ffmpeg build has no encoders anyway, only decoders). Writes to
// `outPath + ".tmp"` first and renames it into place only once the whole
// file has been written successfully, so an interrupted run (crash, disk
// full, killed process) never leaves a half-written file that a later
// launch would mistake for a finished conversion. Prints "[dirk] ..." on
// any failure (missing/unreadable input, no muxer available -- see
// tools/build_ffmpeg_minimal_windows.sh, this needs mpeg2video/vorbis
// decode plus the mpegvideo/ogg demuxers and the matroska muxer all
// enabled -- codec mismatch, disk write error, ...); returns false in
// every one of those cases.
// Every alternate soundtrack in segment.altAudio is added as a further
// audio stream (after the original one), all interleaved by timestamp.
// A multi-piece segment is stitched (see the top of this file).
bool remuxHypseusSegment(const HypseusSegment& segment, const std::string& outPath);

} // namespace dirk
