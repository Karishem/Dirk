#!/usr/bin/env python3
# Audio tracks e2e checks (see run.sh). Dirk's recorded output (S16 stereo
# 44.1 kHz, SDL disk driver) is cut in 100 ms windows; each window's tone
# (440 original / 880 FR / 1320 IT) is found with the Goertzel algorithm.
import sys, re, math, struct
work = sys.argv[1]
fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    if not ok: fails += 1
def log(name): return open(f"{work}/{name}.log", errors="replace").read()

def tones(name, rate=44100, win=0.1):
    data = open(f"{work}/{name}.raw", "rb").read()
    n = len(data) // 4
    left = struct.unpack("<%dh" % (n * 2), data[: n * 4])[0::2]
    step = int(rate * win); out = []
    for i in range(0, n - step, step):
        seg = left[i:i + step]
        if max(abs(v) for v in seg) < 200: out.append(0); continue
        p = {}
        for f in (440, 880, 1320):
            c = 2 * math.cos(2 * math.pi * f / rate); s1 = s2 = 0.0
            for v in seg: s1, s2 = v + c * s1 - s2, s1
            p[f] = s1 * s1 + s2 * s2 - c * s1 * s2
        best = max(p, key=p.get); rest = max(v for k, v in p.items() if k != best)
        out.append(best if p[best] > 50 * rest else -1)  # -1 = two tones mixed
    return out

l1 = log("switch")
m = re.search(r"^TRACKS count=(\d+) names=(\S+) index=(\d+)$", l1, re.M)
check(m is not None and m.groups() == ("3", "Original,French,Italian", "1"), "3 tracks Original,French,Italian, original by default (%s)" % (m.group(0) if m else None,))
check("Hypseus-style game detected" in l1 and "test-fr.ogg + test_it.ogg" in l1, "first launch builds the .mkv with both alternates")
check("[dirk] audio tracks: Original, French, Italian -- playing Original" in l1, "startup log lists the tracks")
check("[dirk] subtitles: French (1 lines)" in l1, "Hypseus game: Video/test-fr.srt found next to the .m2v")
ph = re.findall(r"^PHASE (\d) index=(\d) name=(\S+)$", l1, re.M)
check(ph == [("1", "2", "French"), ("2", "3", "Italian"), ("3", "3", "Italian")], "Lua: French, Italian, out of range stays Italian (%s)" % ph)
t = tones("switch")
seq = []
for v in t:
    if v > 0 and (not seq or seq[-1] != v): seq.append(v)
print("  tone sequence:", seq, " windows:", len(t))
check(seq == [440, 880, 1320], "heard: original, then FR, then IT, never back")
# A 100 ms window can straddle a switch (old track's last samples, new
# track's first ones) -- fine. Anywhere else, two tones at once would mean
# the old track kept playing after the switch.
mixed = [i for i, v in enumerate(t) if v == -1]
stray = [i for i in mixed if len({v for v in t[max(0, i - 2):i + 3] if v > 0}) < 2]
check(not stray and len(mixed) <= 4,
      "two soundtracks never heard together except in the window(s) of a switch (mixed=%s, stray=%s)" % (mixed, stray))
k880 = next((i for i, v in enumerate(t) if v == 880), None); k1320 = next((i for i, v in enumerate(t) if v == 1320), None)
if k880 and k1320:
    gap = (k1320 - k880) * 0.1
    check(1.5 <= gap <= 2.5, "switches 2 s apart as pressed (%.1f s)" % gap)
l2 = log("relaunch")
check("Hypseus-style game detected" not in l2 and "rebuilding" not in l2, "relaunch reuses the cached .mkv")
check("playing Italian" in l2, "relaunch: saved choice Italian used from the start")
t2 = [v for v in tones("relaunch") if v != 0]
check(len(t2) > 5 and all(v == 1320 for v in t2[:5]), "relaunch: IT heard from the first window (%s)" % t2[:8])
l3 = log("added")
check("soundtracks changed" in l3 and "test-de.ogg" in l3 and "test-xx.ogg" in l3, "new test-de.ogg and test-xx.ogg: .mkv rebuilt")
check("[dirk] audio tracks: Original, German, French, Italian, Unknown -- playing Italian" in l3,
      "now 5 tracks, sorted by tag, -xx (not ISO 639) shown as Unknown; the saved choice is still Italian although it is now track 4")
t3 = [v for v in tones("added") if v != 0]
check(len(t3) > 5 and all(v == 1320 for v in t3[:5]), "after the rebuild: IT heard from the start (%s)" % t3[:8])
cfg = open(f"{work}/game/Cfg/game.cfg.after_added").read()
check("dirk_AudioTrack = 3" in cfg and "dirk_AudioTrackName = Italian" in cfg, "game.cfg: dirk_AudioTrack = 3 and dirk_AudioTrackName = Italian")
l4 = log("legacy")
check("playing Italian" in l4, "game.cfg saved by the previous Dirk (dirk_AudioTrackName = IT, number pointing at German): Italian chosen")
t4 = [v for v in tones("legacy") if v != 0]
check(len(t4) > 5 and all(v == 1320 for v in t4[:5]), "legacy name: Italian heard from the start (%s)" % t4[:8])
print("ALL PASS" if fails == 0 else "SOME FAILED")
sys.exit(1 if fails else 0)
