; ============================================================================
; Dirk / KimmyScript installer (NSIS script)
; ============================================================================
; Builds a single DirkSetup.exe that:
;   1. Installs dirk.exe + the shared KimmyScript/ framework folder into a
;      folder the player picks (default: "Dirk" at the root of whichever
;      drive DirkSetup.exe itself was run from, e.g. E:\Dirk -- see .onInit
;      below. Named just "Dirk", not "Dirk Games": since
;      findSharedEngineRoot() in main.cpp checks dirk.exe's own folder
;      first, a game no longer has to live anywhere near this install at
;      all -- it can sit on a completely different drive, see that
;      function's own comment -- so this folder isn't really "where the
;      games are" any more).
;   2. Registers the .dirk file extension so double-clicking a game's own
;      "<GameName>.dirk" file (the launcher file main.cpp looks for inside
;      each game folder -- see resolveGameFolder()/hasDirkExtension() in
;      src/core/main.cpp) opens it directly with dirk.exe, no manual
;      "Open with" needed.
;   3. Adds Start Menu shortcuts and a proper uninstaller.
;   4. Optionally bundles the Mega game downloader companion tool (see
;      downloader/README.md) -- so a player who runs DirkSetup.exe gets
;      dirk.exe, KimmyScript/, AND the means to pull down games from your
;      shared Mega folder, all from the one installer, nothing else to
;      hand out separately. Optional (/nonfatal below) because you might
;      not have that tool built/gathered yet when you compile this
;      installer -- the base engine install still succeeds without it.
;
; IMPORTANT -- what this script does NOT do: it does not compile dirk.exe
; or dirk_downloader.exe. Those still have to happen on a real Windows box
; (or MSYS2/MinGW-w64), per the main README's "Standalone Windows build"
; section and downloader/README.md's own build command -- this sandbox has
; no MinGW-w64 toolchain to produce real binaries. Before compiling THIS
; installer, drop your build's actual output into:
;
;     installer/payload/dirk.exe            <- your compiled Dirk binary
;     installer/payload/KimmyScript/...     <- your real KimmyScript folder
;                                               (Cfg/, Script/, Skin/ etc,
;                                               whatever your install needs)
;     installer/payload/dirk_downloader.exe <- optional, see downloader/README.md
;     installer/payload/megatools.exe       <- optional, same folder -- NOT built by
;                                               this repo, see downloader/README.md
;                                               for where to get it
;     installer/payload/manifest_link.txt   <- optional, same folder -- one line,
;                                               the Mega link to YOUR manifest.txt
;                                               (see downloader/README.md)
;
; replacing the placeholder files this repo ships in payload/ for testing.
; Then compile with NSIS (https://nsis.sourceforge.io/, free) -- either the
; NSIS GUI ("Compile NSI scripts" -> open this file) or from a command
; line/terminal:
;
;     makensis installer\dirk_installer.nsi
;
; which produces installer\DirkSetup.exe. That's the one file you hand to
; players.
;
; WHY the shared folder layout matters: findSharedEngineRoot() in main.cpp
; finds KimmyScript/ by walking UP from each game's own folder (confirmed by
; reading that function, not guessed) -- so KimmyScript/ has to sit as a
; sibling (or closer ancestor) of every game folder, not next to dirk.exe
; specifically. This installer keeps things simple by putting dirk.exe,
; KimmyScript/, and (later) every game folder the player adds all directly
; inside the one folder they pick during install -- so "drop a new game in"
; is just "copy its folder next to KimmyScript here", nothing else to wire
; up.
; ============================================================================

!include "MUI2.nsh"
!include "FileFunc.nsh" ; for ${GetRoot}, used by .onInit below to default
                        ; InstallDir to the root of whichever drive this
                        ; installer itself is run from

; ---------------------------------------------------------------------------
; Basic metadata -- keep in sync with src/core/dirk.rc's VERSIONINFO block
; if you bump the engine version.
; ---------------------------------------------------------------------------
!define APP_NAME        "Dirk"
!define APP_PUBLISHER   "Dirk Project"
!define APP_VERSION     "1.0.0"
!define APP_EXE         "dirk.exe"
!define TUTORIAL_PDF    "Dirk&Kimmy Tutorial.pdf" ; drop the real file at
                                                   ; payload\Dirk&Kimmy Tutorial.pdf
!define DOWNLOADER_EXE  "dirk_downloader.exe" ; the Mega game downloader
                                               ; companion tool -- optional,
                                               ; see downloader/README.md
!define MEGATOOLS_EXE   "megatools.exe" ; NOT built by this repo -- see
                                         ; downloader/README.md for where
                                         ; to get the real Windows build
!define MANIFEST_LINK   "manifest_link.txt" ; one line: the Mega "Get link"
                                             ; for YOUR manifest.txt file
!define UNINST_KEY      "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_NAME}"

Name "${APP_NAME}"
OutFile "DirkSetup.exe"
Unicode true

; Per-user install, no elevation needed: the .dirk file association is
; written to HKCU\Software\Classes (current-user scope), which Windows
; already checks before the machine-wide HKEY_CLASSES_ROOT view -- so it
; takes effect for that player immediately, with no UAC prompt to click
; through. The tradeoff: on a shared/multi-account PC, each Windows account
; that wants to double-click .dirk files needs to run this installer once
; under its own login. Fine for the hobbyist/arcade-cabinet single-account
; case this is aimed at; switch RequestExecutionLevel to "admin" and the
; registry writes below from HKCU to HKLM-backed HKCR if you need an
; all-users install instead.
RequestExecutionLevel user

; Literal fallback only -- .onInit below (which runs before the directory
; page ever shows) replaces this with the root of whichever drive
; DirkSetup.exe was actually run from, explicit user request: not
; $DOCUMENTS, which buries the install deep inside the profile on whatever
; drive Windows happens to keep that, several folders deep and not obvious
; on an arcade-cabinet setup. InstallDirRegKey still wins over both, for a
; repeat install/upgrade that already has a saved custom location.
InstallDir "$PROGRAMFILES\${APP_NAME}"
InstallDirRegKey HKCU "Software\${APP_NAME}" "InstallDir"

Function .onInit
    ; ${GetRoot} on $EXEDIR (the folder containing the running installer,
    ; NOT necessarily its drive's root -- e.g. "E:\Downloads" if that's
    ; where the player put DirkSetup.exe) reduces it down to just "E:\",
    ; matching "root of the drive the installer runs from" exactly, rather
    ; than nesting under wherever the player happened to save the
    ; installer itself.
    ${GetRoot} "$EXEDIR" $R0
    StrCpy $INSTDIR "$R0\${APP_NAME}"
FunctionEnd

; ---------------------------------------------------------------------------
; Modern UI pages
; ---------------------------------------------------------------------------
!define MUI_ABORTWARNING

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "French"
!insertmacro MUI_LANGUAGE "English"

; ---------------------------------------------------------------------------
; Install
; ---------------------------------------------------------------------------
Section "Dirk engine + KimmyScript" SecCore
    SectionIn RO ; not optional -- there's nothing meaningful to install without it

    SetOutPath "$INSTDIR"
    File "payload\${APP_EXE}"
    ; Wizard.exe next to dirk.exe (2026-10-12): launched from here it asks for
    ; the game folder (see src/launcher/wizard_launcher.c). The copies older
    ; installers put in Create\ are taken away (folders only when empty).
    File /nonfatal "payload\Wizard.exe"
    Delete "$INSTDIR\Create\Wizard creator\Wizard.exe"
    Delete "$INSTDIR\Create\Wizard creator\Movie\Drop your movie here.txt"
    RMDir "$INSTDIR\Create\Wizard creator\Movie"
    RMDir "$INSTDIR\Create\Wizard creator"
    Delete "$INSTDIR\Create\Wizard\Wizard.exe"
    RMDir "$INSTDIR\Create\Wizard"

    ; Landed directly in $INSTDIR, next to dirk.exe -- same folder the
    ; "Dirk folder.lnk" Start Menu shortcut already opens, so it's easy to
    ; find without a separate shortcut of its own. /nonfatal: an installer
    ; build shouldn't hard-fail just because this one optional file wasn't
    ; dropped into payload\ yet (unlike dirk.exe/KimmyScript just above,
    ; there IS something meaningful to install without it).
    File /nonfatal "payload\${TUTORIAL_PDF}"

    ; Third-party license notices (2026-09-26): THIRD_PARTY_NOTICES.txt plus
    ; the full license texts it points to, in their own Licenses\ folder.
    ; Required by several of the components' licenses (FFmpeg LGPL/GPL,
    ; megatools GPL, FreeType FTL credit) -- see release checklist.
    File /nonfatal "payload\THIRD_PARTY_NOTICES.txt"
    SetOutPath "$INSTDIR\Licenses"
    File /nonfatal /r "payload\Licenses\*.*"
    SetOutPath "$INSTDIR"

    ; The optional Mega game downloader companion tool (see
    ; downloader/README.md) -- three files, /nonfatal for the same reason
    ; as the tutorial PDF just above: you might not have built/gathered
    ; these yet when you compile this installer, and the base engine
    ; install should still succeed without them. Tucked into its own "Dirk
    ; Downloader" subfolder rather than dropped loose in $INSTDIR, to keep
    ; that root to just dirk.exe/KimmyScript/game folders -- explicit
    ; request, to keep the root tidy the same way Bezel/ and Cfg/ already
    ; keep a game's own folder tidy. A downloaded game doesn't strictly
    ; NEED to land in $INSTDIR to work -- findSharedEngineRoot() in
    ; main.cpp checks dirk.exe's own folder for KimmyScript/ first and uses
    ; it regardless of where a game folder actually lives, even a
    ; different drive (see the "Why the layout matters" note in this
    ; folder's own README) -- but $INSTDIR is still the most convenient
    ; default: one obvious place, the same "Dirk folder.lnk" shortcut
    ; already opens, for every game whether downloaded or added by hand.
    ; dirk_downloader.exe's own defaultDestFolder() picks that default by
    ; looking for KimmyScript/ next to itself first, then one level up
    ; (see dirk_downloader.cpp) -- the player can always redirect
    ; elsewhere via "Parcourir..." regardless, it'll work either way.
    SetOutPath "$INSTDIR\Dirk Downloader"
    File /nonfatal "payload\${DOWNLOADER_EXE}"
    File /nonfatal "payload\${MEGATOOLS_EXE}"
    File /nonfatal "payload\${MANIFEST_LINK}"
    SetOutPath "$INSTDIR"

    ; Recursively copies the whole KimmyScript tree (Cfg/, Script/, Skin/,
    ; and anything else you put under payload\KimmyScript\) into
    ; $INSTDIR\KimmyScript -- SetOutPath + the \*.* wildcard is what makes
    ; File /r land it directly there rather than nested one level deeper.
    SetOutPath "$INSTDIR\KimmyScript"
    File /r "payload\KimmyScript\*.*"

    ; The Create kit (2026-10-11): everything under payload\Create\ as it
    ; is laid out there -- the Blank sheets, and each one's Movie\ folder
    ; with its "Drop your movie here.txt".
    SetOutPath "$INSTDIR\Create"
    File /r "payload\Create\*.*"

    SetOutPath "$INSTDIR"

    ; ---- .dirk file association (current user, no admin) -----------------
    ; DirkGame.File is our own ProgID; .dirk's default value points at it,
    ; which is the indirection Windows expects (lets several extensions
    ; share one ProgID, and is what most "Open with" UIs display instead of
    ; a bare extension).
    WriteRegStr HKCU "Software\Classes\.dirk" "" "DirkGame.File"
    WriteRegStr HKCU "Software\Classes\DirkGame.File" "" "Dirk Game"
    WriteRegStr HKCU "Software\Classes\DirkGame.File\DefaultIcon" "" "$INSTDIR\${APP_EXE},0"
    WriteRegStr HKCU "Software\Classes\DirkGame.File\shell\open\command" "" '"$INSTDIR\${APP_EXE}" "%1"'
    ; .zdirk (2026-09-28): a zipped game folder, opened by the same dirk.exe
    ; (see src/core/GameArchive.h) -- same double-click behaviour as .dirk.
    WriteRegStr HKCU "Software\Classes\.zdirk" "" "DirkZippedGame.File"
    WriteRegStr HKCU "Software\Classes\DirkZippedGame.File" "" "Dirk Game (zipped)"
    WriteRegStr HKCU "Software\Classes\DirkZippedGame.File\DefaultIcon" "" "$INSTDIR\${APP_EXE},0"
    WriteRegStr HKCU "Software\Classes\DirkZippedGame.File\shell\open\command" "" '"$INSTDIR\${APP_EXE}" "%1"'

    ; Tell Explorer the shell association changed right now, instead of
    ; waiting for the next logon/reboot to notice -- without this, a
    ; freshly-installed .dirk file can sit with the wrong icon and a dead
    ; double-click until the user signs out and back in.
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'

    ; ---- Start Menu -------------------------------------------------------
    CreateDirectory "$SMPROGRAMS\${APP_NAME}"
    ; Opens the install folder itself rather than launching dirk.exe with no
    ; arguments -- dirk.exe always expects a game folder or a .dirk file
    ; (see main.cpp's own usage message), so a bare "launch the engine"
    ; shortcut wouldn't do anything a player could see on a GUI-subsystem
    ; build. Opening the folder is what a player actually needs afterward:
    ; a place to drop new game folders next to KimmyScript.
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\Dirk folder.lnk" "$INSTDIR"
    ; Wizard without copying it (2026-10-12): launched from here, Wizard.exe
    ; asks for the game folder (see wizard_launcher.c).
    IfFileExists "$INSTDIR\Wizard.exe" 0 +2
        CreateShortcut "$SMPROGRAMS\${APP_NAME}\Dirk Wizard.lnk" "$INSTDIR\Wizard.exe"
    CreateShortcut "$SMPROGRAMS\${APP_NAME}\Uninstall Dirk.lnk" "$INSTDIR\Uninstall.exe"

    ; ---- Uninstall registration (Windows' Programs & Features list) ------
    WriteRegStr HKCU "Software\${APP_NAME}" "InstallDir" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${APP_VERSION}"
    WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "${APP_PUBLISHER}"
    WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
    WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\${APP_EXE},0"
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
    WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1

    WriteUninstaller "$INSTDIR\Uninstall.exe"
SectionEnd

; ---------------------------------------------------------------------------
; Uninstall
; ---------------------------------------------------------------------------
; Deliberately narrow: $INSTDIR is where the player's own game folders live
; too (see the layout note at the top of this file), so the uninstaller
; must remove ONLY what this installer itself put there -- dirk.exe,
; KimmyScript/, the downloader tool, the shortcuts, the registry keys --
; and must never touch $INSTDIR as a whole (an RMDir /r "$INSTDIR" here
; would silently delete every game the player ever added, INCLUDING any
; downloaded through dirk_downloader.exe -- its default destination is
; this same folder, so a downloaded game's subfolder is otherwise
; indistinguishable from one the player copied in by hand, and is
; protected here the same way).
Section "Uninstall"
    Delete "$INSTDIR\${APP_EXE}"
    Delete "$INSTDIR\Wizard.exe"
    Delete "$INSTDIR\${TUTORIAL_PDF}" ; no-ops harmlessly if it was never installed (/nonfatal above)
    Delete "$INSTDIR\THIRD_PARTY_NOTICES.txt"
    RMDir /r "$INSTDIR\Licenses" ; only ever holds what this installer put there
    ; Whole subfolder, not three individual Delete lines -- safe here (unlike
    ; $INSTDIR itself) because "Dirk Downloader" only ever holds what this
    ; installer put there; no-ops harmlessly if it was never installed.
    RMDir /r "$INSTDIR\Dirk Downloader"
    RMDir /r "$INSTDIR\KimmyScript"
    ; The Create kit, file by file: a player may have dropped a movie in one
    ; of its Movie\ folders or started a game there, so every RMDir below
    ; only removes a folder that is now empty.
    Delete "$INSTDIR\Create\Wizard creator\Wizard.exe"
    Delete "$INSTDIR\Create\Wizard creator\Movie\Drop your movie here.txt"
    RMDir "$INSTDIR\Create\Wizard creator\Movie"
    RMDir "$INSTDIR\Create\Wizard creator"
    Delete "$INSTDIR\Create\Blank\Blank.dirk"
    Delete "$INSTDIR\Create\Blank\Blank minimal demo.dirk"
    Delete "$INSTDIR\Create\Blank\Blank with all options.dirk"
    Delete "$INSTDIR\Create\Blank\Movie\Drop your movie here.txt"
    RMDir "$INSTDIR\Create\Blank\Movie"
    RMDir "$INSTDIR\Create\Blank"
    Delete "$INSTDIR\Create\Wizard\Wizard.exe" ; layout of the 2026-10-10 installer
    RMDir "$INSTDIR\Create\Wizard"
    RMDir "$INSTDIR\Create"
    Delete "$INSTDIR\Uninstall.exe"

    Delete "$SMPROGRAMS\${APP_NAME}\Dirk folder.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\Dirk Wizard.lnk"
    Delete "$SMPROGRAMS\${APP_NAME}\Uninstall Dirk.lnk"
    RMDir "$SMPROGRAMS\${APP_NAME}" ; only removes if now empty -- fine, that's all we put there

    DeleteRegKey HKCU "Software\Classes\.dirk"
    DeleteRegKey HKCU "Software\Classes\DirkGame.File"
    DeleteRegKey HKCU "Software\Classes\.zdirk"
    DeleteRegKey HKCU "Software\Classes\DirkZippedGame.File"
    DeleteRegKey HKCU "${UNINST_KEY}"
    DeleteRegKey HKCU "Software\${APP_NAME}"

    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, i 0, i 0)'

    ; $INSTDIR itself is only removed if it's now empty (i.e. the player
    ; never added any games, or already removed them) -- RMDir with no /r
    ; silently no-ops on a non-empty folder instead of erroring, which is
    ; exactly the "leave games alone" behavior we want here.
    RMDir "$INSTDIR"
SectionEnd
