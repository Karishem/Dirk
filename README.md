# Dirk

A clean-room, from-scratch reimplementation of the native engine layer that
[KimmyScript](../) games run on -- built to run the existing ~21,000 lines
of KimmyScript Lua unmodified, without depending on Singe (the original
engine). See the design doc for the full rationale, API surface analysis,
and phased plan.

Built by reading KimmyScript's own Lua source (what it actually calls) and
Singe's official manual -- never Singe's own source or binary, which we
don't have.

## Building

Dependencies, as pkg-config names:

- `sdl3`, `sdl3-image`, `sdl3-mixer` (SDL_mixer 3.x), `sdl3-ttf`
- `libavformat`, `libavcodec`, `libavutil`, `libswscale`, `libswresample`
  (FFmpeg 7.1 and 8.1 are both tested)
- `lua5.1` -- optional, see below

Plus `build-essential cmake pkg-config` (or your system's equivalent).

```bash
cmake -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
```

**Lua 5.1.** KimmyScript and the Wizard need exactly Lua 5.1 (not 5.2+).
A copy of Lua 5.1.5 is included in `src/third_party/lua-5.1.5` (MIT, see
its `README.dirk`). CMake uses the system's `lua5.1` when pkg-config finds
one, and the bundled copy otherwise. Pass `-DDIRK_BUNDLED_LUA=ON` to always
use the bundled copy -- this is what a distribution package (Batocera...)
should do, so the result never depends on what the build machine has.

**No LuaFileSystem needed.** KimmyScript calls `require("lfs")`, but Dirk
provides the two functions it uses (`lfs.dir`, `lfs.attributes`) itself.

**Other builds.** The self-contained Linux bundles (Batocera / ArkOS
"ready to copy" packages) are built with `tools/build_linux_bundle_zig.sh`;
`tools/build_linux_batocera_libs.sh` builds the exact library versions
Batocera ships, to test against them; the single-file Windows build is
described below in "Standalone Windows build".

## Running

Every game folder has one top-level launcher file, named `<Game>.dirk`
(e.g. `DLe/DLe.dirk`, `Hayate_HD/Hayate.dirk`) -- same KimmyScript/Lua
content either way, dirk just runs whatever it's given regardless of
extension, but given its own extension on purpose: every *other* script
file in a game folder (the shared engine under `KimmyScript/Script/`, the
game's own `Script/addons.kimmy`) is now a `.kimmy` file, dofile'd
internally and never meant to be launched directly, so giving the real
entry point a different extension means it can't be confused with one of
those. Unlike the old `.singe` -> `.kimmy` rename for those internal
files, this one only touches the top-level launcher itself and is a pure
rename -- nothing dofiles it by name, so there's no internal reference to
update, just copy the game's existing top-level file to `.dirk` (or
rename it in place).

That top-level file's own *content*, though, is an untouched copy of the
original launcher -- written before the `.singe` -> `.kimmy` rename
existed, so it still hardcodes the shared engine's OLD extension where it
pulls it in, e.g. a real game's launcher (confirmed by reading it) has:

```lua
dofile("./KimmyScript/Script/globals.singe")
```

Once `globals.singe` has actually been renamed to `globals.kimmy` on disk,
every single game's launcher hits this the same way -- not a one-off typo.
Rather than requiring that line be hand-edited in every game (which would
quietly break the "runs the existing scripts unmodified" promise this
project is built on), `dofile()` itself has a compatibility fallback: if
the exact path it's given doesn't exist but swapping its `.singe`/`.kimmy`
extension resolves to a real file, that's used instead automatically, with
a one-line note on stderr so the substitution isn't silent. Works in
either direction, and for any `dofile()` call anywhere in the script chain,
not just that one line -- so a folder with a partial rename (some shared
files still `.singe`, others already `.kimmy`) works too.

That line can also be deleted from a game's launcher entirely, not just
left to fall back -- dirk loads the shared engine bootstrap itself now
(`KimmyScript/Script/globals.kimmy`, or `.singe`, same fallback as above),
natively, before the game's own top-level script runs at all, so the job
that line used to do is dirk's from the start rather than something each
game has to ask for. This is safe regardless of which order things happen
in: the one piece of state the shared engine needs at load time --
`framework.kimmy`'s own `DIR` global, derived from `dirkGetScriptPath()` --
comes from dirk's native, C++-side game-folder resolution (see `Running`
above), already settled before any Lua runs at all, not from anything the
game's own script sets up first. A launcher that still has the line keeps
working exactly as before either way -- dirk recognizes its own already-
loaded file being asked for a second time and skips re-running the whole
chain (which would otherwise re-execute every shared file a second time and
leak a duplicate sound handle from every `soundLoad()` at the bottom of
`globals.kimmy`), printing a one-line note that it's now safe to delete.
Verified end to end against the real shared-engine source (not just a
synthetic test): zero Lua errors either way, whether the line stays or
goes.

```bash
build/dirk <game-folder>
```

Normal use: pass a game's own folder and dirk figures out the rest -- the
`.dirk` launcher (the one such file directly in that folder) and the disc
video (the largest file in its `Movie/` subfolder, since a folder can hold
more than one -- an attract-mode loop alongside the real disc, say -- with
nothing naming which is which). Both are printed on startup so a wrong
guess is obvious.

```bash
build/dirk <game's .dirk file>
```

Same auto-discovery, given the launcher file instead of the folder --
this is what makes double-clicking it in Explorer work as "launch the
game" once `.dirk` is associated with `dirk.exe` (right-click any
`.dirk` file -> Properties -> "Open with" -> Change -> `dirk.exe` ->
"Always use this app"). Combined with a shortcut or drag-and-drop of the
game folder onto `dirk.exe` (both use the form above), this is what makes
a `.bat` per game unnecessary.

If the exact `.dirk` path given doesn't exist -- e.g. a shortcut or a
typed path assumed the `<Folder>.dirk` convention (`DLe/DLe.dirk`) but
this particular game's launcher isn't named that way (`Hayate_HD/
Hayate.dirk`, not `Hayate_HD/Hayate_HD.dirk`) -- dirk falls back to
the same single-`.dirk`-file search the folder form uses: if exactly
one `.dirk` file actually exists in that folder, it's used instead,
with a warning printed so the mismatch is obvious rather than silent.
Only actually fails (rather than falling back) when the folder has
zero or more than one `.dirk` file, same as the folder form.

```bash
build/dirk <video-file> [script.lua]
```

Manual/override form, for when a folder doesn't fit the one-`.dirk`/one-
obvious-video shape, or to sanity-check a raw video with no script at all
(just plays it, no KimmyScript involved). `script.lua` is the game's own
launcher script -- any extension is accepted here, `.dirk` or `.kimmy`.

However it's launched, dirk switches its own working directory before
running the script -- not to the game's own folder, but to the one
*containing* `KimmyScript/` as a sibling (one level up from the game
folder in every install seen so far; dirk searches a few levels further
up too, in case of a deeper layout, before giving up and defaulting to
that one-level-up shape). That's the one thing in the real engine source
that's a plain, hardcoded relative path -- `globals.kimmy` and
`main.kimmy` `dofile()`/`spriteLoad()` the shared engine and skin assets
as literal `"./KimmyScript/..."`, cwd-relative, no exceptions. Everything
*game*-specific (`Script/addons.kimmy`, `Sounds/*.wav`, `Cfg/` -- which is
also where dirk itself looks for `controls.cfg`, see below -- `Overlay/`,
`Bezel/`, ...) goes through the `DIR` global instead, which dirk always
resolves to an absolute path, so those don't care what cwd is at all --
true no matter which of the three forms above launched it, or from where.

`DIRK_SCREENSHOT=/path/to/out.png` dumps the final composited frame before
exit -- a debug aid for checking render output when running headless
(no window to look at).

### Console output

A normal launch (no environment variables set) prints nothing to the
console beyond a genuine, actionable problem -- video won't open, SDL/Lua
init failed, a script error, sound/rendering degraded because a subsystem
failed to start, and the like. Everything else -- the video's resolution/
frame count, measured audio latency, which audio device/driver got opened,
`controls.cfg`'s binding count, `.kimmy`/`.singe` compatibility notes,
gamepad connect/disconnect, and the various audio-sync tuning diagnostics
added over the course of getting AV sync right (`catchUpAudio` overshoot,
adaptive buffering growth) -- is routine/informational, not a warning, and
stays silent unless `DIRK_VERBOSE=1` is set:

```bash
DIRK_VERBOSE=1 dirk.exe <game-folder>
```

`DIRK_DEBUG_SHOT`/`DIRK_DEBUG_CLICK` (the F6/F7 hotkeys) and
`DIRK_DEBUG_INPUT`/`DIRK_DEBUG_TICK` (raw scancode/detailed AV-sync tick
logging) are separate, narrower debug aids, each already off unless its own
variable is set -- `DIRK_VERBOSE` doesn't turn any of those on by itself.

All of the above (`DIRK_VERBOSE` and the rest) only ever changes *what*
gets printed -- not *where*. On Windows, dirk is a GUI-subsystem .exe (no
console window flashing open on launch), so `std::fprintf(stderr, ...)`
normally only actually shows up somewhere when dirk was itself launched
from an existing console (cmd/PowerShell/MSYS2) -- launched by double-
click/file-association instead, with no console to attach to, every one of
those prints just goes nowhere, silently, regardless of `DIRK_VERBOSE` or
anything else. That's invisible almost all the time, but it becomes a real
problem the moment a bug's *symptom itself* depends on the launch method.
Confirmed at least once, and worth knowing about specifically: double-
clicking a `.dirk` file always runs whatever `.exe` is currently
registered as the default app for that extension (Explorer file-type
association), which is easy to lose track of if more than one build/copy
of `dirk.exe` exists on disk -- a freshly built `.exe` run explicitly from
cmd behaved correctly while the exact same-looking double-click launch
didn't, which looked at first like a launch-method-dependent engine bug
and turned out to just be a stale `.exe` sitting at the double-click
target, never actually replaced by the newer build. If cmd and
double-click ever disagree, checking that both are actually running the
same `.exe` (e.g. compare file timestamps, or check Explorer's "Open
with" -> "Always use this app" target) is worth doing before chasing a
deeper cause.

`DIRK_LOG_FILE=<path>` exists for exactly that case -- checked before
absolutely anything else in `main()`, independent of `DIRK_VERBOSE`/
hideConsole/how dirk was launched. When set, every diagnostic in this
file -- including ones from `DIRK_VERBOSE`/`DIRK_DEBUG_INPUT`/etc, so set
those too if the specific thing being diagnosed needs them -- goes to that
file instead, unbuffered, whether or not there's a console at all:

```bash
DIRK_LOG_FILE=C:\dirk_log.txt DIRK_VERBOSE=1 DIRK_DEBUG_INPUT=1 dirk.exe <game-folder>
```

To capture a launch-method-dependent bug specifically, set `DIRK_LOG_FILE`
(and whichever other `DIRK_*` variables the symptom needs) as a persistent
user/system environment variable -- Windows' System Properties ->
Environment Variables, not `set DIRK_LOG_FILE=... && dirk.exe` typed into
a cmd prompt, since that's still a console launch and defeats the point --
so it's picked up on a genuine double-click too. Then reproduce the issue
exactly the way it actually happens, and read the file afterward.

`DIRK_LOG_FILE` is the power-user/scripted-launch mechanism -- it needs an
environment variable set, which isn't realistic to talk a random stuck
player through over chat or a phone call. **Generate Log**, in the game's
own "UI Options" menu (see "Per-game UI Options" below), is the
zero-configuration version of the same idea: turning it On makes dirk write
this run's whole log to `dirk_log.txt`, right next to the game folder the
player is already in -- no environment variable, no `dirk_settings.cfg`
editing, nothing to type. It also forces `DIRK_VERBOSE`/`DIRK_DEBUG_INPUT`
on for that run (in-process only -- never touches the player's own
environment) so the file is actually useful without them configuring
anything either, input problems being the single most common class of
"it's stuck" report. Talking someone through it is just: "go to Options,
turn on Generate Log, do the thing that's failing again, then find
`dirk_log.txt` next to the game and send it to me." Turning it back Off
only changes what the *next* launch defaults to -- it doesn't try to
un-redirect stdout/stderr mid-run once a log is already being written for
the current one. If `DIRK_LOG_FILE` is *also* set for the same run,
`DIRK_LOG_FILE` wins (it's checked first, before anything else) and
Generate Log's own redirect is skipped rather than clobbering it.

### Launch settings (fullscreen, volumes, scanlines, console, bezel, generate log)

Dirk remembers a small set of "how do you want this to look/sound" choices
between launches: fullscreen/windowed, the movie's own volume vs. sound-
effects volume, the scanline overlay, the per-game bezel on/off, and --
Windows only -- whether to show a debug console (see `LaunchSettings.h`).
None of this has a standalone native settings screen any more (there used
to be one, opened by F10 -- removed once every field became reachable from
a game's own in-script "UI Options" menu instead): dirk boots straight into
the game every time, using `LaunchSettings.h`'s own defaults (fullscreen
on, movie volume 100, sound-effects volume 60, no scanlines, no bezel) the
very first time a given game is launched.

Two native hotkeys work in any game, with no menu involved:

- **F11** -- instant fullscreen/windowed toggle, same convenience most PC
  games/browsers offer.
- **F10** -- instant bezel on/off toggle, a no-op for a game with no
  `Bezel/bezel.png` at all.

Both persist immediately to the shared `dirk_settings.cfg` (see below), the
same as every `dirkSetXxx()` call from KimmyScript does (`main.cpp`'s
`ctx.launchSettingsDirty` handling) -- but, like any native hotkey bypassing
KimmyScript entirely, NOT to the current game's own per-game `Cfg/game.cfg`
(see "Per-game UI Options" below); only a change made from the game's own
in-script menu gets saved per game.

`dirkGetFullscreen()`/`dirkSetFullscreen()`, `dirkGetVolumeVldp()`/
`dirkSetVolumeVldp()`, `dirkGetVolumeNonVldp()`/`dirkSetVolumeNonVldp()`,
`dirkGetScanlines()`/`dirkSetScanlines()`, `dirkGetHideConsole()`/
`dirkSetHideConsole()`, `dirkHasBezel()`/`dirkGetBezelEnabled()`/
`dirkSetBezelEnabled()`, and `dirkGetGenerateLog()`/`dirkSetGenerateLog()`
are the native functions a KimmyScript script uses to read/change these one
field at a time -- e.g. a game's own 4th "UI Options" screen alongside its
existing Game/Graphic/Extra Options ones (see `service.kimmy`'s
`doServiceMenuU()`), listing them as ordinary toggleable/adjustable items
the same way it already does with its own `dip_*` fields. Every setter just
writes the field and flags it dirty; `main.cpp`'s loop re-applies the live
effect and persists to `dirk_settings.cfg` on its next iteration.

```lua
-- e.g. from a "UI Options" screen's own item handling:
dirkSetFullscreen(not dirkGetFullscreen())
dirkSetVolumeVldp(math.min(dirkGetVolumeVldp() + 10, 100))
if dirkHasBezel() then dirkSetBezelEnabled(not dirkGetBezelEnabled()) end
```

The console option (Windows only, ignored elsewhere): real Singe always
launches with a console window showing its logs and errors alongside the
game, which is expected during development but tends to spook a player who
wasn't expecting a black text window. dirk.exe itself never opens a console
of its own (it's a GUI-subsystem build, see below) -- it only *attaches* to
one that's already there when launched from an existing terminal (cmd/
PowerShell/MSYS2), same as Singe would print to. `dirkSetHideConsole(true)`
detaches from that console entirely (and skips attaching on the next
launch) so the game runs silently even when started from a terminal; off
by default. Not exposed on any menu -- only reachable from KimmyScript.

The Generate Log option is the one covered in detail under "Console
output" above -- a zero-configuration, on-demand troubleshooting log,
reachable from the same "UI Options" menu as everything else here.

#### `dirk_settings.cfg` location, and games moved outside dirk's own folder

`dirk_settings.cfg` lives at `<shared engine root>/KimmyScript/Cfg/
dirk_settings.cfg` -- the same search used to `chdir()` for KimmyScript's
own `"./KimmyScript/Script/..."` `dofile()`s (see `findSharedEngineRoot()`),
run once more, early, against whichever game was passed on the command
line. (With no game argument at all -- a bare usage error -- there's
nothing to resolve this against, so dirk just prints the usage message and
exits without touching any settings file.)

`findSharedEngineRoot()` checks two things, in order:

1. **`dirk.exe`'s own install folder** (`exeDirectory()` -- resolved
   properly per platform, e.g. `GetModuleFileNameA()` on Windows, never
   just argv[0] taken at face value, which can be a bare filename or a
   relative path depending on how dirk was launched). Since the installer
   puts dirk and `KimmyScript/` in the same folder together, and dirk
   doesn't run without both anyway, this is normally an instant match.
2. Only if that fails: the older upward search from the *game's own*
   folder (up to 6 levels), looking for a `KimmyScript` sibling.

This matters for a real, previously-unsolved case: a game folder moved
somewhere else entirely -- a different drive, no shared ancestor with the
dirk install at all ("jeux déportés du répertoire de dirk"). Before this
check existed, that game would launch (Windows still runs whatever `.exe`
is registered for `.dirk` files, see "Console output" above) but couldn't
find its own `KimmyScript/`, since the old upward-search-from-the-game
strategy has nothing to walk up to on an unrelated drive/folder. Checking
`exeDir` first fixes this with zero configuration: as long as dirk itself
stays installed next to `KimmyScript/` (which it already is), a game can
live absolutely anywhere and still find it. The old upward search stays as
a fallback for setups where that first check doesn't apply (e.g. a dev/test
build with no real "install" to speak of).

#### Per-game UI Options

A game's own "UI Options" menu screen (`service.kimmy`'s `doServiceMenuU()`)
saves these same six fields into *that game's own* `Cfg/game.cfg` --
`dirk_Fullscreen`/`dirk_VolumeVldp`/`dirk_VolumeNonVldp`/`dirk_Scanlines`/
`dirk_BezelEnabled`/`dirk_GenerateLog`, appended after every
`CONFIG_FIELDS` entry -- the same way its sibling screens already save
their own `dip_*`/`Show*` fields, so each game remembers its own choices
independently (a game.cfg saved before this was added simply has no such
lines yet; `readConfig()` leaves whatever's already loaded untouched in
that case, exactly like a short/old `CONFIG_FIELDS` file already does
today).

#### Loading screen (first-time video indexing only)

The very first time a given game's disc video is opened, dirk has to walk
the whole file once to build its frame index (`VideoSource::buildIndex()`
in `src/video/VideoSource.cpp`) before it can play anything -- a
demux-only pass (no decoding), fast even on a large disc, but not
instant. The result is cached next to the video as `<video>.dirkindex`, so
every later launch just loads that back and skips the scan entirely.

Rather than leave the screen black for however long that first pass
takes, dirk shows a small loading screen -- an optional logo image,
vertically centered as one block with an animated "INDEXING." /
"INDEXING.." / "INDEXING..." caption below it (cycling every 400ms) --
for exactly that one case, and only that case: `main.cpp` checks whether
the cache file already exists (a cheap `stat()`,
`VideoSource::hasCachedIndexFile()`) *before* deciding whether to draw
anything, so an ordinary launch with the cache already in place never
shows this screen at all, not even briefly. The caption's box is sized to
its widest state ("INDEXING...") up front, so the word itself never
shifts side to side as the dot count animates -- only the trailing space
fills in.

The index build itself (`VideoSource::finishOpening()`) runs on a
background thread while this animates on the main thread, so the
animation keeps moving (and the window keeps responding to the OS) for
the whole, real duration of the build -- not a fixed-length decoration
shown before or after the actual work.

Two optional loose files, resolved against `<shared engine root>` (the
same fixed point as `dirk_settings.cfg` above) the same way that file's
own folder is: `<shared engine root>/KimmyScript/Cfg/` is checked first,
then the bare `<shared engine root>` (next to `dirk.exe`) as a fallback
for an older install that still has them there:

- `dirk_loading.png` -- the logo, centered, scaled to fit (capped at 40%
  of the shorter screen dimension so it can't dominate a small window).
- `dirk_loading_font.ttf` -- the font the caption is drawn in. Ships here
  as DejaVu Sans Bold (`assets/dirk_loading/dirk_loading_font.ttf` in this
  repo, freely redistributable, commonly bundled on Linux) -- drop in a
  different `.ttf` under the same name to use another font.

Put both in `KimmyScript/Cfg/`, next to `dirk_settings.cfg` -- keeps the
install root itself free of loose files. Either file missing just means
less to draw (caption alone on black, or the image alone with no
caption) -- never a failed launch; this is cosmetic, not load-bearing.
Same reasoning as the bezel: a missing optional asset degrades
gracefully rather than erroring.

#### Kimmy branding logo

The small "Kimmy" logo drawn top-right on the menu/options screens used to
be entirely KimmyScript's own doing: `fillOVL(KIMMY,"Kimmy.png")` loaded a
loose `Kimmy.png` once, and six call sites across `main.kimmy`/`service.kimmy`
drew it every frame with `spriteDraw(OVLW-KIMW-LINEH,LINEH,sprite[KIMMY])`.
Both the image and its position were therefore plain-text, editable facts --
easy for anyone to change by editing the script or swapping the file.

The image and its position are now native instead, behind a single
`dirkKimmyLogo()` call (`l_dirkKimmyLogo` in `src/lua/api_singe.cpp`):

- The logo's pixels are compiled directly into the `dirk` binary
  (`src/lua/KimmyLogoData.h`, generated from `assets/kimmy_logo/
  kimmy_logo_source.png` by `tools/embed_kimmy_logo.py` -- the one asset in
  this project that is deliberately *not* a loose file next to `dirk.exe`,
  since the whole point here is that it can't be swapped out that way). To
  change the logo, replace that source PNG and re-run the script, then
  rebuild.
- The on-screen position is computed from the same values KimmyScript's own
  formula used (`ctx->overlayWidth`/`overlayHeight`, already set by
  `overlaySetResolution()` and scaled for `dip_Res`'s Full/Lores/Ultralo
  modes; `LINEH = overlayHeight/80`) -- so the logo lands exactly where it
  always did, but the number is no longer something a script can pass in.

What's deliberately left alone: KimmyScript still decides *whether* and
*from where* to call `dirkKimmyLogo()` -- same as any other native
binding -- so the logo still only appears on the screens whose print
functions call it, unchanged from before. `globals.kimmy`'s `KIMMY = 278`
handle constant is now unused (nothing loads or indexes a sprite for it
any more) but harmless to leave in place.

#### Generic arcade joystick support (unrecognized controllers)

SDL only treats a device as a "game controller" (`GAMEPAD_0..3` in
`controls.cfg`) if it matches an entry in SDL's own built-in/community
mapping database. Many DIY or generic arcade control panel encoders
(reported to Windows as e.g. "Generic USB Joystick") don't match anything in
that database -- SDL still sees the raw device, but none of the
`GAMEPAD_0..3` bindings ever fire for it, which looks like the joystick
"doesn't work" even though the OS (and `joy.cpl`/Windows' own game
controller test) sees it fine.

Dirk now drives these through a second, generic path: `RAWJOY_0..3`, exposed
in `controls.cfg` exactly like `SCANCODE`/`GAMEPAD_0..3` are, but built from
the joystick's own *raw* button/axis/hat indices instead of a fixed
per-controller-type enum -- so any unrecognized joystick can be wired up
from `controls.cfg` alone, with no engine rebuild and no code specific to
one exact piece of hardware:

- `RAWJOY_<n>.BUTTON_0` .. `BUTTON_31` -- one entry per raw button index.
- `RAWJOY_<n>.AXIS_<a>_NEG` / `AXIS_<a>_POS` -- the two digital directions
  of raw analog axis `a` (0-7), thresholded the same way `GAMEPAD_0..3`'s
  stick axes already are.
- `RAWJOY_<n>.HAT_<h>_UP` / `_DOWN` / `_LEFT` / `_RIGHT` -- the four
  directions of raw hat (POV/D-pad) index `h` (0-3).

`<n>` (0-3) is assigned in connection order, same as `GAMEPAD_0..3`.

To wire up a new unrecognized joystick: run `dirk.exe` (or `dirk`) with the
environment variable `DIRK_DEBUG_JOYSTICK=1` set, then press each physical
button and move each stick/hat one at a time. Each one prints the exact raw
index it fired, e.g.:

```
[dirk] DIRK_DEBUG_JOYSTICK: joystick 0 button 3 DOWN
[dirk] DIRK_DEBUG_JOYSTICK: joystick 0 axis 1 = -32768
```

Then bind those raw indices in `controls.cfg`, the same way `SCANCODE`/
`GAMEPAD_0` entries already are, e.g.:

```lua
INPUT_ACTION_1 = { SCANCODE.LCTRL, GAMEPAD_0.BUTTON_A, RAWJOY_0.BUTTON_3 }
INPUT_UP       = { SCANCODE.UP,    GAMEPAD_0.DPAD_UP,  RAWJOY_0.AXIS_1_NEG }
```

`controls.cfg` entries are arrays, so a `RAWJOY_0` binding can simply sit
alongside the existing keyboard/`GAMEPAD_0` ones for the same logical
input -- nothing has to be removed.

##### Several unrecognized joysticks connected at once

`<n>` in `RAWJOY_<n>` is assigned in *connection order* -- the order
Windows/SDL happens to enumerate devices at that boot, not anything Dirk
can control. With only one unrecognized joystick plugged in, it's always
`RAWJOY_0` and this doesn't matter. But a real cabinet can have several
similar unrecognized devices at once (confirmed case: two Sinden light
guns *and* the actual arcade stick, all four reported as "unrecognized
joystick") -- which one lands on `RAWJOY_0` vs `RAWJOY_2` is not
guaranteed to stay the same from one boot to the next.

The simple, verified fix: bind the same raw button/axis to **all four**
`RAWJOY_0..3` in each `INPUT_*` entry, so it doesn't matter which slot the
real joystick ends up in on a given boot:

```lua
INPUT_ACTION_1 = { SCANCODE.LCTRL, GAMEPAD_0.BUTTON_A,
                    RAWJOY_0.BUTTON_3, RAWJOY_1.BUTTON_3, RAWJOY_2.BUTTON_3, RAWJOY_3.BUTTON_3 }
```

The only trade-off: if one of the *other* connected raw devices (e.g. a
light gun sitting unused) happens to send an event on the same raw index
while this game is running, it would also fire that input. In practice
this is a non-issue as long as nobody touches the other device during
play, and `controls.cfg` is per-game, so it's easy to only do this
duplication for games that actually need it.

To find out which `RAWJOY_N` a specific device landed on for a given
boot (useful for debugging, not required for the fix above): run with
`DIRK_VERBOSE=1` and look for a line for each unrecognized device, e.g.
`raw joystick connected: "Generic USB Joystick" -> RAWJOY_2`.

#### Mouse button support (light guns in mouse-emulation mode)

Some games running on Dirk aren't KimmyScript games at all -- they're
"classic Singe" games (Mad Dog McCree and similar) that use Dirk's native
engine (`discSearch`/`spriteDraw`/`onInputPressed`/`onMouseMoved`/...) but
ship their own local copy of `framework.singe` and their own game logic,
outside KimmyScript entirely. Several of these are on-rails shooters that
read a continuous cursor position (`mousex`/`mousey`) for aiming and a
button press for the trigger -- which is exactly what a light gun
(a Sinden, for instance) sends when it's configured in *mouse-emulation*
mode rather than joystick mode: motion as mouse movement, trigger as a
mouse click.

Cursor movement already worked before this -- SDL's mouse-move events were
always forwarded to `onMouseMoved`. What was missing was the click itself:
`controls.cfg`'s `MOUSE_0.*` entries (defined by `framework.singe`
alongside `SCANCODE`/`GAMEPAD_0..3`) were parsed but deliberately ignored,
since Dirk had no native handling for mouse buttons yet. That's now wired
up the same way every other physical input source is -- through
`controls.cfg`, not hardcoded:

- `MOUSE_0.BUTTON_LEFT` / `BUTTON_RIGHT` / `BUTTON_MIDDLE` / `BUTTON_X1` /
  `BUTTON_X2` -- one entry per physical mouse button.
- `MOUSE_0.WHEEL_UP` / `WHEEL_DOWN` exist in `framework.singe` too, but
  aren't wired to anything yet (no game examined so far needed them).

```lua
INPUT_ACTION_3 = { SCANCODE.SPACE, GAMEPAD_0.BUTTON_X, MOUSE_0.BUTTON_LEFT }
```

As with `RAWJOY_0..3`, this is additive -- add `MOUSE_0.BUTTON_LEFT`
alongside whatever keyboard/gamepad bindings a game's `controls.cfg`
already has for its trigger action, rather than replacing them.

**`MOUSE_1..3` are defined by `framework.singe` (same idea as
`GAMEPAD_1..3`, for a second/third/fourth device) but are *not* wired up,
deliberately.** SDL2's standard mouse events don't identify which physical
mouse sent them -- Windows composites every mouse into one OS-level
pointer/click stream -- so there's no reliable way to tell two mice apart
without switching to a platform-specific API (Windows' Raw Input, in
particular), which is a separate, bigger piece of work than this. A
`controls.cfg` entry referencing `MOUSE_1.BUTTON_LEFT` will simply never
fire; every physical click is always seen as `MOUSE_0`, regardless of
which mouse (or light gun in mouse mode) sent it. If a cabinet only ever
has one active light gun on screen at a time, this isn't a practical
problem -- but it's worth knowing about before relying on `MOUSE_1..3` for
simultaneous multi-gun input.

#### Classic Singe game compatibility (missing native bindings, startup mouse state)

A few gaps surfaced while getting a real classic Singe game (Mad Dog McCree,
specifically an HD hitmap conversion of it) running on Dirk end to end,
beyond the mouse-button work above:

- **`overlayGetWidth()` / `overlayGetHeight()`** -- the read-back half of
  `overlaySetResolution()`. Some games call this right after setting the
  overlay resolution, to compute their own UI scaling factors. Was entirely
  unregistered -- not stubbed, just absent -- so any script calling it
  aborted immediately with `attempt to call global 'overlayGetWidth' (a nil
  value)`, before the rest of that script (including its own
  `onInputPressed`) ever ran. Now returns `ctx.overlayWidth`/`overlayHeight`,
  matching what `overlaySetResolution()` was given.
- **`singeSetGameName(name)`** -- another native Singe API function some
  classic games call once at startup and Dirk never implemented. Retitles
  the game window (`SDL_SetWindowTitle`) to the game's own name instead of
  the generic "Dirk" title; a no-op if called before the window exists.
  Confirmed on a real game (Mad Dog McCree) that a missing binding here is
  worse than cosmetic: the call sat on the line immediately before that
  game's own state machine left its initial setup state, so the missing-
  global error aborted that transition every single frame, permanently
  blocking the game from progressing past startup -- even though native
  keyboard/controller/mouse dispatch was working fine underneath it.
- **`singeWantsCrosshairs()`** -- asks the engine "are you already drawing a
  hardware/native crosshair overlay, so I shouldn't draw my own sprite on
  top of it?" Dirk has no such native crosshair layer -- the only crosshair
  that ever appears is the game's own `spriteDraw(cursorx, cursory,
  sprCursor)` call, which classic games gate directly on this function's
  return value. Now always returns `true`, which is the only answer that
  lets a classic game's own aiming cursor ever be drawn at all.
- **Startup mouse position** -- Dirk now synthesizes one `onMouseMoved()`
  call, using SDL's real current cursor position, right after a game's
  script (and `controls.cfg`) finish loading and before the main loop
  starts. Some games only ever update their own aiming-cursor globals
  (`mousex`/`mousey` in Mad Dog McCree) inside their own `onMouseMoved()`
  handler, so without a real mouse-move event yet, those stay at whatever
  value the script declared them with -- typically `0, 0`. A game whose
  reload logic treats an off-screen/at-the-border cursor position as "aim
  off-screen and reload" (a standard lightgun convention) can then
  misinterpret that startup `(0, 0)` as the player aiming at the top-left
  corner, firing a reload sound alongside the very first shot, before any
  real aiming has happened. Confirmed against Mad Dog McCree's own reload
  logic (`isOffscreen()`), which checks exactly `mousex <= 10 or mousey <=
  10 or ...`. This fix has no effect on games without an `onMouseMoved()`
  handler.

These were all found and fixed one at a time as real crashes/symptoms
surfaced on real hardware, not from a general audit -- other classic-Singe-
only native functions may still be missing (`framework.singe`'s own
Singe-1.xx compatibility block assumes a few more exist, e.g.
`singeGetWidth`/`singeGetHeight`/`singeScreenshot`, but no game tested so
far has actually called them).

### Subtitles (.srt)

A subtitle file placed next to a game's movie, with the same name, is shown
at the bottom of the picture -- no change to the game or to KimmyScript:

    Movie/Timegal.mp4
    Movie/Timegal.srt       <- a track shown as "On" in the menu
    Movie/Timegal-fr.srt    <- shown as "French"
    Movie/Timegal-pt-br.srt <- shown as "Portuguese (BR)"

(`-fr` is the same suffix as a Hypseus alternate soundtrack `<name>-fr.ogg`;
`_fr` and `.fr` work too.)

Language names (subtitles and soundtracks alike, see `core/Languages.h`):
the tag is an ISO 639-1 code (two letters, all 184 languages) or ISO 639-2
(three letters, "fre"/"fra"), optionally with a region ("pt-br"); the menu
shows the English name ("French"), in plain ASCII so any game font can draw
it. A tag Dirk doesn't know shows as "Unknown"; "jp" is accepted for
Japanese. Two tracks with the same name become "French", "French (2)".
The table is generated by `tools/gen_languages.py`.

Timecodes are positions in the movie file (0 = its first frame), exactly
what a subtitle editor shows when the movie is opened in it (Subtitle Edit,
Aegisub...). Dirk shows the cue for the frame on screen, so it stays right
whatever jumps the game makes in the movie (deaths, branches, attract loop).
Files may be UTF-8 (with or without BOM), UTF-16, or old Windows-1252/
Latin-1 -- all are read correctly; `<i>`/`<b>`/`<font>` and `{\an8}` tags
are removed. A block Dirk can't read is skipped (see `core/Subtitles.h`).

Look: always Dirk's own font (`KimmyScript/Cfg/dirk_loading_font.ttf`,
DejaVu Sans Bold), white with a black outline, sized from the picture's
height. Line breaks are the file's own -- Dirk never wraps; a line wider
than the picture is shrunk to fit. Subtitles are drawn on the picture,
under the game's overlay: menus, scores and any text the game draws stay
on top.

Choice per game (`dirk_SubtitleIndex` in `Cfg/game.cfg`, 0 = off, N =
track N, untagged file first then by language tag; on by default when a
game has subtitles), from KimmyScript's "Dirk Options" screen ("Subtitles:
Off / On / French / English..."), through:

- `dirkGetSubtitleCount()` -- number of tracks for this game's movie (0 = none)
- `dirkGetSubtitleIndex()` / `dirkSetSubtitleIndex(n)` -- 0 = off, n = track n
- `dirkGetSubtitleName(n)` -- the menu label of track n ("On", "French"...)
- `dirkGetSubtitleSize()` / `dirkSetSubtitleSize(percent)` -- text size, 100 =
  normal, 50-150 ("Subtitle Size" in Dirk Options, `dirk_SubtitleSize` in
  `game.cfg`; `subtitle_size=` in `dirk_settings.cfg` sets the default)

The log lists what was found at startup, e.g.
`[dirk] subtitles: On (212 lines), French (212 lines) -- showing French, size 80%`.

### Several soundtracks (languages)

A movie can carry several soundtracks; the player picks one in Dirk Options
("Audio: Original / French / Italian..."), switched live -- Dirk reopens the audio on
the other track and re-seeks to the frame on screen, exactly like a jump, so
it stays in sync. Only the chosen track is decoded (the others are dropped
by the demuxer).

- **Hypseus games**: next to `Video/<name>.m2v` and `<name>.ogg`, alternate
  soundtracks named `<name>-fr.ogg`, `<name>-it.ogg` (what Hypseus plays with
  `-altaudio`; `_fr` and `.fr` work too). They are merged into the cached
  `Movie/<name>.mkv` as extra tracks (title "FR", language "fre"; original
  first, flagged default, shown as "Original"). `Movie/<name>.mkv.audio` records which files it was
  built from: adding, removing or replacing a soundtrack rebuilds it on the
  next launch.
- **Any .mp4/.mkv** with several audio streams: listed as they are, labelled
  from the stream language ("fre" -> "French"), else from a title that is
  a code ("FR"), else from a short title as written; "Original" for a first
  track with no language, "Unknown" otherwise.

Per game in `Cfg/game.cfg` (`dirk_AudioTrack` = number, `dirk_AudioTrackName`
= label; the label wins, so the choice survives a soundtrack added later; a
tag saved by an older Dirk, "FR", still finds "French").
Lua: `dirkGetAudioTrackCount()`, `dirkGetAudioTrackIndex()` /
`dirkSetAudioTrackIndex(n)` (1-based), `dirkGetAudioTrackName(n)`. The log
says `[dirk] audio tracks: Original, French, Italian -- playing French`.

### Zipped games (.zdirk)

A game folder can be shipped as one file: zip it and rename the zip to
`<Game>.zdirk` (Dirk's counterpart of Hypseus's zipped-Lua ROMs, `-zlua`).
Double-clicking it launches the game like a `.dirk` (the installer
associates both); `dirk <Game>.zdirk`, or `dirk <folder>` with one `.zdirk`
and no `.dirk` in it, work too.

    Dirk Games/Timegal/Timegal.zdirk   <- Timegal.dirk, Overlay/, Fonts/,
                                          Sounds/, Script/, Cfg/...
    Dirk Games/Timegal/Movie/...       <- the movie, NOT in the zip
    Dirk Games/Timegal/Cfg/...         <- created: settings, saves, scores
    Dirk Games/Timegal/Custom/...      <- player overrides, as before

- The zip is read in place, like Hypseus does: nothing is extracted to a
  hidden folder. Scripts, images, fonts, sounds, bezels and controls.cfg
  are read from it into memory.
- For the game, its folder is the `.zdirk`'s folder with the zip's content
  laid over it: a file on disk there wins over the zip's (Custom/, Cfg/,
  or any file put there), the game writes there as usual, and a folder
  listing (`lfs.dir`) merges both.
- Like Hypseus's `ram` folder: the zip's own `Cfg/` files (the game's
  writable files) are copied next to the `.zdirk` on launch when missing
  -- the factory settings -- never over the player's.
- Inside the zip: the files at its root, or inside one top folder (what
  Windows' "Send to > Compressed (zipped) folder" makes) -- both work. The
  script run is `<Game>.dirk`, else the only `.dirk`, else the same for
  `.singe`.
- The movie must be next to the `.zdirk` (`Movie/`, or `Video/` for a
  Hypseus game): it is already compressed, and jumps in it must stay
  instant. A movie inside the zip is refused with a message saying so.
- Lua's io functions only take file names: a file only in the zip opened
  through them (not the case of KimmyScript, whose Cfg/ files are on disk)
  gets a temporary copy, removed when Dirk exits.
- Security: zip entries with `..`, a drive or an absolute path are ignored.
- Several games in one zip (like Hypseus's `-usealt` packs): e.g.
  `Hayate.zdirk` holding `Hayate.dirk` and `HayatePSX.dirk`. The `.zdirk`
  runs the script named like it; each other game gets a tiny "shortcut"
  `.dirk` next to the zip, double-clicked like any `.dirk`:

      -- HayatePSX.dirk, next to Hayate.zdirk
      zdirk = "Hayate.zdirk"
      script = "HayatePSX.dirk"   -- optional: defaults to this file's own name

  Dirk recognises it (a file under 4 KB with a `zdirk = "..."` line) and
  runs that script from the zip; the shortcut itself is never taken for a
  game file. See `dirk::readZdirkShortcut()`.
- A Hypseus `-zlua` zip renamed to `.zdirk` is not supported (different
  layout).

Log: `[dirk] zipped game ...Timegal.zdirk: 111 files, read from the zip
(opened in 1 ms)`. Code: `core/GameArchive.h`; zip reading by miniz (MIT,
vendored in `src/third_party/miniz/`).

## Standalone Windows build (single .exe, no DLLs)

`DIRK_STATIC=ON` links SDL2/SDL2_mixer/SDL2_image/SDL2_ttf/ffmpeg/lua *and*
the MinGW C/C++ runtime statically, so the result is one `dirk.exe` with no
DLLs to ship alongside it and no dependency on the Windows `PATH`. Meant for
a MinGW-w64 (MSYS2) toolchain.

The one real wrinkle: MSYS2's own `mingw-w64-x86_64-ffmpeg` package is a
general-purpose build with dozens of optional codecs compiled in (x264,
x265, aom, dav1d, vpx, openmpt, gnutls, ...) that dirk never uses but that
static linking still pulls in as hard dependencies. `tools/
build_ffmpeg_minimal_windows.sh` builds a trimmed, static-only ffmpeg
instead -- just H.264 + H.265/HEVC video, AAC + MP3 audio, the MP4/MOV
container -- which sidesteps that wall of missing `-lxxx` libraries
entirely. Run it once, in an "MSYS2 MinGW64" shell:

```bash
tools/build_ffmpeg_minimal_windows.sh
```

The same problem, and the same fix, applies to SDL2_image, SDL2_ttf and
SDL2_mixer: the MSYS2 packages are general-purpose builds (every image
format + SVG rendering, full Unicode text shaping, every tracker/audio
format) that dirk doesn't need -- confirmed against a real `ldd dirk.exe`,
which showed cairo/librsvg/pango/libjpeg/libtiff/libwebp (from SDL2_image),
harfbuzz/fontconfig/fribidi (from SDL2_ttf) and mpg123/FLAC/vorbis/opus/
wavpack/xmp (from SDL2_mixer) all coming along for the ride. dirk only ever
loads `.png` sprites, renders plain Latin text for its own settings screen,
and plays `.wav` sound effects, so `tools/
build_sdl_satellites_minimal_windows.sh` builds minimal static versions of
all three (PNG only / FreeType-without-HarfBuzz / WAV only):

```bash
tools/build_sdl_satellites_minimal_windows.sh
```

Then build dirk against both minimal builds together (from `build/`, also
inside the MinGW64 shell):

```bash
PKG_CONFIG_PATH="$HOME/ffmpeg-dirk-static/lib/pkgconfig:$HOME/sdl2-satellites-dirk-static/lib/pkgconfig" \
  cmake .. -DDIRK_STATIC=ON -G "MSYS Makefiles"
cmake --build .
```

`PKG_CONFIG_PATH` makes these minimal builds take priority over the system
packages for this configure run, so no `CMakeLists.txt` changes are needed
either way.

Verified end to end on real Windows/MSYS2: `ldd dirk.exe` on the result
shows nothing but core Windows system DLLs (`ntdll`, `KERNEL32`, `USER32`,
`GDI32`, `ole32`, ...) -- no MSYS2, SDL2, ffmpeg, or Lua DLLs, and no
dependency on MSYS2's PATH.

One CMake gotcha that bit this the first few times, in case it comes up
again after a dependency version bump: `pkg_check_modules()` reports which
libraries to link (`*_LIBRARIES`) and where to find them (`*_LIBRARY_DIRS`)
as separate variables -- only using the former means the linker falls back
to the toolchain's own default search path (MSYS2's system `mingw64/lib`)
instead of these minimal builds' install prefixes, silently linking the
general-purpose system libraries again even with `PKG_CONFIG_PATH` set
correctly. `CMakeLists.txt` passes both via `target_link_directories()` +
`target_link_libraries()` now.

## Android

Dirk also runs on Android 11 and later (64-bit ARM, `arm64-v8a`): one APK
with the same engine (`src/core/main.cpp`), a small Java launcher (game
list, storage permission, touch controls) under `android/`, and the
KimmyScript of `installer/payload/KimmyScript`.

**Using it.** Install the APK, open Dirk and allow "All files access" when
asked (Dirk reads games from shared storage). Put each game in its own
folder under `Dirk/Games/` on the device, as on the PC, then pick it in the
list. Touch buttons appear on screen and hide while a gamepad is in use.
Devices with Nintendo-style buttons (A on the right) can set Dirk Options >
Swap A/B X/Y. Engine messages go to `Dirk/dirk.log`.

**Building.** On Linux, without Gradle:

```bash
export ANDROID_HOME=/path/to/android-sdk   # platforms;android-35, build-tools;35.0.0, ndk;28.2.13676358
tools/build_android.sh <src-dir> <out-dir> all
```

- `<src-dir>` holds `SDL3/`, `SDL_image/`, `SDL_mixer/`, `SDL_ttf/` and
  `ffmpeg/`: the same sources and versions as the Windows and Linux builds
  (see `tools/build_linux_bundle_zig.sh`). A JDK 17 or later is needed.
- Everything (SDL3 and its satellites, a minimal LGPL FFmpeg, Lua 5.1.5
  from this repository) is linked statically into one `libmain.so`.
- Steps can be run one at a time: `libs`, `dirk`, `apk`.
- Without a key, the script signs with a throwaway debug key made in
  `$WORK`, for testing only.
- A release is built with `DIRK_UNSIGNED=1`: the script stops after
  `zipalign` and leaves `dirk-<version>-android-arm64-unsigned.apk`,
  `apksigner.jar` and `apksigner-NOTICE.txt` in `<out-dir>`. The key
  holder then signs it on their own machine with a JDK:

  ```bash
  java -jar apksigner.jar sign --ks <keystore> --ks-key-alias dirk \
      --out dirk-<version>-android-arm64.apk dirk-<version>-android-arm64-unsigned.apk
  ```

- `DIRK_KEYSTORE=<file>` and `DIRK_KS_PASS=<password>` (alias `dirk`) sign
  directly instead. `DIRK_VERSION_NAME` and `DIRK_VERSION_CODE` set the
  version.
- **Never commit a keystore**: `.gitignore` excludes `*.keystore`, `*.jks`,
  `*.apk` and `*.idsig`.

Third-party licenses for the APK: `android/THIRD_PARTY_NOTICES.txt` and
`android/Licenses/` (the GPL text is copied from `LICENSE` at build time).

## Layout

- `src/video/` -- `VideoSource`: ffmpeg-backed frame-accurate decode + seek,
  the single highest-risk piece (see design doc section 7). Has its own
  index cache format (not Singe's).
- `src/lua/` -- `LuaEngine` (Lua 5.1 wrapper, KimmyScript's confirmed
  callback set) and `api_singe.cpp` (the 38 native functions KimmyScript
  actually calls, inferred from real call sites + the official manual).
- `src/core/` -- `EngineContext` (shared state) and `main.cpp` (SDL2
  window/render loop).
- `tools/seek_test.cpp` -- smoke test for `VideoSource::seekToFrame()`
  against a synthetic clip (`tools/gen_test_pattern.sh` generates it).

## Status

Phase 1 (minimal prototype) and the start of Phase 2 (full API, real
assets) are done: the full real KimmyScript dofile chain (a game's own
script -> `globals.kimmy` -> `framework/main/moves/hscore/service/toolbox
.kimmy` -> `addons.kimmy`) loads and runs under Dirk with zero Lua errors,
missing only asset files that were never provided to this environment
(sounds, sprites, fonts -- these fail gracefully, logging a warning rather
than crashing).

Known gaps: `menu.kimmy`'s `scriptPush()` (chaining from a game-select menu
into an actual game) still just logs a warning and does nothing -- relevant
only if a multi-game front-end menu gets built; a single game launched
directly (this project's current setup) never calls it. Sound routing
(`discAudio`) doesn't yet distinguish channels (music vs. effects always
mute/unmute together).

No longer gaps, since earlier in this project's history: `SDL_GameController`
input is implemented (gamepad hot-plug, keyboard and controller both
translated through the same logical-switch layer -- see `InputMap.cpp`),
and `menu.kimmy`'s `video*` family (the secondary video stream used for
e.g. attract-mode preview clips in a game-select menu) is a real
implementation, not a stub.

Not a gap, on closer look: the video clock (main.cpp's `frameAccumulator`)
measures real elapsed time via `SDL_GetTicks()` every loop iteration rather
than assuming a fixed step per iteration, so it can't accumulate drift the
way a naive fixed-step pacer would (real Singe's own "video timing
accumulated main loop latency, drifting behind the audio" bug, fixed in
Singe 3.00 by making audio the master clock -- see its CHANGELOG). Verified
directly against real Singe, side by side, on the exact same movie file:
Singe shows that bug (audio noticeably ahead of the picture from frame one,
a constant offset rather than a growing drift), Dirk doesn't.

Dirk *did* have its own real, separate lip-sync bug, in the opposite
direction (audio trailing the picture -- dialogue arriving after the mouth
had already moved), now fixed. `VideoSource::decodeOneFrame()` used to stop
reading from the file the moment it had decoded one video frame, so any
audio the container's own interleaving placed even slightly further ahead
in the file than that frame's own packet didn't get pulled until some
LATER call. Confirmed against a real exported movie (DLe's `lair.mp4`):
its muxer writes ten consecutive video packets, then the ~0.4s audio chunk
covering all ten -- so frames 0-9 played in dead silence and then that
whole chunk arrived in one burst right as frame 10 came up, a measured
~9-10-frame audio lag, repeating in a sawtooth every ten frames (measured
with `tools/`'s standalone `VideoSource` harness, comparing cumulative
audio-seconds decoded against cumulative video-seconds displayed). Fixed
by having `decodeOneFrame()` read ahead past the frame it just decoded --
decoding any audio found immediately, and queuing (not submitting) any
video packets hit along the way for later calls to drain in order -- until
decoded audio actually covers that frame's own timestamp, so the audio for
"now" is already in hand instead of trickling in however many frames later
the file's own interleaving happens to place it. The same real movie now
measures audio consistently at-or-ahead of the picture, never behind, on
every one of the first several hundred frames checked.

That fix targets audio DECODE keeping pace with video DISPLAY -- it doesn't
account for the audio DEVICE's own output latency (driver/OS buffering
between `SDL_QueueAudio` and sound actually reaching the speakers), which
video display has no equivalent of, so decoding audio exactly up to "now"
still leaves it audibly a little behind in practice. This is no longer a
fixed guess to tune by ear: at startup, `AudioLatency.h`'s
`queryAudioOutputLatencySeconds()` asks the OS for the current machine's
real default-output latency (WASAPI on Windows; a safe fallback constant
elsewhere) and that measured number, plus a small safety margin, becomes
the one-time startup holdback and `VideoSource`'s own audio decode-ahead
margin (`main.cpp`'s `audioLeadSecondsCurrent`, seeded from
`kAudioLeadMarginSeconds`). On top of that, the margin is adaptive at
runtime: if a real audio underrun is ever detected during play, it means
THIS machine's actual pipeline needs more headroom than the measured
number gave it (different sound card/driver/CPU load), so the margin grows
live, in place, capped at a bounded worst case -- no relaunch, no player-
facing setting, and no more hand-picking one constant that's wrong for
some other player's machine (this project's whole earlier history of
0.06/0.15/0.30/0.45s guesses never converged that way).

## License

Dirk is free software: you can redistribute it and/or modify it under the
terms of the GNU General Public License as published by the Free Software
Foundation, either version 3 of the License, or (at your option) any later
version. See [`LICENSE`](LICENSE).

This covers the whole repository: the engine (`src/`), the Wizard built
into it (`src/lua/wizard.kimmy`, compiled to `src/lua/WizardChunk.h` by
`tools/embed_wizard.py`), the tools, and the KimmyScript installed with Dirk
(`installer/payload/KimmyScript/`, also GPL).

Dirk is distributed in the hope that it will be useful, but WITHOUT ANY
WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE.

Third-party components (SDL3, FFmpeg, Lua 5.1, miniz, FreeType, libiconv,
DejaVu fonts, megatools...) keep their own licenses, listed in
`installer/payload/THIRD_PARTY_NOTICES.txt` with full texts in
`installer/payload/Licenses/`.
