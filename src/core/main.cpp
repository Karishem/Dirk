// Dirk engine entry point -- Phase 1 milestone per the feasibility study
// (section 8): "Une fenetre, un fichier video qui se lit, un
// discSkipToFrame qui fonctionne, un sprite et du texte affiches par-dessus,
// une touche qui repond via onInputPressed."
//
// Usage: dirk <video-file> [script.lua]
//
// This is deliberately minimal: no audio/video sync clock yet (see
// feasibility study section 6 for that design), no SDL_GameController input
// yet (only keyboard, mapped 1:1 to SDL scancodes -- framework.singe's
// SCANCODE table is confirmed to match SDL2 scancode values), no real font
// rasterizer (see api_singe.cpp's fontPrint placeholder). It exists to prove
// the video decode -> overlay composite -> Lua callback pipeline works
// end to end before investing in those.

#include "video/VideoSource.h"
#include "lua/LuaEngine.h"
#include "lua/api_singe.h"
#include "core/EngineContext.h"

#include <SDL.h>
#include <SDL_mixer.h>

extern "C" {
#include <libavutil/frame.h>
}

#include <cstdio>
#include <string>

namespace {

std::string dirOf(const std::string& path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? "." : path.substr(0, slash);
}

void uploadFrameToTexture(SDL_Texture* tex, const AVFrame* f) {
    if (!tex || !f) return;
    SDL_UpdateYUVTexture(tex, nullptr,
                          f->data[0], f->linesize[0],
                          f->data[1], f->linesize[1],
                          f->data[2], f->linesize[2]);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <video-file> [script.lua]\n", argv[0]);
        return 1;
    }
    const std::string videoPath = argv[1];
    const char* scriptPath = argc >= 3 ? argv[2] : nullptr;

    dirk::VideoSource disc;
    if (!disc.open(videoPath)) {
        std::fprintf(stderr, "[dirk] failed to open video: %s\n", videoPath.c_str());
        return 1;
    }
    std::fprintf(stderr, "[dirk] opened %s: %dx%d, %lld frames indexed\n",
                 videoPath.c_str(), disc.width(), disc.height(),
                 static_cast<long long>(disc.frameCount()));

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[dirk] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    if (Mix_OpenAudio(44100, MIX_DEFAULT_FORMAT, 2, 1024) != 0) {
        std::fprintf(stderr, "[dirk] warning: Mix_OpenAudio failed: %s (sound disabled)\n", Mix_GetError());
    }

    SDL_Window* window = SDL_CreateWindow(
        "Dirk", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
        disc.width(), disc.height(), SDL_WINDOW_SHOWN);
    if (!window) {
        std::fprintf(stderr, "[dirk] SDL_CreateWindow failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Renderer* renderer = SDL_CreateRenderer(window, -1, SDL_RENDERER_ACCELERATED);
    if (!renderer) {
        std::fprintf(stderr, "[dirk] SDL_CreateRenderer failed: %s\n", SDL_GetError());
        return 1;
    }

    SDL_Texture* videoTex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING,
        disc.width(), disc.height());

    SDL_Texture* overlayTex = SDL_CreateTexture(
        renderer, SDL_PIXELFORMAT_RGBA8888, SDL_TEXTUREACCESS_TARGET,
        disc.width(), disc.height());
    SDL_SetTextureBlendMode(overlayTex, SDL_BLENDMODE_BLEND);

    dirk::EngineContext ctx;
    ctx.disc = &disc;
    ctx.renderer = renderer;
    ctx.overlayWidth = disc.width();
    ctx.overlayHeight = disc.height();

    dirk::LuaEngine lua;
    lua.open();
    dirk::registerSingeApi(lua.state(), &ctx);

    if (scriptPath) {
        ctx.scriptPath = dirOf(scriptPath);
        ctx.dataPath = ctx.scriptPath;
        if (!lua.runFile(scriptPath)) {
            std::fprintf(stderr, "[dirk] script error: %s\n", lua.lastError().c_str());
        }
    }

    // Prime the first video frame so there's something on screen even
    // before the first Lua-driven overlay update.
    disc.stepForward();

    Uint32 lastTicks = SDL_GetTicks();
    double frameAccumulator = 0.0;
    bool quit = false;

    while (!quit && !ctx.quitRequested) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_QUIT:
                    quit = true;
                    break;
                case SDL_KEYDOWN:
                    if (!ev.key.repeat) {
                        // framework.singe's SCANCODE table matches SDL2
                        // scancode values directly (feasibility study
                        // section 2, "Entrees").
                        lua.callOnInputPressed(static_cast<int>(ev.key.keysym.scancode));
                    }
                    break;
                case SDL_KEYUP:
                    lua.callOnInputReleased(static_cast<int>(ev.key.keysym.scancode));
                    break;
                case SDL_MOUSEMOTION:
                    lua.callOnMouseMoved(ev.motion.x, ev.motion.y,
                                         ev.motion.xrel, ev.motion.yrel, 0);
                    break;
                default:
                    break;
            }
        }

        Uint32 now = SDL_GetTicks();
        double dt = (now - lastTicks) / 1000.0;
        lastTicks = now;

        if (!ctx.discPaused && ctx.discFps > 0.0) {
            frameAccumulator += dt * ctx.discFps;
            // Cap catch-up so a stall (breakpoint, window drag) doesn't
            // burn through hundreds of frames at once.
            if (frameAccumulator > 5.0) frameAccumulator = 5.0;
            while (frameAccumulator >= 1.0) {
                if (!disc.stepForward()) break; // end of stream
                frameAccumulator -= 1.0;
            }
        }

        uploadFrameToTexture(videoTex, disc.currentPicture());

        SDL_SetRenderTarget(renderer, overlayTex);
        bool updated = false;
        lua.callOnOverlayUpdate(updated);
        SDL_SetRenderTarget(renderer, nullptr);

        SDL_RenderCopy(renderer, videoTex, nullptr, nullptr);
        SDL_RenderCopy(renderer, overlayTex, nullptr, nullptr);
        SDL_RenderPresent(renderer);
    }

    lua.callOnShutdown();

    SDL_DestroyTexture(overlayTex);
    SDL_DestroyTexture(videoTex);
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    Mix_CloseAudio();
    SDL_Quit();
    return 0;
}
