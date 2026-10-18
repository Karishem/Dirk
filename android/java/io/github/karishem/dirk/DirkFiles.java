package io.github.karishem.dirk;

import android.content.Context;
import android.content.Intent;
import android.content.pm.PackageInfo;
import android.content.res.AssetManager;
import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.system.Os;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * Where Dirk keeps its files on Android, and the copy of KimmyScript.
 *
 * Layout, the same as the Linux packages, in the shared storage so it can
 * be reached from a PC (USB, file manager):
 *   Dirk/KimmyScript/   from the APK (DIRK_HOME points here, see main.cpp's
 *                       findSharedEngineRoot)
 *   Dirk/Games/         default games folder (another one can be chosen)
 *   Dirk/Licenses/, Dirk/THIRD_PARTY_NOTICES.txt
 *   Dirk/dirk.log       the last run's messages (DIRK_LOG_FILE)
 */
final class DirkFiles {
    private DirkFiles() {}

    static final String EXTRA_PATH = "path";
    static final String EXTRA_WIZARD = "wizard";
    static final String EXTRA_TOUCH = "touch";
    private static final String MARKER = "KimmyScript/.android-install";
    private static final String LIST = "dirk-files.txt";

    static File root() {
        return new File(Environment.getExternalStorageDirectory(), "Dirk");
    }

    static File defaultGames() {
        return new File(root(), "Games");
    }

    static boolean canAccessFiles() {
        return Environment.isExternalStorageManager();
    }

    /** What the APK installed last time: version code and install time. */
    private static String installStamp(Context c) {
        try {
            PackageInfo p = c.getPackageManager().getPackageInfo(c.getPackageName(), 0);
            return p.getLongVersionCode() + " " + p.lastUpdateTime;
        } catch (Exception e) {
            return "?";
        }
    }

    static boolean needsInstall(Context c) {
        File marker = new File(root(), MARKER);
        if (!marker.isFile() || !new File(root(), "KimmyScript/Script").isDirectory()) return true;
        try (InputStream in = new FileInputStream(marker)) {
            byte[] b = new byte[256];
            int n = in.read(b);
            String s = n > 0 ? new String(b, 0, n, StandardCharsets.UTF_8).trim() : "";
            return !s.equals(installStamp(c));
        } catch (IOException e) {
            return true;
        }
    }

    /**
     * Copies KimmyScript, the licences and the notices out of the APK, after
     * an install or an update. Files in KimmyScript/Cfg/ that already exist
     * are kept (settings, controls, high scores the player changed); every
     * other file is replaced by the APK's own.
     */
    static void install(Context c) throws IOException {
        File root = root();
        if (!root.isDirectory() && !root.mkdirs()) throw new IOException("cannot create " + root);
        defaultGames().mkdirs();
        AssetManager am = c.getAssets();
        byte[] buf = new byte[1 << 16];
        try (BufferedReader r = new BufferedReader(new InputStreamReader(am.open(LIST), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty()) continue;
                File out = new File(root, line);
                if (line.startsWith("KimmyScript/Cfg/") && out.exists()) continue;
                File dir = out.getParentFile();
                if (dir != null && !dir.isDirectory() && !dir.mkdirs()) throw new IOException("cannot create " + dir);
                File tmp = new File(out.getPath() + ".part");
                try (InputStream in = am.open(line); OutputStream os = new FileOutputStream(tmp)) {
                    int n;
                    while ((n = in.read(buf)) > 0) os.write(buf, 0, n);
                }
                if (out.exists() && !out.delete()) throw new IOException("cannot replace " + out);
                if (!tmp.renameTo(out)) throw new IOException("cannot write " + out);
            }
        }
        try (OutputStream os = new FileOutputStream(new File(root, MARKER))) {
            os.write(installStamp(c).getBytes(StandardCharsets.UTF_8));
        }
    }

    /** Environment read by dirk (main.cpp) and SDL, set before SDL starts. */
    static void setEnvironment() {
        try {
            Os.setenv("DIRK_HOME", root().getPath(), true);
            Os.setenv("DIRK_FORCE_FULLSCREEN", "1", true);
            Os.setenv("DIRK_LOG_FILE", new File(root(), "dirk.log").getPath(), true);
            Os.setenv("SDL_ORIENTATIONS", "LandscapeLeft LandscapeRight", true);
        } catch (Exception ignored) {
        }
    }

    /**
     * The on-screen controls' settings, changed in the game (SETUP, under
     * "..."): "size height edge spacing lock", in steps (lock 0 or 1).
     * Older files with fewer numbers still load. A small file in the app's own
     * storage: the game runs in its own process, where shared preferences
     * would not be seen fresh.
     */
    static String touchSettings(Context c) {
        try (InputStream in = new FileInputStream(new File(c.getFilesDir(), "touch-settings"))) {
            byte[] b = new byte[64];
            int n = in.read(b);
            return n > 0 ? new String(b, 0, n, StandardCharsets.UTF_8) : "0 0 0";
        } catch (IOException e) {
            return "0 0 0";
        }
    }

    static void setTouchSettings(Context c, String settings) {
        try (OutputStream os = new FileOutputStream(new File(c.getFilesDir(), "touch-settings"))) {
            os.write(settings.getBytes(StandardCharsets.UTF_8));
        } catch (IOException ignored) {
        }
    }

    /** The game a launch intent names: "path" extra, then the data URI. */
    static String gameFromIntent(Intent in) {
        if (in == null) return null;
        for (String key : new String[] {EXTRA_PATH, "ROM", "rom", "file"}) {
            String s = in.getStringExtra(key);
            if (s != null && !s.isEmpty()) {
                if (s.startsWith("content:") || s.startsWith("file:")) return pathFromUri(Uri.parse(s));
                return s;
            }
        }
        Uri u = in.getData();
        return u != null ? pathFromUri(u) : null;
    }

    /**
     * A real path for a file:// URI, or for a content:// URI of Android's
     * own storage provider (what ES-DE's %ROMSAF% and the Files app give).
     * Other providers give no path: null.
     */
    static String pathFromUri(Uri u) {
        String scheme = u.getScheme();
        if ("file".equals(scheme)) return u.getPath();
        if (!"content".equals(scheme)) return null;
        if (!"com.android.externalstorage.documents".equals(u.getAuthority())) return null;
        String id = null;
        try {
            id = DocumentsContract.getDocumentId(u);
        } catch (Exception e) {
            try {
                id = DocumentsContract.getTreeDocumentId(u);
            } catch (Exception ignored) {
            }
        }
        if (id == null) return null;
        int colon = id.indexOf(':');
        String volume = colon >= 0 ? id.substring(0, colon) : id;
        String rel = colon >= 0 ? id.substring(colon + 1) : "";
        File base = "primary".equalsIgnoreCase(volume)
                ? Environment.getExternalStorageDirectory()
                : new File("/storage", volume);
        return rel.isEmpty() ? base.getPath() : new File(base, rel).getPath();
    }

    static String lower(String s) {
        return s.toLowerCase(Locale.ROOT);
    }

    /** A file dirk starts a game from: a .dirk sheet (or shortcut) or a .zdirk. */
    static boolean isSheet(File f) {
        String n = lower(f.getName());
        return f.isFile() && (n.endsWith(".dirk") || n.endsWith(".zdirk"));
    }

    /**
     * Every way to start the game in this folder, by name: a game sold in
     * two versions has its .zdirk and a .dirk shortcut into it
     * (HayatePSX.dirk next to Hayate.zdirk), each one a version.
     */
    static java.util.List<File> sheets(File dir) {
        java.util.List<File> out = new java.util.ArrayList<>();
        File[] fs = dir.listFiles();
        if (fs == null) return out;
        for (File f : fs) if (isSheet(f)) out.add(f);
        out.sort((x, y) -> x.getName().compareToIgnoreCase(y.getName()));
        return out;
    }

    static boolean hasMovieFolder(File dir) {
        File[] fs = dir.listFiles();
        if (fs == null) return false;
        for (File f : fs) if (f.isDirectory() && lower(f.getName()).equals("movie")) return true;
        return false;
    }

    /**
     * Whether dirk finds a video for this folder, by the same rules as
     * main.cpp's findVideoInMovieFolder: a video file in Movie/, else in
     * Video/, else a Hypseus framefile (a .txt at the folder's root naming
     * .m2v pieces, each with its .ogg; HypseusRemux.cpp's tryParseFrameFile).
     * The Wizard needs one; without it dirk stops at once.
     */
    static boolean hasVideo(File dir) {
        return hasVideoIn(dir, "movie") || hasVideoIn(dir, "video") || hasFrameFile(dir);
    }

    private static boolean hasVideoIn(File dir, String sub) {
        File[] fs = dir.listFiles();
        if (fs == null) return false;
        for (File d : fs) {
            if (!d.isDirectory() || !lower(d.getName()).equals(sub)) continue;
            File[] vs = d.listFiles();
            if (vs == null) continue;
            for (File v : vs) {
                String n = lower(v.getName());
                if (v.isFile() && (n.endsWith(".mp4") || n.endsWith(".mkv") || n.endsWith(".avi") || n.endsWith(".mov"))) return true;
            }
        }
        return false;
    }

    private static final java.util.regex.Pattern FRAME_LINE = java.util.regex.Pattern.compile("^(\\d+)\\s+(.+)$");

    private static boolean hasFrameFile(File dir) {
        File[] fs = dir.listFiles();
        if (fs == null) return false;
        for (File f : fs) {
            String n = f.getName();
            if (!f.isFile() || !n.endsWith(".txt") || n.equals("dirk_log.txt") || n.equals("dirk_error.txt")) continue;
            if (f.length() > 65536) continue;
            if (isFrameFile(f)) return true;
        }
        return false;
    }

    private static boolean isFrameFile(File f) {
        java.util.List<String> lines = new java.util.ArrayList<>();
        try (BufferedReader r = new BufferedReader(new InputStreamReader(new FileInputStream(f), StandardCharsets.UTF_8))) {
            String line;
            while ((line = r.readLine()) != null) {
                String t = trim(line);
                if (!t.isEmpty()) lines.add(t);
            }
        } catch (IOException e) {
            return false;
        }
        if (lines.size() < 2) return false;
        File videoDir = new File(f.getParentFile(), lines.get(0));
        for (int i = 1; i < lines.size(); i++) {
            java.util.regex.Matcher m = FRAME_LINE.matcher(lines.get(i));
            if (!m.matches()) return false;
            String name = trim(m.group(2));
            File video = new File(videoDir, name);
            int dot = name.lastIndexOf('.');
            int slash = Math.max(name.lastIndexOf('/'), name.lastIndexOf('\\'));
            String audio = (dot > slash ? name.substring(0, dot) : name) + ".ogg";
            if (!video.isFile() || !new File(videoDir, audio).isFile()) return false;
        }
        return true;
    }

    /** Spaces, tabs and line ends at both ends, as HypseusRemux.cpp's trim. */
    private static String trim(String s) {
        int a = 0, b = s.length();
        while (a < b && " \t\r\n".indexOf(s.charAt(a)) >= 0) a++;
        while (b > a && " \t\r\n".indexOf(s.charAt(b - 1)) >= 0) b--;
        return s.substring(a, b);
    }

    static boolean hasSheet(File dir) {
        File[] fs = dir.listFiles();
        if (fs == null) return false;
        for (File f : fs) if (isSheet(f)) return true;
        return false;
    }
}
