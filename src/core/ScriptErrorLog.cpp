#include "core/ScriptErrorLog.h"

#include <cctype>
#include <cstdio>

namespace dirk {

namespace {

std::string formatElapsed(uint64_t ms) {
    const uint64_t totalSec = ms / 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02llu:%02llu:%02llu",
                  static_cast<unsigned long long>(totalSec / 3600),
                  static_cast<unsigned long long>((totalSec / 60) % 60),
                  static_cast<unsigned long long>(totalSec % 60));
    return buf;
}

} // namespace

void ScriptErrorLog::begin(const std::string& path, const std::string& header) {
    path_ = path;
    header_ = header;
    if (!path_.empty()) std::remove(path_.c_str()); // see header: never a stale leftover
}

bool ScriptErrorLog::ensureOpen() {
    if (opened_) return out_.is_open();
    if (path_.empty()) return false;
    opened_ = true; // one attempt only -- an unwritable folder shouldn't be retried every error
    out_.open(path_, std::ios::out | std::ios::trunc);
    if (!out_) return false;
    if (!header_.empty()) writeRaw(header_ + "\n\n");
    return true;
}

void ScriptErrorLog::writeRaw(const std::string& text) {
    out_ << text;
    out_.flush(); // a cabinet can be switched off at any moment -- don't sit on buffered errors
    bytesWritten_ += text.size();
}

bool ScriptErrorLog::report(const std::string& message, const std::string& traceback, uint64_t elapsedMs) {
    if (finished_) return false;
    ++total_;

    auto it = counts_.find(message);
    if (it != counts_.end()) {
        ++it->second; // a repeat: counted, never rewritten
        return false;
    }
    if (counts_.size() >= kMaxDistinct) {
        ++overflow_;
        return false;
    }
    counts_.emplace(message, 1);
    order_.push_back(message);

    std::string entry = "[" + formatElapsed(elapsedMs) + "] " + message + "\n";
    if (!traceback.empty()) {
        entry += traceback;
        if (entry.back() != '\n') entry += "\n";
    }
    entry += "\n";

    if (ensureOpen()) {
        if (bytesWritten_ + entry.size() <= kMaxBytes) {
            writeRaw(entry);
        } else if (!capNoticeWritten_) {
            capNoticeWritten_ = true;
            writeRaw("[dirk] dirk_error.txt size cap reached (" + std::to_string(kMaxBytes / 1024) +
                     " KB) -- further new errors are only counted, see the summary below.\n\n");
        }
    }
    return true;
}

void ScriptErrorLog::finish() {
    if (finished_) return;
    finished_ = true;
    if (total_ == 0 || !ensureOpen()) {
        if (out_.is_open()) out_.close();
        return;
    }

    std::vector<std::pair<std::string, uint64_t>> repeated;
    for (const auto& msg : order_) {
        uint64_t n = counts_[msg];
        if (n > 1) repeated.emplace_back(msg, n);
    }
    if (!repeated.empty() || overflow_ > 0) {
        std::string summary = "---- repeat counts for this run ----\n";
        size_t lines = 0;
        for (const auto& [msg, n] : repeated) {
            if (lines++ >= kMaxSummaryLines) {
                summary += "  (" + std::to_string(repeated.size() - kMaxSummaryLines) + " more not listed)\n";
                break;
            }
            summary += "  " + std::to_string(n) + " x  " + msg + "\n";
        }
        if (overflow_ > 0) {
            summary += "  " + std::to_string(overflow_) + " x  (other errors, past the " +
                       std::to_string(kMaxDistinct) + "-message tracking limit)\n";
        }
        writeRaw(summary); // not subject to kMaxBytes: bounded by kMaxSummaryLines itself
    }
    out_.close();
}

std::string shortenErrorForDisplay(const std::string& message, size_t maxChars) {
    std::string s = message;

    // Find "<name>.<ext>:<digits>:" -- the script position Lua puts in
    // front of every runtime error -- and cut the directory off <name>.
    // Only the first such position is touched.
    auto stripDirectory = [&s](const char* ext) -> bool {
        const size_t extLen = std::char_traits<char>::length(ext);
        for (size_t p = s.find(ext); p != std::string::npos; p = s.find(ext, p + 1)) {
            const size_t afterColon = p + extLen;
            if (afterColon >= s.size() || !std::isdigit(static_cast<unsigned char>(s[afterColon]))) continue;
            // Start of the path: just after the "label: " prefix if there
            // is one before it, else the start of the string.
            const size_t labelEnd = s.rfind(": ", p);
            const size_t pathStart = (labelEnd == std::string::npos) ? 0 : labelEnd + 2;
            const size_t lastSep = s.find_last_of("/\\", p);
            if (lastSep != std::string::npos && lastSep >= pathStart) {
                s.erase(pathStart, lastSep + 1 - pathStart);
            }
            return true;
        }
        return false;
    };
    for (const char* ext : {".kimmy:", ".singe:", ".dirk:", ".lua:"}) {
        if (stripDirectory(ext)) break;
    }

    if (maxChars >= 4 && s.size() > maxChars) {
        s.resize(maxChars - 3);
        s += "...";
    }
    return s;
}

} // namespace dirk
