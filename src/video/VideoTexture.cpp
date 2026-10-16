#include "video/VideoTexture.h"

#include <SDL3/SDL.h>

#include <cstdio>

namespace dirk {

namespace {

SDL_Colorspace colorspaceFor(const VideoSource& video) {
    const bool full = video.yuvFullRange();
    if (video.yuvMatrix() == VideoSource::YuvMatrix::BT709) {
        return full ? SDL_COLORSPACE_BT709_FULL : SDL_COLORSPACE_BT709_LIMITED;
    }
    return full ? SDL_COLORSPACE_BT601_FULL : SDL_COLORSPACE_BT601_LIMITED;
}

} // namespace

SDL_Texture* createVideoTexture(SDL_Renderer* renderer, const VideoSource& video, int width, int height) {
    if (!renderer) return nullptr;

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_FORMAT_NUMBER, SDL_PIXELFORMAT_IYUV);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_ACCESS_NUMBER, SDL_TEXTUREACCESS_STREAMING);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(props, SDL_PROP_TEXTURE_CREATE_COLORSPACE_NUMBER, colorspaceFor(video));
    SDL_Texture* tex = SDL_CreateTextureWithProperties(renderer, props);
    SDL_DestroyProperties(props);
    if (tex) return tex;

    std::fprintf(stderr,
                 "[dirk] warning: renderer refused a video texture in %s (%s) -- using SDL's default colours instead\n",
                 describeVideoColors(video).c_str(), SDL_GetError());
    return SDL_CreateTexture(renderer, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, width, height);
}

std::string describeVideoColors(const VideoSource& video) {
    std::string s = video.yuvMatrix() == VideoSource::YuvMatrix::BT709 ? "BT.709" : "BT.601";
    s += video.yuvFullRange() ? ", full range" : ", limited range";
    if (video.yuvMatrixTagged()) {
        s += " (tagged in the file)";
    } else {
        s += " (not tagged -- guessed from " + std::to_string(video.width()) + "x" +
             std::to_string(video.height()) + ")";
    }
    return s;
}

} // namespace dirk
