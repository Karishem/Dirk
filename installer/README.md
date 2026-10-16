# Dirk installer

`dirk_installer.nsi` is an [NSIS](https://nsis.sourceforge.io/) script that
builds `DirkSetup.exe`: a normal Windows installer that puts `dirk.exe` and
the shared `KimmyScript/` folder on a player's PC, and registers the
`.dirk` file extension so double-clicking a game's own launcher file opens
it straight in Dirk -- no manual "Open with" needed.

It's just a script; it does not compile `dirk.exe` for you. That build
still happens the normal way (see the main README's "Standalone Windows
build" section), on a real Windows box or via MSYS2/MinGW-w64 -- this repo
was built in a Linux sandbox with no MinGW-w64 toolchain available, so a
real `dirk.exe` couldn't be produced here to test the installer end to end
against. The script itself *was* compiled and verified here (see below).

## How to build DirkSetup.exe

1. Install NSIS (free): <https://nsis.sourceforge.io/Download> on Windows,
   or `sudo apt install nsis` on Linux/WSL if you'd rather compile there.
2. Drop your real files into `payload/`:
   - `payload/dirk.exe` -- your compiled Dirk binary.
   - `payload/KimmyScript/...` -- your real KimmyScript folder's contents
     (`Cfg/`, `Script/`, `Skin/`, whatever your install needs), copied
     directly inside `payload/KimmyScript/`.
   - `payload/Dirk&Kimmy Tutorial.pdf` -- optional; lands next to `dirk.exe`
     in the install folder if present. Nothing breaks if it's missing
     (`File /nonfatal` in the script), so this one's skippable.
   - `payload/dirk_downloader.exe` + `payload/megatools.exe` +
     `payload/manifest_link.txt` -- also optional, also `/nonfatal`, also
     land next to `dirk.exe`. These three together are the Mega game
     downloader companion tool -- see `downloader/README.md` for what each
     one is, how to build `dirk_downloader.exe`, where to get a real
     `megatools.exe` (not built by this repo), and how to fill in
     `manifest_link.txt` with your own catalogue's Mega link. Drop in all
     three, or skip all three -- the base engine install works either way.

   Each of `dirk.exe`/`KimmyScript/` ships with a placeholder `.txt` file
   explaining exactly this -- delete it once your real files are there (it
   won't get picked up by the installer either way, the script only looks
   for files by their real names). Anything you drop into `payload/`
   itself, other than those six exact names, is likewise ignored -- adding
   a new file for real needs a matching `File` line in the script, not just
   a drag-and-drop into this folder.
3. Compile:
   - NSIS GUI: right-click `dirk_installer.nsi` -> "Compile NSI script"
     (or open the NSIS app, "File > Load Script").
   - Command line: `makensis dirk_installer.nsi`
4. `DirkSetup.exe` appears next to the script. That's the one file you hand
   to players.

## What it does on a player's PC

- Installs to `Dirk` at the root of whichever drive `DirkSetup.exe` itself
  is run from by default (e.g. `E:\Dirk` if the player put the installer on
  their `E:` drive -- changeable on the directory page either way, see
  `.onInit` in the script) -- deliberately *not* Program Files, so the
  player can keep dropping new game folders in there later without needing
  admin rights every time (though, per `findSharedEngineRoot()` in
  `src/core/main.cpp`, a game folder doesn't actually have to live here at
  all -- it can sit anywhere, even a different drive, as long as it's
  launched via this install's `dirk.exe`). `dirk.exe` and `KimmyScript/`
  both land directly inside that folder.
- Registers `.dirk` for the current Windows user only (`HKCU`, no UAC
  prompt) so double-clicking `<GameName>.dirk` inside a game folder launches
  it in Dirk. See `resolveGameFolder()`/`hasDirkExtension()` in
  `src/core/main.cpp` for how Dirk itself expects that file to be found.
- Adds a Start Menu group with a shortcut to the install folder (so the
  player has an easy way back to it to add more games) and an uninstaller
  entry.

## Why the layout matters (or rather, doesn't have to)

`findSharedEngineRoot()` in `src/core/main.cpp` checks `dirk.exe`'s own
folder for `KimmyScript/` *first*, before anything else, and uses it
immediately if found -- confirmed by reading that function, not assumed.
Since this installer always puts `KimmyScript/` right next to `dirk.exe`,
that check always succeeds, which means a game launched through this
`dirk.exe` finds `KimmyScript/` no matter where the game's own folder
actually lives -- a completely different folder, a different drive
entirely, doesn't matter. (Only if `dirk.exe`'s own folder *didn't* have a
`KimmyScript/` -- not the case here -- would it fall back to walking *up*
from the game's own folder instead, the older/stricter behavior this used
to require.)

So the "everything side by side in one folder" layout below is a
convenience this installer chooses, not something a game strictly needs:
`dirk.exe`, `KimmyScript/`, and every game folder a player adds afterward
all live together in the one folder picked during install, so "add a new
game" is just "copy its folder in there, nothing else to configure" and
the "Dirk folder.lnk" Start Menu shortcut is the one obvious place to find
every game. A player is still free to keep a game folder anywhere else
entirely and just point its `.dirk` file's shortcut/launch at this
`dirk.exe` -- it'll work the same either way.

## Uninstalling

The uninstaller only removes what it put there itself: `dirk.exe`,
`KimmyScript/`, the downloader tool (`dirk_downloader.exe`,
`megatools.exe`, `manifest_link.txt`, if present), its own shortcuts and
registry keys. It deliberately never deletes the install folder's other
contents -- a player's own game folders, sitting right there alongside
`KimmyScript/`, are left untouched, and that includes any game
`dirk_downloader.exe` itself downloaded (its default destination is that
same folder, so a downloaded game's subfolder looks no different to the
uninstaller than one the player copied in by hand -- confirmed by actually
uninstalling with a simulated downloaded game folder present, see below).

## Icon

`.dirk` files (and `dirk.exe` itself, in Explorer/Task Manager/Alt-Tab) now
show the real Dirk Engine logo instead of Windows' generic file icon --
`src/core/dirk.ico` is embedded into `dirk.exe` via `src/core/dirk.rc`'s
`ICON` resource, and the installer's `DefaultIcon` registry value
(`"$INSTDIR\dirk.exe,0"`) already picks up whatever icon `dirk.exe`
carries -- no installer-script change needed for this to take effect once
you rebuild `dirk.exe` from the updated source.

`dirk.ico` is built from *two* source images, not a single picture scaled
down to every size, by `tools/generate_dirk_icon.py`:

- `assets/dirk_icon/dirk_icon_full.png` -- the full logo (character +
  circular badge + "DIRK engine" wordmark, same image used on the
  first-run indexing screen) -- used for the 64/128/256px frames, where it
  stays legible.
- `assets/dirk_icon/dirk_icon_notext.png` -- the same badge without the
  wordmark -- used for the 16/24/32/48px frames.

That split exists because the full logo's text turned into an unreadable
smudge once actually rendered at file-icon sizes -- confirmed by generating
a real magnified preview of every embedded frame (not assumed), which is
also how the 48px/64px cutoff between the two source images was picked
(48px: wordmark still a smudge; 64px: clearly legible). The 16px frame is
still fairly soft even without any text -- inherent to fitting a
detailed character illustration into 16x16 pixels, not something the
text/no-text split can fix. A simpler glyph (initial, silhouette) would
render crisper at 16px specifically, if that ever matters enough to be
worth a dedicated third source image.

Want different source art? Overwrite the two PNGs under `assets/dirk_icon/`
and re-run `python3 tools/generate_dirk_icon.py`, then rebuild.

## Verified so far

Not just compiled and reasoned about -- actually run under Wine (a real
x86-64 Windows Wine prefix in this sandbox, with a fake `E:` drive so the
root-of-drive default has a real second drive letter to resolve against,
not just `C:`), against a temporary dummy payload (stub `dirk.exe`,
`KimmyScript/` tree, tutorial PDF, and downloader trio, all removed again
afterward):

- `makensis dirk_installer.nsi` compiles cleanly, producing a valid
  `DirkSetup.exe` (confirmed a real Windows PE32 GUI executable via
  `file`, not just "no compiler errors").
- A silent install (`DirkSetup.exe /S`) from `E:\` actually lands
  everything in `E:\Dirk` as expected -- `dirk.exe`, `KimmyScript/` (with
  its contents), the tutorial PDF, and all three downloader files
  (`dirk_downloader.exe`, `megatools.exe`, `manifest_link.txt`) -- with no
  `/S` needed to prove the directory page's default itself, since
  `.onInit` sets `$INSTDIR` before any page shows.
- Uninstalling afterward (`Uninstall.exe /S`), with a fake downloaded-game
  folder (`Un Jeu Telecharge/`, containing a dummy video file) dropped into
  the install folder first to simulate what `dirk_downloader.exe` would
  leave behind: every file the installer itself put there is gone
  (`dirk.exe`, `KimmyScript/`, the PDF, all three downloader files,
  `Uninstall.exe`), while `Un Jeu Telecharge/` and its file are left
  completely untouched -- confirmed by listing the folder afterward, not
  assumed from reading the script.
- Not yet verified: any of this on a *real* Windows installation
  specifically (Wine implements the same Win32 APIs the script calls, but
  isn't Windows itself), double-clicking a `.dirk` file to confirm the
  file association actually opens Dirk from Explorer, and real
  `dirk.exe`/`dirk_downloader.exe`/`megatools.exe` binaries in place of
  the stubs used here. Worth a real run-through on Windows before handing
  `DirkSetup.exe` to players.

## Possible follow-ups (not done here)

- This installer is per-user (`HKCU`) and needs no admin rights. An
  all-users install would mean switching `RequestExecutionLevel` to
  `admin` and the registry writes from `HKCU` to the `HKLM`-backed
  `HKEY_CLASSES_ROOT`, plus probably defaulting the install folder to
  somewhere shared rather than `Documents`.
