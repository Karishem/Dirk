#pragma once

// ScriptErrorLog -- the always-on <game>/dirk_error.txt (2026-09-26, code
// review point 2, approved by Cyril: "vas y pour les 3").
//
// Why it exists: a Lua error inside a callback (onInputPressed,
// onOverlayUpdate, ...) is caught by lua_pcall so the engine doesn't
// crash, but the game then keeps running half-broken. Until now the only
// trace was one stderr line -- which goes nowhere at all on a cabinet with
// the console hidden and "Generate Log" off. That's exactly how the DL2
// s4p.cfg save bug (MYDIR relative path) stayed invisible: no message,
// the game just silently ignored inputs afterwards.
//
// What it is NOT: a general log. dirk_log.txt ("Generate Log", opt-in)
// still captures everything (every key, every sound) and stays opt-in.
// This file only ever receives SCRIPT ERRORS, which are rare, so it's
// always on -- and a normal, error-free session never creates it at all.
//
// Cost, by design (Cyril asked: "ce n'est pas couteux en memoire ?"):
//   - Each DISTINCT error is written once, in full, with its Lua call
//     chain. An error that repeats every frame is only COUNTED after that
//     (one map lookup + an increment), never rewritten.
//   - Repeat counts are appended as a short summary at shutdown.
//   - Hard caps: kMaxBytes on the file (further new errors are then only
//     counted), kMaxDistinct on how many different messages are tracked
//     in memory (anything past that goes into one overflow counter).
//     Worst case memory is a few hundred short strings.
//   - The file is removed at startup, so "dirk_error.txt exists" always
//     means "errors happened during the LAST run", never a stale leftover.
//
// Deliberately free of SDL/Lua so it can be unit-tested on its own (see
// tools/script_error_log_test.cpp); main.cpp feeds it LuaEngine's
// lastError()/lastTraceback() and SDL_GetTicks().

#include <cstdint>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace dirk {

class ScriptErrorLog {
public:
    static constexpr size_t kMaxBytes = 100 * 1024;
    static constexpr size_t kMaxDistinct = 200;
    static constexpr size_t kMaxSummaryLines = 50;

    // `path` empty = no file at all (e.g. no game script given); errors are
    // still counted so report()'s return value keeps working for the
    // on-screen banner. Deletes any existing file at `path`. `header` is
    // written as the file's first line if (and only if) an error ever
    // gets written.
    void begin(const std::string& path, const std::string& header);

    // `message`: the one-line "context: file:line: text" form (LuaEngine::
    // lastError()); it is also the dedup key. `traceback`: LuaEngine::
    // lastTraceback(), may be empty. `elapsedMs`: time since engine start,
    // printed as hh:mm:ss so an entry can be matched to a moment in play.
    // Returns true only the FIRST time this exact message is seen this
    // run (the caller uses that to show the on-screen banner once, not
    // every frame).
    bool report(const std::string& message, const std::string& traceback, uint64_t elapsedMs);

    // Appends the repeat-count summary (only for messages seen more than
    // once, plus the overflow count if any) and closes the file.
    // Idempotent. Writes nothing -- and creates no file -- if no error was
    // ever reported.
    void finish();

    size_t distinctCount() const { return counts_.size(); }
    uint64_t totalCount() const { return total_; }
    bool fileWritten() const { return opened_; }

private:
    bool ensureOpen();
    void writeRaw(const std::string& text);

    std::string path_;
    std::string header_;
    std::ofstream out_;
    bool opened_ = false;
    bool finished_ = false;
    bool capNoticeWritten_ = false;
    size_t bytesWritten_ = 0;
    uint64_t total_ = 0;
    uint64_t overflow_ = 0; // occurrences of messages past kMaxDistinct
    std::map<std::string, uint64_t> counts_;
    std::vector<std::string> order_; // first-seen order, for a stable summary
};

// Shortens a lastError()-style message for the on-screen banner: drops the
// directory part of the script path so the useful bit fits on one line,
// e.g.
//   "onInputReleased: E:\Dirk Games\DL2e\/Script/addons.kimmy:314: bad argument ..."
//   -> "onInputReleased: addons.kimmy:314: bad argument ..."
// then caps the result at `maxChars` (adding "..."). Leaves a message it
// can't find a "<file>.<ext>:<line>:" in untouched (apart from the cap).
std::string shortenErrorForDisplay(const std::string& message, size_t maxChars = 110);

} // namespace dirk
