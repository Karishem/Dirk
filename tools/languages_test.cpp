// Unit test for core/Languages.* (2026-09-28): the names shown for the
// soundtrack and subtitle choices in Dirk Options.
#include "core/Languages.h"

#include <cstdio>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
static void name(const char* tag, const char* expected) {
    const std::string got = dirk::languageName(tag);
    check(got == expected, std::string("'") + tag + "' -> " + expected + (got == expected ? "" : " (got " + got + ")"));
}
static void stream(const char* title, const char* lang, bool first, const char* expected, const char* why) {
    const std::string got = dirk::audioStreamLabel(title, lang, first);
    check(got == expected, std::string(why) + ": " + expected + (got == expected ? "" : " (got " + got + ")"));
}

int main() {
    // ISO 639-1, any case; 639-2 both forms; regions; the "jp" exception.
    name("fr", "French"); name("FR", "French"); name("en", "English"); name("it", "Italian");
    name("de", "German"); name("es", "Spanish"); name("ja", "Japanese"); name("jp", "Japanese");
    name("zh", "Chinese"); name("el", "Greek"); name("nb", "Norwegian Bokmal"); name("vo", "Volapuk");
    name("fre", "French"); name("fra", "French"); name("ger", "German"); name("deu", "German"); name("jpn", "Japanese");
    name("pt-br", "Portuguese (BR)"); name("pt_BR", "Portuguese (BR)"); name("fr-ca", "French (CA)");
    name("xx", "Unknown"); name("q", "Unknown"); name("french", "Unknown"); name("und", "Unknown");
    name("fr-", "Unknown"); name("fr-toolong", "Unknown"); name("", "");
    // The whole table: 184 codes, every name short, ASCII, and made of letters/spaces.
    int count = 0; bool allClean = true;
    for (char a = 'a'; a <= 'z'; ++a) for (char b = 'a'; b <= 'z'; ++b) {
        const char code[3] = {a, b, 0};
        if (!dirk::isKnownLanguageCode(code) || std::strcmp(code, "jp") == 0) continue;
        ++count;
        const std::string n = dirk::languageName(code);
        if (n.empty() || n.size() > 18) allClean = false;
        for (unsigned char c : n) if (c > 127 || !(std::isalpha(c) || c == ' ' || c == '-')) allClean = false;
    }
    check(count == 184, "all 184 ISO 639-1 codes known (got " + std::to_string(count) + ")");
    check(allClean, "every name: ASCII letters, 18 characters at most");
    check(std::string(dirk::iso639_2Code("fr")) == "fre" && std::string(dirk::iso639_2Code("de-ch")) == "ger" &&
          std::string(dirk::iso639_2Code("ko")) == "kor" && dirk::iso639_2Code("xx") == nullptr,
          "Matroska language codes: fr -> fre, de-ch -> ger, ko -> kor, xx -> none");

    // Soundtracks inside an mp4/mkv.
    stream("", "", true, "Original", "first track, no metadata");
    stream("", "und", true, "Original", "first track, language 'und'");
    stream("", "", false, "Unknown", "other track, no metadata");
    stream("", "fre", false, "French", "language fre");
    stream("Commentary", "eng", false, "English", "known language wins over the title");
    stream("FR", "fre", false, "French", "HypseusRemux track (title FR, language fre)");
    stream("KO", "", false, "Korean", "cached .mkv from before (title only)");
    stream("XX", "", false, "Unknown", "HypseusRemux track with an unknown tag");
    stream("Director cut", "", false, "Director cut", "short free title kept");
    stream("A very long title that does not fit", "", false, "Unknown", "long title, no language");
    stream("", "tlh", false, "Unknown", "language not in ISO 639-1");

    std::vector<std::string> l = {"Original", "French", "French", "Unknown", "French", "Unknown"};
    dirk::makeLabelsUnique(l);
    std::string j; for (auto& s : l) j += s + "|";
    check(j == "Original|French|French (2)|Unknown|French (3)|Unknown (2)|", "duplicates numbered (got " + j + ")");
    std::vector<std::string> r = {"Portuguese (BR)", "Portuguese"};
    dirk::makeLabelsUnique(r);
    check(r[1] == "Portuguese", "'Portuguese' is not a duplicate of 'Portuguese (BR)'");

    std::printf("%s\n", failures == 0 ? "ALL OK" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
