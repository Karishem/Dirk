#include "core/GameArchive.h"

#include "third_party/miniz/miniz.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <regex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace dirk {

namespace {

std::string lowerAscii(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// Windows file names are case-insensitive: "Cfg" and "CFG" are one folder.
#ifdef _WIN32
std::string keyOf(const std::string& s) { return lowerAscii(s); }
#else
std::string keyOf(const std::string& s) { return s; }
#endif

fs::path absoluteNormal(const std::string& p) {
    std::error_code ec;
    fs::path a = fs::absolute(fs::path(p), ec);
    if (ec) a = fs::path(p);
    return a.lexically_normal();
}

// Components of an absolute, normalized path, empty ones dropped (a
// trailing separator gives an empty last one).
std::vector<std::string> parts(const fs::path& p) {
    std::vector<std::string> out;
    for (const auto& c : p) {
        std::string s = c.string();
        if (s.empty() || s == "/" || s == "\\") continue;
        out.push_back(s);
    }
    return out;
}

bool onDisk(const fs::path& p, bool* isDir = nullptr, uint64_t* size = nullptr) {
    std::error_code ec;
    const auto st = fs::status(p, ec);
    if (ec || st.type() == fs::file_type::not_found) return false;
    if (isDir) *isDir = st.type() == fs::file_type::directory;
    if (size) {
        *size = 0;
        if (st.type() == fs::file_type::regular) {
            const auto s = fs::file_size(p, ec);
            if (!ec) *size = static_cast<uint64_t>(s);
        }
    }
    return true;
}

// A zip entry name made safe, or "" to skip it: '\' taken as a separator,
// no absolute path, no drive, no ".." (a crafted zip must not reach
// outside the game's folder), no macOS metadata.
std::string safeEntryName(std::string name) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name[0] == '/' || name.find(':') != std::string::npos) return "";
    std::string out;
    std::stringstream ss(name);
    std::string comp;
    while (std::getline(ss, comp, '/')) {
        if (comp.empty() || comp == ".") continue;
        if (comp == "..") return "";
        if (comp == "__MACOSX" || comp == ".DS_Store") return "";
        out += (out.empty() ? "" : "/") + comp;
    }
    return out;
}

uint64_t fnv1a(const std::string& s) {
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s) { h ^= c; h *= 1099511628211ULL; }
    return h;
}

} // namespace

struct GameArchive::Impl {
    bool active = false;
    std::string zdirk, real, script;
    size_t copied = 0;

    mutable mz_zip_archive zip{};
    bool zipOpen = false;
    mutable std::mutex mu; // miniz reading is not thread-safe

    struct Entry { mz_uint index; uint64_t size; std::string name; };
    std::unordered_map<std::string, Entry> files;                 // key of the path -> entry
    std::unordered_set<std::string> dirs;                         // keys of every folder ("" = root)
    std::map<std::string, std::vector<std::string>> children;     // folder key -> names in it

    mutable std::unordered_map<std::string, std::shared_ptr<const std::vector<unsigned char>>> shared;
    fs::path tempRoot;
    std::vector<std::string> realKeys; // keyOf() of each component of `real`
    std::string hiddenKey;             // the launching shortcut: not a game file ("" = none)

    // Key of `path` relative to the game's folder; false if outside it.
    bool relKey(const std::string& path, std::string& key, fs::path* rel = nullptr) const {
        if (!active) return false;
        const auto p = parts(absoluteNormal(path));
        const auto& r = realKeys;
        if (p.size() < r.size()) return false;
        for (size_t i = 0; i < r.size(); ++i) if (keyOf(p[i]) != r[i]) return false;
        key.clear();
        if (rel) rel->clear();
        for (size_t i = r.size(); i < p.size(); ++i) {
            key += (key.empty() ? "" : "/") + keyOf(p[i]);
            if (rel) *rel /= p[i];
        }
        return true;
    }

    // On disk -- the launching shortcut doesn't count (it bears the name of
    // the script it points to, which must come from the zip).
    bool disk(const std::string& path, bool* isDir = nullptr, uint64_t* size = nullptr) const {
        if (!hiddenKey.empty()) {
            std::string key;
            if (relKey(path, key) && key == hiddenKey) return false;
        }
        return onDisk(path, isDir, size);
    }

    const Entry* zipFile(const std::string& path) const {
        std::string key;
        if (!relKey(path, key)) return nullptr;
        const auto it = files.find(key);
        return it == files.end() ? nullptr : &it->second;
    }

    bool extract(const Entry& e, std::vector<unsigned char>& out) const {
        std::lock_guard<std::mutex> lock(mu);
        out.resize(static_cast<size_t>(e.size));
        if (e.size == 0) return true;
        return mz_zip_reader_extract_to_mem(&zip, e.index, out.data(), out.size(), 0) != 0;
    }

    void reset() {
        if (zipOpen) mz_zip_reader_end(&zip);
        zip = mz_zip_archive{};
        zipOpen = false;
        if (!tempRoot.empty()) {
            std::error_code ec;
            fs::remove_all(tempRoot, ec);
        }
        active = false;
        zdirk.clear(); real.clear(); script.clear();
        copied = 0;
        files.clear(); dirs.clear(); children.clear(); shared.clear();
        tempRoot.clear();
        realKeys.clear();
        hiddenKey.clear();
    }
};

GameArchive::GameArchive() : impl_(new Impl) {}
GameArchive::~GameArchive() { impl_->reset(); }

bool hasZdirkExtension(const std::string& path) {
    return lowerAscii(fs::path(path).extension().string()) == ".zdirk";
}

GameArchive& gameArchive() {
    static GameArchive instance;
    return instance;
}

bool GameArchive::active() const { return impl_->active; }
const std::string& GameArchive::zdirkPath() const { return impl_->zdirk; }
const std::string& GameArchive::realFolder() const { return impl_->real; }
const std::string& GameArchive::scriptPath() const { return impl_->script; }
size_t GameArchive::fileCount() const { return impl_->files.size(); }
size_t GameArchive::cfgFilesCopied() const { return impl_->copied; }

void GameArchive::close() { impl_->reset(); }

bool GameArchive::open(const std::string& zdirkPathIn, std::string& error,
                       const std::string& wantedScript, const std::string& shortcutPath) {
    Impl& m = *impl_;
    m.reset();
    const fs::path zdirk = absoluteNormal(zdirkPathIn);
    std::error_code ec;
    if (!fs::is_regular_file(zdirk, ec)) {
        error = zdirk.string() + ": file not found";
        return false;
    }
    if (!mz_zip_reader_init_file(&m.zip, zdirk.string().c_str(), 0)) {
        error = zdirk.string() + ": not a readable zip file (" + mz_zip_get_error_string(mz_zip_get_last_error(&m.zip)) + ")";
        m.zip = mz_zip_archive{};
        return false;
    }
    m.zipOpen = true;

    // 1. The zip's directory.
    struct Raw { mz_uint index; std::string name; bool dir; uint64_t size; };
    std::vector<Raw> raw;
    const mz_uint count = mz_zip_reader_get_num_files(&m.zip);
    for (mz_uint i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&m.zip, i, &st)) continue;
        const std::string name = safeEntryName(st.m_filename);
        if (name.empty()) continue;
        raw.push_back({i, name, mz_zip_reader_is_file_a_directory(&m.zip, i) != 0, static_cast<uint64_t>(st.m_uncomp_size)});
    }

    // 2. One top folder around everything ("Timegal/Timegal.dirk"...)? Strip it.
    std::string prefix;
    {
        std::string first;
        bool single = !raw.empty();
        for (const auto& e : raw) {
            const size_t slash = e.name.find('/');
            const std::string top = slash == std::string::npos ? e.name : e.name.substr(0, slash);
            if (slash == std::string::npos && !e.dir) { single = false; break; } // a file at the root
            if (first.empty()) first = top;
            else if (keyOf(first) != keyOf(top)) { single = false; break; }
        }
        if (single && !first.empty()) prefix = first + "/";
    }

    // 3. Index: files, folders, what each folder holds.
    m.dirs.insert("");
    std::map<std::string, std::set<std::string>> seen; // folder key -> child keys already listed
    auto addChild = [&](const std::string& dirKey, const std::string& name) {
        if (seen[dirKey].insert(keyOf(name)).second) m.children[dirKey].push_back(name);
    };
    std::vector<std::string> rootScripts;
    for (const auto& e : raw) {
        std::string name = e.name;
        if (!prefix.empty()) name = name.size() > prefix.size() ? name.substr(prefix.size()) : "";
        if (name.empty()) continue;
        // every parent folder
        std::string dirKey;
        size_t start = 0;
        while (true) {
            const size_t slash = name.find('/', start);
            if (slash == std::string::npos) break;
            const std::string comp = name.substr(start, slash - start);
            addChild(dirKey, comp);
            dirKey += (dirKey.empty() ? "" : "/") + keyOf(comp);
            m.dirs.insert(dirKey);
            start = slash + 1;
        }
        const std::string leaf = name.substr(start);
        const std::string key = (dirKey.empty() ? "" : dirKey + "/") + keyOf(leaf);
        if (e.dir) {
            addChild(dirKey, leaf);
            m.dirs.insert(key);
            continue;
        }
        addChild(dirKey, leaf);
        m.files[key] = {e.index, e.size, name};
        if (dirKey.empty()) rootScripts.push_back(name);
    }

    // 4. The script to run.
    std::string script;
    if (!wantedScript.empty()) {
        for (const auto& f : rootScripts) {
            if (keyOf(f) == keyOf(wantedScript)) { script = f; break; }
        }
        if (script.empty()) {
            std::string list;
            for (const auto& f : rootScripts) {
                const std::string ext = lowerAscii(fs::path(f).extension().string());
                if (ext == ".dirk" || ext == ".singe") list += (list.empty() ? "" : ", ") + f;
            }
            error = zdirk.string() + ": no " + wantedScript + " at the top of the zip (it has: " +
                    (list.empty() ? std::string("no script") : list) + ")";
            m.reset();
            return false;
        }
    }
    for (const char* ext : {".dirk", ".singe"}) {
        if (!script.empty()) break;
        std::vector<std::string> found;
        for (const auto& f : rootScripts) {
            if (lowerAscii(fs::path(f).extension().string()) == ext) found.push_back(f);
        }
        for (const auto& f : found) {
            if (keyOf(fs::path(f).stem().string()) == keyOf(zdirk.stem().string())) { script = f; break; }
        }
        if (script.empty() && found.size() == 1) script = found.front();
        if (!script.empty()) break;
        if (found.size() > 1) {
            error = zdirk.string() + ": several " + ext + " scripts and none named " + zdirk.stem().string() + ext;
            m.reset();
            return false;
        }
    }
    if (script.empty()) {
        error = zdirk.string() + ": no .dirk (or .singe) script at the top of the zip";
        m.reset();
        return false;
    }

    m.zdirk = zdirk.string();
    {
        std::string real = zdirk.parent_path().string();
        if (real.empty() || (real.back() != '/' && real.back() != '\\')) real += static_cast<char>(fs::path::preferred_separator);
        m.real = real;
        for (const auto& c : parts(absoluteNormal(real))) m.realKeys.push_back(keyOf(c));
    }
    m.script = (zdirk.parent_path() / script).string();
    m.active = true; // relKey() needs it
    if (!shortcutPath.empty()) {
        std::string key;
        if (m.relKey(shortcutPath, key)) m.hiddenKey = key;
    }
    m.active = false;
    char hash[17];
    std::snprintf(hash, sizeof hash, "%016llx", static_cast<unsigned long long>(fnv1a(keyOf(m.zdirk))));
    m.tempRoot = fs::temp_directory_path(ec) / "dirk-zdirk" / hash;
    fs::remove_all(m.tempRoot, ec); // leftovers of a run that didn't exit normally
    m.active = true;

    // 5. The game's writable files: the zip's Cfg/ ones missing on disk.
    for (const auto& kv : m.files) {
        const std::string& name = kv.second.name;
        if (keyOf(name.substr(0, 4)) != keyOf("Cfg/")) continue;
        const fs::path target = zdirk.parent_path() / fs::path(name);
        if (onDisk(target)) continue;
        std::vector<unsigned char> bytes;
        if (!m.extract(kv.second, bytes)) continue;
        std::error_code cec;
        fs::create_directories(target.parent_path(), cec);
        std::ofstream out(target, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (out) ++m.copied;
    }
    return true;
}

bool GameArchive::archiveHasFile(const std::string& path) const {
    return impl_->zipFile(path) != nullptr && !impl_->disk(path);
}

bool GameArchive::readArchiveFile(const std::string& path, std::vector<unsigned char>& out) const {
    if (impl_->disk(path)) return false;
    const Impl::Entry* e = impl_->zipFile(path);
    return e && impl_->extract(*e, out);
}

std::shared_ptr<const std::vector<unsigned char>> GameArchive::sharedArchiveFile(const std::string& path) const {
    if (impl_->disk(path)) return nullptr;
    std::string key;
    if (!impl_->relKey(path, key)) return nullptr;
    const auto f = impl_->files.find(key);
    if (f == impl_->files.end()) return nullptr;
    {
        std::lock_guard<std::mutex> lock(impl_->mu);
        const auto it = impl_->shared.find(key);
        if (it != impl_->shared.end()) return it->second;
    }
    auto bytes = std::make_shared<std::vector<unsigned char>>();
    if (!impl_->extract(f->second, *bytes)) return nullptr;
    std::lock_guard<std::mutex> lock(impl_->mu);
    impl_->shared[key] = bytes;
    return bytes;
}

bool GameArchive::exists(const std::string& path, bool* isDirectory, uint64_t* size) const {
    if (impl_->disk(path, isDirectory, size)) return true;
    std::string key;
    if (!impl_->relKey(path, key)) return false;
    const auto f = impl_->files.find(key);
    if (f != impl_->files.end()) {
        if (isDirectory) *isDirectory = false;
        if (size) *size = f->second.size;
        return true;
    }
    if (impl_->dirs.count(key)) {
        if (isDirectory) *isDirectory = true;
        if (size) *size = 0;
        return true;
    }
    return false;
}

bool GameArchive::listDirectory(const std::string& path, std::vector<std::string>& names) const {
    names.clear();
    bool any = false;
    std::set<std::string> seen;
    std::error_code ec;
    if (fs::is_directory(path, ec)) {
        any = true;
        for (auto it = fs::directory_iterator(path, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string n = it->path().filename().string();
            if (seen.insert(keyOf(n)).second) names.push_back(n);
        }
    }
    std::string key;
    if (impl_->relKey(path, key) && impl_->dirs.count(key)) {
        any = true;
        const auto c = impl_->children.find(key);
        if (c != impl_->children.end()) {
            for (const auto& n : c->second) if (seen.insert(keyOf(n)).second) names.push_back(n);
        }
    }
    return any;
}

std::string GameArchive::resolveRead(const std::string& path) const {
    fs::path rel;
    std::string key;
    if (!impl_->relKey(path, key, &rel) || impl_->disk(path)) return path;
    const auto f = impl_->files.find(key);
    if (f == impl_->files.end()) return path;
    const fs::path copy = impl_->tempRoot / rel;
    if (!onDisk(copy)) {
        std::vector<unsigned char> bytes;
        if (!impl_->extract(f->second, bytes)) return path;
        std::error_code ec;
        fs::create_directories(copy.parent_path(), ec);
        std::ofstream out(copy, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    }
    return copy.string();
}

std::string GameArchive::resolveWrite(const std::string& path) const {
    fs::path rel;
    std::string key;
    if (!impl_->relKey(path, key, &rel) || !rel.has_parent_path()) return path;
    const fs::path parent = fs::path(path).parent_path();
    const size_t slash = key.rfind('/');
    const std::string parentKey = slash == std::string::npos ? "" : key.substr(0, slash);
    if (!onDisk(parent) && impl_->dirs.count(parentKey)) {
        std::error_code ec;
        fs::create_directories(parent, ec);
    }
    return path;
}

bool readZdirkShortcut(const std::string& dirkPath, std::string& zdirk, std::string& script) {
    std::error_code ec;
    const auto size = fs::file_size(dirkPath, ec);
    if (ec || size > 4096) return false; // a real game script is far bigger
    std::ifstream in(dirkPath, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string text = ss.str();
    static const std::regex zdirkLine(R"((^|\n)[ \t]*zdirk[ \t]*=[ \t]*["']([^"'\r\n]+)["'])");
    static const std::regex scriptLine(R"((^|\n)[ \t]*script[ \t]*=[ \t]*["']([^"'\r\n]+)["'])");
    std::smatch m;
    if (!std::regex_search(text, m, zdirkLine)) return false;
    const fs::path folder = absoluteNormal(dirkPath).parent_path();
    fs::path target = fs::path(m[2].str());
    zdirk = (target.is_absolute() ? target : folder / target).lexically_normal().string();
    script = std::regex_search(text, m, scriptLine) ? m[2].str() : fs::path(dirkPath).filename().string();
    return true;
}

} // namespace dirk
