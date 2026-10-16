// Unit test for review point 2 (2026-09-26, Cyril: "vas y pour les 3"):
//   - dirk::ScriptErrorLog (src/core/ScriptErrorLog.*): the always-on
//     <game>/dirk_error.txt -- one entry per DISTINCT error, repeats only
//     counted, size/distinct caps, stale file removed at startup, no file
//     at all for an error-free run;
//   - dirk::shortenErrorForDisplay(): the on-screen banner's short form;
//   - dirk::LuaEngine::pcall()'s new traceback message handler: the Lua
//     call chain is captured, lastError() keeps its historical one-line
//     form, lastError() is cleared on success and for an undefined
//     optional hook, and the Lua stack is left exactly balanced (the
//     handler is slotted in under the called function and removed again).
//
// The Lua part replays the real DL2 bug's shape: onInputReleased ->
// writeSave -> writeSavePlus -> io.output() on the relative
// "DL2e/Cfg/s4p.cfg" path that doesn't resolve -- the exact error that was
// invisible on Cyril's cabinet. Real files on disk (under the system temp
// dir), real Lua state, no SDL window.

#include "core/ScriptErrorLog.h"
#include "lua/LuaEngine.h"

extern "C" {
#include <lua.h>
}

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

static bool g_allPass = true;

static void check(bool cond, const std::string& what) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) g_allPass = false;
}

static std::string readFile(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static size_t countOccurrences(const std::string& hay, const std::string& needle) {
    size_t n = 0;
    for (size_t p = hay.find(needle); p != std::string::npos; p = hay.find(needle, p + needle.size())) ++n;
    return n;
}

int main() {
    const fs::path dir = fs::temp_directory_path() / "dirk_script_error_log_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir, ec);
    const fs::path logPath = dir / "dirk_error.txt";

    const std::string dl2Message =
        "onInputReleased: E:\\Dirk Games\\DL2e\\/Script/addons.kimmy:314: "
        "bad argument #1 to 'output' (DL2e/Cfg/s4p.cfg: No such file or directory)";

    // 1) Stale file from a previous run is removed by begin(); an
    //    error-free run then leaves no file at all.
    {
        std::ofstream(logPath) << "old run's errors\n";
        dirk::ScriptErrorLog log;
        log.begin(logPath.string(), "header");
        check(!fs::exists(logPath), "begin() deletes a leftover dirk_error.txt from a previous run");
        log.finish();
        check(!fs::exists(logPath), "error-free run: finish() creates no file at all");
        check(!log.fileWritten(), "...and reports nothing written");
    }

    // 2) One distinct error, repeated: written once (with its traceback),
    //    repeats only counted, summary at finish.
    {
        dirk::ScriptErrorLog log;
        log.begin(logPath.string(), "Dirk script errors -- test header");
        const std::string tb = "stack traceback:\n\t[C]: in function 'output'\n\taddons.kimmy:314: in function 'writeSavePlus'";
        check(log.report(dl2Message, tb, 65000), "first occurrence -> report() returns true (banner shows once)");
        check(!log.report(dl2Message, tb, 65100), "repeat -> report() returns false (no banner again)");
        log.report(dl2Message, tb, 65200);
        log.report(dl2Message, tb, 65300);
        check(log.totalCount() == 4 && log.distinctCount() == 1, "4 occurrences counted, 1 distinct");
        const std::string before = readFile(logPath);
        check(countOccurrences(before, dl2Message) == 1, "message written exactly once despite 4 occurrences");
        check(before.find("[00:01:05] ") != std::string::npos, "entry stamped with elapsed time (65000ms -> 00:01:05)");
        check(before.find("in function 'writeSavePlus'") != std::string::npos, "traceback written with the entry");
        check(before.rfind("Dirk script errors -- test header", 0) == 0, "header is the file's first line");
        log.finish();
        const std::string after = readFile(logPath);
        check(after.find("4 x  " + dl2Message) != std::string::npos, "finish() appends the repeat count (4 x)");
        log.finish(); // idempotent
        check(readFile(logPath) == after, "finish() is idempotent");
        check(!log.report("late", "", 0), "report() after finish() is ignored");
    }

    // 3) Distinct-message cap: past kMaxDistinct, new messages only feed
    //    one overflow counter -- bounded memory whatever a script does.
    {
        dirk::ScriptErrorLog log;
        log.begin(logPath.string(), "h");
        const size_t n = dirk::ScriptErrorLog::kMaxDistinct + 50;
        for (size_t i = 0; i < n; ++i) log.report("err #" + std::to_string(i), "", 0);
        check(log.distinctCount() == dirk::ScriptErrorLog::kMaxDistinct,
              "distinct messages tracked in memory capped at kMaxDistinct");
        log.finish();
        const std::string text = readFile(logPath);
        check(text.find("50 x  (other errors") != std::string::npos, "overflow occurrences summarized as one line");
    }

    // 4) File size cap: huge tracebacks can't grow the file past kMaxBytes
    //    (+ the one-time notice and the bounded summary).
    {
        dirk::ScriptErrorLog log;
        log.begin(logPath.string(), "h");
        const std::string bigTb = "stack traceback:\n" + std::string(4000, 'x');
        for (int i = 0; i < 100; ++i) {
            log.report("distinct " + std::to_string(i), bigTb, 0);
            log.report("distinct " + std::to_string(i), bigTb, 0); // make it show up in the summary too
        }
        log.finish();
        const auto size = fs::file_size(logPath);
        check(size <= dirk::ScriptErrorLog::kMaxBytes + 8 * 1024,
              "file size stays bounded (" + std::to_string(size) + " bytes, cap " +
              std::to_string(dirk::ScriptErrorLog::kMaxBytes) + " + notice + summary)");
        const std::string text = readFile(logPath);
        check(countOccurrences(text, "size cap reached") == 1, "size-cap notice written exactly once");
        check(countOccurrences(text, "\n  2 x  ") <= dirk::ScriptErrorLog::kMaxSummaryLines,
              "summary itself capped at kMaxSummaryLines");
        check(text.find("more not listed") != std::string::npos, "summary says how many repeats it left out");
    }

    // 5) No path (no game folder): still counts / returns "new", never
    //    touches the disk.
    {
        fs::remove(logPath, ec);
        dirk::ScriptErrorLog log;
        log.begin("", "h");
        check(log.report("x", "", 0), "no path: first occurrence still reported as new (banner still works)");
        log.finish();
        check(!fs::exists(logPath) && !log.fileWritten(), "no path: nothing written anywhere");
    }

    // 6) Banner text shortening.
    {
        check(dirk::shortenErrorForDisplay(dl2Message) ==
                  "onInputReleased: addons.kimmy:314: bad argument #1 to 'output' (DL2e/Cfg/s4p.cfg: No such file or directory)",
              "real DL2 message: directory stripped, label + file:line + text kept");
        check(dirk::shortenErrorForDisplay("/home/x/game/Script/main.kimmy:12: boom") == "main.kimmy:12: boom",
              "no label, POSIX path: directory stripped");
        check(dirk::shortenErrorForDisplay("onShutdown: something odd") == "onShutdown: something odd",
              "no script position in the message: left untouched");
        const std::string longMsg = "onOverlayUpdate: C:/g/main.kimmy:1: " + std::string(300, 'y');
        const std::string shortMsg = dirk::shortenErrorForDisplay(longMsg, 110);
        check(shortMsg.size() == 110 && shortMsg.substr(107) == "...", "long message capped at 110 chars with ...");
    }

    // 7) LuaEngine traceback, on the real DL2 bug's shape.
    {
        const fs::path script = dir / "dl2_like.lua";
        std::ofstream(script) <<
            "function writeSavePlus(slot)\n"
            "  local h1 = io.output('DL2e_does_not_exist/Cfg/s' .. slot .. 'p.cfg')\n"
            "end\n"
            "function writeSave(slot)\n"
            "  writeSavePlus(slot)\n"
            "end\n"
            "function onInputReleased(what)\n"
            "  writeSave(4)\n"
            "end\n"
            "OVERLAY_UPDATED = 1\n"
            "function onOverlayUpdate() return OVERLAY_UPDATED end\n"
            "function onInputPressed(what) end\n";

        dirk::LuaEngine lua;
        check(lua.open(), "LuaEngine opens");
        check(lua.runFile(script.string()), "script loads");
        check(lua.lastError().empty() && lua.lastTraceback().empty(), "successful runFile leaves no error behind");

        lua_State* L = lua.state();
        const int top = lua_gettop(L);
        check(!lua.callOnInputReleased(20), "DL2-shaped callback fails (relative path doesn't exist)");
        check(lua_gettop(L) == top, "Lua stack exactly balanced after a failed call (handler removed)");
        const std::string& err = lua.lastError();
        check(err.rfind("onInputReleased: ", 0) == 0 && err.find("bad argument #1 to 'output'") != std::string::npos,
              "lastError() keeps its historical one-line form: " + err);
        check(err.find('\n') == std::string::npos, "lastError() is a single line (traceback split off)");
        const std::string& tb = lua.lastTraceback();
        check(tb.rfind("stack traceback:", 0) == 0, "lastTraceback() starts with 'stack traceback:'");
        check(tb.find("writeSavePlus") != std::string::npos && tb.find("writeSave'") != std::string::npos,
              "call chain shows writeSave -> writeSavePlus (the part that was missing before)");
        std::printf("---- captured traceback ----\n%s\n----------------------------\n", tb.c_str());

        bool updated = false;
        check(lua.callOnOverlayUpdate(updated) && updated,
              "callOnOverlayUpdate still reads its return value correctly with the handler in place");
        check(lua_gettop(L) == top, "Lua stack balanced after a successful call with a return value");
        check(lua.lastError().empty() && lua.lastTraceback().empty(),
              "a successful call clears the previous error (it used to linger)");

        lua.callOnInputReleased(20); // put an error back in lastError()
        lua_pushnil(L);
        lua_setglobal(L, "onOverlayUpdate"); // now undefined
        check(!lua.callOnOverlayUpdate(updated) && lua.lastError().empty(),
              "undefined optional hook: returns false with an EMPTY lastError() (no stale re-report)");

        // Non-string error object -> readable placeholder, not a lost error.
        std::ofstream(script) << "function onInputPressed(w) error({code = 1}) end\n";
        check(lua.runFile(script.string()), "second script loads");
        const bool failed = !lua.callOnInputPressed(1); // evaluated first: the label below reads lastError()
        check(failed && lua.lastError().find("error object is a table value") != std::string::npos,
              "error({...}) reported as a readable placeholder: " + lua.lastError());
        check(lua_gettop(L) == top, "stack still balanced after a non-string error");
    }

    fs::remove_all(dir, ec);
    std::printf(g_allPass ? "\nALL PASS\n" : "\nSOME FAILED\n");
    return g_allPass ? 0 : 1;
}
