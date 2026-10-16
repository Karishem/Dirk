// Unit test for the .srt reader (src/core/Subtitles.*): parsing (encodings,
// line endings, tags, broken blocks), the cue lookup Dirk does for the
// frame on screen, and finding the files next to a movie. Real files on
// disk for the last part.
#include "core/Subtitles.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using dirk::SubtitleCue;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
static bool near(double a, double b) { return a - b < 1e-6 && b - a < 1e-6; }
static std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (size_t i = 0; i < v.size(); ++i) s += (i ? "|" : "") + v[i];
    return s;
}
static void writeFile(const fs::path& p, const std::string& content) {
    std::ofstream(p, std::ios::binary) << content;
}

int main() {
    // 1. Standard file: UTF-8 BOM, CRLF, index lines, two-line cue, tags.
    {
        const std::string srt =
            "\xEF\xBB\xBF" "1\r\n00:00:01,000 --> 00:00:02,500\r\n<i>Bonjour</i> Reika !\r\n\r\n"
            "2\r\n00:01:02,250 --> 00:01:04,000\r\nPremi\xC3\xA8re ligne\r\n{\\an8}<font color=\"#ff0\">Deuxi\xC3\xA8me</font>\r\n\r\n";
        auto cues = dirk::parseSrt(srt);
        check(cues.size() == 2, "standard file: 2 cues");
        if (cues.size() == 2) {
            check(near(cues[0].startSeconds, 1.0) && near(cues[0].endSeconds, 2.5), "cue 1 timecodes");
            check(join(cues[0].lines) == "Bonjour Reika !", "tags <i> removed");
            check(near(cues[1].startSeconds, 62.25) && near(cues[1].endSeconds, 64.0), "cue 2 timecodes (minutes)");
            check(join(cues[1].lines) == "Premi\xC3\xA8re ligne|Deuxi\xC3\xA8me", "two lines kept, {\\an8} and <font> removed, UTF-8 intact");
        }
    }
    // 2. Sloppy file: "." before ms, no index, no blank line between cues,
    //    LF only, position hints after the end time, 1-digit hours.
    {
        const std::string srt =
            "0:00:05.5 --> 0:00:06.25 X1:10 X2:20\nA\n00:00:07.000 --> 00:00:08.000\nB\n";
        auto cues = dirk::parseSrt(srt);
        check(cues.size() == 2, "sloppy file: 2 cues");
        if (cues.size() == 2) {
            check(near(cues[0].startSeconds, 5.5) && near(cues[0].endSeconds, 6.25), "short ms and '.' separator");
            check(join(cues[0].lines) == "A" && join(cues[1].lines) == "B", "cues split without blank line");
        }
    }
    // 3. Windows-1252 / Latin-1 (old French .srt): e-acute, oe, curly apostrophe.
    {
        const std::string srt = "1\n00:00:01,000 --> 00:00:02,000\nD\xE9j\xE0 l\x92\x9Cuvre\n";
        auto cues = dirk::parseSrt(srt);
        check(cues.size() == 1 && cues[0].lines[0] == "D\xC3\xA9j\xC3\xA0 l\xE2\x80\x99\xC5\x93uvre",
              "Windows-1252 converted to UTF-8");
    }
    // 4. UTF-16 LE with BOM.
    {
        const std::string ascii = "1\r\n00:00:03,000 --> 00:00:04,000\r\nCaf\xE9\r\n";
        std::string u16 = "\xFF\xFE";
        for (unsigned char c : ascii) { u16 += static_cast<char>(c); u16 += '\0'; }
        auto cues = dirk::parseSrt(u16);
        check(cues.size() == 1 && cues[0].lines[0] == "Caf\xC3\xA9" && near(cues[0].startSeconds, 3.0), "UTF-16 LE converted");
    }
    // 5. Broken blocks skipped, the rest kept; out-of-order cues sorted.
    {
        const std::string srt =
            "1\n00:00:10,000 --> 00:00:11,000\nLater\n\n"
            "2\nnot a timecode\nJunk\n\n"
            "3\n00:00:05,000 --> 00:00:04,000\nEnds before it starts\n\n"
            "4\n00:00:06,000 --> 00:00:07,000\n\n"
            "5\n00:99:00,000 --> 01:00:00,000\nBad minutes\n\n"
            "6\n00:00:02,000 --> 00:00:03,000\nEarlier\n";
        auto cues = dirk::parseSrt(srt);
        check(cues.size() == 2, "broken blocks skipped (bad timecode, end<start, empty text, 99 minutes)");
        if (cues.size() == 2) check(join(cues[0].lines) == "Earlier" && join(cues[1].lines) == "Later", "cues sorted by start time");
    }
    // 6. Lookup at the frame's time.
    {
        dirk::SubtitleTrack t;
        t.cues = dirk::parseSrt(
            "1\n00:00:01,000 --> 00:00:02,000\nOne\n\n"
            "2\n00:00:01,500 --> 00:00:03,000\nTwo\n\n"
            "3\n00:00:10,000 --> 00:00:11,000\nTen\n");
        check(dirk::activeSubtitleLines(t, 0.999).empty(), "nothing just before the first cue");
        check(join(dirk::activeSubtitleLines(t, 1.0)) == "One", "cue shows from its exact start");
        check(join(dirk::activeSubtitleLines(t, 0.99999)) == "One", "frame time a hair early (rounding) still counts");
        check(join(dirk::activeSubtitleLines(t, 1.6)) == "One|Two", "overlapping cues both shown, in order");
        check(join(dirk::activeSubtitleLines(t, 2.0)) == "Two", "end time is exclusive");
        check(dirk::activeSubtitleLines(t, 5.0).empty(), "gap between cues: nothing");
        check(join(dirk::activeSubtitleLines(t, 10.5)) == "Ten", "after a jump forward");
        check(join(dirk::activeSubtitleLines(t, 1.2)) == "One", "after a jump back");
        check(dirk::activeSubtitleLines(t, -1.0).empty(), "no frame yet (negative time): nothing");
    }
    // 7. Files next to the movie.
    {
        const fs::path dir = fs::temp_directory_path() / "dirk_subtitles_test";
        fs::remove_all(dir);
        fs::create_directories(dir);
        const std::string cue = "1\n00:00:01,000 --> 00:00:02,000\nX\n";
        writeFile(dir / "Timegal.mp4", "");
        writeFile(dir / "timegal.srt", cue);        // untagged, other case
        writeFile(dir / "Timegal.FR.srt", cue);     // tag, upper case
        writeFile(dir / "Timegal-en.srt", cue);      // "-" like Hypseus's <name>-fr.ogg
        writeFile(dir / "Timegal_it.srt", cue);      // "_" too
        writeFile(dir / "Timegal2.srt", cue);       // other movie
        writeFile(dir / "Timegal.fr.bak.srt", cue); // not a language tag
        writeFile(dir / "Timegal.de.srt", "no cue in here\n"); // empty -> left out
        writeFile(dir / "Timegal-fr.srt", cue);      // second French file -> "French (2)"
        writeFile(dir / "Timegal-pt-br.srt", cue);   // with a region
        writeFile(dir / "Timegal-xx.srt", cue);      // not ISO 639 -> "Unknown"
        fs::create_directories(dir / "Timegal.it.srt"); // a folder
        auto tracks = dirk::findSubtitleTracks((dir / "Timegal.mp4").string());
        std::vector<std::string> labels;
        for (auto& t : tracks) labels.push_back(t.label);
        check(join(labels) == "On|English|French|French (2)|Italian|Portuguese (BR)|Unknown",
              "tracks found: untagged 'On' first, then by tag with English names (-en, .FR and -fr, _it, -pt-br, -xx) (got " + join(labels) + ")");
        check(dirk::findSubtitleTracks((dir / "Other.mp4").string()).empty(), "movie without .srt: no track");
        fs::remove_all(dir);
    }

    std::printf("%s\n", failures == 0 ? "ALL OK" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
