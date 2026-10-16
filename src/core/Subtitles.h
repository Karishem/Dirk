#pragma once

#include <string>
#include <vector>

namespace dirk {

// .srt subtitles for the main movie (2026-09-27, Cyril: Time Gal / Ninja
// Hayate have Japanese dialogue). A subtitle file sits next to the movie,
// same base name:
//
//   Movie/Timegal.srt      -> an untagged track, shown as "On"
//   Movie/Timegal-fr.srt   -> shown as "French"
//   Movie/Timegal-pt-br.srt-> shown as "Portuguese (BR)"
//   Movie/Timegal-xx.srt   -> shown as "Unknown" (not an ISO 639 code)
// (language names: see core/Languages.h)
//
// "-fr" is the same suffix as a Hypseus alternate soundtrack
// (<name>-fr.ogg); "_fr" and ".fr" are accepted too.
//
// Timecodes are positions in the movie FILE (0 = its first frame), which
// is what any subtitle editor shows when the movie is opened in it. Dirk
// looks up the cue for the frame on screen, so whatever jumps the game
// makes in the movie, the right line shows -- games need no change.

struct SubtitleCue {
    double startSeconds = 0.0;
    double endSeconds = 0.0;
    std::vector<std::string> lines; // UTF-8, tags stripped, never empty
};

struct SubtitleTrack {
    std::string path;  // the .srt file
    std::string label; // "On" for an untagged file, else the language name ("French")
    std::vector<SubtitleCue> cues; // sorted by start time
};

// Parses .srt text. Tolerant: UTF-8 with or without BOM, UTF-16 with BOM,
// and legacy Windows-1252 / Latin-1 files (common for old French .srt) are
// all turned into UTF-8; CRLF/LF/CR; "," or "." before the milliseconds;
// missing index lines; <i>/<b>/<font ...> and {\...} tags are removed.
// A block that can't be understood is skipped, never fatal.
std::vector<SubtitleCue> parseSrt(const std::string& bytes);

// Every subtitle file for `moviePath` (see above), untagged first, then by
// tag. Files that parse to no cue at all are left out. Case-insensitive on
// the base name and extension (Windows).
std::vector<SubtitleTrack> findSubtitleTracks(const std::string& moviePath);

// The lines to show at movie position `seconds` (all cues active then, in
// start order), or an empty vector. `seconds` < 0 -> empty.
std::vector<std::string> activeSubtitleLines(const SubtitleTrack& track, double seconds);

} // namespace dirk
