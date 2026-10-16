#include "core/Subtitles.h"
#include "core/Languages.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>

namespace dirk {

namespace {

void appendUtf8(std::string& out, uint32_t cp) {
    if (cp < 0x80) {
        out += static_cast<char>(cp);
    } else if (cp < 0x800) {
        out += static_cast<char>(0xC0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out += static_cast<char>(0xE0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (cp & 0x3F));
    }
}

bool isValidUtf8(const std::string& s) {
    size_t i = 0;
    const size_t n = s.size();
    while (i < n) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = 0;
        if (c < 0x80) extra = 0;
        else if (c >= 0xC2 && c <= 0xDF) extra = 1;
        else if (c >= 0xE0 && c <= 0xEF) extra = 2;
        else if (c >= 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        for (int k = 1; k <= extra; ++k) {
            if (i + k >= n) return false;
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        }
        i += extra + 1;
    }
    return true;
}

// Windows-1252 bytes 0x80-0x9F (the rest of the high half is Latin-1 = the
// same code point). 0 = undefined in 1252, mapped to U+FFFD.
const uint16_t kCp1252High[32] = {
    0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
    0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
    0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
    0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
};

std::string cp1252ToUtf8(const std::string& s) {
    std::string out;
    out.reserve(s.size() + s.size() / 8);
    for (unsigned char c : s) {
        if (c < 0x80) out += static_cast<char>(c);
        else if (c < 0xA0) appendUtf8(out, kCp1252High[c - 0x80] ? kCp1252High[c - 0x80] : 0xFFFD);
        else appendUtf8(out, c);
    }
    return out;
}

std::string utf16ToUtf8(const std::string& s, bool bigEndian) {
    std::string out;
    for (size_t i = 0; i + 1 < s.size(); i += 2) {
        const auto b0 = static_cast<unsigned char>(s[i]);
        const auto b1 = static_cast<unsigned char>(s[i + 1]);
        uint32_t u = bigEndian ? (b0 << 8 | b1) : (b1 << 8 | b0);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < s.size()) {
            const auto c0 = static_cast<unsigned char>(s[i + 2]);
            const auto c1 = static_cast<unsigned char>(s[i + 3]);
            const uint32_t lo = bigEndian ? (c0 << 8 | c1) : (c1 << 8 | c0);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        appendUtf8(out, u);
    }
    return out;
}

std::string toUtf8(const std::string& bytes) {
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF) {
        return bytes.substr(3);
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFF && static_cast<unsigned char>(bytes[1]) == 0xFE) {
        return utf16ToUtf8(bytes.substr(2), false);
    }
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xFE && static_cast<unsigned char>(bytes[1]) == 0xFF) {
        return utf16ToUtf8(bytes.substr(2), true);
    }
    return isValidUtf8(bytes) ? bytes : cp1252ToUtf8(bytes);
}

std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    const size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

// "01:02:03,456" / "1:02:03.456" / "01:02:03" -> seconds; false if not a timecode.
bool parseTimecode(const std::string& t, double& seconds) {
    int h = 0, m = 0, s = 0, ms = 0;
    size_t i = 0;
    auto readInt = [&](int& v, int maxDigits) {
        const size_t start = i;
        v = 0;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i])) && static_cast<int>(i - start) < maxDigits) {
            v = v * 10 + (t[i] - '0');
            ++i;
        }
        return i > start;
    };
    if (!readInt(h, 3) || i >= t.size() || t[i] != ':') return false;
    ++i;
    if (!readInt(m, 2) || i >= t.size() || t[i] != ':') return false;
    ++i;
    if (!readInt(s, 2)) return false;
    if (i < t.size() && (t[i] == ',' || t[i] == '.')) {
        ++i;
        const size_t start = i;
        if (!readInt(ms, 3)) return false;
        const size_t digits = i - start;
        if (digits == 1) ms *= 100;
        else if (digits == 2) ms *= 10;
    }
    if (m > 59 || s > 59) return false;
    seconds = h * 3600.0 + m * 60.0 + s + ms / 1000.0;
    return true;
}

bool parseTimingLine(const std::string& line, double& start, double& end) {
    const size_t arrow = line.find("-->");
    if (arrow == std::string::npos) return false;
    const std::string left = trim(line.substr(0, arrow));
    std::string right = trim(line.substr(arrow + 3));
    const size_t sp = right.find_first_of(" \t"); // "X1:... Y1:..." position hints after the end time
    if (sp != std::string::npos) right = right.substr(0, sp);
    return parseTimecode(left, start) && parseTimecode(right, end);
}

// Removes <i>, </i>, <b>, <font color=...>, ... and {\an8}-style tags.
std::string stripTags(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '<') {
            const size_t close = s.find('>', i);
            const bool looksLikeTag = close != std::string::npos && close > i + 1 &&
                                      (std::isalpha(static_cast<unsigned char>(s[i + 1])) || s[i + 1] == '/');
            if (looksLikeTag) { i = close; continue; }
        } else if (c == '{' && i + 1 < s.size() && s[i + 1] == '\\') {
            const size_t close = s.find('}', i);
            if (close != std::string::npos) { i = close; continue; }
        }
        out += c;
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::vector<SubtitleCue> parseSrt(const std::string& bytes) {
    std::string text = toUtf8(bytes);
    // Normalize line endings to \n.
    std::string norm;
    norm.reserve(text.size());
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == '\r') {
            norm += '\n';
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            norm += text[i];
        }
    }

    std::vector<std::string> allLines;
    {
        std::istringstream in(norm);
        std::string l;
        while (std::getline(in, l)) allLines.push_back(l);
    }

    std::vector<SubtitleCue> cues;
    size_t i = 0;
    while (i < allLines.size()) {
        // Find the next timing line; everything before it that isn't a
        // timing line (index numbers, blank lines, junk) is skipped.
        double start = 0, end = 0;
        if (!parseTimingLine(allLines[i], start, end)) { ++i; continue; }
        ++i;
        SubtitleCue cue;
        cue.startSeconds = start;
        cue.endSeconds = end;
        while (i < allLines.size() && !trim(allLines[i]).empty()) {
            // A timing line right after (missing blank separator): new cue.
            double s2, e2;
            if (parseTimingLine(allLines[i], s2, e2)) break;
            // A bare index number followed by a timing line: also a new cue.
            const std::string t = trim(allLines[i]);
            const bool isIndex = !t.empty() && std::all_of(t.begin(), t.end(), [](unsigned char ch) { return std::isdigit(ch); });
            if (isIndex && i + 1 < allLines.size() && parseTimingLine(allLines[i + 1], s2, e2)) break;
            const std::string clean = trim(stripTags(allLines[i]));
            if (!clean.empty()) cue.lines.push_back(clean);
            ++i;
        }
        if (!cue.lines.empty() && cue.endSeconds > cue.startSeconds) cues.push_back(std::move(cue));
    }
    std::stable_sort(cues.begin(), cues.end(),
                     [](const SubtitleCue& a, const SubtitleCue& b) { return a.startSeconds < b.startSeconds; });
    return cues;
}

std::vector<SubtitleTrack> findSubtitleTracks(const std::string& moviePath) {
    namespace fs = std::filesystem;
    std::vector<SubtitleTrack> tracks;
    const fs::path movie(moviePath);
    const std::string stem = lower(movie.stem().string());
    fs::path dir = movie.parent_path();
    if (dir.empty()) dir = ".";
    std::error_code ec;
    if (stem.empty() || !fs::is_directory(dir, ec)) return tracks;

    struct Found { std::string tag; fs::path path; };
    std::vector<Found> found;
    for (fs::directory_iterator it(dir, ec), endIt; !ec && it != endIt; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string name = lower(it->path().filename().string());
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".srt") != 0) continue;
        const std::string base = name.substr(0, name.size() - 4); // without ".srt"
        if (base == stem) {
            found.push_back({"", it->path()});
        } else if (base.size() > stem.size() + 1 && base.compare(0, stem.size(), stem) == 0 &&
                   (base[stem.size()] == '-' || base[stem.size()] == '_' || base[stem.size()] == '.')) {
            // "-fr" like Hypseus's alternate soundtracks (<name>-fr.ogg,
            // Cyril 2026-09-27: same naming for everything); "_fr" and
            // ".fr" (the usual video-player convention) work too.
            const std::string tag = base.substr(stem.size() + 1);
            const bool okTag = tag.size() <= 10 && std::all_of(tag.begin(), tag.end(), [](unsigned char ch) {
                return std::isalnum(ch) || ch == '-' || ch == '_';
            });
            if (okTag) found.push_back({tag, it->path()});
        }
    }
    std::sort(found.begin(), found.end(), [](const Found& a, const Found& b) { return a.tag < b.tag; }); // "" (untagged) first

    for (const Found& f : found) {
        std::ifstream in(f.path, std::ios::binary);
        if (!in) continue;
        const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        SubtitleTrack t;
        t.path = f.path.string();
        if (f.tag.empty()) {
            t.label = "On";
        } else {
            t.label = languageName(f.tag); // "fr" -> "French", unknown -> "Unknown"
        }
        t.cues = parseSrt(bytes);
        if (!t.cues.empty()) tracks.push_back(std::move(t));
    }
    // Timegal-fr.srt and Timegal.fr.srt: "French", "French (2)".
    std::vector<std::string> labels;
    for (const SubtitleTrack& t : tracks) labels.push_back(t.label);
    makeLabelsUnique(labels);
    for (size_t i = 0; i < tracks.size(); ++i) tracks[i].label = labels[i];
    return tracks;
}

std::vector<std::string> activeSubtitleLines(const SubtitleTrack& track, double seconds) {
    std::vector<std::string> out;
    if (seconds < 0) return out;
    // Half a millisecond of slack: a frame timestamp computed from the
    // stream's time base can land a hair before a cue's rounded start.
    const double t = seconds + 0.0005;
    for (const SubtitleCue& c : track.cues) {
        if (c.startSeconds > t) break; // sorted by start
        if (t < c.endSeconds) out.insert(out.end(), c.lines.begin(), c.lines.end());
    }
    return out;
}

} // namespace dirk
