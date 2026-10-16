// Dirk Downloader: a small standalone Windows companion tool that lists
// the games available on Cyril's shared Mega folder and downloads
// whichever ones the player checks, straight into the right folder next
// to dirk.exe.
//
// WHY A MANIFEST FILE, NOT LIVE MEGA FOLDER BROWSING:
// megatools (the command-line tool this wraps -- see README.md in this
// folder) is the only tool confirmed to accept a public Mega link with no
// login at all (`megatools dl`), and it downloads -- there is no
// documented, non-interactive way to just LIST a public folder's contents
// without pulling its data. The one option that looks related
// (`--choose-files`) is explicitly interactive in a terminal, with its
// exact protocol undocumented -- not something this program could drive
// reliably without live testing against a real Mega link, which wasn't
// possible while writing this (the sandbox this was built in blocks
// mega.nz outright). So instead: a tiny manifest text file, hosted as its
// own small Mega link (or anywhere else reachable the same way), lists
// each game's name and its OWN individual Mega link (each game's own
// subfolder inside the shared parent folder has its own "get link" in
// Mega's web UI, a few clicks). This program downloads that manifest
// first (small, fast, always current), builds the checklist from it, then
// downloads only the specific games checked -- no blind "download
// everything" and no undocumented interactive parsing.
//
// WHAT THIS PROGRAM DOES NOT DO: verify its own Mega network behavior end
// to end. Every megatools call here is exactly what megatools' own
// documentation says it does (see README.md for the exact citations), but
// actually confirming it against a real shared folder needs a real
// network connection this sandbox didn't have -- that verification still
// needs to happen on a real Windows machine.

#define WIN32_LEAN_AND_MEAN
// UNICODE/_UNICODE are also passed on the command line via -municode (see
// this folder's README) -- not redefined here to avoid the harmless but
// noisy "redefined" warning that produces.
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shellapi.h>
#include <string>
#include <vector>
#include <cstdio>
#include <sstream>
#include <functional>

// (No #pragma comment(lib, ...) here -- that's an MSVC-only mechanism;
// this is built with MinGW like the rest of dirk, so comctl32/shell32/
// ole32 are linked explicitly on the command line instead, see README.md.)

namespace {

// ---------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------

// The folder containing this running .exe -- same idea as dirk's own
// exeDirectory() in src/core/main.cpp (confirmed there that GetModuleFileName
// is the reliable way, not argv[0]), reused here for the same reason: the
// default download destination should be "next to me", not whatever the
// current working directory happens to be (which depends on how Explorer/
// a shortcut launched this .exe).
std::wstring exeDirectory() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return L".";
    std::wstring path(buf, len);
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : path.substr(0, slash);
}

std::wstring joinPath(const std::wstring& a, const std::wstring& b) {
    if (a.empty()) return b;
    wchar_t last = a[a.size() - 1];
    if (last == L'\\' || last == L'/') return a + b;
    return a + L"\\" + b;
}

bool directoryExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
}

// Picks a sensible default download destination -- the same "next to
// KimmyScript" spot dirk.exe itself looks for via findSharedEngineRoot()
// in src/core/main.cpp (confirmed by reading that function, not assumed).
// NOT a functional requirement any more, to be clear: findSharedEngineRoot()
// checks dirk.exe's own folder for KimmyScript/ FIRST and uses it
// regardless of where a game folder actually lives -- a completely
// different folder, even a different drive works fine, since dirk.exe
// always finds its own adjacent KimmyScript/. So a game downloaded
// anywhere at all would still launch correctly. This default is purely
// about convenience: keeping every game, downloaded or added by hand, in
// the one place the "Dirk folder.lnk" Start Menu shortcut already opens,
// rather than scattered around. This tool can ship two ways: directly
// beside dirk.exe (KimmyScript/ right next to it), or tidied away in its
// own "Dirk Downloader" subfolder one level below the real Dirk install
// (KimmyScript/ one level up instead) -- the installer uses the second
// layout, see installer/README.md. Rather than hardcoding "always go up
// one level" (which would silently pick the wrong folder if this tool is
// ever moved, copied standalone, or the layout changes again later), this
// actively looks for KimmyScript/ in both spots and only defaults to the
// parent folder when it's actually found there. Falls back to "next to
// me" if neither has it (e.g. running before any engine install exists
// yet) -- always a safe, inert default, never a folder that doesn't
// exist. Either way, "Browse..." lets the player redirect anywhere,
// including a different drive entirely, with no downside.
std::wstring defaultDestFolder(const std::wstring& exeDir) {
    if (directoryExists(joinPath(exeDir, L"KimmyScript"))) {
        return exeDir;
    }
    size_t slash = exeDir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        std::wstring parent = exeDir.substr(0, slash);
        if (directoryExists(joinPath(parent, L"KimmyScript"))) {
            return parent;
        }
    }
    return exeDir;
}

// Last destination folder the player picked with "Browse..." (2026-09-26,
// Cyril: "generally games are installed in the same place"). Stored in the
// current user's registry, under the same HKCU\Software\Dirk key the
// installer already uses (InstallDir) -- always writable without admin
// rights, unlike a file next to this .exe when it sits under Program
// Files, and removed by the uninstaller together with that key.
const wchar_t* const kSettingsKey = L"Software\\Dirk";
const wchar_t* const kLastDestValue = L"DownloaderLastDest";

// The remembered folder, or empty if there is none.
std::wstring loadLastDestFolder() {
    wchar_t buf[MAX_PATH] = {};
    DWORD size = sizeof(buf);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kLastDestValue, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) {
        return L"";
    }
    return buf;
}

bool saveLastDestFolder(const std::wstring& folder) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) {
        return false;
    }
    const LONG r = RegSetValueExW(key, kLastDestValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(folder.c_str()),
                                  static_cast<DWORD>((folder.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return r == ERROR_SUCCESS;
}

// Where downloads go when the tool opens: the folder remembered from last
// time if it still exists (an external drive may be unplugged, a folder
// deleted), otherwise defaultDestFolder(). `note` gets a line for the log
// when a remembered folder had to be skipped.
std::wstring initialDestFolder(const std::wstring& exeDir, std::wstring& note) {
    const std::wstring last = loadLastDestFolder();
    if (!last.empty() && directoryExists(last)) return last;
    if (!last.empty()) note = L"Last destination folder not found (" + last + L") -- using the default one.";
    return defaultDestFolder(exeDir);
}

// Trims ASCII whitespace (spaces, tabs, CR) from both ends -- manifest and
// config files are plain hand-edited text, likely to pick up trailing
// spaces or Windows line endings depending on the editor used.
std::wstring trim(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

// _wfopen (not std::ifstream, which MinGW's libstdc++ doesn't accept a
// wide std::wstring path for -- confirmed while compiling this) is the
// standard MSVCRT way to open a file by a wide path on Windows.
bool readTextFileUtf8(const std::wstring& path, std::string& outUtf8) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    outUtf8.clear();
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        outUtf8.append(buf, n);
    }
    fclose(f);
    return true;
}

std::wstring utf8ToWide(const std::string& s) {
    if (s.empty()) return L"";
    int needed = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (needed <= 0) return L"";
    std::wstring out(needed, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], needed);
    return out;
}

// A UTF-8 file may start with a BOM (very common out of Notepad on
// Windows) -- strip it before parsing, otherwise the BOM bytes would end
// up glued onto the first line's game name.
void stripUtf8Bom(std::string& s) {
    if (s.size() >= 3 &&
        (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF) {
        s.erase(0, 3);
    }
}

// One downloadable piece of a game (2026-09-28, Cyril: .zdirk games).
//   Folder  -- the old form: one link to the game's whole Mega folder,
//              downloaded as-is into the game's own folder.
//   Game    -- the .zdirk file (link to that FILE on Mega).
//   Movie   -- the movie (link to the Movie folder, or to one movie file).
//   Media   -- frontend media (box art, marquee, video...): optional, only
//              with "Also download frontend media"; lands in Media\ as-is.
enum class PartRole { Folder, Game, Movie, Media };

struct GamePart {
    PartRole role;
    std::wstring link;
};

struct GameEntry {
    std::wstring name;
    std::vector<GamePart> parts;
    std::wstring status;  // "Not installed", "Up to date"... (see gameStatus())
    bool checked = false; // unticked by default, on request (was true) -- player
                           // picks what to download rather than unchecking what
                           // they don't want
};

const wchar_t* roleKey(PartRole r) {
    switch (r) {
        case PartRole::Folder: return L"folder";
        case PartRole::Game: return L"game";
        case PartRole::Movie: return L"movie";
        case PartRole::Media: return L"media";
    }
    return L"";
}

// Parses the manifest (see this folder's README for the exact format):
//   Name=Link                          -- the old form, one folder link
//   Name=GameLink | MovieLink          -- a .zdirk game and its movie
//   Name=GameLink | MovieLink | MediaLink
// Blank lines and lines starting with '#' are skipped, so Cyril can
// annotate the manifest for himself. A line with no '=' is skipped rather
// than treated as a fatal parse error -- one malformed line shouldn't hide
// every other game in the list.
std::vector<GameEntry> parseManifest(const std::wstring& text) {
    std::vector<GameEntry> games;
    std::wistringstream stream(text);
    std::wstring line;
    while (std::getline(stream, line)) {
        std::wstring trimmed = trim(line);
        if (trimmed.empty() || trimmed[0] == L'#') continue;
        size_t eq = trimmed.find(L'=');
        if (eq == std::wstring::npos) continue;
        GameEntry g;
        g.name = trim(trimmed.substr(0, eq));
        std::vector<std::wstring> links;
        std::wstring rest = trimmed.substr(eq + 1);
        size_t start = 0;
        while (true) {
            const size_t bar = rest.find(L'|', start);
            const std::wstring piece = trim(rest.substr(start, bar == std::wstring::npos ? std::wstring::npos : bar - start));
            links.push_back(piece);
            if (bar == std::wstring::npos) break;
            start = bar + 1;
        }
        if (g.name.empty() || links.empty() || links[0].empty()) continue;
        if (links.size() == 1) {
            g.parts.push_back({PartRole::Folder, links[0]});
        } else {
            g.parts.push_back({PartRole::Game, links[0]});
            if (!links[1].empty()) g.parts.push_back({PartRole::Movie, links[1]});
            if (links.size() > 2 && !links[2].empty()) g.parts.push_back({PartRole::Media, links[2]});
        }
        games.push_back(g);
    }
    return games;
}

// Runs `exePath arguments`, waits for it to finish, and returns its exit
// code (-1 if it couldn't even be started). If `outputSink` is non-null,
// every line the child process writes to stdout+stderr (merged) is
// delivered to it as it arrives, decoded from the console's own active
// code page rather than assumed UTF-8 -- megatools is a console
// application, so its own text output follows the console codepage, not
// necessarily UTF-8.
//
// Progress (2026-09-28): with `watchFolder`, the heartbeat line says how
// much has arrived in that folder so far and at what speed ("... 420.3 MB
// received, 2.1 MB/s, 3:20"), measured on the disk itself rather than
// parsed from megatools' own output. Heartbeat lines start with
// kHeartbeatMark: the window replaces the previous one instead of adding
// a line each time. If nothing arrives for g_stallTimeoutMs, megatools is
// stopped and kStalled is returned.
constexpr wchar_t kHeartbeatMark = L'\x01';
constexpr int kStalled = -2;
DWORD g_stallTimeoutMs = 5 * 60 * 1000;

// Bytes in a folder and below. Each file's size is asked of the file itself
// (not read from the folder listing, which Windows updates lazily for a
// file still being written).
unsigned long long folderBytes(const std::wstring& dir) {
    unsigned long long total = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        const std::wstring path = dir + L"\\" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            total += folderBytes(path);
            continue;
        }
        unsigned long long size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        HANDLE f = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            LARGE_INTEGER li;
            if (GetFileSizeEx(f, &li)) size = (unsigned long long)li.QuadPart;
            CloseHandle(f);
        }
        total += size;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return total;
}

std::wstring megabytes(unsigned long long bytes) {
    wchar_t b[32];
    swprintf(b, 32, L"%.1f MB", bytes / 1048576.0);
    return b;
}

std::wstring minutesSeconds(DWORD ms) {
    wchar_t b[32];
    swprintf(b, 32, L"%lu:%02lu", (unsigned long)(ms / 60000), (unsigned long)((ms / 1000) % 60));
    return b;
}

int runProcessCapture(const std::wstring& exePath, const std::wstring& arguments,
                       const std::wstring& workingDir,
                       const std::function<void(const std::wstring&)>& outputSink,
                       const std::wstring& watchFolder = L"") {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE readPipe = nullptr, writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) return -1;
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = nullptr;

    PROCESS_INFORMATION pi{};
    std::wstring cmdLine = L"\"" + exePath + L"\" " + arguments;
    // CreateProcessW can rewrite its command-line buffer in place -- needs
    // a mutable buffer, not a temporary's read-only c_str().
    std::vector<wchar_t> cmdBuf(cmdLine.begin(), cmdLine.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE,
                              CREATE_NO_WINDOW, nullptr,
                              workingDir.empty() ? nullptr : workingDir.c_str(),
                              &si, &pi);
    CloseHandle(writePipe);
    if (!ok) {
        CloseHandle(readPipe);
        return -1;
    }

    // Read raw bytes and split into lines ourselves -- megatools' progress
    // output uses carriage returns (\r) to redraw a single line rather
    // than newlines, so a plain ReadFile+treat-as-lines loop would show
    // nothing until the whole process exits. Treat \r the same as \n here
    // so the UI gets a fresh status line each time the progress redraws,
    // not just at real newlines.
    //
    // Heartbeat: the per-game download runs megatools with --no-progress
    // (see startDownload's caller -- its own carriage-return redraw style
    // doesn't translate into this line-based log, and its exact text isn't
    // confirmed from a primary source to parse for a real percentage, see
    // README's "Possible follow-ups"). That means a single big game can
    // legitimately produce ZERO output for minutes while it downloads --
    // a plain blocking ReadFile loop looks IDENTICAL whether that's normal
    // or the process is actually hung, which is exactly what was reported
    // ("progress bar doesn't move, no sign of life"). Rather than guess at
    // megatools' undocumented progress format to fill the bar itself, poll
    // with PeekNamedPipe (anonymous CreatePipe handles don't support
    // overlapped/async ReadFile per MSDN, so peek-then-read is the correct
    // approach here, not a workaround) and print a plain elapsed-time
    // heartbeat line whenever nothing else has arrived for a while -- a
    // real, always-true signal ("still working, Ns elapsed") instead of a
    // fake percentage.
    constexpr DWORD kHeartbeatIntervalMs = 2000;
    DWORD lastActivity = GetTickCount();
    const DWORD startTime = lastActivity;
    // Progress on disk (watchFolder): what was there at the start doesn't count.
    const unsigned long long startBytes = watchFolder.empty() ? 0 : folderBytes(watchFolder);
    unsigned long long lastBytes = startBytes;
    DWORD lastGrowth = startTime, lastSample = startTime;
    bool stalled = false;

    std::string pending;
    char buf[4096];
    DWORD n = 0;
    for (;;) {
        DWORD avail = 0;
        if (!PeekNamedPipe(readPipe, nullptr, 0, nullptr, &avail, nullptr)) {
            break; // pipe broken -- process exited and closed its write end
        }
        if (avail == 0) {
            if (WaitForSingleObject(pi.hProcess, 0) == WAIT_OBJECT_0) {
                break; // process already gone, nothing left to read
            }
            DWORD now = GetTickCount();
            if (!watchFolder.empty() && now - lastSample >= kHeartbeatIntervalMs) {
                const unsigned long long bytes = folderBytes(watchFolder);
                const double seconds = (now - lastSample) / 1000.0;
                const double speed = bytes > lastBytes && seconds > 0 ? (bytes - lastBytes) / 1048576.0 / seconds : 0.0;
                if (bytes != lastBytes) lastGrowth = now;
                lastBytes = bytes;
                lastSample = now;
                lastActivity = now;
                if (outputSink) {
                    wchar_t sp[32];
                    swprintf(sp, 32, L"%.1f MB/s", speed);
                    std::wstring line = std::wstring(1, kHeartbeatMark) + L"... " +
                                        megabytes(bytes > startBytes ? bytes - startBytes : 0) + L" received, " + sp +
                                        L", " + minutesSeconds(now - startTime);
                    if (now - lastGrowth >= 30000) line += L" -- nothing new for " + minutesSeconds(now - lastGrowth);
                    outputSink(line);
                }
                if (now - lastGrowth >= g_stallTimeoutMs) {
                    stalled = true;
                    TerminateProcess(pi.hProcess, 1);
                    break;
                }
            } else if (watchFolder.empty() && now - lastActivity >= kHeartbeatIntervalMs) {
                lastActivity = now;
                if (outputSink) {
                    outputSink(std::wstring(1, kHeartbeatMark) + L"... still working (" +
                               std::to_wstring((now - startTime) / 1000) + L"s) ...");
                }
            }
            Sleep(200);
            continue;
        }
        if (!ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) || n == 0) break;
        lastActivity = GetTickCount();
        pending.append(buf, n);
        size_t pos;
        while ((pos = pending.find_first_of("\r\n")) != std::string::npos) {
            std::string lineUtf = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (lineUtf.empty()) continue;
            if (outputSink) {
                int wlen = MultiByteToWideChar(CP_OEMCP, 0, lineUtf.c_str(), (int)lineUtf.size(), nullptr, 0);
                std::wstring wline(wlen > 0 ? wlen : 0, L'\0');
                if (wlen > 0) MultiByteToWideChar(CP_OEMCP, 0, lineUtf.c_str(), (int)lineUtf.size(), &wline[0], wlen);
                outputSink(wline);
            }
        }
    }
    CloseHandle(readPipe);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return stalled ? kStalled : (int)exitCode;
}

// ---------------------------------------------------------------------
// Installed games: what's there, what changed (2026-09-28)
// ---------------------------------------------------------------------

bool fileExists(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && !(attrs & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring lowerCase(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool endsWithNoCase(const std::wstring& s, const std::wstring& suffix) {
    return s.size() >= suffix.size() && lowerCase(s.substr(s.size() - suffix.size())) == lowerCase(suffix);
}

// Names in a folder (no "." / ".."); `dirsOnly`/`filesOnly` filter.
std::vector<std::wstring> listFolder(const std::wstring& dir, bool* anyDir = nullptr) {
    std::vector<std::wstring> names;
    if (anyDir) *anyDir = false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(joinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        names.push_back(fd.cFileName);
        if (anyDir && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) *anyDir = true;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return names;
}

void deleteTree(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) return;
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        for (const auto& n : listFolder(path)) deleteTree(joinPath(path, n));
        RemoveDirectoryW(path.c_str());
    } else {
        SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(path.c_str());
    }
}

// Moves everything inside `src` into `dst` (created if needed), replacing
// same-name files, keeping the rest of `dst` (e.g. the player's own
// subtitles or Dirk's .dirkindex next to a movie).
bool moveInto(const std::wstring& src, const std::wstring& dst) {
    CreateDirectoryW(dst.c_str(), nullptr);
    bool ok = true;
    for (const auto& n : listFolder(src)) {
        const std::wstring from = joinPath(src, n), to = joinPath(dst, n);
        if (directoryExists(from)) {
            ok = moveInto(from, to) && ok;
            RemoveDirectoryW(from.c_str());
        } else if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
            ok = false;
        }
    }
    return ok;
}

bool folderHasFiles(const std::wstring& dir) {
    return directoryExists(dir) && !listFolder(dir).empty();
}

// A downloaded .zdirk is a whole zip: it starts with a zip entry and ends
// with the zip's directory (its "end of central directory" record, within
// the last 64 KB + 22 bytes). A download cut short loses that end.
bool looksLikeCompleteZip(const std::wstring& path) {
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    unsigned char head[4] = {};
    const bool startOk = fread(head, 1, 4, f) == 4 && head[0] == 'P' && head[1] == 'K' && head[2] == 3 && head[3] == 4;
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    const long long tailLen = size < 65557 ? size : 65557;
    std::vector<unsigned char> tail((size_t)tailLen);
    _fseeki64(f, size - tailLen, SEEK_SET);
    const bool read = fread(tail.data(), 1, tail.size(), f) == tail.size();
    fclose(f);
    if (!startOk || !read || tail.size() < 22) return false;
    for (size_t i = tail.size() - 22 + 1; i-- > 0;) {
        if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) return true;
    }
    return false;
}

// The names inside a zip, read from its directory at the end of the file
// (nothing is extracted). Folders end with '/'. False if it isn't a
// readable zip. Handles zip64 (a zip over 4 GB or 65535 files).
bool zipEntryNames(const std::wstring& path, std::vector<std::wstring>& names) {
    names.clear();
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return false;
    auto u16 = [](const unsigned char* p) { return (unsigned)p[0] | ((unsigned)p[1] << 8); };
    auto u32 = [](const unsigned char* p) { return (unsigned long long)p[0] | ((unsigned long long)p[1] << 8) |
                                                   ((unsigned long long)p[2] << 16) | ((unsigned long long)p[3] << 24); };
    auto u64 = [&](const unsigned char* p) { return u32(p) | (u32(p + 4) << 32); };
    auto readAt = [&](long long pos, std::vector<unsigned char>& buf) {
        return pos >= 0 && _fseeki64(f, pos, SEEK_SET) == 0 && fread(buf.data(), 1, buf.size(), f) == buf.size();
    };
    _fseeki64(f, 0, SEEK_END);
    const long long size = _ftelli64(f);
    const long long tailLen = size < 65557 ? size : 65557;
    std::vector<unsigned char> tail((size_t)(tailLen > 0 ? tailLen : 0));
    bool ok = tail.size() >= 22 && readAt(size - tailLen, tail);
    long long eocd = -1;
    for (size_t i = ok ? tail.size() - 22 + 1 : 0; i-- > 0;) {
        if (tail[i] == 'P' && tail[i + 1] == 'K' && tail[i + 2] == 5 && tail[i + 3] == 6) { eocd = (long long)i; break; }
    }
    if (eocd < 0) { fclose(f); return false; }
    unsigned long long count = u16(&tail[eocd + 10]), cdSize = u32(&tail[eocd + 12]), cdOffset = u32(&tail[eocd + 16]);
    if (count == 0xFFFF || cdSize == 0xFFFFFFFFull || cdOffset == 0xFFFFFFFFull) { // zip64: its own end record
        const long long locator = eocd - 20;
        if (locator < 0 || tail[locator] != 'P' || tail[locator + 1] != 'K' || tail[locator + 2] != 6 || tail[locator + 3] != 7) {
            fclose(f); return false;
        }
        std::vector<unsigned char> rec64(56);
        if (!readAt((long long)u64(&tail[locator + 8]), rec64) || rec64[0] != 'P' || rec64[1] != 'K' || rec64[2] != 6 || rec64[3] != 6) {
            fclose(f); return false;
        }
        count = u64(&rec64[32]); cdSize = u64(&rec64[40]); cdOffset = u64(&rec64[48]);
    }
    if (cdSize > 256ull * 1024 * 1024 || (long long)(cdOffset + cdSize) > size) { fclose(f); return false; }
    std::vector<unsigned char> cd((size_t)cdSize);
    ok = readAt((long long)cdOffset, cd);
    fclose(f);
    if (!ok) return false;
    size_t p = 0;
    for (unsigned long long n = 0; n < count; ++n) {
        if (p + 46 > cd.size() || cd[p] != 'P' || cd[p + 1] != 'K' || cd[p + 2] != 1 || cd[p + 3] != 2) return false;
        const unsigned flags = u16(&cd[p + 8]), nameLen = u16(&cd[p + 28]), extraLen = u16(&cd[p + 30]), commentLen = u16(&cd[p + 32]);
        if (p + 46 + nameLen > cd.size()) return false;
        const std::string raw(reinterpret_cast<const char*>(&cd[p + 46]), nameLen);
        const UINT codePage = (flags & 0x800) ? CP_UTF8 : CP_OEMCP; // bit 11: UTF-8 names
        const int wn = MultiByteToWideChar(codePage, 0, raw.data(), (int)raw.size(), nullptr, 0);
        std::wstring name(wn > 0 ? wn : 0, L'\0');
        if (wn > 0) MultiByteToWideChar(codePage, 0, raw.data(), (int)raw.size(), &name[0], wn);
        for (auto& c : name) if (c == L'\\') c = L'/';
        names.push_back(name);
        p += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

// What was downloaded for a game, kept next to it in a small hidden file
// (".dirk-download": one "role=link" per line, plus "gamefile=<.zdirk
// name>"). Mega gives a replaced file a new link, and Cyril puts that new
// link in the manifest: a link that differs from the recorded one means
// "this part was updated".
const wchar_t* const kRecordFile = L".dirk-download";

std::vector<std::pair<std::wstring, std::wstring>> readRecord(const std::wstring& gameDest) {
    std::vector<std::pair<std::wstring, std::wstring>> out;
    std::string raw;
    if (!readTextFileUtf8(joinPath(gameDest, kRecordFile), raw)) return out;
    std::wistringstream stream(utf8ToWide(raw));
    std::wstring line;
    while (std::getline(stream, line)) {
        line = trim(line);
        const size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) continue;
        out.push_back({line.substr(0, eq), line.substr(eq + 1)});
    }
    return out;
}

std::wstring recordValue(const std::vector<std::pair<std::wstring, std::wstring>>& rec, const std::wstring& key) {
    for (const auto& kv : rec) if (kv.first == key) return kv.second;
    return L"";
}

void setRecordValue(std::vector<std::pair<std::wstring, std::wstring>>& rec, const std::wstring& key, const std::wstring& value) {
    for (auto& kv : rec) if (kv.first == key) { kv.second = value; return; }
    rec.push_back({key, value});
}

void writeRecord(const std::wstring& gameDest, const std::vector<std::pair<std::wstring, std::wstring>>& rec) {
    const std::wstring path = joinPath(gameDest, kRecordFile);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
    std::wstring text;
    for (const auto& kv : rec) text += kv.first + L"=" + kv.second + L"\r\n";
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string utf8(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), &utf8[0], n, nullptr, nullptr);
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    fwrite(utf8.data(), 1, utf8.size(), f);
    fclose(f);
    SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_HIDDEN);
}

// The .zdirk in a game's folder: the recorded one if still there, else any.
std::wstring installedZdirk(const std::wstring& gameDest, const std::vector<std::pair<std::wstring, std::wstring>>& rec) {
    const std::wstring recorded = recordValue(rec, L"gamefile");
    if (!recorded.empty() && fileExists(joinPath(gameDest, recorded))) return recorded;
    for (const auto& n : listFolder(gameDest)) {
        if (endsWithNoCase(n, L".zdirk") && fileExists(joinPath(gameDest, n))) return n;
    }
    return L"";
}

bool movieInstalled(const std::wstring& gameDest) {
    return folderHasFiles(joinPath(gameDest, L"Movie")) || folderHasFiles(joinPath(gameDest, L"Video"));
}

using LogFn = std::function<void(const std::wstring&)>;

// Games with several versions in one .zdirk (2026-09-28, Cyril): e.g.
// Hayate.zdirk holding Hayate.dirk and HayatePSX.dirk. Dirk runs the script
// named like the .zdirk; each other one is reached through a tiny shortcut
// .dirk next to the .zdirk (see readZdirkShortcut() in
// src/core/GameArchive.h). The tool writes those shortcuts itself, from the
// zip's own contents -- nothing extra to put on Mega or in the manifest.
//
// The .dirk scripts at the top of the zip (or of its one top folder -- the
// same rule as Dirk's GameArchive::open()) that need a shortcut: all but the
// one Dirk runs on its own. Only file names, e.g. "HayatePSX.dirk".
std::vector<std::wstring> zdirkShortcutScripts(const std::wstring& zdirkPath) {
    std::vector<std::wstring> entries, scripts, out;
    if (!zipEntryNames(zdirkPath, entries)) return out;
    // Clean names ("./a", "a//b"), minus Mac clutter -- as Dirk does.
    std::vector<std::pair<std::wstring, bool>> clean; // name, is a folder
    for (const auto& e : entries) {
        std::wstring name, comp;
        bool skip = false;
        std::wistringstream ss(e);
        while (std::getline(ss, comp, L'/')) {
            if (comp.empty() || comp == L".") continue;
            if (comp == L".." || comp == L"__MACOSX" || comp == L".DS_Store" || comp.find(L':') != std::wstring::npos) { skip = true; break; }
            name += (name.empty() ? L"" : L"/") + comp;
        }
        if (!skip && !name.empty()) clean.push_back({name, !e.empty() && e.back() == L'/'});
    }
    std::wstring prefix, first;
    bool single = !clean.empty();
    for (const auto& c : clean) {
        const size_t slash = c.first.find(L'/');
        const std::wstring top = slash == std::wstring::npos ? c.first : c.first.substr(0, slash);
        if (slash == std::wstring::npos && !c.second) { single = false; break; }
        if (first.empty()) first = top;
        else if (lowerCase(first) != lowerCase(top)) { single = false; break; }
    }
    if (single && !first.empty()) prefix = first + L"/";
    for (const auto& c : clean) {
        if (c.second || c.first.size() <= prefix.size()) continue;
        const std::wstring name = c.first.substr(prefix.size());
        if (name.find(L'/') == std::wstring::npos && endsWithNoCase(name, L".dirk")) scripts.push_back(name);
    }
    std::wstring stem = zdirkPath.substr(zdirkPath.find_last_of(L"\\/") + 1);
    stem = stem.substr(0, stem.size() - 6); // ".zdirk"
    bool hasMain = false;
    for (const auto& s : scripts) hasMain = hasMain || lowerCase(s) == lowerCase(stem + L".dirk");
    if (!hasMain && scripts.size() == 1) return out; // Dirk runs that only one
    for (const auto& s : scripts) {
        if (lowerCase(s) != lowerCase(stem + L".dirk")) out.push_back(s);
    }
    return out;
}

// A shortcut .dirk (small, with a `zdirk = "..."` line), not a real game
// script the player put there -- which is never overwritten or deleted.
bool isZdirkShortcut(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) ||
        (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || info.nFileSizeHigh != 0 || info.nFileSizeLow > 4096) {
        return false;
    }
    std::string text;
    if (!readTextFileUtf8(path, text)) return false;
    std::istringstream ss(text);
    std::string line;
    while (std::getline(ss, line)) {
        size_t i = line.find_first_not_of(" \t");
        if (i == std::string::npos || line.compare(i, 5, "zdirk") != 0) continue;
        i = line.find_first_not_of(" \t", i + 5);
        if (i != std::string::npos && line[i] == '=') return true;
    }
    return false;
}

std::string wideToUtf8(const std::wstring& s) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

std::vector<std::wstring> splitBar(const std::wstring& s) {
    std::vector<std::wstring> out;
    std::wistringstream ss(s);
    std::wstring item;
    while (std::getline(ss, item, L'|')) if (!item.empty()) out.push_back(item);
    return out;
}

// A shortcut the game needs is absent (e.g. installed by an older version
// of this tool): Status "Shortcut missing".
bool shortcutMissing(const std::wstring& gameDest, const std::wstring& zdirk) {
    for (const auto& s : zdirkShortcutScripts(joinPath(gameDest, zdirk))) {
        if (!fileExists(joinPath(gameDest, s))) return true;
    }
    return false;
}

// Writes the shortcuts `zdirk` needs next to it, removes the ones this tool
// wrote earlier that the game no longer has (recorded as "shortcuts=").
void updateShortcuts(const std::wstring& gameDest, const std::wstring& zdirk,
                     std::vector<std::pair<std::wstring, std::wstring>>& rec, const LogFn& log) {
    const auto wanted = zdirkShortcutScripts(joinPath(gameDest, zdirk));
    std::wstring written;
    for (const auto& s : wanted) {
        const std::wstring target = joinPath(gameDest, s);
        if (fileExists(target) && !isZdirkShortcut(target)) {
            log(L"Kept your own " + s + L" (not a shortcut).");
            continue;
        }
        const std::wstring stem = s.substr(0, s.size() - 5);
        const std::string text = "-- " + wideToUtf8(stem) + ": a game inside " + wideToUtf8(zdirk) +
                                 " (shortcut written by Dirk Downloader)\r\n"
                                 "zdirk = \"" + wideToUtf8(zdirk) + "\"\r\n"
                                 "script = \"" + wideToUtf8(s) + "\"\r\n";
        std::string current;
        const bool same = readTextFileUtf8(target, current) && current == text;
        if (!same) {
            FILE* f = _wfopen(target.c_str(), L"wb");
            if (!f) { log(L"Could not write the shortcut " + s + L"."); continue; }
            fwrite(text.data(), 1, text.size(), f);
            fclose(f);
            log(L"shortcut: " + s);
        }
        written += (written.empty() ? L"" : L"|") + s;
    }
    for (const auto& old : splitBar(recordValue(rec, L"shortcuts"))) {
        bool still = false;
        for (const auto& s : wanted) still = still || lowerCase(s) == lowerCase(old);
        const std::wstring target = joinPath(gameDest, old);
        if (!still && isZdirkShortcut(target)) {
            DeleteFileW(target.c_str());
            log(L"shortcut removed: " + old + L" (no longer in the game)");
        }
    }
    setRecordValue(rec, L"shortcuts", written);
}

const GamePart* findPart(const GameEntry& g, PartRole r) {
    for (const auto& p : g.parts) if (p.role == r) return &p;
    return nullptr;
}

// The Status column.
std::wstring gameStatus(const GameEntry& g, const std::wstring& destFolder) {
    const std::wstring gameDest = joinPath(destFolder, g.name);
    if (!directoryExists(gameDest)) return L"Not installed";
    const auto rec = readRecord(gameDest);
    if (const GamePart* folder = findPart(g, PartRole::Folder)) {
        const std::wstring was = recordValue(rec, L"folder");
        return (!was.empty() && was != folder->link) ? L"Update available" : L"Installed";
    }
    const std::wstring zdirk = installedZdirk(gameDest, rec);
    if (zdirk.empty()) return L"Not installed";
    const GamePart* game = findPart(g, PartRole::Game);
    const std::wstring wasGame = recordValue(rec, L"game");
    if (game && !wasGame.empty() && wasGame != game->link) return L"Update available";
    if (const GamePart* movie = findPart(g, PartRole::Movie)) {
        if (!movieInstalled(gameDest)) return L"Movie missing";
        const std::wstring wasMovie = recordValue(rec, L"movie");
        if (!wasMovie.empty() && wasMovie != movie->link) return L"Movie update available";
    }
    if (const GamePart* media = findPart(g, PartRole::Media)) {
        const std::wstring wasMedia = recordValue(rec, L"media");
        if (!wasMedia.empty() && wasMedia != media->link) return L"Media update available";
    }
    if (shortcutMissing(gameDest, zdirk)) return L"Shortcut missing";
    return L"Up to date";
}

// Downloads one part through a temporary folder inside the game's folder,
// then puts it in place. The game's Cfg\ (settings, saves, scores) is never
// touched. `fileName` gets the .zdirk's name for a Game part.
bool fetchPart(const std::wstring& megatools, const std::wstring& workDir, const std::wstring& gameDest,
               const GamePart& part, const LogFn& log, std::wstring& fileName) {
    const std::wstring staging = joinPath(gameDest, L".dirk-download-tmp");
    deleteTree(staging);
    CreateDirectoryW(staging.c_str(), nullptr);
    SetFileAttributesW(staging.c_str(), FILE_ATTRIBUTE_HIDDEN);
    const int rc = runProcessCapture(megatools, L"dl --no-progress --path \"" + staging + L"\" \"" + part.link + L"\"",
                                     workDir, log, staging);
    if (rc == kStalled) {
        log(L"Stopped: nothing received for " + minutesSeconds(g_stallTimeoutMs) +
            L". Mega may be limiting downloads right now -- try again later (what is already installed is kept).");
        deleteTree(staging);
        return false;
    }
    if (rc != 0) {
        log(L"megatools failed (code " + std::to_wstring(rc) + L").");
        deleteTree(staging);
        return false;
    }
    // A folder link may arrive as that folder itself: look inside it.
    std::wstring payload = staging, wrapperName;
    bool anyDir = false;
    auto items = listFolder(payload, &anyDir);
    while (items.size() == 1 && directoryExists(joinPath(payload, items[0]))) {
        wrapperName = items[0];
        payload = joinPath(payload, items[0]);
        items = listFolder(payload, &anyDir);
    }
    if (items.empty()) {
        log(L"Nothing was downloaded -- check this link in the catalogue.");
        deleteTree(staging);
        return false;
    }
    bool ok = true;
    if (part.role == PartRole::Game) {
        std::wstring zdirk;
        for (const auto& n : items) if (endsWithNoCase(n, L".zdirk") && fileExists(joinPath(payload, n))) zdirk = n;
        if (zdirk.empty() || items.size() != 1) {
            log(L"The game link should point to the game's .zdirk FILE (\"Get link\" on that file).");
            ok = false;
        } else if (!looksLikeCompleteZip(joinPath(payload, zdirk))) {
            log(L"The downloaded " + zdirk + L" is incomplete or damaged -- try again.");
            ok = false;
        } else {
            ok = MoveFileExW(joinPath(payload, zdirk).c_str(), joinPath(gameDest, zdirk).c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
            if (!ok) log(L"Could not put " + zdirk + L" in place (is the game running?).");
            fileName = zdirk;
        }
    } else if (part.role == PartRole::Movie) {
        const std::wstring target = lowerCase(wrapperName) == L"video" ? L"Video" : L"Movie";
        ok = moveInto(payload, joinPath(gameDest, target));
        if (!ok) log(L"Could not put the movie in place (is the game running?).");
    } else {
        ok = moveInto(payload, joinPath(gameDest, L"Media"));
        if (!ok) log(L"Could not put the media in place.");
    }
    deleteTree(staging);
    return ok;
}

// Downloads what a checked game needs: everything the first time; then
// only the parts whose link changed in the catalogue (or that are missing).
bool downloadGame(const std::wstring& megatools, const std::wstring& workDir, const std::wstring& destFolder,
                  const GameEntry& g, bool wantMedia, const LogFn& log) {
    const std::wstring gameDest = joinPath(destFolder, g.name);
    CreateDirectoryW(gameDest.c_str(), nullptr); // fine if it already exists
    auto rec = readRecord(gameDest);

    if (const GamePart* folder = findPart(g, PartRole::Folder)) { // the old one-link form: as before
        const int rc = runProcessCapture(megatools, L"dl --no-progress --path \"" + gameDest + L"\" \"" + folder->link + L"\"",
                                         workDir, log, gameDest);
        if (rc == kStalled) {
            log(L"Stopped: nothing received for " + minutesSeconds(g_stallTimeoutMs) +
                L". Mega may be limiting downloads right now -- try again later.");
            return false;
        }
        if (rc != 0) {
            log(L"megatools failed (code " + std::to_wstring(rc) + L").");
            return false;
        }
        setRecordValue(rec, L"folder", folder->link);
        writeRecord(gameDest, rec);
        return true;
    }

    bool ok = true;
    for (const auto& part : g.parts) {
        const std::wstring key = roleKey(part.role);
        const std::wstring was = recordValue(rec, key);
        bool present = false;
        if (part.role == PartRole::Game) present = !installedZdirk(gameDest, rec).empty();
        if (part.role == PartRole::Movie) present = movieInstalled(gameDest);
        if (part.role == PartRole::Media) {
            if (!wantMedia) continue;
            present = folderHasFiles(joinPath(gameDest, L"Media"));
        }
        // A part already there with no record (a movie copied by hand, a
        // game installed before this version): kept, recorded as current
        // -- except the .zdirk, small enough to always refresh.
        const bool needed = !present || (!was.empty() && was != part.link) || (part.role == PartRole::Game && was.empty());
        if (!needed) {
            if (was.empty()) setRecordValue(rec, key, part.link);
            log(key + L": up to date");
            continue;
        }
        log(key + L": downloading" + (present ? L" (update)" : L""));
        std::wstring fileName;
        if (!fetchPart(megatools, workDir, gameDest, part, log, fileName)) {
            ok = false;
            continue;
        }
        if (part.role == PartRole::Game) {
            const std::wstring old = recordValue(rec, L"gamefile");
            if (!old.empty() && lowerCase(old) != lowerCase(fileName)) {
                DeleteFileW(joinPath(gameDest, old).c_str()); // renamed on Mega: don't leave two games
            }
            setRecordValue(rec, L"gamefile", fileName);
        }
        setRecordValue(rec, key, part.link);
    }
    // The shortcuts for the other versions in the .zdirk (e.g. HayatePSX.dirk
    // next to Hayate.zdirk): written from the zip itself, checked every time
    // -- so a game installed before this existed gets them too.
    const std::wstring zdirk = installedZdirk(gameDest, rec);
    if (!zdirk.empty()) updateShortcuts(gameDest, zdirk, rec, log);
    writeRecord(gameDest, rec);
    return ok;
}

} // namespace

// ---------------------------------------------------------------------
// Application state and window procedure
// ---------------------------------------------------------------------

namespace {

constexpr int ID_LISTVIEW = 1001;
constexpr int ID_BROWSE = 1002;
constexpr int ID_DOWNLOAD = 1003;
constexpr int ID_DEST_EDIT = 1004;
constexpr int ID_LOG_EDIT = 1005;
constexpr int ID_SELECT_ALL = 1006;
constexpr int ID_MEDIA = 1007;

// Posted from the worker thread to the UI thread. wParam/lParam carry a
// heap-allocated std::wstring* the UI thread takes ownership of (deletes
// after use) -- PostMessage can't safely carry a std::wstring by value,
// and these updates are infrequent enough that this is simpler than a
// producer/consumer queue with its own locking.
constexpr UINT WM_APP_LOG = WM_APP + 1;
constexpr UINT WM_APP_DONE = WM_APP + 2;
constexpr UINT WM_APP_MANIFEST_READY = WM_APP + 3;
constexpr UINT WM_APP_MANIFEST_FAILED = WM_APP + 4;
// wParam = games completed so far, lParam = total games in this run -- both
// plain integers, so no heap allocation needed for this one (unlike the
// others above, which carry a std::wstring*). Whole-download progress only
// (X of N games), not a byte-level percentage within one game's own
// download: that would mean parsing megatools' own progress-line format,
// which isn't confirmed from primary sources (see README's own note on
// this) -- this way stays accurate using only counts this program already
// knows for certain.
constexpr UINT WM_APP_PROGRESS = WM_APP + 5;

struct AppState {
    HWND hwnd = nullptr;
    HWND listView = nullptr;
    HWND destEdit = nullptr;
    HWND logEdit = nullptr;
    HWND downloadBtn = nullptr;
    HWND selectAllBtn = nullptr;
    HWND progressBar = nullptr;
    HWND mediaCheck = nullptr;

    std::wstring exeDir;
    std::wstring megatoolsPath;
    std::wstring destFolder;
    std::wstring startupNote; // see initialDestFolder()
    std::vector<GameEntry> games;
    bool downloading = false;
    bool allChecked = false; // matches GameEntry::checked's new unticked-by-default
    bool wantMedia = false;  // "Also download frontend media" (remembered, see kMediaValue)
    int heartbeatStart = -1; // where the live progress line starts in the log, -1 if none
};

// "Also download frontend media", remembered like the destination folder.
const wchar_t* const kMediaValue = L"DownloaderMedia";

bool loadWantMedia() {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, kSettingsKey, kMediaValue, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return false;
    return value != 0;
}

void saveWantMedia(bool on) {
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kSettingsKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
    const DWORD value = on ? 1 : 0;
    RegSetValueExW(key, kMediaValue, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
    RegCloseKey(key);
}

// A heartbeat line (see kHeartbeatMark) replaces the previous heartbeat
// line, if it was the last one added: one live progress line per download.
void appendLog(AppState* app, const std::wstring& lineIn) {
    std::wstring line = lineIn;
    const size_t mark = line.find(kHeartbeatMark);
    const bool heartbeat = mark != std::wstring::npos;
    if (heartbeat) line.erase(mark, 1);
    const int len = GetWindowTextLengthW(app->logEdit);
    const int from = heartbeat && app->heartbeatStart >= 0 && app->heartbeatStart <= len ? app->heartbeatStart : len;
    SendMessageW(app->logEdit, EM_SETSEL, from, len);
    std::wstring withNewline = line + L"\r\n";
    SendMessageW(app->logEdit, EM_REPLACESEL, FALSE, (LPARAM)withNewline.c_str());
    app->heartbeatStart = heartbeat ? from : -1;
}

void populateListView(AppState* app) {
    ListView_DeleteAllItems(app->listView);
    for (size_t i = 0; i < app->games.size(); ++i) {
        app->games[i].status = gameStatus(app->games[i], app->destFolder);
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = (int)i;
        item.pszText = const_cast<wchar_t*>(app->games[i].name.c_str());
        ListView_InsertItem(app->listView, &item);
        ListView_SetItemText(app->listView, (int)i, 1, const_cast<wchar_t*>(app->games[i].status.c_str()));
        ListView_SetCheckState(app->listView, (int)i, app->games[i].checked ? TRUE : FALSE);
    }
}

// Status column only (after a download, or another destination folder):
// keeps the player's ticks.
void refreshStatuses(AppState* app) {
    for (size_t i = 0; i < app->games.size() && (int)i < ListView_GetItemCount(app->listView); ++i) {
        app->games[i].status = gameStatus(app->games[i], app->destFolder);
        ListView_SetItemText(app->listView, (int)i, 1, const_cast<wchar_t*>(app->games[i].status.c_str()));
    }
}

// Runs on a worker thread (spawned from WM_COMMAND/ID_DOWNLOAD or at
// startup for the manifest fetch) -- never touches HWNDs directly, only
// ever through PostMessage, since Win32 controls aren't safe to update
// from a thread other than the one that created them.
DWORD WINAPI fetchManifestThread(LPVOID param) {
    AppState* app = (AppState*)param;

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring manifestLinkPath = joinPath(app->exeDir, L"manifest_link.txt");
    std::string manifestLinkUtf8Raw;
    if (!readTextFileUtf8(manifestLinkPath, manifestLinkUtf8Raw)) {
        PostMessageW(app->hwnd, WM_APP_MANIFEST_FAILED, 0,
            (LPARAM) new std::wstring(L"manifest_link.txt not found next to this program. "
                                       L"Create this file with, on a single line, the Mega link to the catalogue."));
        return 0;
    }
    stripUtf8Bom(manifestLinkUtf8Raw);
    std::wstring manifestLink = trim(utf8ToWide(manifestLinkUtf8Raw));
    if (manifestLink.empty()) {
        PostMessageW(app->hwnd, WM_APP_MANIFEST_FAILED, 0,
            (LPARAM) new std::wstring(L"manifest_link.txt is empty."));
        return 0;
    }

    // megatools' --path is ALWAYS a destination DIRECTORY the downloaded
    // file lands inside under its own remote name -- confirmed against
    // megatools-dl's own manual ("--path <path>: Local directory to
    // download to"), true for a single-file link exactly like a folder
    // link, with no option to rename/relocate to an exact output file path.
    // An earlier version of this function passed a specific FILE path here
    // instead, which real megatools rejects with "<path> must be a
    // directory" -- caught against a real Mega link on a real Windows
    // machine, not something the fake stand-in used during development
    // happened to exercise the same way. So: a fresh, empty directory, then
    // whichever single file megatools actually places inside it afterward
    // -- this also means it doesn't matter what the manifest file is
    // actually named on Mega.
    std::wstring localManifestDir = joinPath(tempDir, L"dirk_downloader_manifest");
    {
        // Best-effort clean slate -- a leftover file in here from a killed
        // previous run could otherwise get mistaken for this run's result
        // below if this run's own download fails to produce one.
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(joinPath(localManifestDir, L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                DeleteFileW(joinPath(localManifestDir, fd.cFileName).c_str());
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
        RemoveDirectoryW(localManifestDir.c_str());
    }
    CreateDirectoryW(localManifestDir.c_str(), nullptr);

    std::wstring args = L"dl --disable-resume --no-progress --path \"" + localManifestDir +
                         L"\" \"" + manifestLink + L"\"";
    int rc = runProcessCapture(app->megatoolsPath, args, app->exeDir,
        [app](const std::wstring& line) {
            PostMessageW(app->hwnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(L"[catalogue] " + line));
        });

    if (rc != 0) {
        PostMessageW(app->hwnd, WM_APP_MANIFEST_FAILED, 0,
            (LPARAM) new std::wstring(L"Could not fetch the catalogue from Mega (code " +
                                       std::to_wstring(rc) + L"). Check your internet connection and the link in manifest_link.txt."));
        return 0;
    }

    // Find whichever (single) file megatools actually placed in the
    // directory -- for a correctly-configured manifest_link (a FILE link,
    // not a folder link) there should be exactly one.
    std::wstring foundFile;
    {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(joinPath(localManifestDir, L"*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue; // a folder link's contents -- not expected here
                foundFile = joinPath(localManifestDir, fd.cFileName);
                break;
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

    if (foundFile.empty()) {
        PostMessageW(app->hwnd, WM_APP_MANIFEST_FAILED, 0,
            (LPARAM) new std::wstring(L"The catalogue was downloaded but no file was found. "
                                       L"Make sure manifest_link.txt contains the link to the catalogue FILE "
                                       L"itself (\"Get link\" on that file), not a folder's link."));
        return 0;
    }

    std::string manifestUtf8;
    if (!readTextFileUtf8(foundFile, manifestUtf8)) {
        PostMessageW(app->hwnd, WM_APP_MANIFEST_FAILED, 0,
            (LPARAM) new std::wstring(L"The catalogue was downloaded but could not be read."));
        return 0;
    }
    stripUtf8Bom(manifestUtf8);
    std::vector<GameEntry>* parsed = new std::vector<GameEntry>(parseManifest(utf8ToWide(manifestUtf8)));
    PostMessageW(app->hwnd, WM_APP_MANIFEST_READY, 0, (LPARAM)parsed);
    return 0;
}

DWORD WINAPI downloadThread(LPVOID param) {
    AppState* app = (AppState*)param;

    std::vector<GameEntry> toDownload;
    for (int i = 0; i < ListView_GetItemCount(app->listView); ++i) {
        if (ListView_GetCheckState(app->listView, i) && (size_t)i < app->games.size()) {
            toDownload.push_back(app->games[i]);
        }
    }

    PostMessageW(app->hwnd, WM_APP_PROGRESS, 0, (LPARAM)toDownload.size());

    int okCount = 0, failCount = 0;
    for (size_t i = 0; i < toDownload.size(); ++i) {
        const GameEntry& g = toDownload[i];
        PostMessageW(app->hwnd, WM_APP_LOG, 0,
            (LPARAM) new std::wstring(L"--- Downloading \"" + g.name + L"\" (" +
                                       std::to_wstring(i + 1) + L"/" + std::to_wstring(toDownload.size()) + L") ---"));

        const bool ok = downloadGame(app->megatoolsPath, app->exeDir, app->destFolder, g, app->wantMedia,
            [app, &g](const std::wstring& line) {
                PostMessageW(app->hwnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(L"[" + g.name + L"] " + line));
            });
        const int rc = ok ? 0 : 1;

        if (rc == 0) {
            ++okCount;
            PostMessageW(app->hwnd, WM_APP_LOG, 0, (LPARAM) new std::wstring(L"\"" + g.name + L"\": done."));
        } else {
            ++failCount;
            PostMessageW(app->hwnd, WM_APP_LOG, 0,
                (LPARAM) new std::wstring(L"\"" + g.name + L"\": FAILED -- see the lines above."));
        }

        PostMessageW(app->hwnd, WM_APP_PROGRESS, (WPARAM)(i + 1), (LPARAM)toDownload.size());
    }

    std::wstring* summary = new std::wstring(
        L"Done: " + std::to_wstring(okCount) + L" succeeded, " +
        std::to_wstring(failCount) + L" failed.");
    PostMessageW(app->hwnd, WM_APP_DONE, 0, (LPARAM)summary);
    return 0;
}

void startManifestFetch(AppState* app) {
    EnableWindow(app->downloadBtn, FALSE);
    appendLog(app, L"Fetching game catalogue...");
    HANDLE t = CreateThread(nullptr, 0, fetchManifestThread, app, 0, nullptr);
    if (t) CloseHandle(t);
}

void startDownload(AppState* app) {
    app->downloading = true;
    EnableWindow(app->downloadBtn, FALSE);
    EnableWindow(app->listView, FALSE);
    EnableWindow(app->selectAllBtn, FALSE);
    EnableWindow(app->mediaCheck, FALSE);
    HANDLE t = CreateThread(nullptr, 0, downloadThread, app, 0, nullptr);
    if (t) CloseHandle(t);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* app = (AppState*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);

    switch (msg) {
    case WM_CREATE: {
        auto* created = (CREATESTRUCTW*)lParam;
        app = (AppState*)created->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
        app->hwnd = hwnd;

        CreateWindowW(L"STATIC", L"Available games:", WS_VISIBLE | WS_CHILD,
                      12, 10, 300, 18, hwnd, nullptr, nullptr, nullptr);

        app->listView = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                         WS_VISIBLE | WS_CHILD | WS_BORDER | LVS_REPORT | LVS_SINGLESEL,
                                         12, 30, 460, 220, hwnd, (HMENU)(INT_PTR)ID_LISTVIEW, nullptr, nullptr);
        ListView_SetExtendedListViewStyle(app->listView, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT);
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH;
        col.cx = 270;
        col.pszText = const_cast<wchar_t*>(L"Game name");
        ListView_InsertColumn(app->listView, 0, &col);
        col.cx = 170;
        col.pszText = const_cast<wchar_t*>(L"Status");
        ListView_InsertColumn(app->listView, 1, &col);

        app->selectAllBtn = CreateWindowW(L"BUTTON", L"Select/deselect all", WS_VISIBLE | WS_CHILD,
                                           12, 258, 180, 24, hwnd, (HMENU)(INT_PTR)ID_SELECT_ALL, nullptr, nullptr);
        app->mediaCheck = CreateWindowW(L"BUTTON", L"Also download frontend media", WS_VISIBLE | WS_CHILD | BS_AUTOCHECKBOX,
                                         210, 258, 262, 24, hwnd, (HMENU)(INT_PTR)ID_MEDIA, nullptr, nullptr);
        SendMessageW(app->mediaCheck, BM_SETCHECK, app->wantMedia ? BST_CHECKED : BST_UNCHECKED, 0);

        CreateWindowW(L"STATIC", L"Destination folder:", WS_VISIBLE | WS_CHILD,
                      12, 294, 200, 18, hwnd, nullptr, nullptr, nullptr);
        app->destEdit = CreateWindowW(L"EDIT", app->destFolder.c_str(),
                                       WS_VISIBLE | WS_CHILD | WS_BORDER | ES_READONLY,
                                       12, 314, 350, 22, hwnd, (HMENU)(INT_PTR)ID_DEST_EDIT, nullptr, nullptr);
        CreateWindowW(L"BUTTON", L"Browse...", WS_VISIBLE | WS_CHILD,
                      370, 314, 100, 22, hwnd, (HMENU)(INT_PTR)ID_BROWSE, nullptr, nullptr);

        app->downloadBtn = CreateWindowW(L"BUTTON", L"Download checked games",
                                          WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
                                          12, 346, 220, 30, hwnd, (HMENU)(INT_PTR)ID_DOWNLOAD, nullptr, nullptr);

        // Whole-run progress (X of N games), not a byte-level percentage --
        // see WM_APP_PROGRESS's own comment above for why. Range/position
        // are set once a download actually starts; stays at its default
        // empty state (0-100 range, pos 0) until then.
        app->progressBar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                                            WS_VISIBLE | WS_CHILD | PBS_SMOOTH,
                                            12, 382, 460, 18, hwnd, nullptr, nullptr, nullptr);

        CreateWindowW(L"STATIC", L"Log:", WS_VISIBLE | WS_CHILD,
                      12, 410, 100, 18, hwnd, nullptr, nullptr, nullptr);
        app->logEdit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
                                        WS_VISIBLE | WS_CHILD | WS_BORDER | WS_VSCROLL |
                                        ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
                                        12, 430, 460, 140, hwnd, (HMENU)(INT_PTR)ID_LOG_EDIT, nullptr, nullptr);

        if (!app->startupNote.empty()) appendLog(app, app->startupNote);
        EnableWindow(app->downloadBtn, FALSE); // re-enabled once the manifest actually loads
        startManifestFetch(app);
        return 0;
    }

    case WM_COMMAND: {
        int id = LOWORD(wParam);
        if (id == ID_BROWSE) {
            BROWSEINFOW bi{};
            wchar_t pathBuf[MAX_PATH] = {};
            bi.hwndOwner = hwnd;
            bi.pszDisplayName = pathBuf;
            bi.lpszTitle = L"Choose where to save downloaded games";
            bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
            LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
            if (pidl) {
                wchar_t chosen[MAX_PATH];
                if (SHGetPathFromIDListW(pidl, chosen)) {
                    app->destFolder = chosen;
                    SetWindowTextW(app->destEdit, app->destFolder.c_str());
                    refreshStatuses(app);
                    if (!saveLastDestFolder(app->destFolder)) {
                        appendLog(app, L"Note: couldn't remember this folder for next time (registry write failed).");
                    }
                }
                CoTaskMemFree(pidl);
            }
        } else if (id == ID_MEDIA) {
            app->wantMedia = SendMessageW(app->mediaCheck, BM_GETCHECK, 0, 0) == BST_CHECKED;
            saveWantMedia(app->wantMedia);
        } else if (id == ID_SELECT_ALL) {
            app->allChecked = !app->allChecked;
            for (int i = 0; i < ListView_GetItemCount(app->listView); ++i) {
                ListView_SetCheckState(app->listView, i, app->allChecked ? TRUE : FALSE);
            }
        } else if (id == ID_DOWNLOAD && !app->downloading) {
            bool anyChecked = false;
            for (int i = 0; i < ListView_GetItemCount(app->listView); ++i) {
                if (ListView_GetCheckState(app->listView, i)) { anyChecked = true; break; }
            }
            if (!anyChecked) {
                MessageBoxW(hwnd, L"Check at least one game before downloading.", L"Dirk Downloader", MB_ICONINFORMATION);
            } else {
                startDownload(app);
            }
        }
        return 0;
    }

    case WM_APP_LOG: {
        std::wstring* line = (std::wstring*)lParam;
        appendLog(app, *line);
        delete line;
        return 0;
    }

    case WM_APP_MANIFEST_READY: {
        auto* games = (std::vector<GameEntry>*)lParam;
        app->games = *games;
        delete games;
        populateListView(app);
        appendLog(app, L"Catalogue fetched: " + std::to_wstring(app->games.size()) + L" game(s) available.");
        EnableWindow(app->downloadBtn, TRUE);
        return 0;
    }

    case WM_APP_MANIFEST_FAILED: {
        std::wstring* err = (std::wstring*)lParam;
        appendLog(app, L"ERROR: " + *err);
        MessageBoxW(hwnd, err->c_str(), L"Dirk Downloader", MB_ICONERROR);
        delete err;
        return 0;
    }

    case WM_APP_PROGRESS: {
        int done = (int)wParam;
        int total = (int)lParam;
        SendMessageW(app->progressBar, PBM_SETRANGE32, 0, total > 0 ? total : 1);
        SendMessageW(app->progressBar, PBM_SETPOS, (WPARAM)done, 0);
        return 0;
    }

    case WM_APP_DONE: {
        // No completion popup, on request -- it added a click for no real
        // information the log line below doesn't already give just as
        // clearly (visually set off with === so it's easy to spot at the
        // bottom of the log without having to read every line above it).
        std::wstring* summary = (std::wstring*)lParam;
        appendLog(app, L"=== " + *summary + L" ===");
        delete summary;
        app->downloading = false;
        EnableWindow(app->downloadBtn, TRUE);
        EnableWindow(app->listView, TRUE);
        EnableWindow(app->selectAllBtn, TRUE);
        EnableWindow(app->mediaCheck, TRUE);
        refreshStatuses(app);
        return 0;
    }

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE hInstance, HINSTANCE, LPWSTR, int nCmdShow) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    AppState app;
    app.exeDir = exeDirectory();
    app.megatoolsPath = joinPath(app.exeDir, L"megatools.exe");
    app.destFolder = initialDestFolder(app.exeDir, app.startupNote); // last "Browse..." choice, else defaultDestFolder()
    app.wantMedia = loadWantMedia();

    if (GetFileAttributesW(app.megatoolsPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr,
            L"megatools.exe was not found next to this program.\n"
            L"Place megatools.exe in the same folder as Dirk Downloader.",
            L"Dirk Downloader", MB_ICONERROR);
        return 1;
    }

    const wchar_t* CLASS_NAME = L"DirkDownloaderWindow";
    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, CLASS_NAME, L"Dirk Downloader",
                                 WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 502, 616,
                                 nullptr, nullptr, hInstance, &app);
    if (!hwnd) return 1;

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return 0;
}
