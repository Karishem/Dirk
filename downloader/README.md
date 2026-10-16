# Dirk Downloader

`dirk_downloader.cpp` builds `dirk_downloader.exe`: a small standalone
Windows companion tool, separate from `dirk.exe` itself, that lists the
games available on a shared Mega folder and downloads whichever ones the
player checks -- by default into the same folder as `dirk.exe` (its
`defaultDestFolder()` looks for `KimmyScript/` next to itself, then one
level up, so it finds that folder whether it's installed loose or tucked
into its own `Dirk Downloader\` subfolder -- see `installer/README.md`).
That default is convenience, not a requirement: `dirk.exe` finds its own
`KimmyScript/` no matter where a game folder actually lives, even a
different drive (see `findSharedEngineRoot()` in `src/core/main.cpp`), so
"Parcourir..." can point anywhere with no downside.

It wraps [megatools](https://megatools.megous.com/), a free command-line
tool, to talk to Mega -- it does not implement the Mega protocol itself.

## Why a manifest file, not live Mega folder browsing

`megatools dl` is the only megatools command confirmed to accept a public
Mega link with no login at all, and it downloads -- there is no documented,
non-interactive way to just *list* a public folder's contents without
pulling its data. The one option that looks related (`--choose-files`) is
explicitly interactive in a terminal, with its exact protocol undocumented,
so it's not something this tool could drive reliably.

Instead: a tiny manifest text file lists each game's name and its own
individual Mega link, and is itself hosted as its own small Mega link. The
tool downloads that manifest first (small, fast, always current), builds
the checklist from it, then downloads only the specific games checked. This
matches the real setup this was built for: fewer than 50 games, no
frequent additions, so hand-maintaining one text file is simpler than
anything cleverer.

## Setting it up on the Mega side

1. Keep the existing shared parent Mega folder, with each game in its own
   subfolder inside it (unchanged from today).
2. For each game's subfolder, get its own "get link" from Mega's web UI
   (right-click the subfolder -> Get link). That's the game's entry link.
3. Create a plain text file, `manifest.txt` say, with one game per line:

   ```
   # Lines starting with # are comments, ignored -- as are blank lines.
   Chantze Stone=https://mega.nz/folder/xxxxxxxx#yyyyyyyyyyyyyyyyyyyyyy
   Triad Stone=https://mega.nz/folder/xxxxxxxx#zzzzzzzzzzzzzzzzzzzzzz
   ```

   Exactly `Name=Link`, nothing more -- the name is what the player sees in
   the checklist and becomes the destination subfolder's name, so keep it
   free of `\ / : * ? " < > |` (illegal in a Windows folder name).
4. Upload `manifest.txt` to Mega anywhere reachable the same way (the
   shared parent folder itself is fine) and get its own "get link" too.
   Adding, removing or renaming a game later is just editing this one file
   and re-uploading it -- no changes to the tool itself.

## .zdirk games: separate game, movie and media links (2026-09-28)

A game shipped as a `.zdirk` (see the main README, "Zipped games") can be
listed with two or three links instead of one, separated by `|`:

```
# Name = game link | movie link | media link (optional)
Timegal = https://mega.nz/file/...    | https://mega.nz/folder/...  | https://mega.nz/folder/...
Hayate  = https://mega.nz/file/...    | https://mega.nz/folder/...
```

- **game link**: "Get link" on the `.zdirk` FILE itself. Downloaded into
  `<destination>\<Name>\`. Checked after download: a zip cut short is
  refused with a clear message, and the previous `.zdirk` stays.
- **movie link**: the game's `Movie` folder (or a single movie file).
  Lands in `<Name>\Movie\` (a folder named `Video`, Hypseus-style, stays
  `Video\`). Whether megatools delivers a folder link as the folder itself
  or as its contents, both are handled.
- **media link** (optional): frontend media (3D box, marquee, video...),
  downloaded only when the player ticks "Also download frontend media"
  (remembered, registry `HKCU\Software\Dirk\DownloaderMedia`). Files land
  as they are in `<Name>\Media\` -- no frontend configuration is done.

Updates: the tool records, in a hidden `<Name>\.dirk-download` file, the
links each part was downloaded from. Mega gives a replaced file a new
link, and that new link goes into the manifest -- so a link that differs
from the recorded one means "updated", with no version number to keep.
The **Status** column shows `Not installed`, `Up to date`,
`Update available`, `Movie update available`, `Movie missing` or
`Media update available` or `Shortcut missing` (see below); downloading a checked game then fetches only
the parts that changed or are missing (a game update = the small `.zdirk`
only, never the movie again). `Cfg\` (settings, saves, high scores) is
never touched, and files the player added next to the movie (subtitles,
extra languages, Dirk's index) are kept. A movie already there without a
record (copied by hand) is kept and recorded, not downloaded again. A
`.zdirk` renamed on Mega replaces the old one (no two games left behind).
The old one-link form (`Name=Link` to the whole game folder) works as
before, and also gets `Installed` / `Update available`.

### Progress, and a download that stops arriving (2026-09-28)

megatools runs with `--no-progress` (its own redrawn progress line isn't a
documented format). Instead, every 2 seconds the tool measures what has
arrived on disk (the hidden `.dirk-download-tmp\` folder, each file's size
asked of the file itself, since Windows updates a folder listing lazily
for a file still being written -- which is also why Explorer can show it
stuck) and shows ONE live line in the log, updated in place:
`... 420.3 MB received, 2.1 MB/s, 3:20`. After 30 s with nothing new it
adds `-- nothing new for 0:45`; after 5 minutes it stops megatools,
removes the temporary folder and says Mega may be limiting downloads --
the game stays installed and its status (`Movie missing`...) makes the
next attempt fetch only what's missing.

Note: megatools downloads public links anonymously, so Mega's limits for
anonymous downloads apply to everyone -- the uploader's own Mega
subscription doesn't change them.

### Several versions in one .zdirk: the shortcuts are written by the tool

A `.zdirk` holding several games (e.g. `Hayate.zdirk` with `Hayate.dirk`
and `HayatePSX.dirk` at its top) needs, for each game other than the one
named like the zip, a tiny shortcut `.dirk` next to it (see the main
README, "Zipped games"). Nothing to put on Mega or in the manifest for
that: after each download (or on an already installed game, when checked
again) the tool reads the list of files inside the `.zdirk` -- its zip
directory only, nothing extracted -- and writes the missing shortcuts
itself:

```
-- HayatePSX: a game inside Hayate.zdirk (shortcut written by Dirk Downloader)
zdirk = "Hayate.zdirk"
script = "HayatePSX.dirk"
```

Same rule as Dirk for which scripts count: `.dirk` files at the top of the
zip or of its one top folder; the one named like the `.zdirk` gets none
(double-clicking the `.zdirk` runs it); a zip with a single `.dirk` gets
none at all; a zip with several and none named like it gets one per script.
On an update, a shortcut the tool wrote for a script no longer in the zip
is removed. A `.dirk` of the same name that is NOT a shortcut (a real game
script the player put there) is never overwritten or deleted -- the log
says it was kept. The shortcuts written are recorded (`shortcuts=` in
`.dirk-download`). Status `Shortcut missing` shows when one is absent
(e.g. a game installed by the previous version of the tool, or a shortcut
deleted by hand): checking the game and downloading writes it back with no
download at all.

Each part is downloaded into a hidden `<Name>\.dirk-download-tmp\` first,
then moved into place, so a failed download never leaves a half-written
game behind.

## Setting it up next to the tool

Three files sit side by side (normally the same folder `DirkSetup.exe`
installs `dirk.exe` into):

- `dirk_downloader.exe` -- this tool.
- `megatools.exe` -- the real Windows build of megatools, **not built by
  this repo**. `megatools.megous.com` is the project's usual landing page
  but goes down for maintenance sometimes -- when it does, the actual
  binaries live at <https://xff.cz/megatools/builds/builds/> regardless
  (confirmed reachable directly, independent of megous.com's own uptime;
  grab the newest `megatools-<version>-win64.zip`, or `-win32.zip` on a
  32-bit Windows). Two other routes to the same official binary: Chocolatey
  (`choco install megatools`) or Scoop (`scoop install megatools`), both of
  which fetch from that same `xff.cz/megatools/builds/` location under the
  hood, so either sidesteps megous.com's uptime too. All of this is
  documentation research, not something run against a real Mega server from
  here: see "Verified so far" below for what actually was and wasn't
  tested. The tool refuses to start with a clear message if `megatools.exe`
  is missing (see screenshot-grade verification below).
- `manifest_link.txt` -- a plain text file containing, on a single line,
  the "get link" for `manifest.txt` from step 4 above. Nothing else in the
  file. The tool refuses to fetch the catalogue with a clear message if
  this file is missing or empty.

On launch, the tool reads `manifest_link.txt`, runs `megatools dl` to fetch
that one small manifest file, parses it, and shows every listed game in a
checklist (all checked by default). Picking "Browse..." changes where
downloaded games land (default: see `defaultDestFolder()` in the source --
next to the tool itself, or one level up if this tool sits in its own
subfolder with `KimmyScript/` up there instead). The folder picked with
"Browse..." is remembered for next time (registry value
`HKCU\Software\Dirk\DownloaderLastDest`, the same key the installer
uses, removed by its uninstaller); if that folder no longer exists (an
unplugged drive, say) the tool says so in its log and falls back to the
default. "Download checked games"
downloads each checked game into its own subfolder there (created if
needed), one `megatools dl` call per game, logging progress live, filling
a progress bar (games completed / total -- not a byte-level percentage
within a single game's own download, since that would mean parsing
megatools' own progress-line format, which this README couldn't confirm
from a primary source -- see "Verified so far" below), and finishing with
a summary (`Done: N succeeded, M failed`).

The UI is in English throughout (an earlier version was in French; changed
on request).

**Important about `manifest_link.txt`'s link specifically**: it has to be
the "Obtenir le lien" / "Get link" for the manifest **file itself**
(right-click that exact file in Mega's web UI), which produces a
`https://mega.nz/file/...#...` link. Two other links look similar but don't
work here: a `https://mega.nz/fm/...` URL (what shows in the browser's
address bar while just navigating Mega's site logged in -- it has no
decryption key in it, so a login-free tool like megatools can't use it at
all and silently skips it) and a `https://mega.nz/folder/...` link for the
*parent* folder the manifest sits in (megatools then tries to download that
whole folder's contents, not just the one file, and fails differently). If
the manifest file gets replaced/re-uploaded on Mega later, its "Get link"
has to be regenerated too -- Mega ties a link's decryption key to that
specific upload, so an old link stops decrypting the new content.

## How to build dirk_downloader.exe

### Why build it yourself instead of using a prebuilt .exe

A `.exe` handed to you as a download (through this tool, email, a link,
anywhere) carries no history with Microsoft and isn't code-signed -- Windows
SmartScreen flags exactly that combination ("Windows protected your PC" /
unrecognized publisher), regardless of which compiler produced it or how
harmless it actually is. That's not a bug in the file, and not something
fixable from this end without a paid code-signing certificate. Compiling it
yourself, on your own machine, sidesteps the warning entirely: the result
never gets Windows' "downloaded from the internet" mark-of-the-web tag in
the first place, so SmartScreen has nothing to flag it over. This is why
the source (`dirk_downloader.cpp`) is included alongside the `.exe` in
every delivery, not just the binary.

### On Windows itself (recommended if you're avoiding SmartScreen)

1. Install [MSYS2](https://www.msys2.org/) (the standard way to get a real
   MinGW-w64 toolchain on Windows).
2. Open the **"MSYS2 MinGW x64"** shortcut specifically (not the plain
   MSYS2 shortcut -- that one's a different environment/compiler set) and
   run once:
   ```
   pacman -S mingw-w64-x86_64-gcc
   ```
3. In that same MinGW x64 shell, `cd` to the folder with
   `dirk_downloader.cpp` and run:
   ```
   g++ -std=c++17 -Wall -Wextra -municode -mwindows -static -static-libgcc -static-libstdc++ \
     dirk_downloader.cpp -lcomctl32 -lshell32 -lole32 -ladvapi32 -o dirk_downloader.exe
   ```
   Same flags as the cross-compile below (see why `-static*` matters
   further down) -- just `g++` instead of `x86_64-w64-mingw32-g++`, since
   on native Windows MinGW there's no cross-compile prefix needed.
4. The resulting `dirk_downloader.exe` is right there in that folder,
   ready to run alongside `megatools.exe` and `manifest_link.txt` as usual.

(MSVC's `cl.exe`/Visual Studio would also work in principle -- this file
only uses plain Win32 APIs, nothing MinGW-specific -- but hasn't been
tried; the `-static` flags above would need MSVC equivalents, e.g.
`/MT`, to keep the same no-extra-DLLs property.)

### Cross-compiled from Linux (how this project's own deliveries are built)

```
x86_64-w64-mingw32-g++ -std=c++17 -Wall -Wextra -municode -mwindows -static -static-libgcc -static-libstdc++ \
  dirk_downloader.cpp -lcomctl32 -lshell32 -lole32 -ladvapi32 -o dirk_downloader.exe
```

Same MinGW cross-compiler as the rest of Dirk (see the main README's
"Standalone Windows build" section) -- no CMake target for this one yet
since it's a single self-contained file with no shared dependency on the
rest of `src/`, but it could be folded into the main `CMakeLists.txt` as
its own `add_executable` later if that becomes worth it.

The `-static -static-libgcc -static-libstdc++` flags matter and were not
optional here: an earlier build of this same file, compiled without them,
produced a `.exe` that compiled cleanly and *looked* fine (`file` reported
a normal PE32+ GUI executable) but silently depends on
`libgcc_s_seh-1.dll` and `libstdc++-6.dll` from the MinGW install --
DLLs that don't exist on a player's Windows PC. Running that build under
Wine immediately surfaced the real failure (`Library libgcc_s_seh-1.dll ...
not found`) before any window even opened -- confirmed the same way
`main.cpp`'s own build already avoids this, see the identical comment next
to `target_link_options(dirk_core PUBLIC -static ...)` in the main
`CMakeLists.txt`. This is exactly why the tool gets run for real under
Wine rather than just cross-compiled and inspected with `file` -- a clean
compile alone would have shipped a broken installer-adjacent tool.

## Verified so far

**2026-09-28 (.zdirk games):** `selftest/run.sh` (Linux, Wine + MinGW)
runs the real download/update code against a fake `megatools.exe` and a
fake Mega store -- 30 checks: first install (2 downloads, no media),
game update (1 download, save and movie untouched), media asked later,
media and movie updates (flat and wrapped folder links, the player's own
file next to the movie kept), movie deleted, a cut download refused (and
the check proven useful: with it disabled the test fails), megatools
error, a game link that isn't a `.zdirk`, a `.zdirk` renamed on Mega, a
movie copied by hand, a Hypseus `Video` folder, the old one-link form.
Then the real window under Wine + Xvfb: Status column filled from the
disk, media box remembered, two games ticked and downloaded, statuses
updated to `Up to date` (screenshot).

**2026-09-28 (shortcuts for several versions in one .zdirk):** the same
self-test, now 45 checks. Added: `HayatePSX.dirk` written next to
`Hayate.zdirk`, none for the main script nor for a `.dirk` in a subfolder;
shortcut deleted -> `Shortcut missing` -> written back with 0 download;
update to a zip with one top folder (PSX shortcut removed, Saturn written,
the player's own `HayateArcade.dirk` kept and said so); back to a
one-version zip (shortcut removed); no script named like the zip (one
shortcut per script, names with spaces); one-script zips (no shortcut); a
zip64 `.zdirk` made with `zip -fz`; a non-zip refused. The shortcuts
written were then opened with Dirk's own code (`readZdirkShortcut()` +
`GameArchive::open()`, Linux): each opens the right script from the zip.
Real window under Wine + Xvfb: an installed Hayate without its shortcut
shows `Shortcut missing`.

**2026-09-28 (progress, stalled download):** self-test now 51 checks:
a movie arriving in 12 steps over 6 s gives rising `MB received` lines
marked to replace each other, with the speed; a download that stops
after 64 KB is stopped after the timeout (5 s in the test), clear message,
temporary folder removed, the `.zdirk` kept, status `Movie missing`. Real
window under Wine + Xvfb: one live line (`2.0 MB received, 0.5 MB/s,
0:04`, then `3.0 MB ... 0:06` in its place), then `done`.

Earlier checks:

Actually run under Wine (a real x86-64 Windows Wine prefix, not just
reasoned about), with a fake stand-in `megatools.exe` (a tiny compiled C
program that mimics real megatools' `dl --path <dest> <link>` interface
and exit codes closely enough to exercise every code path here -- see
caveat below for what this does and doesn't prove):

- `megatools.exe` missing next to the tool: clear error message box,
  clean exit, confirmed via screenshot.
- `manifest_link.txt` missing next to the tool: main window opens, catalogue
  fetch fails with a clear error message box and matching log line,
  "Download checked games" stays disabled, confirmed via screenshot.
- `manifest_link.txt` present, manifest fetch succeeds: catalogue parses
  correctly (3 test entries, `#`-comments and the `Name=Link` format both
  handled), checklist populates with every game **un**checked by default
  (changed on request -- see the dated entry below), "Download checked
  games" becomes enabled, confirmed via screenshot.
- Clicking "Download checked games" (via a real simulated click, not just a
  message posted from a script) with a mix of a link that "succeeds" and
  one that "fails" in the fake tool: per-game log lines stream in live, the
  progress bar fills in step with each finished game, controls (checklist,
  "Select/deselect all", the download button) grey out during the download
  and re-enable after, a subfolder is actually created on disk per game
  (confirmed by listing the destination folder afterward), and a final
  `=== Done: 2 succeeded, 1 failed. ===` log line reads the correct count
  for that 2-success/1-failure test case (no message box any more -- see
  below), confirmed via screenshot.

**Also fixed after a real run against a real Mega link** (on a real Windows
machine, not this sandbox): the first version of this tool passed
`--path` a specific *file* path when fetching the manifest, assuming
megatools would write the download there under that exact name. Real
megatools rejected that outright ("`<path> must be a directory`") --
confirmed against `megatools-dl`'s own manual, which documents `--path` as
always a destination *directory* the download is placed inside (under its
own remote name), for a file link exactly like a folder link. Fixed by
downloading into a fresh temp directory instead and reading back whichever
file megatools actually put there. Re-verified the same way as above (fake
stand-in enforcing the same "`--path` must be a directory" rule for
realism) once the fix was in: manifest fetch and the full download flow
both work end to end again.

**Since confirmed working end to end on a real Windows machine, with a
real Mega account and real links** -- the gap noted below (this sandbox
has no network access to mega.nz, confirmed separately: `curl` to
`mega.nz` and to Mega's own API host both fail at the network level here)
has since been closed by real-world use, not just documentation research.
Two real issues turned up along the way, both since fixed and
re-verified: `manifest_link.txt` pointing at the shared parent folder's
own link instead of the manifest file's own link (megatools tried to walk
the whole folder tree instead of fetching one small file -- fixed by using
the file's own "Get link", not the folder's); and a stale/duplicated
`manifest.txt` on Mega after an edit that didn't actually save to the
linked file (worth double-checking directly via the manifest link in a
browser if the catalogue ever looks out of date). Neither was a bug in
this tool's own code.

Also added since, on request: the whole UI translated to English (was
French), and a progress bar -- games completed out of the total in the
current run, updated live via `WM_APP_PROGRESS`, re-verified under Wine
with a simulated multi-game download exactly like the rest of this
section. Deliberately not a byte-level percentage within a single game's
own download -- that would mean parsing megatools' own progress-line
text, whose exact format this README was not able to confirm from a
primary source (a few source/mailing-list links turned up dead ends), so
it's left as a documented "not attempted, not guessed" gap rather than
built on an unverified assumption -- see the follow-ups below.

**2026-09-22, three UX changes on request, all re-verified under Wine with
real simulated clicks/screenshots (fake `megatools.exe` this time made to
sleep 12s with zero stdout before "finishing", to actually exercise the
no-output case rather than just reason about it):**

- **Progress bar gave no sign of life during a single game's download.**
  Root cause confirmed by reading the code, not guessed: the per-game
  `megatools dl` call runs with `--no-progress` (its own carriage-return
  redraw doesn't translate into this line-based log, and its exact text was
  never confirmed from a primary source -- see the follow-up below, still
  true), so a big/slow game can legitimately produce zero output for
  minutes; the old blocking `ReadFile` loop looked identical whether that
  was normal or actually hung, and the whole-run progress bar only moves
  once per *finished* game, not smoothly, so with one or few games checked
  it visually sits still for the whole download. Rather than reverse-engineer
  megatools' undocumented progress format to drive a real percentage (the
  guessy path this README already avoided once, see below), added a plain
  heartbeat instead: `runProcessCapture` now polls the child's output pipe
  with `PeekNamedPipe` (anonymous `CreatePipe` handles don't support
  overlapped/async `ReadFile` per MSDN, so peek-then-read is the correct
  approach, not a workaround) and logs a `"... still working (Ns) ..."`
  line every 5s of silence (English, matching the rest of the UI -- an
  earlier draft of this had it in French by mistake, fixed on request).
  Confirmed via a real run: with the fake tool sleeping 12s with no output,
  the log showed the heartbeat at 5s and 10s right on schedule before the
  game's own "done" line, screenshot captured mid-wait.
- **Checklist now starts fully unchecked** (`GameEntry::checked` and
  `AppState::allChecked` both flipped to `false`) -- was ticked-by-default
  before. Confirmed via screenshot: a fresh catalogue fetch shows every
  game's checkbox empty.
- **Removed the "Done: N succeeded, M failed" message box** at the end of a
  download run -- confirmed by window-listing under Wine (only the main
  "Dirk Downloader" window exists after a run finishes, no second window).
  The same text is still written to the log, just set off with `===` so
  it's easy to spot without reading every line above it
  (`=== Done: 1 succeeded, 0 failed. ===`, confirmed via screenshot).

## Possible follow-ups (not done here)

- No byte-level progress *within* a single game's own download (the
  overall "games completed / total" progress bar above is as far as this
  goes) -- megatools' own progress output is suppressed via
  `--no-progress` specifically because its carriage-return redraw style
  doesn't translate cleanly into the log box's line-based display, and
  this README couldn't confirm the exact text megatools prints for a
  percentage from a primary source to parse against. Worth attempting for
  real (drop `--no-progress`, log a few real download runs, adjust the
  parser to match) now that this ships on a real machine with real
  network access, rather than guessing the format blind.
- No retry-on-failure button for just the games that failed -- rerunning
  means re-checking them and clicking "Download checked games" again
  (which re-checks every currently-checked game, not just the failed
  ones).
- No built-in way to verify a download's integrity beyond megatools' own
  exit code -- fine for the "check the log, redownload if unsure" scale
  this was built for, less so at a much larger catalogue size.
