// Unit test for the per-game Custom/ override on sound files (see
// customOverridePath() in src/lua/api_singe.cpp) -- Cyril, 2026-09-24:
// "vu que maintenant on a la fonctionalite /custom peut etre qu'on
// pourrait aussi mettre les sons dedans". Custom/ already existed for
// Overlay sprites and Fonts (KimmyScript-side, main.kimmy's
// fillOVL()/getFont()); this is the C++-side equivalent for soundLoad(),
// added because sound has no shared KimmyScript entry point every game
// goes through the way Overlay/Fonts do (see customOverridePath()'s own
// comment for the full reasoning).
//
// Drives the REAL l_soundLoad() through the real Lua registration
// (registerSingeApi()) with a real SDL3_mixer mixer and real, valid WAV
// files on disk -- not a reimplementation of the path logic in the test
// itself. What actually got loaded is read back from the exact same
// diagnostic line production code prints (see l_soundLoad's "overridden
// by"/"not found -- using" fprintf calls), captured via freopen(stderr)
// into a real file and grepped back -- the same signal Cyril would see in
// his own dirk_log.txt.
#include "core/EngineContext.h"
#include "lua/api_singe.h"

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
}

#include <SDL3/SDL.h>
#include <SDL3_mixer/SDL_mixer.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <io.h>
#define DIRK_DUP _dup
#define DIRK_DUP2 _dup2
#else
#include <unistd.h>
#define DIRK_DUP dup
#define DIRK_DUP2 dup2
#endif

static bool g_allPass = true;

static void check(bool cond, const std::string& what) {
    if (cond) {
        std::printf("PASS %s\n", what.c_str());
    } else {
        std::printf("FAIL %s\n", what.c_str());
        g_allPass = false;
    }
}

// Minimal valid 16-bit PCM mono WAV -- a handful of silent samples is
// plenty for MIX_LoadAudio to succeed; the test cares which PATH got
// loaded, not what the audio sounds like.
static void writeTinyWav(const std::filesystem::path& path) {
    std::filesystem::create_directories(path.parent_path());
    constexpr uint32_t kSampleRate = 8000;
    constexpr uint16_t kNumSamples = 200;
    constexpr uint32_t kDataBytes = kNumSamples * 2;

    std::ofstream f(path, std::ios::binary);
    auto writeU32 = [&](uint32_t v) { f.write(reinterpret_cast<const char*>(&v), 4); };
    auto writeU16 = [&](uint16_t v) { f.write(reinterpret_cast<const char*>(&v), 2); };

    f.write("RIFF", 4);
    writeU32(36 + kDataBytes);
    f.write("WAVE", 4);
    f.write("fmt ", 4);
    writeU32(16);
    writeU16(1);              // PCM
    writeU16(1);              // mono
    writeU32(kSampleRate);
    writeU32(kSampleRate * 2); // byte rate (mono, 16-bit)
    writeU16(2);               // block align
    writeU16(16);              // bits per sample
    f.write("data", 4);
    writeU32(kDataBytes);
    for (int i = 0; i < kNumSamples; ++i) writeU16(0); // silence
}

// Reads the whole file at `path` (the redirected stderr capture) as text.
static std::string readAll(const std::filesystem::path& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

int main() {
    if (!SDL_Init(SDL_INIT_AUDIO)) {
        std::fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!MIX_Init()) {
        std::fprintf(stderr, "FAIL: MIX_Init: %s\n", SDL_GetError());
        return 1;
    }
    MIX_Mixer* mixer = MIX_CreateMixerDevice(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, nullptr);
    check(mixer != nullptr, "MIX_CreateMixerDevice (dummy driver)");

    lua_State* L = luaL_newstate();
    luaL_openlibs(L);

    dirk::EngineContext ctx;
    ctx.soundMixer = mixer;
    dirk::registerSingeApi(L, &ctx);

    namespace fs = std::filesystem;
    fs::path root = fs::temp_directory_path() / "dirk_custom_sound_override_test";
    std::error_code ec;
    fs::remove_all(root, ec);
    fs::create_directories(root, ec);

    // A file to redirect this PROCESS's stderr into for the duration of
    // each soundLoad() call, so the check below reads exactly what
    // l_soundLoad() itself printed -- not a copy of its logic.
    fs::path logPath = root / "stderr.log";

    // Saved real stderr fd, restored after each capture -- dup/dup2 rather
    // than freopen("/dev/tty", ...), since this runs headless under ctest
    // with no controlling terminal to reopen.
    int savedStderrFd = DIRK_DUP(fileno(stderr));
    check(savedStderrFd >= 0, "saved the real stderr fd for later restoration");

    auto callSoundLoad = [&](const std::string& path) -> std::string {
        std::fflush(stderr);
        FILE* redirected = std::freopen(logPath.string().c_str(), "w", stderr);
        (void)redirected;
        std::string lua = "return soundLoad(\"" + path + "\")";
        luaL_dostring(L, lua.c_str());
        std::fflush(stderr);
        DIRK_DUP2(savedStderrFd, fileno(stderr)); // restore stderr to the real console
        return readAll(logPath);
    };

    // -----------------------------------------------------------------
    // 1) Custom/ present alongside the game's own Sounds/ file: the
    //    override must win, and the diagnostic log must say so.
    // -----------------------------------------------------------------
    {
        fs::path game = root / "game1";
        writeTinyWav(game / "Sounds" / "right.wav");
        writeTinyWav(game / "Custom" / "right.wav");

        std::string log = callSoundLoad((game / "Sounds" / "right.wav").string());
        check(log.find("overridden by") != std::string::npos &&
              log.find((game / "Custom" / "right.wav").string()) != std::string::npos,
              "Custom/right.wav overrides Sounds/right.wav when both exist (log confirms which path loaded)");
    }

    // -----------------------------------------------------------------
    // 2) No Custom/ folder at all (the common, unchanged case): the
    //    literal Sounds/ file loads normally, no override logged --
    //    confirms a game that never uses Custom/ sees no behavior change.
    // -----------------------------------------------------------------
    {
        fs::path game = root / "game2";
        writeTinyWav(game / "Sounds" / "right.wav");

        std::string log = callSoundLoad((game / "Sounds" / "right.wav").string());
        check(log.find("overridden by") == std::string::npos,
              "no Custom/ folder: literal Sounds/right.wav loads with no override logged");
    }

    // -----------------------------------------------------------------
    // 3) Custom/ exists but does NOT contain this particular file: falls
    //    through to the game's own Sounds/ file untouched, same as any
    //    other missing-from-Custom/ asset.
    // -----------------------------------------------------------------
    {
        fs::path game = root / "game3";
        writeTinyWav(game / "Sounds" / "right.wav");
        writeTinyWav(game / "Custom" / "some_other_file.wav"); // Custom/ dir exists, just not for "right.wav"

        std::string log = callSoundLoad((game / "Sounds" / "right.wav").string());
        check(log.find("overridden by") == std::string::npos,
              "Custom/ exists but has no right.wav of its own: falls through to Sounds/right.wav, no override logged");
    }

    // -----------------------------------------------------------------
    // 4) Nothing anywhere (no Custom/, no Sounds/ file, no skin/default
    //    fallback either): must not crash, handle stays silent, and the
    //    ordinary "failed" warning still fires -- confirms this change
    //    didn't disturb the pre-existing total-failure path.
    // -----------------------------------------------------------------
    {
        fs::path game = root / "game4"; // Sounds/ folder deliberately never created

        std::string log = callSoundLoad((game / "Sounds" / "right.wav").string());
        check(log.find("overridden by") == std::string::npos &&
              log.find("warning: soundLoad") != std::string::npos,
              "nothing anywhere: no crash, ordinary failure warning still fires, no bogus override claim");
    }

    std::printf(g_allPass ? "\nALL TESTS PASSED\n" : "\nSOME TESTS FAILED\n");
    return g_allPass ? 0 : 1;
}
