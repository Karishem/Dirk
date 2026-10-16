#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dirk {

// .zdirk games (2026-09-28, Cyril): a game folder zipped into one file, the
// Dirk counterpart of Hypseus's zipped-Lua ROMs (-zlua). Double-clicking
// Timegal.zdirk launches it like Timegal.dirk.
//
//   Dirk Games/Timegal/Timegal.zdirk   <- the zip: Timegal.dirk, Overlay/,
//                                          Fonts/, Sounds/, Script/, Cfg/...
//   Dirk Games/Timegal/Movie/...       <- the movie stays OUTSIDE (it is
//                                          already compressed, and seeking
//                                          must stay instant)
//   Dirk Games/Timegal/Cfg/...         <- the game's writable files
//   Dirk Games/Timegal/Custom/...      <- player overrides, as before
//
// The zip is read in place, like Hypseus does: nothing is extracted to a
// hidden folder. For the game, its folder is the .zdirk's folder with the
// zip's content laid over it:
//   - a file on disk in that folder wins over the zip's (Custom/, Cfg/, or
//     any file the player puts there);
//   - otherwise the file is read straight from the zip, into memory
//     (scripts, images, fonts, sounds, bezels, controls.cfg);
//   - the game writes into that folder, as usual;
//   - a folder listing merges both.
// Like Hypseus's "ram" folder, the zip's own Cfg/ files (the game's
// writable files: settings, saves, scores) are copied next to the .zdirk
// on launch when missing -- never over the player's.
//
// The zip may hold the files at its root, or inside one top folder (what
// "Send to > Compressed folder" on a game folder gives) -- both work. The
// script run is <zdirk name>.dirk, else the only .dirk, else the same for
// .singe.
class GameArchive {
public:
    GameArchive();
    ~GameArchive();
    GameArchive(const GameArchive&) = delete;
    GameArchive& operator=(const GameArchive&) = delete;

    // Opens and indexes the zip, copies the missing Cfg/ files. On failure
    // returns false with a message for the player in `error`; stays inactive.
    // `wantedScript` (optional): run this script of the zip rather than the
    // default choice -- a zip holding several games (see
    // readZdirkShortcut()). `shortcutPath` (optional): the shortcut file on
    // disk that launched it; it is ignored as a game file, since it bears
    // the same name as the script it points to.
    bool open(const std::string& zdirkPath, std::string& error,
              const std::string& wantedScript = "", const std::string& shortcutPath = "");
    void close(); // back to inactive; also removes temporary copies (see resolveRead)

    bool active() const;
    const std::string& zdirkPath() const;
    const std::string& realFolder() const; // the .zdirk's folder, ends with a separator
    const std::string& scriptPath() const; // realFolder() + the script's name (read from the zip)
    size_t fileCount() const;              // files in the zip
    size_t cfgFilesCopied() const;         // Cfg/ files copied by the last open()

    // `path` (anywhere; only the .zdirk's folder is concerned) is a file
    // that exists only in the zip -- not on disk.
    bool archiveHasFile(const std::string& path) const;
    // Its bytes, read from the zip (false if archiveHasFile() is false).
    bool readArchiveFile(const std::string& path, std::vector<unsigned char>& out) const;
    // Same, kept in memory until close() and shared: for fonts and sounds,
    // which SDL keeps reading while they're in use. nullptr if not in the zip.
    std::shared_ptr<const std::vector<unsigned char>> sharedArchiveFile(const std::string& path) const;

    // File or folder, on disk or in the zip.
    bool exists(const std::string& path, bool* isDirectory = nullptr, uint64_t* size = nullptr) const;
    // Names in `path`, disk and zip merged, disk first, no duplicates
    // (case-insensitive on Windows). False if the folder is in neither.
    bool listDirectory(const std::string& path, std::vector<std::string>& names) const;

    // For code that can only open a file by name (Lua's io library): the
    // path itself when it is on disk or not the game's; for a zip-only file,
    // a temporary copy (removed at exit). The game's own reads never need
    // it in practice (its Cfg/ files are on disk).
    std::string resolveRead(const std::string& path) const;
    // Before writing `path`: creates its missing parent folders when they
    // exist in the zip (e.g. a game that writes into Script/). Returns path.
    std::string resolveWrite(const std::string& path) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// The one instance Dirk uses (inactive unless a .zdirk was launched).
GameArchive& gameArchive();

bool hasZdirkExtension(const std::string& path);

// A zip holding several games (like Hypseus's -usealt packs, e.g.
// Hayate.zdirk with Hayate.dirk and HayatePSX.dirk): each game other than
// the one named like the zip gets a tiny "shortcut" .dirk next to the zip,
// double-clicked like any .dirk:
//
//     -- HayatePSX.dirk, next to Hayate.zdirk
//     zdirk = "Hayate.zdirk"
//     script = "HayatePSX.dirk"      -- optional: this file's own name
//
// True if `dirkPath` is such a shortcut (a small file with a
// `zdirk = "..."` line); `zdirk` comes back as a full path (relative to
// the shortcut's folder), `script` as the script's name.
bool readZdirkShortcut(const std::string& dirkPath, std::string& zdirk, std::string& script);

} // namespace dirk
