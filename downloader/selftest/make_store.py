#!/usr/bin/env python3
# Builds the fake Mega store the self-test downloads from (see run.sh):
# store/<id> is what the link .../<id> holds (see fake_megatools.cpp).
import zipfile, os, sys
S = os.path.join(sys.argv[1] if len(sys.argv) > 1 else ".", "store")
def zd(path, tag, inner="Timegal.dirk"):
    with zipfile.ZipFile(path, "w") as z:
        z.writestr(inner, "-- " + tag + "\n"); z.writestr("Overlay/a.png", tag * 50)
def f(p, s):
    os.makedirs(os.path.dirname(p) or ".", exist_ok=True); open(p, "w").write(s)
os.makedirs(S, exist_ok=True)
zd(S + "/g1", "v1"); f(S + "/g1.fname", "Timegal.zdirk")
zd(S + "/g2", "v2"); f(S + "/g2.fname", "Timegal.zdirk")
zd(S + "/g3", "v3"); f(S + "/g3.fname", "TimegalHD.zdirk")
zd(S + "/gtrunc", "v9"); f(S + "/gtrunc.fname", "Timegal.zdirk"); f(S + "/gtrunc.trunc", "")
with zipfile.ZipFile(S + "/gtrunc", "a") as z: z.writestr("pad.bin", os.urandom(1000))
f(S + "/m1/timegal.mp4", "MOVIE1"); f(S + "/m1.name", "Movie")                  # folder link, arrives wrapped
f(S + "/m2/timegal.mp4", "MOVIE2"); f(S + "/m2/flat.flag", "")                  # folder link, arrives flat
f(S + "/md1/box3d.png", "BOX"); f(S + "/md1/marquee.png", "MARQUEE"); f(S + "/md1.name", "Timegal media")
f(S + "/md2/box3d.png", "BOX2"); f(S + "/md2/marquee.png", "MARQUEE"); f(S + "/md2.name", "Timegal media")
f(S + "/vid1/fb.m2v", "M2V"); f(S + "/vid1.name", "Video")
f(S + "/f1/Oldgame.dirk", "-- old"); f(S + "/f1/Movie/old.mp4", "OLD"); f(S + "/f1/flat.flag", "")
# Several versions in one .zdirk (shortcuts written by the tool).
def zmulti(path, names, top=""):
    with zipfile.ZipFile(path, "w") as z:
        for n in names: z.writestr(top + n, "-- " + n + "\n" + "x" * 5000)
        z.writestr(top + "Overlay/a.png", "PNG"); z.writestr(top + "Script/extra.dirk", "-- not at the top")
zmulti(S + "/h1", ["Hayate.dirk", "HayatePSX.dirk"]); f(S + "/h1.fname", "Hayate.zdirk")
zmulti(S + "/h2", ["Hayate.dirk", "HayateSaturn.dirk", "HayateArcade.dirk"], "Hayate/"); f(S + "/h2.fname", "Hayate.zdirk")
zmulti(S + "/h3", ["Hayate.dirk"]); f(S + "/h3.fname", "Hayate.zdirk")
zmulti(S + "/k1", ["Kyle A.dirk", "Kyle B.dirk"]); f(S + "/k1.fname", "Kyle.zdirk")   # none named like the zip
# Progress and stall (a big movie arriving slowly / never).
f(S + "/slow.slow", ""); f(S + "/stall.stall", "")
