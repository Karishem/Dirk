#include "core/KimmyFingerprint.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <vector>

extern "C" {
#include <libavutil/md5.h>
#include <libavutil/mem.h>
}

namespace dirk {

std::string shortFileMd5(const std::string& path) {
    std::ifstream in(std::filesystem::path(path), std::ios::binary);
    if (!in) return "????????";
    AVMD5* ctx = av_md5_alloc();
    if (!ctx) return "????????";
    av_md5_init(ctx);
    std::vector<char> buf(64 * 1024);
    while (in) {
        in.read(buf.data(), static_cast<std::streamsize>(buf.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) av_md5_update(ctx, reinterpret_cast<const uint8_t*>(buf.data()), static_cast<size_t>(got));
    }
    uint8_t digest[16];
    av_md5_final(ctx, digest);
    av_free(ctx);
    char hex[9];
    std::snprintf(hex, sizeof(hex), "%02x%02x%02x%02x", digest[0], digest[1], digest[2], digest[3]);
    return hex;
}

std::string describeKimmyScript(const std::string& sharedRoot) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path folder = fs::path(sharedRoot) / "KimmyScript";
    fs::path absFolder = fs::absolute(folder, ec);
    if (ec) absFolder = folder;
    absFolder = absFolder.lexically_normal();

    std::string out = "KimmyScript used: " + absFolder.string();
    const fs::path script = absFolder / "Script";
    if (!fs::is_directory(script, ec)) {
        out += "\n  (no Script folder there)";
        return out;
    }

    std::vector<std::string> names;
    for (fs::directory_iterator it(script, ec), end; !ec && it != end; it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        const std::string ext = it->path().extension().string();
        if (ext == ".kimmy" || ext == ".singe") names.push_back(it->path().filename().string());
    }
    std::sort(names.begin(), names.end());
    if (names.empty()) out += "\n  (Script folder is empty)";
    for (const std::string& n : names) {
        out += "\n  " + shortFileMd5((script / n).string()) + " " + n;
    }
    return out;
}

} // namespace dirk
