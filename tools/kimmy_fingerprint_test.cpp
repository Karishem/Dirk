// Unit test for describeKimmyScript()/shortFileMd5() (src/core/
// KimmyFingerprint.*): the "which KimmyScript did this run use" lines in
// dirk_log.txt and the dirk_error.txt header. Real files on disk; expected
// MD5s are the published RFC 1321 test vectors, not values computed by the
// code under test.
#include "core/KimmyFingerprint.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

static int failures = 0;
static void check(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "OK  " : "FAIL", what.c_str());
    if (!ok) ++failures;
}
static void writeFile(const fs::path& p, const std::string& content) {
    std::ofstream(p, std::ios::binary) << content;
}

int main() {
    const fs::path root = fs::temp_directory_path() / "dirk_kimmy_fingerprint_test";
    fs::remove_all(root);
    const fs::path script = root / "KimmyScript" / "Script";
    fs::create_directories(script);

    // RFC 1321: MD5("") = d41d8cd9..., MD5("abc") = 90015098...,
    // MD5("message digest") = f96b697d...
    writeFile(script / "main.kimmy", "abc");
    writeFile(script / "addons.kimmy", "");
    writeFile(script / "toolbox.singe", "message digest");
    writeFile(script / "notes.txt", "ignored");
    fs::create_directories(script / "sub.kimmy"); // a folder, not a file: ignored

    check(dirk::shortFileMd5((script / "main.kimmy").string()) == "90015098", "MD5 of \"abc\"");
    check(dirk::shortFileMd5((script / "addons.kimmy").string()) == "d41d8cd9", "MD5 of an empty file");
    check(dirk::shortFileMd5((script / "missing.kimmy").string()) == "????????", "unreadable file");

    // A file bigger than the 64 KB read buffer: 1,000,000 x 'a' is RFC-style
    // vector 7707d6ae4e027c70eea2a935c2296f21.
    writeFile(root / "big.bin", std::string(1000000, 'a'));
    check(dirk::shortFileMd5((root / "big.bin").string()) == "7707d6ae", "MD5 of a 1 MB file (several reads)");

    const std::string d = dirk::describeKimmyScript(root.string());
    const std::string expectedFolder = (root / "KimmyScript").lexically_normal().string();
    const std::string expected = "KimmyScript used: " + expectedFolder +
                                 "\n  d41d8cd9 addons.kimmy"
                                 "\n  90015098 main.kimmy"
                                 "\n  f96b697d toolbox.singe";
    check(d == expected, "full description (sorted, .kimmy/.singe only, same form as md5sum | cut)");
    if (d != expected) std::printf("got:\n%s\nexpected:\n%s\n", d.c_str(), expected.c_str());

    const fs::path noScript = root / "empty_root";
    fs::create_directories(noScript / "KimmyScript");
    const std::string d2 = dirk::describeKimmyScript(noScript.string());
    check(d2.find("(no Script folder there)") != std::string::npos, "missing Script folder is reported");

    fs::remove_all(root);
    std::printf("%s\n", failures == 0 ? "ALL OK" : "FAILURES");
    return failures == 0 ? 0 : 1;
}
