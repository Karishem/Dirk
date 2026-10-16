// Stand-in for megatools.exe in the Dirk Downloader tests (Wine):
//   megatools dl [--disable-resume] [--no-progress] --path <DIR> <LINK>
// LINK ends with an id: https://mega.nz/file/<id> or .../folder/<id>.
// <exe dir>\store\<id> is what that link holds:
//   a file   -> copied into DIR (like a file link)
//   a folder -> copied into DIR as DIR\<folder name>\... (like a folder
//               link); with a "flat.flag" file inside, its CONTENTS are
//               copied straight into DIR instead (the other possible
//               megatools behaviour, both must work)
// id "fail" -> exit 1. A file store\<id>.trunc -> copied minus its last 200
// bytes (a cut download). Every call is appended to store\calls.log.
#include <windows.h>
#include <string>
#include <cstdio>
static std::wstring exeDir() {
    wchar_t b[MAX_PATH]; GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring p(b); return p.substr(0, p.find_last_of(L"\\/"));
}
static bool isDir(const std::wstring& p) { DWORD a = GetFileAttributesW(p.c_str()); return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY); }
static void copyTree(const std::wstring& src, const std::wstring& dst) {
    CreateDirectoryW(dst.c_str(), nullptr);
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || n == L"flat.flag") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) copyTree(src + L"\\" + n, dst + L"\\" + n);
        else CopyFileW((src + L"\\" + n).c_str(), (dst + L"\\" + n).c_str(), FALSE);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
int wmain(int argc, wchar_t** argv) {
    std::wstring path, link;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--path" && i + 1 < argc) path = argv[++i];
        else if (a.rfind(L"--", 0) != 0 && a != L"dl") link = a;
    }
    const std::wstring store = exeDir() + L"\\store";
    FILE* log = _wfopen((store + L"\\calls.log").c_str(), L"ab");
    if (log) { fwprintf(log, L"%ls\n", link.c_str()); fclose(log); }
    const std::wstring id = link.substr(link.find_last_of(L"/") + 1);
    if (id == L"fail") { fwprintf(stderr, L"ERROR: fake failure\n"); return 1; }
    if (!isDir(path)) { fwprintf(stderr, L"ERROR: %ls must be a directory\n", path.c_str()); return 1; }
    const std::wstring src = store + L"\\" + id;
    // store\<id>.slow: a 3 MB movie written in 12 steps over 6 s, like a real
    // download (into .megatmp.<id>, renamed at the end, as megatools does).
    // store\<id>.stall: 64 KB written, then nothing ever again.
    const bool slow = GetFileAttributesW((src + L".slow").c_str()) != INVALID_FILE_ATTRIBUTES;
    const bool stall = GetFileAttributesW((src + L".stall").c_str()) != INVALID_FILE_ATTRIBUTES;
    if (slow || stall) {
        const std::wstring tmp = path + L"\\.megatmp." + id;
        HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, 0, nullptr);
        std::string chunk(slow ? 256 * 1024 : 64 * 1024, 'M');
        for (int i = 0; i < (slow ? 12 : 1); ++i) {
            DWORD w = 0; WriteFile(f, chunk.data(), (DWORD)chunk.size(), &w, nullptr);
            Sleep(500);
        }
        if (stall) Sleep(INFINITE);
        CloseHandle(f);
        CreateDirectoryW((path + L"\\Movie").c_str(), nullptr);
        MoveFileW(tmp.c_str(), (path + L"\\Movie\\big.mp4").c_str());
        wprintf(L"Downloaded folder %ls\n", id.c_str());
        return 0;
    }
    if (isDir(src)) {
        if (GetFileAttributesW((src + L"\\flat.flag").c_str()) != INVALID_FILE_ATTRIBUTES) copyTree(src, path);
        else {
            WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW((src + L"\\*").c_str(), &fd); // folder's own display name: name.txt inside, else id
            std::wstring name = id;
            FILE* nf = _wfopen((src + L".name").c_str(), L"rb");
            if (nf) { char b[256] = {}; fread(b, 1, 255, nf); fclose(nf); std::string s(b); while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back(); name = std::wstring(s.begin(), s.end()); }
            if (h != INVALID_HANDLE_VALUE) FindClose(h);
            copyTree(src, path + L"\\" + name);
        }
        wprintf(L"Downloaded folder %ls\n", id.c_str());
        return 0;
    }
    // a file: store\<id> holds "<filename>" in <id>.fname, content in <id>
    std::wstring fname = id;
    FILE* nf = _wfopen((src + L".fname").c_str(), L"rb");
    if (nf) { char b[256] = {}; fread(b, 1, 255, nf); fclose(nf); std::string s(b); while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back(); fname = std::wstring(s.begin(), s.end()); }
    if (GetFileAttributesW(src.c_str()) == INVALID_FILE_ATTRIBUTES) { fwprintf(stderr, L"ERROR: no such link\n"); return 1; }
    const std::wstring dst = path + L"\\" + fname;
    CopyFileW(src.c_str(), dst.c_str(), FALSE);
    if (GetFileAttributesW((src + L".trunc").c_str()) != INVALID_FILE_ATTRIBUTES) {
        HANDLE f = CreateFileW(dst.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER sz; GetFileSizeEx(f, &sz); sz.QuadPart -= 200; SetFilePointerEx(f, sz, nullptr, FILE_BEGIN); SetEndOfFile(f); CloseHandle(f);
    }
    wprintf(L"Downloaded %ls\n", fname.c_str());
    return 0;
}
