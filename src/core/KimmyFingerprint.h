#pragma once

#include <string>

namespace dirk {

// Which KimmyScript a run actually used (2026-09-27, Cyril: several copies
// of KimmyScript/ on one machine -- next to dirk.exe, next to the games,
// installer/payload/, a work folder -- and no way to tell from a log which
// one a given run read). Returns, for <sharedRoot>/KimmyScript/Script:
//
//   KimmyScript used: <absolute folder>
//     52351419 addons.kimmy
//     b8b7b6e1 framework.kimmy
//     ...
//
// one line per *.kimmy / *.singe file, sorted by name, each prefixed with
// the first 8 hex digits of its MD5 -- the same form as
// `md5sum *.kimmy | cut -c1-8,33-`, which is what every KimmyScript
// delivery's LISEZMOI lists, so the two compare line for line. If the
// Script folder is missing, says so on the second line instead. No
// trailing newline. Reads ~0.5 MB for a full KimmyScript: a few ms, once
// per launch.
std::string describeKimmyScript(const std::string& sharedRoot);

// First 8 hex digits of the MD5 of the file at `path`, or "????????" if it
// can't be read. Exposed for the unit test.
std::string shortFileMd5(const std::string& path);

} // namespace dirk
