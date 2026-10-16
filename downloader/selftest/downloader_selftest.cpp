// Self-test of the Dirk Downloader's install/update logic (2026-09-28):
// the real code (this includes dirk_downloader.cpp), a fake megatools.exe
// and a fake Mega store, run under Wine. Build and run: see README.md,
// "Verified so far". No window is opened.
#include "../dirk_downloader.cpp"

namespace {
int failures = 0;
void check(bool ok, const std::wstring& what) {
    wprintf(L"%ls %ls\n", ok ? L"OK  " : L"FAIL", what.c_str());
    if (!ok) ++failures;
}
std::string slurp(const std::wstring& p) { std::string s; readTextFileUtf8(p, s); return s; }
void spit(const std::wstring& p, const std::string& s) { FILE* f = _wfopen(p.c_str(), L"wb"); fwrite(s.data(), 1, s.size(), f); fclose(f); }
int calls(const std::wstring& store) { // megatools calls so far
    std::string s = slurp(joinPath(store, L"calls.log")); int n = 0; for (char c : s) n += c == '\n'; return n;
}
GameEntry entry(const std::wstring& line) { return parseManifest(line).at(0); }
}

int wmain() {
    const std::wstring dir = exeDirectory();
    const std::wstring megatools = joinPath(dir, L"megatools.exe");
    const std::wstring store = joinPath(dir, L"store");
    const std::wstring dest = joinPath(dir, L"games");
    deleteTree(dest);
    CreateDirectoryW(dest.c_str(), nullptr);
    std::wstring logText;
    LogFn log = [&](const std::wstring& l) { logText += l + L"\n"; };
    const std::wstring T = joinPath(dest, L"Timegal");

    // Manifest parsing.
    auto parsed = parseManifest(L"# comment\r\nOld=https://mega.nz/folder/f1\r\n"
                                L"Timegal = https://mega.nz/file/g1 | https://mega.nz/folder/m1 | https://mega.nz/folder/md1\r\n"
                                L"NoMedia=https://mega.nz/file/g1|https://mega.nz/folder/m1\r\nbroken line\r\n");
    check(parsed.size() == 3 && parsed[0].parts.size() == 1 && parsed[0].parts[0].role == PartRole::Folder &&
          parsed[1].parts.size() == 3 && parsed[1].parts[2].role == PartRole::Media && parsed[2].parts.size() == 2,
          L"manifest: old one-link form, game|movie|media, game|movie, bad line skipped");

    // 1. First install, without media.
    GameEntry tg = entry(L"Timegal=https://mega.nz/file/g1|https://mega.nz/folder/m1|https://mega.nz/folder/md1");
    check(gameStatus(tg, dest) == L"Not installed", L"status before: Not installed");
    int c0 = calls(store);
    check(downloadGame(megatools, dir, dest, tg, false, log), L"first install succeeds");
    check(slurp(joinPath(T, L"Timegal.zdirk")).find("v1") != std::string::npos, L".zdirk in the game's folder");
    check(slurp(joinPath(T, L"Movie\\timegal.mp4")) == "MOVIE1", L"movie in Movie\\ (folder link arriving wrapped)");
    check(!directoryExists(joinPath(T, L"Media")) && calls(store) - c0 == 2, L"media not downloaded when not asked (2 downloads)");
    check((GetFileAttributesW(joinPath(T, L".dirk-download").c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0 &&
          !directoryExists(joinPath(T, L".dirk-download-tmp")), L"hidden record written, temporary folder gone");
    check(gameStatus(tg, dest) == L"Up to date", L"status after: Up to date");

    // The player plays: settings, saves, subtitles next to the movie.
    CreateDirectoryW(joinPath(T, L"Cfg").c_str(), nullptr);
    spit(joinPath(T, L"Cfg\\s1.cfg"), "MY SAVE");
    spit(joinPath(T, L"Movie\\timegal-fr.srt"), "MY SUBS");

    // 2. The game is updated on Mega (new link): only the .zdirk comes again.
    GameEntry tg2 = entry(L"Timegal=https://mega.nz/file/g2|https://mega.nz/folder/m1|https://mega.nz/folder/md1");
    check(gameStatus(tg2, dest) == L"Update available", L"new game link: Update available");
    c0 = calls(store);
    check(downloadGame(megatools, dir, dest, tg2, false, log), L"update succeeds");
    check(calls(store) - c0 == 1 && slurp(joinPath(T, L"Timegal.zdirk")).find("v2") != std::string::npos,
          L"only the .zdirk downloaded again (1 download), new version in place");
    check(slurp(joinPath(T, L"Cfg\\s1.cfg")) == "MY SAVE" && slurp(joinPath(T, L"Movie\\timegal.mp4")) == "MOVIE1",
          L"the player's save and the movie untouched");
    check(gameStatus(tg2, dest) == L"Up to date", L"then Up to date");

    // 3. Frontend media, asked for later.
    c0 = calls(store);
    check(downloadGame(megatools, dir, dest, tg2, true, log) && calls(store) - c0 == 1 &&
          slurp(joinPath(T, L"Media\\box3d.png")) == "BOX" && slurp(joinPath(T, L"Media\\marquee.png")) == "MARQUEE",
          L"media asked: only the media downloaded, files straight into Media\\");
    GameEntry tg3 = entry(L"Timegal=https://mega.nz/file/g2|https://mega.nz/folder/m1|https://mega.nz/folder/md2");
    check(gameStatus(tg3, dest) == L"Media update available", L"new media link: Media update available");
    check(downloadGame(megatools, dir, dest, tg3, true, log) && slurp(joinPath(T, L"Media\\box3d.png")) == "BOX2",
          L"media updated");

    // 4. The movie is republished (flat folder link this time).
    GameEntry tg4 = entry(L"Timegal=https://mega.nz/file/g2|https://mega.nz/folder/m2|https://mega.nz/folder/md2");
    check(gameStatus(tg4, dest) == L"Movie update available", L"new movie link: Movie update available");
    c0 = calls(store);
    check(downloadGame(megatools, dir, dest, tg4, true, log) && calls(store) - c0 == 1 &&
          slurp(joinPath(T, L"Movie\\timegal.mp4")) == "MOVIE2", L"only the movie downloaded again (flat folder link)");
    check(slurp(joinPath(T, L"Movie\\timegal-fr.srt")) == "MY SUBS", L"the player's own file next to the movie kept");

    // 5. Movie deleted by the player.
    deleteTree(joinPath(T, L"Movie"));
    check(gameStatus(tg4, dest) == L"Movie missing", L"movie deleted: Movie missing");
    c0 = calls(store);
    check(downloadGame(megatools, dir, dest, tg4, true, log) && calls(store) - c0 == 1 &&
          slurp(joinPath(T, L"Movie\\timegal.mp4")) == "MOVIE2", L"only the movie downloaded again");

    // 6. A download cut short: refused, old game kept, still "update available".
    GameEntry tgT = entry(L"Timegal=https://mega.nz/file/gtrunc|https://mega.nz/folder/m2|https://mega.nz/folder/md2");
    logText.clear();
    check(!downloadGame(megatools, dir, dest, tgT, true, log) && logText.find(L"incomplete") != std::wstring::npos,
          L"cut download: failure, clear message");
    check(slurp(joinPath(T, L"Timegal.zdirk")).find("v2") != std::string::npos && gameStatus(tgT, dest) == L"Update available",
          L"previous .zdirk kept, update still offered");

    // 7. A failing link, and a game link that isn't a .zdirk.
    logText.clear();
    check(!downloadGame(megatools, dir, dest, entry(L"Timegal=https://mega.nz/file/fail|https://mega.nz/folder/m2"), false, log) &&
          logText.find(L"megatools failed") != std::wstring::npos, L"megatools error: failure reported");
    logText.clear();
    check(!downloadGame(megatools, dir, dest, entry(L"Timegal=https://mega.nz/folder/m1|https://mega.nz/folder/m2"), false, log) &&
          logText.find(L".zdirk FILE") != std::wstring::npos, L"game link to a folder: says to use the .zdirk file's link");

    // 8. The .zdirk renamed on Mega: the old one removed.
    check(downloadGame(megatools, dir, dest, entry(L"Timegal=https://mega.nz/file/g3|https://mega.nz/folder/m2"), false, log) &&
          fileExists(joinPath(T, L"TimegalHD.zdirk")) && !fileExists(joinPath(T, L"Timegal.zdirk")),
          L"renamed .zdirk: new one in, old one out");

    // 9. A movie already there (copied by hand), game installed for the first time: movie kept.
    const std::wstring H = joinPath(dest, L"Hayate");
    CreateDirectoryW(H.c_str(), nullptr);
    CreateDirectoryW(joinPath(H, L"Movie").c_str(), nullptr);
    spit(joinPath(H, L"Movie\\hayate.mp4"), "HANDCOPY");
    c0 = calls(store);
    GameEntry hy = entry(L"Hayate=https://mega.nz/file/g1|https://mega.nz/folder/m1");
    check(downloadGame(megatools, dir, dest, hy, false, log) && calls(store) - c0 == 1 &&
          slurp(joinPath(H, L"Movie\\hayate.mp4")) == "HANDCOPY" && gameStatus(hy, dest) == L"Up to date",
          L"movie already there: kept, not downloaded (1 download), then Up to date");

    // 10. Hypseus-style movie folder "Video".
    GameEntry fb = entry(L"FutureBoy=https://mega.nz/file/g1|https://mega.nz/folder/vid1");
    check(downloadGame(megatools, dir, dest, fb, false, log) &&
          slurp(joinPath(dest, L"FutureBoy\\Video\\fb.m2v")) == "M2V", L"a Video folder link stays Video\\ (Hypseus)");

    // 11. The old one-link form.
    GameEntry old1 = entry(L"Oldgame=https://mega.nz/folder/f1");
    check(downloadGame(megatools, dir, dest, old1, false, log) && gameStatus(old1, dest) == L"Installed",
          L"old one-link form: downloaded as before, Installed");
    check(gameStatus(entry(L"Oldgame=https://mega.nz/folder/f2"), dest) == L"Update available",
          L"old form, new link: Update available");

    // 12. Several versions in one .zdirk: the shortcuts for the others.
    const std::wstring HY = joinPath(dest, L"HayateMulti");
    GameEntry hm1 = entry(L"HayateMulti=https://mega.nz/file/h1|https://mega.nz/folder/m1");
    logText.clear();
    check(downloadGame(megatools, dir, dest, hm1, false, log), L"multi: install succeeds");
    const std::string psx = slurp(joinPath(HY, L"HayatePSX.dirk"));
    check(psx.find("zdirk = \"Hayate.zdirk\"") != std::string::npos && psx.find("script = \"HayatePSX.dirk\"") != std::string::npos &&
          isZdirkShortcut(joinPath(HY, L"HayatePSX.dirk")), L"multi: HayatePSX.dirk shortcut written next to Hayate.zdirk");
    check(!fileExists(joinPath(HY, L"Hayate.dirk")) && !fileExists(joinPath(HY, L"extra.dirk")),
          L"multi: no shortcut for the main script, nor for a .dirk in a subfolder");
    check(gameStatus(hm1, dest) == L"Up to date", L"multi: Up to date");

    // Deleted by the player (or installed by the previous tool): repaired with no download.
    DeleteFileW(joinPath(HY, L"HayatePSX.dirk").c_str());
    check(gameStatus(hm1, dest) == L"Shortcut missing", L"multi: shortcut deleted -> Shortcut missing");
    c0 = calls(store);
    check(downloadGame(megatools, dir, dest, hm1, false, log) && calls(store) - c0 == 0 &&
          isZdirkShortcut(joinPath(HY, L"HayatePSX.dirk")) && gameStatus(hm1, dest) == L"Up to date",
          L"multi: shortcut written again, nothing downloaded (0 download), Up to date");

    // Update: zip with one top folder, PSX gone, Saturn + Arcade added; the
    // player's own HayateArcade.dirk (a real script) is never touched.
    spit(joinPath(HY, L"HayateArcade.dirk"), "-- MY OWN SCRIPT\n" + std::string(6000, 'y'));
    GameEntry hm2 = entry(L"HayateMulti=https://mega.nz/file/h2|https://mega.nz/folder/m1");
    logText.clear();
    check(downloadGame(megatools, dir, dest, hm2, false, log), L"multi: update succeeds");
    check(!fileExists(joinPath(HY, L"HayatePSX.dirk")) && isZdirkShortcut(joinPath(HY, L"HayateSaturn.dirk")),
          L"multi update (top folder in the zip): PSX shortcut removed, Saturn shortcut written");
    check(slurp(joinPath(HY, L"HayateArcade.dirk")).rfind("-- MY OWN SCRIPT", 0) == 0 && logText.find(L"Kept your own HayateArcade.dirk") != std::wstring::npos,
          L"multi update: the player's own .dirk of the same name kept, and said so");
    check(gameStatus(hm2, dest) == L"Up to date", L"multi update: Up to date");

    // Update to a single-version zip: the tool's shortcuts go, the player's file stays.
    GameEntry hm3 = entry(L"HayateMulti=https://mega.nz/file/h3|https://mega.nz/folder/m1");
    check(downloadGame(megatools, dir, dest, hm3, false, log) && !fileExists(joinPath(HY, L"HayateSaturn.dirk")) &&
          fileExists(joinPath(HY, L"HayateArcade.dirk")), L"back to one version: Saturn shortcut removed, player's file kept");

    // No script named like the zip, several scripts: a shortcut for each.
    GameEntry ky = entry(L"Kyle=https://mega.nz/file/k1|https://mega.nz/folder/m1");
    check(downloadGame(megatools, dir, dest, ky, false, log) &&
          isZdirkShortcut(joinPath(dest, L"Kyle\\Kyle A.dirk")) && isZdirkShortcut(joinPath(dest, L"Kyle\\Kyle B.dirk")),
          L"no script named like the .zdirk: one shortcut per script (names with spaces)");

    // Single-script zips (all earlier games): no shortcut at all.
    bool noneElsewhere = true;
    for (const auto& gname : {L"Timegal", L"Hayate", L"FutureBoy"})
        for (const auto& n : listFolder(joinPath(dest, gname)))
            if (endsWithNoCase(n, L".dirk")) noneElsewhere = false;
    check(noneElsewhere, L"one-script .zdirk (even named differently, TimegalHD): no shortcut written");

    // zip64 directory (zip -fz), read the same.
    std::vector<std::wstring> names64;
    check(zipEntryNames(joinPath(dir, L"z64.zdirk"), names64) && zdirkShortcutScripts(joinPath(dir, L"z64.zdirk")).size() == 1 &&
          zdirkShortcutScripts(joinPath(dir, L"z64.zdirk"))[0] == L"Z64B.dirk", L"zip64 .zdirk: directory read, shortcut found");
    check(!zipEntryNames(joinPath(store, L"m1\\timegal.mp4"), names64), L"not a zip: refused");

    // 13. Progress: a big movie arriving over 6 s shows how much has arrived.
    const std::wstring P = joinPath(dest, L"Slow");
    logText.clear();
    check(downloadGame(megatools, dir, dest, entry(L"Slow=https://mega.nz/file/g1|https://mega.nz/folder/slow"), false, log) &&
          fileExists(joinPath(P, L"Movie\\big.mp4")), L"slow movie: downloaded, in Movie\\");
    {
        std::vector<double> mb; // every "... N MB received" line, in order
        std::wistringstream ls(logText);
        std::wstring l;
        bool marked = true;
        while (std::getline(ls, l)) {
            const size_t at = l.find(L" MB received");
            if (at == std::wstring::npos) continue;
            marked = marked && l[0] == kHeartbeatMark;
            mb.push_back(wcstod(l.c_str() + l.find(L"... ") + 4, nullptr));
        }
        bool rising = mb.size() >= 2;
        for (size_t i = 1; i < mb.size(); ++i) rising = rising && mb[i] >= mb[i - 1];
        check(rising && mb.back() > 0.2 && mb.back() <= 3.0 && marked,
              L"progress: " + std::to_wstring(mb.size()) + L" live lines, MB received rising (last " +
              std::to_wstring(mb.empty() ? 0.0 : mb.back()) + L"), marked to replace each other");
    }
    check(logText.find(L"MB/s") != std::wstring::npos, L"progress: speed shown");

    // 14. A download that stops receiving anything: stopped, clear message, nothing left behind.
    g_stallTimeoutMs = 5000;
    const std::wstring ST = joinPath(dest, L"Stall");
    logText.clear();
    const DWORD t0 = GetTickCount();
    const bool stallOk = downloadGame(megatools, dir, dest, entry(L"Stall=https://mega.nz/file/g1|https://mega.nz/folder/stall"), false, log);
    const DWORD took = GetTickCount() - t0;
    check(!stallOk && logText.find(L"Stopped: nothing received for 0:05") != std::wstring::npos && took < 20000,
          L"stalled download: stopped after the timeout (" + std::to_wstring(took / 1000) + L" s), clear message");
    check(!directoryExists(joinPath(ST, L".dirk-download-tmp")) && !directoryExists(joinPath(ST, L"Movie")) &&
          fileExists(joinPath(ST, L"Timegal.zdirk")), L"stalled download: temporary folder removed, the game itself kept");
    check(gameStatus(entry(L"Stall=https://mega.nz/file/g1|https://mega.nz/folder/stall"), dest) == L"Movie missing",
          L"stalled download: status Movie missing (retry later fetches only the movie)");

    wprintf(L"%ls\n", failures == 0 ? L"ALL OK" : L"FAILURES");
    return failures == 0 ? 0 : 1;
}
