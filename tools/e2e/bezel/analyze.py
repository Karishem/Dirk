#!/usr/bin/env python3
# Bezels e2e checks (see run.sh). Red pixels can only be the bezel's frame,
# blue ones only the movie (a solid 0x1030c0 clip).
import sys, re, os
from PIL import Image

work = sys.argv[1]
fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    if not ok: fails += 1

def shot(name):
    p = os.path.join(work, name + ".png")
    if not os.path.exists(p): return None
    im = Image.open(p).convert("RGB"); W, H = im.size; px = im.load()
    red = 0; bx = []; by = []
    for y in range(0, H, 2):
        for x in range(0, W, 2):
            r, g, b = px[x, y]
            if r > 150 and g < 80 and b < 80: red += 1
            elif b > 140 and r < 80: bx.append(x); by.append(y)
    blue = (min(bx), max(bx), min(by), max(by)) if bx else None
    return {"W": W, "H": H, "red": red, "blue": blue,
            "bw": (blue[1] - blue[0] + 2) if blue else 0, "bh": (blue[3] - blue[2] + 2) if blue else 0}

def log(name):
    p = os.path.join(work, name + ".log")
    return open(p, errors="replace").read() if os.path.exists(p) else ""

def state(text, when):
    m = re.search(r"^BEZEL %s count=(\d+) index=(\d+)$" % when, text, re.M)
    return (int(m.group(1)), int(m.group(2))) if m else None

def near(a, b, tol=12): return abs(a - b) <= tol

R = {}
for c in ["m43_wide", "m43_square", "m169_wide", "sar_wide_nobezel", "sar_wide"]:
    R[c] = (shot(c + "_before"), shot(c + "_after"), log(c))
    print("  %-17s before=%s after=%s start=%s after=%s" % (c, R[c][0], R[c][1], state(R[c][2], "start"), state(R[c][2], "after")))

b, a, t = R["m43_wide"]
check(b and a and b["W"] == 1920 and b["H"] == 1080, "4:3 movie, 16:9 screen: fullscreen 1920x1080")
check(state(t, "start") == (1, 1), "4:3 movie, 16:9 screen: bezel available, #1 chosen")
check(b and b["red"] > 1000 and near(b["bw"], 1440) and near(b["bh"], 1080), "...bezel drawn, picture in its 4:3 window")
check(state(t, "after") == (1, 0) and a and a["red"] == 0, "...F11 still works (bezel #1 -> none)")
check("[dirk] bezel: available" in t, "...logged as available")

b, a, t = R["m43_square"]
check(b and b["W"] == 1024 and b["H"] == 768, "4:3 movie, 4:3 screen: fullscreen 1024x768")
check(state(t, "start") == (0, 1), "4:3 screen: no bezel for the menu (count 0), saved choice kept (index 1)")
check(b and b["red"] == 0 and near(b["bw"], 1024) and near(b["bh"], 768), "...no bezel drawn, picture fills the screen")
check(state(t, "after") == (0, 1) and a and a["red"] == 0, "...F11 does nothing, choice still 1")
check("not available, no room beside the picture (screen 1024x768" in t, "...logged as not available")

b, a, t = R["m169_wide"]
check("[dirk] bezel bezel.png left out: its screen window is 720x540 (1.33), the movie is 1.78" in t,
      "16:9 movie, bezel cut for 4:3: left out, logged")
check(state(t, "start") == (0, 0), "...the script sees no bezel")
check(b and b["red"] == 0 and near(b["bw"], 1920) and near(b["bh"], 1080), "...picture 16:9 fills the screen, not squeezed")

b, a, t = R["sar_wide_nobezel"]
check("[dirk] video shape: 720x480 with 8:9 pixels, shown as 640x480 (1.333)" in t, "720x480 with 8:9 pixels: logged as 4:3")
check(b and near(b["bw"], 1440) and near(b["bh"], 1080), "...shown 4:3 (1440 wide at 1080), not 3:2 (1620)")

b, a, t = R["sar_wide"]
check(state(t, "start") == (1, 1) and b and b["red"] > 1000 and near(b["bw"], 1440),
      "720x480 with 8:9 pixels + 4:3 bezel: bezel fits and is drawn")

print("FAILED" if fails else "all passed")
sys.exit(1 if fails else 0)
