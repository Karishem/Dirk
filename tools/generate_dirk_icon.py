#!/usr/bin/env python3
"""Regenerates src/core/dirk.ico from the two source logos under
assets/dirk_icon/.

A single Windows .ico is actually a small container of several images, one
per size -- it does NOT have to be the same picture just scaled down. That
matters here: the full Dirk Engine logo (character + circular badge +
"DIRK engine" wordmark) reads fine at 128px/256px, but the wordmark
dissolves into an unreadable smudge once shrunk to the 16-48px range most
Windows UI actually uses for a file-type icon (confirmed visually -- see
the icon preview generated while building this). So small sizes use a
second, text-free version of the same logo instead of a shrunk copy of the
first; only the large sizes carry the wordmark.

  assets/dirk_icon/dirk_icon_notext.png -- used for sizes <= NOTEXT_MAX_PX
  assets/dirk_icon/dirk_icon_full.png    -- used for sizes >  NOTEXT_MAX_PX

To change either source image: overwrite the corresponding PNG in
assets/dirk_icon/, then re-run this script from the repo root:

    python3 tools/generate_dirk_icon.py

and rebuild (Windows/MinGW build only -- dirk.ico is only ever linked in
via src/core/dirk.rc's ICON resource, which is MINGW-gated in
CMakeLists.txt).
"""
import pathlib

from PIL import Image

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
FULL_PNG = REPO_ROOT / "assets" / "dirk_icon" / "dirk_icon_full.png"
NOTEXT_PNG = REPO_ROOT / "assets" / "dirk_icon" / "dirk_icon_notext.png"
OUT_ICO = REPO_ROOT / "src" / "core" / "dirk.ico"

# <= this size uses the text-free source; above it, the full logo (with the
# "DIRK engine" wordmark). 48 was chosen from the actual rendered preview:
# the wordmark was still an unreadable smudge at 48px and only became
# clearly legible at 64px+, so 64 is the first size that gets the full logo.
NOTEXT_MAX_PX = 48

ALL_SIZES = [16, 24, 32, 48, 64, 128, 256]


def squared(im: Image.Image) -> Image.Image:
    """Pads to a square canvas (transparent) so every resize is uniform --
    both source PNGs are close to square but not pixel-exact, and a
    non-square source produces non-square (invalid-looking) .ico frames."""
    w, h = im.size
    side = max(w, h)
    canvas = Image.new("RGBA", (side, side), (0, 0, 0, 0))
    canvas.paste(im, ((side - w) // 2, (side - h) // 2), im)
    return canvas


def main() -> None:
    full = squared(Image.open(FULL_PNG).convert("RGBA"))
    notext = squared(Image.open(NOTEXT_PNG).convert("RGBA"))

    small_sizes = [s for s in ALL_SIZES if s <= NOTEXT_MAX_PX]
    large_sizes = [s for s in ALL_SIZES if s > NOTEXT_MAX_PX]

    # Pillow's ICO writer takes one base image + a `sizes` list and resizes
    # that SAME image for every entry -- there's no built-in way to hand it
    # a different source per size. So each size's own correctly-cropped
    # frame is pre-rendered by hand, then spliced into one multi-image ICO
    # via frombytes-free append_images (every frame after the first/largest
    # one is taken from `append_images`, in the given size order).
    frames = {}
    for s in small_sizes:
        frames[s] = notext.resize((s, s), Image.LANCZOS)
    for s in large_sizes:
        frames[s] = full.resize((s, s), Image.LANCZOS)

    ordered_sizes = sorted(frames, reverse=True)  # largest first -- Pillow's ICO base image should be the biggest
    base = frames[ordered_sizes[0]]
    rest = [frames[s] for s in ordered_sizes[1:]]

    base.save(
        OUT_ICO,
        sizes=[(s, s) for s in ordered_sizes],
        append_images=rest,
    )
    print(
        f"wrote {OUT_ICO.relative_to(REPO_ROOT)}: "
        f"{small_sizes} <- {NOTEXT_PNG.name}, {large_sizes} <- {FULL_PNG.name}"
    )


if __name__ == "__main__":
    main()
