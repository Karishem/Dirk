package io.github.karishem.dirk;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.view.DisplayCutout;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;

import org.libsdl.app.SDLActivity;

import java.util.TreeSet;

/**
 * On-screen controls drawn over the game (TouchLayout holds where they are
 * and what they press). They send keyboard keys to SDL, as a real keyboard
 * would. Hidden while a gamepad or keyboard is used (DirkActivity); a touch
 * brings them back. The extra buttons close by themselves a few seconds
 * after their last use.
 */
final class TouchControls extends View {
    private static final long EXTRAS_CLOSE_MS = 5000;

    private final TouchLayout layout = new TouchLayout();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF rect = new RectF();
    private TreeSet<Integer> held = new TreeSet<>();
    private boolean shown;
    private float insetL, insetT, insetR, insetB;
    private final Runnable closeExtras = new Runnable() {
        @Override
        public void run() {
            if (!layout.extrasOpen) return;
            if (layout.extraHeld()) {
                postDelayed(this, EXTRAS_CLOSE_MS);
                return;
            }
            layout.closeExtras();
            sync();
            invalidate();
        }
    };

    /** Told when the player changes the settings in the game (SETUP), to keep them for next time. */
    interface SettingsListener {
        void onSettings(String settings);
    }

    private SettingsListener settingsListener;

    void setSettingsListener(SettingsListener l) {
        settingsListener = l;
    }

    TouchControls(Context context, boolean shown, String settings) {
        super(context);
        this.shown = shown;
        layout.setSettings(settings);
        ring.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
    }

    void setShown(boolean on) {
        if (shown == on) return;
        shown = on;
        if (!on) {
            layout.releaseAll();
            layout.closeExtras();
            if (layout.setupOpen) {
                layout.setupOpen = false;
                saveSettings();
                tap(KeyEvent.KEYCODE_P);
            }
            sync();
        }
        invalidate();
    }

    @Override
    public WindowInsets onApplyWindowInsets(WindowInsets insets) {
        DisplayCutout cut = insets.getDisplayCutout();
        insetL = cut != null ? cut.getSafeInsetLeft() : 0;
        insetT = cut != null ? cut.getSafeInsetTop() : 0;
        insetR = cut != null ? cut.getSafeInsetRight() : 0;
        insetB = cut != null ? cut.getSafeInsetBottom() : 0;
        layout.layout(getWidth(), getHeight(), insetL, insetT, insetR, insetB);
        invalidate();
        return insets;
    }

    @Override
    protected void onSizeChanged(int w, int h, int oldw, int oldh) {
        layout.layout(w, h, insetL, insetT, insetR, insetB);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        int action = e.getActionMasked();
        if (!shown) {
            if (action == MotionEvent.ACTION_DOWN) {
                shown = true;
                invalidate();
            }
            return true;
        }
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int i = e.getActionIndex();
                layout.pointer(e.getPointerId(i), e.getX(i), e.getY(i), true);
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    layout.pointer(e.getPointerId(i), e.getX(i), e.getY(i), false);
                }
                break;
            case MotionEvent.ACTION_POINTER_UP:
                layout.release(e.getPointerId(e.getActionIndex()));
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_CANCEL:
                layout.releaseAll();
                break;
            default:
                return true;
        }
        int events = layout.events;
        layout.events = 0;
        removeCallbacks(closeExtras);
        if (layout.extrasOpen) postDelayed(closeExtras, EXTRAS_CLOSE_MS);
        sync();
        // The game pauses while the panel is open (P = PAUSE_GAME, like Y),
        // and goes on when it closes.
        if ((events & TouchLayout.EV_SETUP_OPENED) != 0) tap(KeyEvent.KEYCODE_P);
        if ((events & TouchLayout.EV_SETTINGS) != 0) saveSettings();
        if ((events & TouchLayout.EV_SETUP_CLOSED) != 0) {
            saveSettings();
            tap(KeyEvent.KEYCODE_P);
        }
        invalidate();
        return true;
    }

    private static void tap(int key) {
        SDLActivity.onNativeKeyDown(key);
        SDLActivity.onNativeKeyUp(key);
    }

    private void saveSettings() {
        if (settingsListener != null) settingsListener.onSettings(layout.settings());
    }

    /** Sends SDL the keys that went down or up since last time. */
    private void sync() {
        TreeSet<Integer> now = layout.keys();
        for (Integer k : held) if (!now.contains(k)) SDLActivity.onNativeKeyUp(k);
        for (Integer k : now) if (!held.contains(k)) SDLActivity.onNativeKeyDown(k);
        held = now;
    }

    private static final int DISC = 0x386E6E78, IDLE = 0x8CAFAFB9, PRESSED = 0xD9E6E6F0, LABEL = 0xF0FFFFFF;

    @Override
    protected void onDraw(Canvas canvas) {
        if (!shown) return;
        float s = Math.min(getWidth(), getHeight());
        text.setShadowLayer(Math.max(2f, 0.005f * s), 0, 0, 0xCC000000);
        fill.setColor(DISC);
        canvas.drawCircle(layout.dpad.cx, layout.dpad.cy, layout.discR, fill);
        canvas.drawCircle(layout.diamondX, layout.diamondY, layout.diamondR, fill);
        ring.setStrokeWidth(Math.max(1.5f, 0.003f * s));
        ring.setColor(0x40FFFFFF);
        for (TouchLayout.Control c : layout.controls) {
            if (layout.visible(c) && c.group != TouchLayout.SETUP) drawControl(canvas, c);
        }
        if (!layout.setupOpen) return;
        fill.setColor(0xE0141418);
        rect.set(layout.panelL, layout.panelT, layout.panelR, layout.panelB);
        canvas.drawRoundRect(rect, 0.04f * s, 0.04f * s, fill);
        canvas.drawRoundRect(rect, 0.04f * s, 0.04f * s, ring);
        text.setColor(LABEL);
        text.setTextSize(0.046f * s);
        for (int i = 0; i < layout.rowY.length; i++) {
            canvas.drawText(layout.rowText(i), layout.panelX, layout.rowY[i] - (text.ascent() + text.descent()) / 2, text);
        }
        for (TouchLayout.Control c : layout.controls) {
            if (c.group == TouchLayout.SETUP) drawControl(canvas, c);
        }
    }

    private void drawControl(Canvas canvas, TouchLayout.Control c) {
        boolean down = layout.isPressed(c) || (layout.locked && c.label.equals("LOCK"));
        fill.setColor(down ? PRESSED : IDLE);
        text.setColor(LABEL);
        switch (c.shape) {
            case TouchLayout.DPAD:
                drawCross(canvas, c);
                break;
            case TouchLayout.TAB:
                if (layout.extrasOpen) fill.setColor(PRESSED);
                canvas.drawCircle(c.cx, c.cy, c.r, fill);
                for (int i = -1; i <= 1; i++) canvas.drawCircle(c.cx + i * c.r * 0.42f, c.cy, c.r * 0.13f, text);
                break;
            case TouchLayout.PILL:
                rect.set(c.cx - c.halfW, c.cy - c.halfH, c.cx + c.halfW, c.cy + c.halfH);
                canvas.drawRoundRect(rect, c.halfH, c.halfH, fill);
                canvas.drawRoundRect(rect, c.halfH, c.halfH, ring);
                text.setTextSize(c.halfH * 0.9f);
                canvas.drawText(c.text, c.cx, c.cy - (text.ascent() + text.descent()) / 2, text);
                break;
            default:
                canvas.drawCircle(c.cx, c.cy, c.r, fill);
                text.setTextSize(c.r * 1.0f);
                canvas.drawText(c.text, c.cx, c.cy - (text.ascent() + text.descent()) / 2, text);
                break;
        }
    }

    private void drawCross(Canvas canvas, TouchLayout.Control d) {
        float a = d.r * 0.3f, r = d.r, x = d.cx, y = d.cy, round = a * 0.5f;
        TreeSet<Integer> keys = layout.isPressed(d) ? layout.keys() : new TreeSet<>();
        int[][] arms = {
            {KeyEvent.KEYCODE_DPAD_UP, 0, -1},
            {KeyEvent.KEYCODE_DPAD_DOWN, 0, 1},
            {KeyEvent.KEYCODE_DPAD_LEFT, -1, 0},
            {KeyEvent.KEYCODE_DPAD_RIGHT, 1, 0},
        };
        for (int[] arm : arms) {
            fill.setColor(keys.contains(arm[0]) ? PRESSED : IDLE);
            float x0 = arm[1] == 0 ? x - a : (arm[1] < 0 ? x - r : x + a * 1.15f);
            float x1 = arm[1] == 0 ? x + a : (arm[1] < 0 ? x - a * 1.15f : x + r);
            float y0 = arm[2] == 0 ? y - a : (arm[2] < 0 ? y - r : y + a * 1.15f);
            float y1 = arm[2] == 0 ? y + a : (arm[2] < 0 ? y - a * 1.15f : y + r);
            rect.set(x0, y0, x1, y1);
            canvas.drawRoundRect(rect, round, round, fill);
        }
    }
}
