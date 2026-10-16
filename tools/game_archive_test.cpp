// Unit test for core/GameArchive.* (.zdirk games, 2026-09-28): zips built
// here with miniz, opened in place (nothing extracted), and the
// disk-over-zip rules checked on real files.
#include "core/GameArchive.h"
#include "third_party/miniz/miniz.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
static std::string slurp(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s; s << in.rdbuf(); return s.str();
}
static void spit(const fs::path& p, const std::string& s) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << s;
}
static bool makeZip(const fs::path& out, const std::vector<std::pair<std::string, std::string>>& files) {
    fs::remove(out);
    mz_zip_archive z{};
    if (!mz_zip_writer_init_file(&z, out.string().c_str(), 0)) return false;
    for (const auto& f : files) {
        if (!mz_zip_writer_add_mem(&z, f.first.c_str(), f.second.data(), f.second.size(), MZ_DEFAULT_COMPRESSION)) return false;
    }
    const bool ok = mz_zip_writer_finalize_archive(&z);
    mz_zip_writer_end(&z);
    return ok;
}
static bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
static std::string str(const std::vector<unsigned char>& v) { return std::string(v.begin(), v.end()); }
static size_t countFiles(const fs::path& dir) {
    size_t n = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_regular_file()) ++n;
    }
    return n;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "dirk_game_archive_test";
    fs::remove_all(root);
    const fs::path g = root / "games" / "Timegal";
    fs::create_directories(g);
    dirk::GameArchive& a = dirk::gameArchive();
    std::string err;

    check(dirk::hasZdirkExtension("x/Timegal.zdirk") && dirk::hasZdirkExtension("T.ZDIRK") && !dirk::hasZdirkExtension("T.zip"),
          ".zdirk extension, any case");

    // 1. A game folder zipped with its top folder (Explorer's "Compressed folder").
    const fs::path z = g / "Timegal.zdirk";
    check(makeZip(z, {{"Timegal/Timegal.dirk", "-- script"},
                      {"Timegal/Overlay/title.png", "PNG-FROM-ZIP"},
                      {"Timegal/Overlay/Lores/small.png", "LORES"},
                      {"Timegal/Custom/font.ttf", "FONT-FROM-ZIP"},
                      {"Timegal/Cfg/game.cfg", "factory game.cfg"},
                      {"Timegal/Cfg/hscore.cfg", "factory hscore"},
                      {"Timegal/Script/", ""},
                      {"../evil.txt", "escape"},
                      {"Timegal/../../evil2.txt", "escape"},
                      {"..\\evil3.txt", "escape"},
                      {"C:/abs.txt", "drive"},
                      {"__MACOSX/Timegal/._x", "junk"}}),
          "test zip built");
    spit(g / "Cfg" / "hscore.cfg", "PLAYER hscore");   // the player's own, must survive
    spit(g / "Custom" / "title.png", "PNG-CUSTOM");     // a player override
    const bool opened = a.open(z.string(), err);
    check(opened && a.active(), "open: " + err);
    check(fs::path(a.scriptPath()) == g / "Timegal.dirk" && !fs::exists(a.scriptPath()),
          "top folder stripped; the script is seen in the .zdirk's folder, read from the zip (" + a.scriptPath() + ")");
    check(fs::equivalent(fs::path(a.realFolder()).parent_path(), g), "game folder = the .zdirk's folder");
    check(a.fileCount() == 6, "6 files indexed, the 4 dangerous entries and __MACOSX left out (" + std::to_string(a.fileCount()) + ")");
    check(slurp(g / "Cfg" / "game.cfg") == "factory game.cfg" && a.cfgFilesCopied() == 1,
          "factory Cfg/game.cfg copied next to the .zdirk (only Cfg/)");
    check(slurp(g / "Cfg" / "hscore.cfg") == "PLAYER hscore", "the player's Cfg/hscore.cfg left alone");
    check(countFiles(g) == 4 && !fs::exists(g / "Overlay"), "nothing else written on disk, no copy of the game (" +
          std::to_string(countFiles(g)) + " files: the .zdirk, 2 in Cfg/, 1 in Custom/)");
    check(!fs::exists(root / "evil.txt") && !fs::exists(root / "evil2.txt") && !fs::exists(root / "games" / "evil3.txt"),
          "'..', '\\', drive entries never reach the disk");

    // 2. Reading: disk wins, the zip fills in.
    std::vector<unsigned char> bytes;
    check(a.archiveHasFile((g / "Overlay" / "title.png").string()) &&
          a.readArchiveFile((g / "Overlay" / "title.png").string(), bytes) && str(bytes) == "PNG-FROM-ZIP",
          "a zip-only file read into memory");
    check(!a.archiveHasFile((g / "Cfg" / "hscore.cfg").string()) && !a.readArchiveFile((g / "Cfg" / "hscore.cfg").string(), bytes),
          "a file on disk is not taken from the zip");
    check(a.archiveHasFile((g / "Custom" / "font.ttf").string()) && !a.archiveHasFile((g / "Custom" / "title.png").string()),
          "Custom/: the zip's file visible, the player's own wins where both exist");
    check(a.archiveHasFile(g.string() + "/Overlay//title.png"), "doubled separator handled");
    check(!a.archiveHasFile("/somewhere/else.txt") && !a.archiveHasFile((g / "nope.png").string()), "outside / missing: no");
    auto shared1 = a.sharedArchiveFile((g / "Custom" / "font.ttf").string());
    auto shared2 = a.sharedArchiveFile((g / "Custom" / "font.ttf").string());
    check(shared1 && shared1 == shared2 && str(*shared1) == "FONT-FROM-ZIP", "fonts/sounds: one shared copy kept in memory");

    // 3. exists / listing.
    bool isDir = false; uint64_t size = 0;
    check(a.exists((g / "Overlay").string(), &isDir) && isDir, "a zip-only folder exists, as a folder");
    check(a.exists((g / "Overlay" / "title.png").string(), &isDir, &size) && !isDir && size == 12, "a zip-only file, with its size");
    check(a.exists((g / "Script").string(), &isDir) && isDir, "an empty folder from the zip");
    check(!a.exists((g / "Nothing").string()), "missing everywhere");
    std::vector<std::string> names;
    check(a.listDirectory((g / "Cfg").string(), names) && has(names, "game.cfg") && has(names, "hscore.cfg") &&
          std::count(names.begin(), names.end(), "hscore.cfg") == 1, "Cfg/ listing: no duplicate");
    check(a.listDirectory((g / "Custom").string(), names) && has(names, "title.png") && has(names, "font.ttf"),
          "Custom/ listing: the player's and the zip's");
    check(a.listDirectory((g / "Overlay").string(), names) && has(names, "title.png") && has(names, "Lores"),
          "Overlay/ listing from the zip, with its subfolder");
    check(a.listDirectory(g.string(), names) && has(names, "Timegal.zdirk") && has(names, "Overlay") && has(names, "Timegal.dirk"),
          "the game's folder: disk and zip merged");
    check(!a.listDirectory((g / "Nothing").string(), names), "folder in neither: false");

    // 4. Lua io by name: a temporary copy only for a zip-only file.
    const std::string tmp = a.resolveRead((g / "Overlay" / "Lores" / "small.png").string());
    check(tmp != (g / "Overlay" / "Lores" / "small.png").string() && slurp(tmp) == "LORES", "zip-only file for io.open: temporary copy");
    check(a.resolveRead((g / "Cfg" / "hscore.cfg").string()) == (g / "Cfg" / "hscore.cfg").string(), "file on disk: its own path");
    check(a.resolveWrite((g / "Script" / "save.txt").string()) == (g / "Script" / "save.txt").string() &&
          fs::is_directory(g / "Script"), "writing into a zip folder: the folder is created on disk");

    // 5. Reopening after the player deleted a Cfg file; closing cleans up.
    fs::remove(g / "Cfg" / "game.cfg");
    check(a.open(z.string(), err) && slurp(g / "Cfg" / "game.cfg") == "factory game.cfg", "a deleted Cfg file comes back (factory setting)");
    const std::string tmp2 = a.resolveRead((g / "Overlay" / "title.png").string());
    a.close();
    check(!fs::exists(tmp2) && !a.active(), "close(): temporary copies removed, inactive");
    check(a.resolveRead("/x/y") == "/x/y" && !a.archiveHasFile((g / "Overlay" / "title.png").string()), "inactive: nothing from the zip");

    // 6. Script choice and errors.
    const fs::path other = root / "games" / "Pack";
    fs::create_directories(other);
    makeZip(other / "Hayate.zdirk", {{"HayatePSX.dirk", "a"}, {"Hayate.dirk", "b"}});
    check(a.open((other / "Hayate.zdirk").string(), err) && fs::path(a.scriptPath()).filename() == "Hayate.dirk",
          "two scripts: the one named like the .zdirk");
    makeZip(other / "Two.zdirk", {{"A.dirk", "a"}, {"B.dirk", "b"}});
    const bool twoOk = a.open((other / "Two.zdirk").string(), err);
    check(!twoOk && !a.active() && err.find("several") != std::string::npos, "two scripts, none matching: clear error (" + err + ")");
    makeZip(other / "Old.zdirk", {{"old.singe", "s"}, {"Overlay/x.png", "x"}});
    check(a.open((other / "Old.zdirk").string(), err) && fs::path(a.scriptPath()).filename() == "old.singe", "a lone .singe script is used");
    makeZip(other / "None.zdirk", {{"Overlay/x.png", "x"}});
    const bool noneOk = a.open((other / "None.zdirk").string(), err);
    check(!noneOk && err.find("no .dirk") != std::string::npos, "no script: clear error");
    spit(other / "Bad.zdirk", "this is not a zip");
    const bool badOk = a.open((other / "Bad.zdirk").string(), err);
    check(!badOk && err.find("not a readable zip") != std::string::npos, "not a zip: clear error");
    check(!a.open((other / "Missing.zdirk").string(), err), "missing file: error");

    // 7. Several games in one zip + a shortcut .dirk next to it (like -usealt).
    const fs::path h = root / "games" / "Hayate";
    fs::create_directories(h);
    makeZip(h / "Hayate.zdirk", {{"Hayate.dirk", "-- hayate"}, {"HayatePSX.dirk", "-- psx"}, {"Overlay/x.png", "x"}});
    spit(h / "HayatePSX.dirk", "-- Raccourci\nzdirk = \"Hayate.zdirk\"\n");
    std::string sz, ss;
    check(dirk::readZdirkShortcut((h / "HayatePSX.dirk").string(), sz, ss) &&
          fs::path(sz) == h / "Hayate.zdirk" && ss == "HayatePSX.dirk", "shortcut read: zip next to it, script = its own name");
    spit(h / "Other.dirk", "zdirk = 'Hayate.zdirk'\r\n  script = \"Hayate.dirk\"\r\n");
    check(dirk::readZdirkShortcut((h / "Other.dirk").string(), sz, ss) && ss == "Hayate.dirk", "shortcut with script =, single quotes, CRLF");
    spit(h / "Real.dirk", "MYDIR = \"Hayate\"\ndofile(\"x\")\n");
    check(!dirk::readZdirkShortcut((h / "Real.dirk").string(), sz, ss), "a real game script is not a shortcut");
    std::string big(5000, '-'); big += "\nzdirk = \"Hayate.zdirk\"\n";
    spit(h / "Big.dirk", big);
    check(!dirk::readZdirkShortcut((h / "Big.dirk").string(), sz, ss), "a big file is never taken for a shortcut");
    check(a.open((h / "Hayate.zdirk").string(), err, "HayatePSX.dirk", (h / "HayatePSX.dirk").string()) &&
          fs::path(a.scriptPath()) == h / "HayatePSX.dirk", "open via the shortcut: HayatePSX.dirk chosen");
    check(a.readArchiveFile((h / "HayatePSX.dirk").string(), bytes) && str(bytes) == "-- psx",
          "the shortcut on disk is skipped: the script comes from the zip");
    check(a.open((h / "Hayate.zdirk").string(), err) && fs::path(a.scriptPath()).filename() == "Hayate.dirk",
          "the .zdirk itself: Hayate.dirk (named like it)");
    const bool missOk = a.open((h / "Hayate.zdirk").string(), err, "Nope.dirk");
    check(!missOk && err.find("Nope.dirk") != std::string::npos && err.find("HayatePSX.dirk") != std::string::npos,
          "shortcut to a script the zip doesn't have: says which ones it has (" + err + ")");

#ifdef _WIN32
    check(a.open(z.string(), err) && a.archiveHasFile((g / "OVERLAY" / "TITLE.PNG").string()) &&
          a.archiveHasFile(g.string() + "\\Overlay/title.png"), "Windows: other letter case, mixed separators");
#endif

    a.close();
    fs::remove_all(root);
    std::printf("%s\n", failures == 0 ? "ALL OK" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
