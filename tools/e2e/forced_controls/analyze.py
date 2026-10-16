#!/usr/bin/env python3
# Forced controls -- what each press gave, run by run.
import sys, re
runs, cur = {}, None
for l in open(sys.argv[1], errors="replace"):
    l = l.rstrip("\n")
    m = re.match(r"^RUN (\w+)$", l)
    if m: cur = m.group(1); runs[cur] = {"log": [], "got": []}; continue
    if cur is None: continue
    runs[cur]["log"].append(l)
    m = re.match(r"^SHIM t=\d+ (\w+) ", l)
    if m and m.group(1) != "END": runs[cur]["got"].append([m.group(1), None])
    m = re.match(r"^GAME PRESSED (\d+)$", l)
    if m and runs[cur]["got"] and runs[cur]["got"][-1][1] is None: runs[cur]["got"][-1][1] = int(m.group(1))
fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    if not ok: fails += 1
def got(run): return dict((k, v) for k, v in runs.get(run, {}).get("got", []))
for r in ("forced", "none", "path"): print("  %s: %s" % (r, got(r)))
f, n, p = got("forced"), got("none"), got("path")
check(f.get("pad_X") == 8, "forced: gamepad X -> BUTTON_3 (8), not the game's TEST_MODE (15)")
check(f.get("raw_0") == 8, "forced: raw button 0 -> BUTTON_3 (8), over its joystick profile (BUTTON_2)")
check(f.get("space") == 8, "forced: Space, unbound in the game, -> BUTTON_3 (8)")
check(f.get("pad_A") == 6 and f.get("raw_3") == 6, "forced: what the file doesn't list stays the game's (A, raw 3 -> 6)")
check(any("1 forced binding(s)" in l or "3 forced binding(s)" in l for l in runs["forced"]["log"]), "forced: the log says how many")
check(n.get("pad_X") == 15 and n.get("raw_0") == 7 and n.get("space") is None, "no forcedControls: game's bindings and profile only (15, 7, nothing)")
check(p.get("pad_X") == 15 and any("forcedControls" in l and "ignored" in l for l in runs["path"]["log"]), "a name with a path in it: refused, said so, game's bindings only")
print("ALL PASS" if fails == 0 else "%d FAILED" % fails)
sys.exit(1 if fails else 0)
