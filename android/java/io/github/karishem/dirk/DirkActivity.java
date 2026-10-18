package io.github.karishem.dirk;

import android.content.Intent;
import android.os.Bundle;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ViewGroup;
import android.widget.Toast;

import org.libsdl.app.SDLActivity;

/**
 * Runs one game: dirk's main() (libmain.so, SDL_main) with the game's path,
 * or "--wizard <folder>". Lives in its own process (AndroidManifest.xml),
 * ended when the game ends, so the next game starts from a fresh engine.
 * Puts the on-screen controls (TouchControls) over the game.
 */
public class DirkActivity extends SDLActivity {
    static final int TOUCH_AUTO = 0, TOUCH_ON = 1, TOUCH_OFF = 2;

    private String[] arguments = new String[0];
    private TouchControls touch;
    private int touchMode = TOUCH_AUTO;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        Intent in = getIntent();
        String game = DirkFiles.gameFromIntent(in);
        boolean ready = game != null && DirkFiles.canAccessFiles();
        if (ready && DirkFiles.needsInstall(this)) {
            try {
                DirkFiles.install(this);
            } catch (Exception e) {
                ready = false;
            }
        }
        if (ready) {
            boolean wizard = in.getBooleanExtra(DirkFiles.EXTRA_WIZARD, false);
            arguments = wizard ? new String[] {"--wizard", game} : new String[] {game};
            DirkFiles.setEnvironment();
        }
        super.onCreate(savedInstanceState);
        if (!ready) {
            Toast.makeText(this, R.string.no_game_given, Toast.LENGTH_LONG).show();
            startActivity(new Intent(this, LauncherActivity.class));
            finish();
            return;
        }
        touchMode = in.getIntExtra(DirkFiles.EXTRA_TOUCH, TOUCH_AUTO);
        if (touchMode != TOUCH_OFF && mLayout != null) {
            touch = new TouchControls(this, touchMode == TOUCH_ON || !hasGamepad(),
                    DirkFiles.touchSettings(this));
            touch.setSettingsListener(settings -> DirkFiles.setTouchSettings(this, settings));
            mLayout.addView(touch, new ViewGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    /** A gamepad or joystick built in or plugged in (the Retroid's own controls count). */
    private static boolean hasGamepad() {
        for (int id : InputDevice.getDeviceIds()) {
            InputDevice d = InputDevice.getDevice(id);
            if (d == null || d.isVirtual()) continue;
            int s = d.getSources();
            if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                    || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) return true;
        }
        return false;
    }

    /** A real gamepad button or keyboard key (not volume, Back, Home...). */
    private static boolean isPlayerKey(KeyEvent e) {
        InputDevice d = e.getDevice();
        if (d == null || d.isVirtual() || e.isSystem()) return false;
        int s = e.getSource();
        if ((s & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (s & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK
                || (s & InputDevice.SOURCE_DPAD) == InputDevice.SOURCE_DPAD) return true;
        return d.getKeyboardType() == InputDevice.KEYBOARD_TYPE_ALPHABETIC;
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        if (touch != null && touchMode == TOUCH_AUTO && isPlayerKey(e)) touch.setShown(false);
        return super.dispatchKeyEvent(e);
    }

    @Override
    public boolean dispatchGenericMotionEvent(MotionEvent e) {
        if (touch != null && touchMode == TOUCH_AUTO
                && (e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
            touch.setShown(false);
        }
        return super.dispatchGenericMotionEvent(e);
    }

    @Override
    protected String[] getLibraries() {
        return new String[] {"main"};
    }

    @Override
    protected String[] getArguments() {
        return arguments;
    }

    @Override
    protected void onDestroy() {
        Thread engine = mSDLThread;
        super.onDestroy();
        // Leave dirk time to finish writing (saves, settings) before the
        // process goes away.
        if (engine != null) {
            try {
                engine.join(5000);
            } catch (InterruptedException ignored) {
            }
        }
        System.exit(0);
    }
}
