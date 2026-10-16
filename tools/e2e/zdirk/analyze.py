#!/usr/bin/env python3
# .zdirk e2e checks (see run.sh).
import sys, os, re
work = sys.argv[1]
G = os.path.join(work, "games", "Zgame")
fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    if not ok: fails += 1
def z(log):
    return dict(re.findall(r"^ZCHECK (\w+)=(.*)$", log, re.M))
logs = {n: open(os.path.join(work, n + ".log"), errors="replace").read() for n in ("first", "folder", "updated", "shortcut")}
c1, c2, c3 = z(logs["first"]), z(logs["folder"]), z(logs["updated"])
check("read from the zip" in logs["first"], "1st launch (.zdirk double-clicked): zip opened")
check(os.path.normpath(c1.get("dir", "")) == os.path.normpath(G), "the game's folder is the .zdirk's folder (%s)" % c1.get("dir"))
check(c1.get("zipfile") == "FROM-ZIP", "a file only in the zip is read")
check(c1.get("custom") == "CUSTOM" and c1.get("customdir") == "directory", "Custom/ next to the .zdirk is seen and read")
check(c1.get("overlay") == "pic.png,swap.png", "lfs.dir lists the zip's Overlay/ (%s)" % c1.get("overlay"))
check(c1.get("runs") == "1" and c2.get("runs") == "2" and c3.get("runs") == "3", "save written, read back, kept across launches and an update (%s,%s,%s)" % (c1.get("runs"), c2.get("runs"), c3.get("runs")))
check(open(os.path.join(G, "Cfg", "runs.txt")).read() == "3", "the save is in Cfg/ next to the .zdirk")
on_disk = sorted(os.listdir(G))
check(on_disk == ["Blocker", "Cfg", "Custom", "Movie", "Zgame.zdirk"] and not os.path.exists(os.path.join(G, "Overlay")), "no copy of the game on disk, only what was there + Cfg/ (%s)" % on_disk)
left = [f for _, _, fs in os.walk(os.path.join(work, "tmp")) for f in fs]
check(not left, "no temporary file left behind (%s)" % left)
check(os.path.exists(os.path.join(G, "Cfg", "defaults.txt")), "factory Cfg/ file copied next to the .zdirk")
check(c1.get("cfg") == "defaults.txt,runs.txt", "lfs.dir(Cfg) merged (%s)" % c1.get("cfg"))
check(c1.get("extra") == "42", "dofile() of a script inside the zip")
check(c1.get("renamed") == "from .kimmy", "dofile(x.singe) finds x.kimmy in the zip, like on disk")
def tone(name, f, rate=44100):
    import struct, math
    data = open(os.path.join(work, name + ".raw"), "rb").read()
    n = len(data) // 4
    left = struct.unpack("<%dh" % (n * 2), data[: n * 4])[0::2]
    best = 0.0
    step = rate // 10
    for i in range(0, n - step, step):
        seg = left[i:i + step]
        c = 2 * math.cos(2 * math.pi * f / rate); s1 = s2 = 0.0
        for v in seg: s1, s2 = v + c * s1 - s2, s1
        p = s1 * s1 + s2 * s2 - c * s1 * s2
        other = 2 * math.cos(2 * math.pi * (f * 1.5) / rate); a1 = a2 = 0.0
        for v in seg: a1, a2 = v + other * a1 - a2, a1
        q = a1 * a1 + a2 * a2 - other * a1 * a2
        if p > 1e9 and p > 20 * q: best = max(best, p)
    return best > 0
check(tone("first", 660), "the zip's 660 Hz beep is heard in Dirk's real audio output")
check(c1.get("syntax") == "reported", "a script error in the zip names the file and line (%s)" % c1.get("syntax"))
check(c1.get("loadfile") == "function", "loadfile() of a script inside the zip")
check('fontLoad("' in logs["first"] and "Fonts/font.ttf\", 20) ok" in logs["first"], "fontLoad() of a font inside the zip")
check("soundLoad(" not in logs["first"] and c1.get("sound") not in (None, "-1"), "soundLoad() of a sound inside the zip, no warning (handle %s)" % c1.get("sound"))
check(c1.get("sprite") == "3x2" and c1.get("swap") == "5x4", "spriteLoad() of images inside the zip (%s, %s)" % (c1.get("sprite"), c1.get("swap")))
check(c1.get("nilmsg") == "true", "io.open of a missing file: nil + message, as in plain Lua")
check(c1.get("errname") == "output", "a failing io.output still says \"bad argument #1 to 'output'\" (%s)" % c1.get("errname"))
check("read from the zip" in logs["folder"] and c2.get("zipfile") == "FROM-ZIP", "2nd launch (the folder): same game")
check("read from the zip" in logs["updated"] and c3.get("sprite") == "3x2", "3rd launch, .zdirk replaced: fine")
check("Cfg file(s) copied" in logs["first"] and "Cfg file(s) copied" not in logs["folder"], "factory Cfg/ copied once only")
for n, l in logs.items():
    check("video decoding:" in l and "no video file" not in l and "movie is inside" not in l, "%s: movie next to the .zdirk opened" % n)
c4 = z(logs["shortcut"])
check("shortcut to Zalt.dirk in" in logs["shortcut"] and c4.get("alt") == "3x2",
      "shortcut Zalt.dirk next to the zip: its second game runs, images from the zip (%s)" % c4.get("alt"))
check("alt" not in c1, "the .zdirk itself still runs the game named like it")
err = os.path.join(G, "dirk_error.txt")
body = open(err, errors="replace").read() if os.path.exists(err) else ""
check("[0" not in body, "no script error (dirk_error.txt next to the .zdirk%s)" % ("" if not body else ": " + body[-300:]))
check("Zgame.zdirk" in body or not body, "dirk_error.txt names the .zdirk")
print("ALL PASS" if fails == 0 else "SOME FAILED")
sys.exit(1 if fails else 0)
