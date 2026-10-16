#pragma once

#include <string>
#include <vector>

namespace dirk {

// Language names for the soundtrack and subtitle choices in Dirk Options
// (Cyril 2026-09-28: show real names, in English like the rest of the
// menu; an untagged file stays "Original"/"On"; an unknown tag shows
// "Unknown").
//
// A tag is what follows the movie name in a file name ("fr" in
// Timegal-fr.srt / timegal-fr.ogg) or the "language" of a stream inside an
// mp4/mkv:
//   - ISO 639-1, two letters: "fr" -> "French" (all 184 languages)
//   - ISO 639-2, three letters, both forms: "fre"/"fra" -> "French"
//   - with a region: "pt-br" / "pt_BR" -> "Portuguese (BR)"
//   - "jp" is accepted for Japanese (common mistake, not ISO)
// Case does not matter. Anything else -> "Unknown". Empty tag -> "".
std::string languageName(const std::string& tag);

// True for a 2- or 3-letter code of the table above (no region).
bool isKnownLanguageCode(const std::string& code);

// The ISO 639-2/B code Matroska expects ("fre" for "fr" or "fr-ca"), or
// nullptr for an unknown tag.
const char* iso639_2Code(const std::string& tag);

// Label for a soundtrack inside an mp4/mkv, from the stream's "title" and
// "language" fields:
//   1. a known language          -> its name ("fre" -> "French")
//   2. else a title that is a code ("FR", "pt-BR": what HypseusRemux
//      writes)                   -> its name, or "Unknown"
//   3. else a short title (<= 20) -> the title as written
//   4. else an unknown language   -> "Unknown" ("und" counts as none)
//   5. nothing                    -> "Original" for the first track,
//                                    "Unknown" for the others
std::string audioStreamLabel(const std::string& title, const std::string& language, bool firstTrack);

// Two tracks with the same label get " (2)", " (3)"... on the later ones,
// so every choice in the menu is distinct and the saved choice (by name)
// finds the right one.
void makeLabelsUnique(std::vector<std::string>& labels);

} // namespace dirk
