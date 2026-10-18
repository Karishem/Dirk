package io.github.karishem.dirk;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.res.Configuration;
import android.graphics.Color;
import android.net.Uri;
import android.os.Bundle;
import android.provider.Settings;
import android.text.InputType;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.WindowInsets;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ListView;
import android.widget.TextView;

import java.io.File;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/**
 * Dirk's home screen: asks for file access, copies KimmyScript out of the
 * APK, then lists the games folder. A game folder or a .zdirk starts the
 * game (DirkActivity); a game folder also offers the Wizard; any other
 * folder is opened, so games can be grouped in sub-folders.
 * Works with a gamepad (D-pad + A/B), a keyboard or the touchscreen.
 */
public class LauncherActivity extends Activity {
    private static final String PREF_GAMES = "games";
    private static final String PREF_TOUCH = "touch";
    private Button touchButton;
    private Button[] barButtons;
    private LinearLayout bar;

    private TextView header;
    private TextView message;
    private Button permissionButton;
    private ListView list;
    private ArrayAdapter<String> adapter;
    private final List<File> entries = new ArrayList<>();
    private File games;
    private File current;
    private boolean installing;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        int pad = dp(16);
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setPadding(pad, pad, pad, pad);
        // Android 15+ draws the app under the status and navigation bars
        // (targetSdk 35): keep the list clear of them, and of a camera notch.
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            android.graphics.Insets i = insets.getInsets(
                    WindowInsets.Type.systemBars() | WindowInsets.Type.displayCutout());
            v.setPadding(pad + i.left, pad + i.top, pad + i.right, pad + i.bottom);
            return insets;
        });
        root.setBackgroundColor(Color.rgb(16, 16, 20));

        // Path on its own line, then one row of short buttons: fits a phone
        // held upright as well as a handheld's wide screen.
        header = new TextView(this);
        header.setTextColor(Color.WHITE);
        header.setTextSize(TypedValue.COMPLEX_UNIT_SP, 15);
        header.setSingleLine(true);
        header.setEllipsize(android.text.TextUtils.TruncateAt.START);
        root.addView(header);
        barButtons = new Button[] {
            barButton(getString(R.string.change_folder), v -> askGamesFolder()),
            touchButton = barButton("", v -> {
                prefs().edit().putInt(PREF_TOUCH, (touchMode() + 1) % 3).apply();
                showTouchMode();
            }),
            barButton(getString(R.string.refresh), v -> refresh()),
        };
        showTouchMode();
        bar = new LinearLayout(this);
        bar.setOrientation(LinearLayout.VERTICAL);
        bar.setPadding(0, dp(6), 0, 0);
        root.addView(bar);
        arrangeBar();

        message = new TextView(this);
        message.setTextColor(Color.rgb(220, 220, 220));
        message.setTextSize(TypedValue.COMPLEX_UNIT_SP, 16);
        message.setPadding(0, pad, 0, pad);
        root.addView(message);

        permissionButton = new Button(this);
        permissionButton.setText(R.string.permission_button);
        permissionButton.setOnClickListener(v -> askFileAccess());
        root.addView(permissionButton, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.WRAP_CONTENT, LinearLayout.LayoutParams.WRAP_CONTENT));

        list = new ListView(this);
        adapter = new ArrayAdapter<String>(this, android.R.layout.simple_list_item_1, new ArrayList<>()) {
            @Override
            public View getView(int position, View convertView, android.view.ViewGroup parent) {
                TextView t = (TextView) super.getView(position, convertView, parent);
                t.setTextColor(Color.WHITE);
                t.setTextSize(TypedValue.COMPLEX_UNIT_SP, 20);
                return t;
            }
        };
        list.setAdapter(adapter);
        list.setOnItemClickListener((AdapterView<?> p, View v, int pos, long id) -> open(pos));
        root.addView(list, new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT, 0, 1));
        setContentView(root);
    }

    @Override
    protected void onResume() {
        super.onResume();
        refresh();
    }

    /** The buttons on one row, or on several when the screen is narrow (phone upright). */
    private void arrangeBar() {
        bar.removeAllViews();
        int perRow = getResources().getConfiguration().screenWidthDp >= 340 ? barButtons.length : 2;
        LinearLayout row = null;
        for (int i = 0; i < barButtons.length; i++) {
            if (i % perRow == 0) {
                row = new LinearLayout(this);
                row.setOrientation(LinearLayout.HORIZONTAL);
                bar.addView(row);
            }
            if (barButtons[i].getParent() != null) ((ViewGroup) barButtons[i].getParent()).removeView(barButtons[i]);
            row.addView(barButtons[i], barCell());
        }
    }

    @Override
    public void onConfigurationChanged(Configuration c) {
        super.onConfigurationChanged(c);
        arrangeBar();
    }

    private Button barButton(String label, View.OnClickListener click) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextSize(TypedValue.COMPLEX_UNIT_SP, 14);
        b.setSingleLine(true);
        b.setOnClickListener(click);
        return b;
    }

    private static LinearLayout.LayoutParams barCell() {
        return new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1);
    }

    private int dp(int v) {
        return (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, v, getResources().getDisplayMetrics());
    }

    private SharedPreferences prefs() {
        return getSharedPreferences("dirk", MODE_PRIVATE);
    }

    private void show(String text, boolean permission) {
        message.setText(text);
        message.setVisibility(text == null ? View.GONE : View.VISIBLE);
        permissionButton.setVisibility(permission ? View.VISIBLE : View.GONE);
        if (permission) permissionButton.requestFocus();
    }

    private void refresh() {
        if (installing) return;
        if (!DirkFiles.canAccessFiles()) {
            header.setText(R.string.app_name);
            setEntries(new ArrayList<>());
            show(getString(R.string.permission_text), true);
            return;
        }
        if (DirkFiles.needsInstall(this)) {
            installing = true;
            show(getString(R.string.installing), false);
            new Thread(() -> {
                String error = null;
                try {
                    DirkFiles.install(this);
                } catch (Exception e) {
                    error = String.valueOf(e.getMessage());
                }
                final String err = error;
                runOnUiThread(() -> {
                    installing = false;
                    if (err != null) show(getString(R.string.install_failed, err), false);
                    else refresh();
                });
            }).start();
            return;
        }
        games = new File(prefs().getString(PREF_GAMES, DirkFiles.defaultGames().getPath()));
        games.mkdirs();
        if (current == null || !inside(current, games) || !current.isDirectory()) current = games;
        header.setText(getString(R.string.games_folder, current.getPath()));
        List<File> found = new ArrayList<>();
        File[] fs = current.listFiles();
        if (fs != null) {
            Arrays.sort(fs, (a, b) -> a.getName().compareToIgnoreCase(b.getName()));
            for (File f : fs) {
                if (f.getName().startsWith(".")) continue;
                if (f.isDirectory() && !f.getName().equalsIgnoreCase("KimmyScript")) found.add(f);
            }
            for (File f : fs) if (f.isFile() && DirkFiles.lower(f.getName()).endsWith(".zdirk")) found.add(f);
        }
        if (!current.equals(games)) found.add(0, null);
        setEntries(found);
        show(found.isEmpty() ? getString(R.string.no_games, games.getPath()) : null, false);
        if (!found.isEmpty()) list.requestFocus();
    }

    private void setEntries(List<File> found) {
        entries.clear();
        entries.addAll(found);
        adapter.clear();
        for (File f : found) {
            if (f == null) adapter.add(getString(R.string.up));
            else if (f.isFile()) adapter.add(f.getName());
            else if (isGame(f)) adapter.add(f.getName());
            else adapter.add(f.getName() + "/");
        }
        adapter.notifyDataSetChanged();
    }

    private void open(int pos) {
        if (pos < 0 || pos >= entries.size()) return;
        File f = entries.get(pos);
        if (f == null) {
            goUp();
            return;
        }
        if (f.isFile()) {
            launch(f, false);
            return;
        }
        // One line per way to start the game (each .dirk and .zdirk of the
        // folder, so both versions of a two-version game), then Wizard.
        // dirk is given the file itself: given the folder, it would always
        // take the .dirk over the .zdirk.
        final List<File> sheets = DirkFiles.sheets(f);
        if (!isGame(f)) {
            current = f;
            refresh();
            return;
        }
        List<String> labels = new ArrayList<>();
        for (File sh : sheets) {
            labels.add(sheets.size() == 1 ? getString(R.string.play) : getString(R.string.play_version, sh.getName()));
        }
        labels.add(getString(R.string.wizard));
        new AlertDialog.Builder(this)
                .setTitle(f.getName())
                .setItems(labels.toArray(new String[0]), (d, which) -> {
                    if (which < sheets.size()) {
                        launch(sheets.get(which), false);
                    } else if (!DirkFiles.hasVideo(f)) {
                        show(getString(R.string.no_movie), false);
                    } else {
                        launch(f, true);
                    }
                })
                .setNegativeButton(R.string.cancel, null)
                .show();
    }

    /** A game folder: something to play, or a video for the Wizard (or a Movie folder still to fill). */
    private static boolean isGame(File f) {
        return DirkFiles.hasSheet(f) || DirkFiles.hasMovieFolder(f) || DirkFiles.hasVideo(f);
    }

    private void goUp() {
        File p = current.getParentFile();
        current = (p != null && inside(p, games)) ? p : games;
        refresh();
    }

    private static boolean inside(File f, File top) {
        return f.equals(top) || f.getPath().startsWith(top.getPath() + "/");
    }

    private int touchMode() {
        int m = prefs().getInt(PREF_TOUCH, DirkActivity.TOUCH_AUTO);
        return m >= 0 && m <= 2 ? m : DirkActivity.TOUCH_AUTO;
    }

    private void showTouchMode() {
        int[] names = {R.string.touch_auto, R.string.touch_on, R.string.touch_off};
        touchButton.setText(getString(names[touchMode()]));
    }

    private void launch(File game, boolean wizard) {
        Intent i = new Intent(this, DirkActivity.class);
        i.putExtra(DirkFiles.EXTRA_PATH, game.getPath());
        i.putExtra(DirkFiles.EXTRA_WIZARD, wizard);
        i.putExtra(DirkFiles.EXTRA_TOUCH, touchMode());
        startActivity(i);
    }

    private void askFileAccess() {
        try {
            startActivity(new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION,
                    Uri.parse("package:" + getPackageName())));
        } catch (Exception e) {
            startActivity(new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION));
        }
    }

    private void askGamesFolder() {
        final EditText input = new EditText(this);
        input.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI);
        input.setText(games != null ? games.getPath() : DirkFiles.defaultGames().getPath());
        new AlertDialog.Builder(this)
                .setTitle(R.string.folder_prompt)
                .setView(input)
                .setPositiveButton(R.string.ok, (d, w) -> {
                    String s = input.getText().toString().trim();
                    if (s.isEmpty()) s = DirkFiles.defaultGames().getPath();
                    while (s.length() > 1 && s.endsWith("/")) s = s.substring(0, s.length() - 1);
                    prefs().edit().putString(PREF_GAMES, s).apply();
                    current = null;
                    refresh();
                })
                .setNegativeButton(R.string.cancel, null)
                .show();
    }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int k = e.getKeyCode();
        if (e.getAction() == KeyEvent.ACTION_DOWN && list.hasFocus()) {
            if (k == KeyEvent.KEYCODE_BUTTON_A) {
                open(list.getSelectedItemPosition());
                return true;
            }
            if ((k == KeyEvent.KEYCODE_BUTTON_B || k == KeyEvent.KEYCODE_BACK) && current != null && games != null
                    && !current.equals(games)) {
                goUp();
                return true;
            }
        }
        return super.dispatchKeyEvent(e);
    }
}
