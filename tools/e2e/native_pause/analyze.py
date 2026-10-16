# Review point 4 e2e (2026-09-26): reads the log of run.sh and checks, input
# segment by input segment, whether the picture played or stayed frozen.
import sys
expect = [("start","playing"),("A_hat_press1","frozen"),("A_hat_press2","playing"),
          ("B_axis_press1","frozen"),("B_axis_press2","playing"),
          ("C_gpaxis_press1","frozen"),("C_gpaxis_press2","playing"),
          ("D_native_pause","frozen"),("D_menu","frozen@10"),("D_pause_again","frozen@10"),
          ("E_script_play","playing"),("E_F9_wizard","frozen@20"),("E_esc_close","frozen@20")]
segs=[["start",[]]]
for line in open(sys.argv[1]):
    p=line.split()
    if line.startswith("SHIM t=") and p[2] not in ("-",):
        if p[2]=="END": break
        segs.append([p[2],[]])
    elif line.startswith("F ") and len(p)==3:
        segs[-1][1].append(int(p[2]))
ok=True
for (label,frames),(elabel,exp) in zip(segs,expect):
    f=frames[4:]  # a few ticks of latency after each input
    tail = f[len(f)//2:]  # the script may seek mid-segment (menu parking)
    adv = tail[-1]-tail[0] if tail else 0
    state = "playing" if adv>5 else "frozen"
    if state=="frozen" and "@" in exp: state += "@%d" % f[-1]
    good = (state==exp)
    ok &= good and label==elabel
    print("%-4s %-16s frames %4d..%-4d -> %-10s (expected %s)" % ("PASS" if good else "FAIL", label, f[0] if f else -1, f[-1] if f else -1, state, exp))
if len(segs)!=len(expect): ok=False; print("FAIL segment count", len(segs))
print("ALL PASS" if ok else "SOME FAILED"); sys.exit(0 if ok else 1)
