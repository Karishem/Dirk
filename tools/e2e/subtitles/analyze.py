#!/usr/bin/env python3
# Subtitles e2e checks (see run.sh). On a solid dark-blue picture, white
# pixels can only be subtitle text and near-black ones only its outline.
import sys, re
from PIL import Image

log, work = sys.argv[1], sys.argv[2]
text = open(log, errors="replace").read()
fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    if not ok: fails += 1

m = re.search(r"^SUBS count=(\d+) names=(\S*) index=(\d+) name99=\[(.*)\]$", text, re.M)
check(m is not None and m.group(1) == "2" and m.group(2) == "On,French" and m.group(3) == "1" and m.group(4) == "",
      "script sees 2 tracks 'On,French', index 1 by default, empty name out of range (%s)" % (m.group(0) if m else "no SUBS line"))
check("[dirk] subtitles: On (3 lines), French (2 lines) -- showing On, size 100%" in text, "startup log line lists the tracks")
phases = dict(re.findall(r"^PHASE (\d+) frame=\d+ index=(\d+) size=\d+$", text, re.M))
sizes = dict(re.findall(r"^PHASE (\d+) frame=\d+ index=\d+ size=(\d+)$", text, re.M))
check(sizes.get("1") == "100" and sizes.get("8") == "50" and sizes.get("9") == "150",
      "dirkGet/SetSubtitleSize: 100 by default, 50, 300 clamped to 150 (%s)" % sizes)
check(phases.get("5") == "0" and phases.get("6") == "2" and phases.get("7") == "2",
      "dirkSetSubtitleIndex: 0 = off, 2 = FR, 7 clamped to 2 (%s)" % phases)

def measure(name):
    try:
        im = Image.open("%s/%s.png" % (work, name)).convert("RGB")
    except OSError:
        return None
    W, H = im.size
    px = im.load()
    xs, ys, dark = [], [], 0
    for y in range(0, H):
        for x in range(0, W, 2):
            r, g, b = px[x, y]
            if r > 200 and g > 200 and b > 200:
                xs.append(x); ys.append(y)
            elif r < 20 and g < 20 and b < 20:
                dark += 1
    if not xs:
        return {"W": W, "H": H, "text": False, "dark": dark}
    return {"W": W, "H": H, "text": True, "x0": min(xs), "x1": max(xs), "y0": min(ys), "y1": max(ys),
            "w": max(xs) - min(xs), "h": max(ys) - min(ys), "dark": dark}

shots = {n: measure(n) for n in ["s0_before", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9"]}
for n, s in shots.items():
    print("  %-10s %s" % (n, s))
check(all(s is not None for s in shots.values()), "all 10 screenshots taken")
if all(s is not None for s in shots.values()):
    s1, s3, s4, s6, s7 = shots["s1"], shots["s3"], shots["s4"], shots["s6"], shots["s7"]
    check(not shots["s0_before"]["text"], "before the first cue: no subtitle")
    check(s1["text"], "cue 1 shown after a jump into it")
    if s1["text"]:
        W, H = s1["W"], s1["H"]
        check(abs((s1["x0"] + s1["x1"]) / 2 - W / 2) < W * 0.02, "cue 1 centred horizontally")
        check(s1["y1"] > H * 0.85 and s1["y1"] < H * 0.97, "cue 1 near the bottom, above the edge")
        check(s1["dark"] > 200, "black outline around the text")
    check(not shots["s2"]["text"], "gap between cues: nothing shown")
    # Height compared to s6 ("Une seule ligne"): like cue 2's lines it has a
    # descender, so the white-pixel heights are comparable ("Hello Reika"
    # has none and measures shorter).
    check(s3["text"] and s6["text"] and 1.6 < s3["h"] / max(1, s6["h"]) < 2.6, "two-line cue about twice as tall as a one-line one")
    if s3["text"]:
        check(abs((s3["x0"] + s3["x1"]) / 2 - s3["W"] / 2) < s3["W"] * 0.02, "two-line cue centred")
    check(s4["text"] and s4["w"] <= s4["W"] * 0.93 and s4["w"] >= s4["W"] * 0.85,
          "too-wide line shrunk to fit the picture (width %s of %s)" % (s4.get("w"), s4["W"]))
    check(s4["text"] and s1["text"] and s4["h"] < s1["h"], "shrunk line is smaller (same font, scaled down)")
    check(not shots["s5"]["text"], "subtitles off (index 0): nothing shown during a cue")
    check(s6["text"] and s3["text"] and s6["h"] < s3["h"] * 0.7, "FR track: its own one-line cue, not the two-line one")
    check(s7["text"], "FR track after a jump back")
    s8, s9 = shots["s8"], shots["s9"]
    if s8["text"] and s9["text"] and s1["text"]:
        r8, r9 = s8["w"] / s1["w"], s9["w"] / s1["w"]
        check(0.42 < r8 < 0.58, "Subtitle Size 50%%: cue 1 half as wide (%.2f)" % r8)
        check(1.35 < r9 < 1.65, "Subtitle Size 150%%: cue 1 1.5x as wide (%.2f)" % r9)
        check(abs((s8["x0"] + s8["x1"]) / 2 - s8["W"] / 2) < s8["W"] * 0.02 and s8["y1"] > s8["H"] * 0.85,
              "smaller subtitles stay centred at the bottom")
    else:
        check(False, "Subtitle Size shots have text")
print("ALL PASS" if fails == 0 else "SOME FAILED")
sys.exit(1 if fails else 0)
