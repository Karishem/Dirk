package io.github.karishem.dirk;

import android.view.KeyEvent;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeSet;

/**
 * The on-screen controls' geometry and logic, without any drawing: where
 * each control sits, which keys the fingers on screen hold down.
 *
 * Always there: the cross on the left, and A B X Y in a diamond on the
 * right, as on a gamepad. The rest only shows after a touch on the small
 * tab in the top right corner, each where Cyril wanted it (18/10/2026):
 * COIN, OPTIONS, START along the bottom; L1 above the cross, R1 above
 * A B X Y; EXIT, SETUP and LOCK at the top. It goes away again
 * (TouchControls closes it a few seconds after its last use, or on a second
 * touch of the tab), except COIN, START, L1 and R1 while LOCK is on: they
 * stay on screen, for the Wizard (Cyril, 18/10/2026).
 *
 * SETUP opens a small panel to adjust the cross and A B X Y: size, height,
 * distance from the screen's edges, and the spacing of A B X Y (Cyril,
 * 18/10/2026). While it is open the game is paused (TouchControls) and the
 * controls send nothing.
 *
 * Every control sends the keyboard key that KimmyScript's default
 * controls.cfg (KimmyScript/Cfg/controls.cfg) binds to the same action as
 * the gamepad button it stands for: A = Left Ctrl (BUTTON_1, JOY_A),
 * B = Left Alt (BUTTON_2, JOY_B), X = F4 (SAVE_MENU, JOY_X), Y = P
 * (PAUSE_GAME, JOY_Y), arrows, COIN = 5, START = 1, L1/R1 = F1/F2,
 * OPTIONS = 9, EXIT = Escape. Going through the keyboard keeps
 * the game's own bindings, Wizard's forced controls and the gamepad
 * settings (swap_face_buttons) out of the way.
 *
 * Kept free of Android classes at run time (KeyEvent's constants are
 * compiled in) so it can be checked on a PC.
 */
final class TouchLayout {
    static final int CIRCLE = 0, PILL = 1, DPAD = 2, TAB = 3;
    static final int MAIN = 0, EXTRA = 1, SETUP = 2;

    static final class Control {
        final String label;
        final String text;
        final int key;
        final int shape;
        final int group;
        float cx, cy, r, halfW, halfH;

        Control(String label, String text, int key, int shape, int group) {
            this.label = label;
            this.text = text;
            this.key = key;
            this.shape = shape;
            this.group = group;
        }

        boolean hit(float x, float y, float slack) {
            if (shape == PILL) {
                return Math.abs(x - cx) <= halfW * slack && Math.abs(y - cy) <= halfH * slack;
            }
            float dx = x - cx, dy = y - cy, rr = r * slack;
            return dx * dx + dy * dy <= rr * rr;
        }
    }

    // The four settings, in steps. Size: 10 % per step; height: 5 % of
    // the screen's short side per step, up is positive; edge: 4 % per step
    // towards the middle; spacing: A B X Y 10 % further apart per step.
    static final int SIZE_MIN = -3, SIZE_MAX = 6, LIFT_MAX = 6, INSET_MAX = 8, SPREAD_MAX = 6;
    int sizeStep, liftStep, insetStep, spreadStep;

    // What happened on the last pointer() call, for the view.
    static final int EV_SETUP_OPENED = 1, EV_SETUP_CLOSED = 2, EV_SETTINGS = 4;
    int events;

    final List<Control> controls = new ArrayList<>();
    final Control dpad = new Control("", "", 0, DPAD, MAIN);
    final Control tab = new Control("...", "", 0, TAB, MAIN);
    boolean extrasOpen;
    /** LOCK: COIN, START, L1 and R1 stay on screen with the extras closed. */
    boolean locked;
    boolean setupOpen;
    float diamondX, diamondY, discR, diamondR;
    /** Setup panel: its box and the y of its three rows (drawn by the view). */
    float panelL, panelT, panelR, panelB, panelX;
    final float[] rowY = new float[4];
    private final Map<Integer, Control> owner = new HashMap<>();
    private final Map<Integer, float[]> where = new HashMap<>();
    private float lastW, lastH, lastL, lastT, lastR, lastB;

    private static final String[] EXTRAS = {"COIN", "OPTIONS", "START", "L1", "R1", "EXIT", "SETUP", "LOCK"};
    private static final List<String> LOCKABLE = java.util.Arrays.asList("COIN", "START", "L1", "R1");
    private static final String[] ROWS = {"SIZE", "HEIGHT", "EDGE", "SPACING"};

    TouchLayout() {
        controls.add(dpad);
        controls.add(new Control("A", "A", KeyEvent.KEYCODE_CTRL_LEFT, CIRCLE, MAIN));
        controls.add(new Control("B", "B", KeyEvent.KEYCODE_ALT_LEFT, CIRCLE, MAIN));
        controls.add(new Control("X", "X", KeyEvent.KEYCODE_F4, CIRCLE, MAIN));
        controls.add(new Control("Y", "Y", KeyEvent.KEYCODE_P, CIRCLE, MAIN));
        controls.add(tab);
        int[] keys = {KeyEvent.KEYCODE_5, KeyEvent.KEYCODE_9, KeyEvent.KEYCODE_1, KeyEvent.KEYCODE_F1,
                KeyEvent.KEYCODE_F2, KeyEvent.KEYCODE_ESCAPE, 0, 0};
        for (int i = 0; i < EXTRAS.length; i++) controls.add(new Control(EXTRAS[i], EXTRAS[i], keys[i], PILL, EXTRA));
        for (String row : ROWS) {
            controls.add(new Control(row + "-", "-", 0, CIRCLE, SETUP));
            controls.add(new Control(row + "+", "+", 0, CIRCLE, SETUP));
        }
        controls.add(new Control("RESET", "RESET", 0, PILL, SETUP));
        controls.add(new Control("DONE", "DONE", 0, PILL, SETUP));
    }

    Control byLabel(String label) {
        for (Control c : controls) if (c.label.equals(label)) return c;
        return null;
    }

    boolean visible(Control c) {
        if (c.group == SETUP) return setupOpen;
        if (c.group == EXTRA) return !setupOpen && (extrasOpen || (locked && LOCKABLE.contains(c.label)));
        return c != tab || !setupOpen;
    }

    float scale() {
        return 1f + 0.1f * sizeStep;
    }

    float spread() {
        return 1f + 0.1f * spreadStep;
    }

    /** "SIZE 120%", "HEIGHT +2", "EDGE 3", "SPACING 110%": the setup panel's row texts. */
    String rowText(int row) {
        if (row == 0) return "SIZE " + Math.round(100 * scale()) + "%";
        if (row == 1) return "HEIGHT " + (liftStep > 0 ? "+" : "") + liftStep;
        if (row == 2) return "EDGE " + insetStep;
        return "SPACING " + Math.round(100 * spread()) + "%";
    }

    /** Lays out again with the last screen size (after a setting changed). */
    void relayout() {
        layout(lastW, lastH, lastL, lastT, lastR, lastB);
    }

    /**
     * Places the controls for a w x h screen; the insets keep the top row
     * clear of a notch or rounded corners. Sizes follow the short side, so
     * they keep the same proportion on a phone and on a handheld. Settings
     * that would push the cross or A B X Y off the screen, or into each
     * other, are brought back to the largest that fits.
     */
    void layout(float w, float h, float insetL, float insetT, float insetR, float insetB) {
        lastW = w;
        lastH = h;
        lastL = insetL;
        lastT = insetT;
        lastR = insetR;
        lastB = insetB;
        float s = Math.min(w, h);
        float m = 0.04f * s;
        float left = insetL + m, right = w - insetR - m, top = insetT + m, bottom = h - insetB - m;
        float pw = 0.065f * s, ph = 0.03f * s, gap = 0.02f * s;

        // Top: the tab in the corner, EXIT and SETUP centred.
        place(tab, right - 0.035f * s, top + 0.035f * s, 0.035f * s);
        pill(byLabel("EXIT"), w / 2 - 2 * pw - gap, top + ph, pw, ph);
        pill(byLabel("SETUP"), w / 2, top + ph, pw, ph);
        pill(byLabel("LOCK"), w / 2 + 2 * pw + gap, top + ph, pw, ph);
        float topBottom = Math.max(top + 2 * ph, tab.cy + tab.r) + gap;
        // Bottom: COIN, OPTIONS, START, centred.
        String[] low = {"COIN", "OPTIONS", "START"};
        for (int i = 0; i < 3; i++) pill(byLabel(low[i]), w / 2 + (i - 1) * (2 * pw + gap), bottom - ph, pw, ph);
        float lowTop = bottom - 2 * ph - gap;
        float lowL = w / 2 - 3 * pw - gap - gap, lowR = w / 2 + 3 * pw + gap + gap;

        // The cross and the diamond, each on a faint disc, against the
        // sides by default even where a camera hole asks for a safe margin
        // (Cyril, 18/10/2026: the hole is small, the controls see-through).
        // Settings that would push them off the screen, under the top row or
        // into each other are brought back to the largest that fits.
        float side = 0.02f * s;
        sizeStep = Math.max(SIZE_MIN, Math.min(SIZE_MAX, sizeStep));
        spreadStep = Math.max(0, Math.min(SPREAD_MAX, spreadStep));
        while (sizeStep > SIZE_MIN && 0.13f * s * scale() > h / 2 - topBottom) sizeStep--;
        float k = scale();
        float pad = 0.13f * s * k;
        float d = 0.072f * s * k * spread(), br = 0.042f * s * k;
        float padD = Math.max(pad, d + br + 0.016f * s * k);
        while (spreadStep > 0 && padD > h / 2 - topBottom) {
            spreadStep--;
            d = 0.072f * s * k * spread();
            padD = Math.max(pad, d + br + 0.016f * s * k);
        }
        float maxInset = Math.min(w / 2 - 0.03f * s - side - 2 * pad, w / 2 - 0.03f * s - side - 2 * padD) / (0.04f * s);
        insetStep = (int) Math.max(0, Math.min(Math.min(INSET_MAX, Math.floor(maxInset)), insetStep));
        float inset = insetStep * 0.04f * s;
        float dx = side + pad + inset, ax = w - side - padD - inset;
        // Going down stops above COIN OPTIONS START when a disc is above them.
        float biggest = Math.max(pad, padD);
        boolean overLow = dx + pad > lowL || ax - padD < lowR;
        float floor = overLow ? lowTop : h;
        int maxUp = (int) Math.max(0, Math.min(LIFT_MAX, Math.floor((h / 2 - biggest - topBottom) / (0.05f * s))));
        int maxDown = (int) Math.max(0, Math.min(LIFT_MAX, Math.floor((floor - h / 2 - biggest) / (0.05f * s))));
        liftStep = Math.max(-maxDown, Math.min(maxUp, liftStep));
        float cy = h / 2 - liftStep * 0.05f * s;

        dpad.r = 0.11f * s * k;
        dpad.cx = dx;
        dpad.cy = cy;
        place(byLabel("A"), ax, cy + d, br);
        place(byLabel("B"), ax + d, cy, br);
        place(byLabel("X"), ax - d, cy, br);
        place(byLabel("Y"), ax, cy - d, br);
        diamondX = ax;
        diamondY = cy;
        discR = pad;
        diamondR = padD;

        // L1 over the cross, R1 over A B X Y, a little above their discs
        // (below them when there is no room above).
        float lift = 0.06f * s + ph;
        float l1y = cy - pad - lift, r1y = cy - padD - lift;
        if (l1y - ph < topBottom) l1y = cy + pad + lift;
        if (r1y - ph < topBottom) r1y = cy + padD + lift;
        pill(byLabel("L1"), dx, l1y, pw, ph);
        pill(byLabel("R1"), ax, r1y, pw, ph);

        // The setup panel, in the middle of the screen.
        panelX = w / 2;
        float py = h / 2;
        panelL = panelX - 0.42f * s;
        panelR = panelX + 0.42f * s;
        panelT = py - 0.33f * s;
        panelB = py + 0.34f * s;
        for (int i = 0; i < ROWS.length; i++) {
            rowY[i] = py + (-0.25f + 0.12f * i) * s;
            place(byLabel(ROWS[i] + "-"), panelX - 0.34f * s, rowY[i], 0.05f * s);
            place(byLabel(ROWS[i] + "+"), panelX + 0.34f * s, rowY[i], 0.05f * s);
        }
        pill(byLabel("RESET"), panelX - 0.14f * s, py + 0.25f * s, 0.11f * s, 0.045f * s);
        pill(byLabel("DONE"), panelX + 0.14f * s, py + 0.25f * s, 0.11f * s, 0.045f * s);
    }

    private static void place(Control c, float x, float y, float r) {
        c.cx = x;
        c.cy = y;
        c.r = r;
        c.halfW = r;
        c.halfH = r;
    }

    private static void pill(Control c, float x, float y, float hw, float hh) {
        c.cx = x;
        c.cy = y;
        c.halfW = hw;
        c.halfH = hh;
        c.r = Math.max(hw, hh);
    }

    /** The control a finger landing at (x, y) takes, or null. */
    Control controlAt(float x, float y) {
        if (!setupOpen && dpad.hit(x, y, 1.3f)) return dpad;
        Control best = null;
        float bestD = Float.MAX_VALUE;
        for (Control c : controls) {
            if (c == dpad || !visible(c)) continue;
            if (setupOpen && c.group != SETUP) continue;
            if (!c.hit(x, y, c == tab ? 1.6f : c.shape == CIRCLE ? 1.45f : 1.25f)) continue;
            float dx = x - c.cx, dy = y - c.cy, dd = dx * dx + dy * dy;
            if (dd < bestD) {
                bestD = dd;
                best = c;
            }
        }
        return best;
    }

    /**
     * A finger went down (down = true) or moved. A finger on the cross keeps
     * it while it stays near. The tab, SETUP and the setup panel's buttons
     * act once, when the finger lands on them; `events` tells the view.
     */
    void pointer(int id, float x, float y, boolean down) {
        Control was = owner.get(id);
        Control now = (was == dpad && dpad.hit(x, y, 2.0f)) ? dpad : controlAt(x, y);
        if (now != null && now.key == 0 && now != dpad) {
            if (down) press(now);
            now = was == now ? now : (down && visible(now) ? now : null);
        }
        if (now == null) owner.remove(id);
        else owner.put(id, now);
        where.put(id, new float[] {x, y});
    }

    private void press(Control c) {
        if (c == tab) {
            extrasOpen = !extrasOpen;
            if (!extrasOpen) dropExtras();
            return;
        }
        switch (c.label) {
            case "SETUP":
                setupOpen = true;
                extrasOpen = false;
                owner.clear();
                where.clear();
                events |= EV_SETUP_OPENED;
                return;
            case "DONE":
                setupOpen = false;
                owner.clear();
                where.clear();
                events |= EV_SETUP_CLOSED;
                return;
            case "LOCK":
                locked = !locked;
                events |= EV_SETTINGS;
                return;
            case "RESET":
                sizeStep = liftStep = insetStep = spreadStep = 0;
                break;
            case "SIZE-": sizeStep--; break;
            case "SIZE+": sizeStep++; break;
            case "HEIGHT-": liftStep--; break;
            case "HEIGHT+": liftStep++; break;
            case "EDGE-": insetStep--; break;
            case "EDGE+": insetStep++; break;
            case "SPACING-": spreadStep--; break;
            case "SPACING+": spreadStep++; break;
            default:
                return;
        }
        relayout();
        events |= EV_SETTINGS;
    }

    void release(int id) {
        owner.remove(id);
        where.remove(id);
    }

    void releaseAll() {
        owner.clear();
        where.clear();
    }

    /** Closes the extra buttons (fingers on them let go). */
    void closeExtras() {
        extrasOpen = false;
        dropExtras();
    }

    private void dropExtras() {
        owner.values().removeIf(c -> c.group == EXTRA && !visible(c));
        where.keySet().retainAll(owner.keySet());
    }

    boolean extraHeld() {
        for (Control c : owner.values()) if ((c.group == EXTRA && !(locked && LOCKABLE.contains(c.label))) || c == tab) return true;
        return false;
    }

    boolean isPressed(Control c) {
        return owner.containsValue(c);
    }

    /** Every key the fingers on screen hold down now (none while setting up). */
    TreeSet<Integer> keys() {
        TreeSet<Integer> keys = new TreeSet<>();
        if (setupOpen) return keys;
        for (Map.Entry<Integer, Control> e : owner.entrySet()) {
            Control c = e.getValue();
            if (c.key == 0 && c != dpad) continue;
            if (c != dpad) {
                keys.add(c.key);
                continue;
            }
            float[] p = where.get(e.getKey());
            float dx = p[0] - dpad.cx, dy = p[1] - dpad.cy;
            float dead = 0.25f * dpad.r, ax = Math.abs(dx), ay = Math.abs(dy);
            if (ax > dead && ax > 0.414f * ay) keys.add(dx < 0 ? KeyEvent.KEYCODE_DPAD_LEFT : KeyEvent.KEYCODE_DPAD_RIGHT);
            if (ay > dead && ay > 0.414f * ax) keys.add(dy < 0 ? KeyEvent.KEYCODE_DPAD_UP : KeyEvent.KEYCODE_DPAD_DOWN);
        }
        return keys;
    }

    /** The settings as saved: "size lift inset spread lock". */
    String settings() {
        return sizeStep + " " + liftStep + " " + insetStep + " " + spreadStep + " " + (locked ? 1 : 0);
    }

    void setSettings(String saved) {
        try {
            String[] p = saved.trim().split("\\s+");
            sizeStep = Integer.parseInt(p[0]);
            liftStep = Integer.parseInt(p[1]);
            insetStep = Integer.parseInt(p[2]);
            spreadStep = p.length > 3 ? Integer.parseInt(p[3]) : 0;
            locked = p.length > 4 && Integer.parseInt(p[4]) == 1;
        } catch (Exception e) {
            sizeStep = liftStep = insetStep = spreadStep = 0;
            locked = false;
        }
    }
}
