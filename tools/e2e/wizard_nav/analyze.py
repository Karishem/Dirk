#!/usr/bin/env python3
# Joystick wizard without a keyboard -- checks on the order of the log lines.
import sys, re
lines = [l.rstrip("\n") for l in open(sys.argv[1], errors="replace")]
fails = 0
def check(ok, what):
    global fails
    print(("PASS " if ok else "FAIL ") + what)
    if not ok: fails += 1
def idx(pattern, start=0):
    for i in range(start, len(lines)):
        if re.search(pattern, lines[i]): return i
    return -1
for l in lines:
    if "GAME PRESSED" in l or "joystick wizard" in l or "OPEN REQUEST" in l: print("  " + l)
a = idx(r"^GAME PRESSED 6$")
check(a >= 0, "before: gamepad A reaches the game as action 1")
o = idx(r"^OPEN REQUEST$", a + 1)
w = idx(r"joystick wizard: ouvert", o + 1)
check(o > a and w > o, "action 2 -> script asks, the wizard opens (dirkOpenJoystickWizard)")
s = idx(r"joystick wizard: portee = generale", w + 1)
check(s > w, "stick down + A on the scope screen -> 'general' chosen")
d = idx(r'joystick wizard: configuration de "Dirk test gamepad"', s + 1)
check(d > s, "stick down + A on the device list -> the gamepad (2nd in the list) is being configured")
c = idx(r'joystick wizard: ".*" -> bouton manette 2', d + 1)
check(c > d, "while recording, a gamepad button is a binding (X -> first prompt), not navigation")
x = idx(r"joystick wizard: annule", c + 1)
check(x > c, "Escape key still cancels")
o2 = idx(r"^OPEN REQUEST$", x + 1)
w2 = idx(r"joystick wizard: ouvert", o2 + 1)
x2 = idx(r"joystick wizard: annule", w2 + 1)
check(o2 > x and w2 > o2 and x2 > w2, "reopened from the script, then B (action 2) on the scope screen closes it")
between = [i for i in range(w, x2) if lines[i].startswith("GAME PRESSED") and not (x < i < w2)]
check(not between, "no input reached the game while the wizard was open (%s)" % [lines[i] for i in between])
a2 = idx(r"^GAME PRESSED 6$", x2 + 1)
check(a2 > x2, "after: gamepad A reaches the game again")
r = idx(r'joystick wizard: "Right" -> bouton manette 14', a2 + 1)
q = idx(r"second stick demande", r + 1)
st = idx(r'joystick wizard: "Start" -> bouton manette 6', r + 1)
check(r > a2 and q < 0, "D-pad directions, then Start on the 'other input too?' question: not taken as 'yes'")
check(st > r, "after B ('no'), Start is recorded for the Start prompt")
print("FAILED" if fails else "all passed")
sys.exit(1 if fails else 0)
