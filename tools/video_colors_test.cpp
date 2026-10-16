// Regression test for review point 8 (2026-09-26): every movie must be
// displayed in the colour matrix + range it was encoded with, whoever made
// it -- not SDL3's one-size-fits-all default (BT.601 limited), which shows
// HD/BT.709 material with shifted hues (measured before this fix: pure red
// displayed as RGB(233,0,2), pure green as (21,255,10)).
//
// End to end, per clip: real encode (ffmpeg, see below) -> VideoSource's
// detection -> createVideoTexture() -> real render (SDL software renderer,
// dummy video driver) -> the pixel actually displayed, read back.
//
// Clips (assets/, tiny, generated with ffmpeg + libx264/libvpx-vp9):
//   color_red_bt709.mp4          pure red, BT.709, limited, TAGGED bt709
//   color_red_bt601.mp4          pure red, BT.601, limited, TAGGED smpte170m
//   color_red_untagged_hd.mp4    pure red, BT.709, limited, NO tag, 1280x720
//                                (the common real-world case: must be
//                                guessed as BT.709 from the HD size)
//   color_grey200_fullrange_vp9.mkv  RGB(200,200,200), FULL range, VP9
//                                (a plain yuv420p frame tagged full range:
//                                its values reach the texture unconverted)
// plus assets/test_pattern.mp4 (64x64, untagged -> guessed BT.601 / SD).

#include "video/VideoSource.h"
#include "video/VideoTexture.h"

extern "C" {
#include <libavutil/frame.h>
}

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <string>

static bool g_allPass = true;

static void check(bool cond, const std::string& what) {
    std::printf("%s %s\n", cond ? "PASS" : "FAIL", what.c_str());
    if (!cond) g_allPass = false;
}

struct Rgb { int r, g, b; };

static bool near(Rgb a, Rgb b, int tol = 4) {
    return std::abs(a.r - b.r) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.b - b.b) <= tol;
}

static std::string str(Rgb c) {
    return "(" + std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + ")";
}

// Decodes frame 0 of `path`, draws it through createVideoTexture() and
// returns the colour of the displayed centre pixel.
static bool displayedColour(SDL_Renderer* r, const std::string& path, dirk::VideoSource& v, Rgb& out) {
    if (!v.open(path) || !v.seekToFrame(0) || !v.currentPicture()) return false;
    SDL_Texture* tex = dirk::createVideoTexture(r, v, v.width(), v.height());
    if (!tex) return false;
    const AVFrame* f = v.currentPicture();
    SDL_UpdateYUVTexture(tex, nullptr, f->data[0], f->linesize[0], f->data[1], f->linesize[1], f->data[2], f->linesize[2]);
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_RenderTexture(r, tex, nullptr, nullptr);
    SDL_Rect px{32, 32, 1, 1};
    SDL_Surface* s = SDL_RenderReadPixels(r, &px);
    Uint8 R = 0, G = 0, B = 0, A = 0;
    const bool ok = s && SDL_ReadSurfacePixel(s, 0, 0, &R, &G, &B, &A);
    if (s) SDL_DestroySurface(s);
    SDL_DestroyTexture(tex);
    out = Rgb{R, G, B};
    return ok;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <assets dir>\n", argv[0]);
        return 1;
    }
    const std::string dir = std::string(argv[1]) + "/";

    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("video_colors_test", 64, 64, 0);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    check(renderer != nullptr, "renderer created (dummy video driver)");
    if (!renderer) return 1;

    using M = dirk::VideoSource::YuvMatrix;
    struct Case {
        const char* file;
        M matrix;
        bool full;
        bool tagged;
        Rgb expected;
        const char* what;
    };
    const Case cases[] = {
        {"color_red_bt709.mp4", M::BT709, false, true, {255, 0, 0}, "BT.709-tagged red"},
        {"color_red_bt601.mp4", M::BT601, false, true, {255, 0, 0}, "BT.601-tagged red"},
        {"color_red_untagged_hd.mp4", M::BT709, false, false, {255, 0, 0}, "untagged HD red (guessed BT.709)"},
        {"color_grey200_fullrange_vp9.mkv", M::BT709, true, true, {200, 200, 200}, "full-range VP9 grey 200"},
    };
    for (const Case& c : cases) {
        dirk::VideoSource v;
        Rgb got{-1, -1, -1};
        const bool ok = displayedColour(renderer, dir + c.file, v, got);
        check(ok, std::string(c.what) + ": opened, decoded and drawn");
        if (!ok) continue;
        check(v.yuvMatrix() == c.matrix && v.yuvFullRange() == c.full && v.yuvMatrixTagged() == c.tagged,
              std::string(c.what) + ": detected as " + dirk::describeVideoColors(v));
        check(near(got, c.expected),
              std::string(c.what) + ": displayed " + str(got) + ", expected " + str(c.expected));
    }

    // SD, untagged: stays on BT.601 (the SD convention), i.e. exactly what
    // it got before this change.
    {
        dirk::VideoSource v;
        check(v.open(dir + "test_pattern.mp4"), "test_pattern.mp4 opens");
        check(v.yuvMatrix() == M::BT601 && !v.yuvFullRange() && !v.yuvMatrixTagged(),
              "untagged SD 64x64 -> guessed BT.601 limited (unchanged from before): " + dirk::describeVideoColors(v));
    }

    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    std::printf(g_allPass ? "\nALL PASS\n" : "\nSOME FAILED\n");
    return g_allPass ? 0 : 1;
}
