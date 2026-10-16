#!/usr/bin/env python3
"""Regenerates src/lua/KimmyLogoData.h from assets/kimmy_logo/kimmy_logo_source.png.

The Kimmy branding logo is baked directly into the compiled dirk binary
(see l_dirkKimmyLogo() in src/lua/api_singe.cpp) instead of being loaded
from a loose file next to dirk.exe, unlike every other optional asset in
this project (dirk_loading.png, bezel.png, ...). That's a deliberate,
one-off exception: the point of this particular asset is that a player
editing the plain-text .kimmy scripts, or swapping a file in the game
folder, can no longer change or remove it -- see the README section on
the Kimmy logo for the full reasoning.

To replace the logo image: overwrite assets/kimmy_logo/kimmy_logo_source.png
with a new PNG, then re-run this script from the repo root:

    python3 tools/embed_kimmy_logo.py

and rebuild. Never hand-edit the generated header.
"""
import pathlib

REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
SRC_PNG = REPO_ROOT / "assets" / "kimmy_logo" / "kimmy_logo_source.png"
OUT_HEADER = REPO_ROOT / "src" / "lua" / "KimmyLogoData.h"

BYTES_PER_LINE = 20


def main() -> None:
    data = SRC_PNG.read_bytes()

    try:
        from PIL import Image
        width, height = Image.open(SRC_PNG).size
        dims_comment = f"{width}x{height} PNG, {len(data)} bytes"
    except ImportError:
        dims_comment = f"{len(data)} bytes (install Pillow to also record WxH here)"

    lines = [
        "#pragma once",
        "// Embedded Kimmy branding logo (PNG bytes), baked into the compiled",
        "// binary instead of loaded from a loose file next to dirk.exe -- see",
        "// l_dirkKimmyLogo() in api_singe.cpp for why, and",
        "// assets/kimmy_logo/kimmy_logo_source.png for the source image",
        f"// ({dims_comment}).",
        "//",
        "// GENERATED FILE -- do not hand-edit. Regenerate with:",
        "//   python3 tools/embed_kimmy_logo.py",
        "",
        "#include <cstddef>",
        "",
        "namespace dirk {",
        "",
        "inline constexpr unsigned char kKimmyLogoPng[] = {",
    ]
    for i in range(0, len(data), BYTES_PER_LINE):
        chunk = data[i : i + BYTES_PER_LINE]
        lines.append("    " + ",".join(str(b) for b in chunk) + ",")
    lines += [
        "};",
        "",
        "inline constexpr std::size_t kKimmyLogoPngLen = sizeof(kKimmyLogoPng);",
        "",
        "} // namespace dirk",
        "",
    ]

    OUT_HEADER.write_text("\n".join(lines))
    print(f"wrote {len(data)} bytes ({dims_comment}) -> {OUT_HEADER.relative_to(REPO_ROOT)}")


if __name__ == "__main__":
    main()
