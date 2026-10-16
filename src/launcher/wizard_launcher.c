// Wizard.exe (2026-10-10): KimmyScript's create mode, the way people see it.
// It holds no part of Wizard itself: it finds the installed dirk.exe and runs
//     dirk.exe --wizard "<game folder>"
// (Wizard is built into dirk, see LuaEngine::runWizardChunk). Windows only;
// on Linux the same is just `dirk --wizard <folder>`.
//
// The game folder (2026-10-12, Cyril: no need to copy Wizard.exe any more):
//   - a folder given on the command line (a folder dropped on Wizard.exe);
//   - else the folder Wizard.exe is in, when it holds the movie (Wizard.exe
//     copied into a game folder, as before);
//   - else Windows' "choose a folder" window, opened where the last game
//     was (HKCU\Software\Dirk\WizardFolder), until a folder that suits is
//     chosen or the window is cancelled.
// A folder suits when its Movie/ (or Video/) holds a video (.mp4, .mkv,
// .avi, .mov, as dirk looks for it), and when it is not a game whose only
// sheet is inside a .zdirk (nothing Wizard could open there).
//
// Where dirk.exe is looked for, in order, without ever asking:
//   1. the program a double-click on a .dirk file opens -- asked to Windows
//      the way Explorer does (AssocQueryString), so a .dirk association
//      changed with "Open with" is followed too;
//   2. the same, read directly in the installer's own keys: the open
//      command of HKCU Software\Classes\DirkGame.File, then
//      HKCU\Software\Dirk\InstallDir;
//   3. next to Wizard.exe, then in each folder above it.
#define WIN32_LEAN_AND_MEAN
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define COBJMACROS
#include <windows.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <wchar.h>

#define TITLE L"Wizard"

static void fail(const wchar_t* text) {
    MessageBoxW(NULL, text, TITLE, MB_OK | MB_ICONEXCLAMATION);
}

static int isFile(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static int isDir(const wchar_t* path) {
    DWORD a = GetFileAttributesW(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void stripLastPart(wchar_t* path) {
    wchar_t* slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0; else path[0] = 0;
}

static int readRegString(const wchar_t* key, const wchar_t* value, wchar_t* out, DWORD outChars) {
    DWORD bytes = outChars * sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, key, value, RRF_RT_REG_SZ, NULL, out, &bytes) != ERROR_SUCCESS) return 0;
    out[outChars - 1] = 0;
    return out[0] != 0;
}

// A video in <folder>\<sub>, by the extensions dirk itself looks for.
static int hasVideoIn(const wchar_t* folder, const wchar_t* sub) {
    static const wchar_t* exts[] = { L"mp4", L"mkv", L"avi", L"mov" };
    for (int i = 0; i < 4; ++i) {
        wchar_t pattern[MAX_PATH + 32];
        _snwprintf(pattern, MAX_PATH + 32, L"%ls\\%ls\\*.%ls", folder, sub, exts[i]);
        pattern[MAX_PATH + 31] = 0;
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pattern, &fd);
        if (h != INVALID_HANDLE_VALUE) {
            FindClose(h);
            return 1;
        }
    }
    return 0;
}

static int hasFiles(const wchar_t* folder, const wchar_t* ext) {
    wchar_t pattern[MAX_PATH + 16];
    _snwprintf(pattern, MAX_PATH + 16, L"%ls\\*.%ls", folder, ext);
    pattern[MAX_PATH + 15] = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) return 0;
    FindClose(h);
    return 1;
}

// NULL when Wizard can work on this folder, else why not (shown as is).
static const wchar_t* folderProblem(const wchar_t* folder) {
    if (!hasVideoIn(folder, L"Movie") && !hasVideoIn(folder, L"Video"))
        return L"There is no movie in this folder.\n\n"
               L"Choose the folder of your game: the video (.mp4, .mkv, .avi or .mov) goes in its Movie folder.";
    if (hasFiles(folder, L"zdirk") && !hasFiles(folder, L"dirk") && !hasFiles(folder, L"singe"))
        return L"This game is a .zdirk: its sheet is inside the zip.\n\n"
               L"To change it, open the .zdirk with 7-Zip and extract its .dirk into this folder, then choose the folder again.";
    return NULL;
}

// Windows' folder window. 1 and the folder in out, or 0 when cancelled.
static int pickFolder(const wchar_t* startIn, wchar_t* out, DWORD n) {
    int ok = 0;
    IFileOpenDialog* dlg = NULL;
    if (FAILED(CoCreateInstance(&CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, (void**)&dlg))) return 0;
    FILEOPENDIALOGOPTIONS opts = 0;
    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
    IFileOpenDialog_SetTitle(dlg, L"Wizard: choose the folder of your game (the one with its Movie folder)");
    IFileOpenDialog_SetOkButtonLabel(dlg, L"Open this game");
    if (startIn && startIn[0] && isDir(startIn)) {
        IShellItem* item = NULL;
        if (SUCCEEDED(SHCreateItemFromParsingName(startIn, NULL, &IID_IShellItem, (void**)&item))) {
            IFileOpenDialog_SetFolder(dlg, item);
            IShellItem_Release(item);
        }
    }
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, NULL))) {
        IShellItem* item = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
            PWSTR path = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &path))) {
                wcsncpy(out, path, n);
                out[n - 1] = 0;
                CoTaskMemFree(path);
                ok = 1;
            }
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dlg);
    return ok;
}

static int findDirk(const wchar_t* here, wchar_t* dirk, DWORD n) {
    wchar_t buf[MAX_PATH * 2];
    DWORD chars = n;
    if (AssocQueryStringW(ASSOCF_NONE, ASSOCSTR_EXECUTABLE, L".dirk", L"open", dirk, &chars) == S_OK && isFile(dirk)) return 1;
    if (readRegString(L"Software\\Classes\\DirkGame.File\\shell\\open\\command", NULL, buf, MAX_PATH * 2)) {
        const wchar_t* p = buf;
        wchar_t end = L' ';
        if (*p == L'"') { ++p; end = L'"'; }
        DWORD i = 0;
        while (p[i] && p[i] != end && i < n - 1) { dirk[i] = p[i]; ++i; }
        dirk[i] = 0;
        if (isFile(dirk)) return 1;
    }
    if (readRegString(L"Software\\Dirk", L"InstallDir", buf, MAX_PATH)) {
        _snwprintf(dirk, n, L"%ls\\dirk.exe", buf);
        dirk[n - 1] = 0;
        if (isFile(dirk)) return 1;
    }
    wchar_t dir[MAX_PATH];
    wcsncpy(dir, here, MAX_PATH);
    dir[MAX_PATH - 1] = 0;
    while (dir[0]) {
        _snwprintf(dirk, n, L"%ls\\dirk.exe", dir);
        dirk[n - 1] = 0;
        if (isFile(dirk)) return 1;
        stripLastPart(dir);
    }
    return 0;
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, PWSTR cmdLine, int show) {
    (void)inst; (void)prev; (void)show;
    wchar_t here[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, here, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) {
        fail(L"Wizard could not find its own folder.");
        return 1;
    }
    stripLastPart(here);

    wchar_t game[MAX_PATH];
    game[0] = 0;
    if (cmdLine && cmdLine[0]) {
        const wchar_t* p = cmdLine;
        while (*p == L' ') ++p;
        wchar_t end = L' ';
        if (*p == L'"') { ++p; end = L'"'; }
        DWORD i = 0;
        while (p[i] && p[i] != end && i < MAX_PATH - 1) { game[i] = p[i]; ++i; }
        game[i] = 0;
        while (i > 3 && game[i - 1] == L'\\') game[--i] = 0;
        if (!isDir(game)) game[0] = 0;
    }
    if (!game[0] && (hasVideoIn(here, L"Movie") || hasVideoIn(here, L"Video"))) {
        wcsncpy(game, here, MAX_PATH);
        game[MAX_PATH - 1] = 0;
    }
    if (game[0]) {
        const wchar_t* why = folderProblem(game);
        if (why) {
            fail(why);
            return 1;
        }
    } else {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        wchar_t start[MAX_PATH];
        if (!readRegString(L"Software\\Dirk", L"WizardFolder", start, MAX_PATH)
            && !readRegString(L"Software\\Dirk", L"InstallDir", start, MAX_PATH)) start[0] = 0;
        for (;;) {
            if (!pickFolder(start, game, MAX_PATH)) {
                CoUninitialize();
                return 0;
            }
            const wchar_t* why = folderProblem(game);
            if (!why) break;
            fail(why);
            wcsncpy(start, game, MAX_PATH);
            start[MAX_PATH - 1] = 0;
            stripLastPart(start);
        }
        CoUninitialize();
        wchar_t parent[MAX_PATH];
        wcsncpy(parent, game, MAX_PATH);
        parent[MAX_PATH - 1] = 0;
        stripLastPart(parent);
        if (parent[0]) RegSetKeyValueW(HKEY_CURRENT_USER, L"Software\\Dirk", L"WizardFolder", REG_SZ,
                                      parent, (DWORD)((wcslen(parent) + 1) * sizeof(wchar_t)));
    }

    wchar_t dirk[MAX_PATH * 2];
    if (!findDirk(here, dirk, MAX_PATH * 2)) {
        fail(L"Wizard needs Dirk, and dirk.exe was not found.\n\n"
             L"Install Dirk (DirkSetup.exe), or put this folder inside the Dirk folder.");
        return 1;
    }
    wchar_t dirkDir[MAX_PATH * 2];
    wcsncpy(dirkDir, dirk, MAX_PATH * 2);
    dirkDir[MAX_PATH * 2 - 1] = 0;
    stripLastPart(dirkDir);

    wchar_t cmd[MAX_PATH * 5];
    _snwprintf(cmd, MAX_PATH * 5, L"\"%ls\" --wizard \"%ls\"", dirk, game);
    cmd[MAX_PATH * 5 - 1] = 0;

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    ZeroMemory(&pi, sizeof pi);
    if (!CreateProcessW(dirk, cmd, NULL, NULL, FALSE, 0, NULL, dirkDir, &si, &pi)) {
        fail(L"Wizard could not start dirk.exe.");
        return 1;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 0;
}
