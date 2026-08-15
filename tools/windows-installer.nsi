; BENCsnip for Windows - installer and upgrader.
;
; Machine-wide, into Program Files, which means it asks for administrator
; rights once and installs for everyone on the box. That is the right shape
; here: BENCsnip is one executable with no per-user state, and nothing it
; writes goes near its own directory.
;
; Upgrading is the point. An installer that leaves the previous version beside
; the new one, or merges into it and leaves whatever the new build stopped
; shipping, is worse than no installer. This runs the old uninstaller first -
; after the person has clicked Install, not in .onInit, so cancelling on the
; directory page cannot leave them with nothing.
;
;   makensis -DSRCDIR=stage -DVERSION=v0.1.1 tools/windows-installer.nsi
;
; Graphics are the disk image's, in the sizes MUI fixes: assets/brand made by
; tools/make-installer-art.sh, and the same .ico the window already uses.

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!ifndef VERSION
  !define VERSION "dev"
!endif
!ifndef SRCDIR
  !define SRCDIR "stage"
!endif
!ifndef ICONFILE
  ; Relative to wherever makensis was invoked, which is the repository root.
  ; Forward slashes: the compiler reads this on the build machine, which is
  ; Linux, where a backslash is not a path separator and the error you get is
  ; "can't open file" three macros deep.
  !define ICONFILE "assets/icon/bencsnip.ico"
!endif
!ifndef ARTDIR
  !define ARTDIR "assets/brand"
!endif

Name "BENCsnip ${VERSION}"
!ifndef OUTFILE
  !define OUTFILE "bencsnip-${VERSION}-windows-setup.exe"
!endif
; Absolute when the caller says so - makensis writes a relative OutFile beside
; the script, not into the working directory.
OutFile "${OUTFILE}"
Unicode true
RequestExecutionLevel admin

; Under a BENCO folder, not straight into Program Files. There are several of
; these now and they are one company's; a row of BENC-somethings scattered
; through Program Files reads as several unrelated things from several
; unrelated people.
;
; The uninstaller takes the BENCO folder with it only when this was the last
; one in it - see the end of the uninstall section.
InstallDir "$PROGRAMFILES64\BENCO\BENCsnip"
ShowInstDetails show
ShowUninstDetails show

; One 28 MB executable, most of which is ffmpeg. LZMA takes it to about a
; third of that; the zlib default leaves it near two thirds, on a download
; people are waiting for.
SetCompressor /SOLID lzma

; The strip along the bottom of every page. It says Nullsoft otherwise, which
; is true and is not the name anyone is looking for there.
BrandingText "BENCO Holdings - MIT licensed"

!define REGKEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\BENCsnip"

; Four numbers or the compiler refuses it, so the workflow passes this in
; alongside the display version. Without it the setup .exe has no version at
; all in its properties, which is the one place a person checks when they have
; two of them in Downloads and no idea which is newer.
!ifndef VIVERSION
  !define VIVERSION "0.0.0.0"
!endif
VIProductVersion "${VIVERSION}"
VIAddVersionKey "ProductName"     "BENCsnip"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "FileVersion"     "${VIVERSION}"
VIAddVersionKey "FileDescription" "BENCsnip video editor - setup"
VIAddVersionKey "CompanyName"     "BENCO Holdings"
VIAddVersionKey "LegalCopyright"  "MIT licensed; this build is GPL as a whole - see NOTICE"

; ---------------------------------------------------------------- interface

!define MUI_ABORTWARNING
!define MUI_ICON   "${ICONFILE}"
!define MUI_UNICON "${ICONFILE}"

!define MUI_HEADERIMAGE
!define MUI_HEADERIMAGE_BITMAP "${ARTDIR}/nsis-header.bmp"
!define MUI_HEADERIMAGE_RIGHT
!define MUI_WELCOMEFINISHPAGE_BITMAP "${ARTDIR}/nsis-welcome.bmp"

; MUI_BGCOLOR is deliberately left alone. It would paint the header strip and
; the welcome page in the phosphor near-black, which is the look - and leave
; MUI's own header text black on top of it, which is not readable. The art is
; dark tiles on MUI's white; that part is by design.

!define MUI_WELCOMEPAGE_TITLE "BENCsnip ${VERSION}"
!define MUI_WELCOMEPAGE_TEXT "A video editor for the jobs that should take a minute. Drag a file in, cut the boring bit out, export.$\r$\n$\r$\nThis installs BENCsnip into Program Files\BENCO for everyone who uses this computer. It carries its own ffmpeg, so there is nothing else to install.$\r$\n$\r$\nAn existing BENCsnip is replaced, not installed beside."

!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Run BENCsnip"
!define MUI_FINISHPAGE_RUN_FUNCTION LaunchUnelevated

; A link rather than MUI_FINISHPAGE_SHOWREADME, which would hand README.md to
; the shell - and a stock Windows has nothing registered for .md, so the last
; thing a successful install would do is ask which program to open it with.
!define MUI_FINISHPAGE_LINK "github.com/bropple/BENCsnip"
!define MUI_FINISHPAGE_LINK_LOCATION "https://github.com/bropple/BENCsnip"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SRCDIR}/LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Var PrevUninst      ; the old version's uninstaller, "" if this is a fresh box
Var PrevDir
Var PrevVersion
Var WorkDir         ; what the shortcuts start in - see the comment in .onInit

; The installer runs elevated, so anything it starts is elevated too, and a
; program launched that way writes files nobody can then edit - which for a
; video editor means every export lands owned by Administrator. Handing the
; path to Explorer, which is running as the person at the keyboard, starts it
; as them. It is a trick, but it is the one that works without a plugin.
Function LaunchUnelevated
  Exec '"$WINDIR\explorer.exe" "$INSTDIR\bencsnip.exe"'
FunctionEnd

Function .onInit
  ; A 32-bit installer sees a redirected registry and a redirected Program
  ; Files unless it says otherwise. Everything below - including the entry
  ; Apps & Features reads - belongs in the 64-bit view, because the program
  ; being installed is 64-bit.
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP \
      "BENCsnip is built for 64-bit Windows and this is a 32-bit system."
    Abort
  ${EndIf}
  SetRegView 64
  SetShellVarContext all      ; Start Menu and Desktop for every user

  ; The shortcuts start in the person's Videos folder rather than in Program
  ; Files. Nothing about opening a file cares, but the export dialog and the
  ; save dialog open wherever the program started, and that has to be a folder
  ; they can write to. Program Files is not, and Videos is where the footage
  ; is.
  ;
  ; "current" while reading it, because with the context still set to all this
  ; is the Public folder. Under an over-the-shoulder UAC prompt this resolves
  ; to the administrator whose password was typed rather than to the person
  ; using the machine; there is no way to ask NSIS for the latter, and the
  ; dialog simply opens somewhere else if that folder is not theirs.
  SetShellVarContext current
  StrCpy $WorkDir "$VIDEOS"
  ${If} $WorkDir == ""
    StrCpy $WorkDir "$DOCUMENTS"
  ${EndIf}
  SetShellVarContext all
  ${If} $WorkDir == ""
    StrCpy $WorkDir "$INSTDIR"
  ${EndIf}

  ; Find whatever is already installed, and install over the top of it rather
  ; than into the default directory - someone who moved it to D: meant it.
  ReadRegStr $PrevUninst   HKLM "${REGKEY}" "UninstallString"
  ReadRegStr $PrevDir      HKLM "${REGKEY}" "InstallLocation"
  ReadRegStr $PrevVersion  HKLM "${REGKEY}" "DisplayVersion"
  ${If} $PrevUninst != ""
    ; Written quoted, read back quoted. ExecWait wants it that way and
    ; ${GetParent} does not.
    StrCpy $0 $PrevUninst 1
    ${If} $0 == '"'
      StrLen $1 $PrevUninst
      IntOp $1 $1 - 2
      StrCpy $PrevUninst $PrevUninst $1 1
    ${EndIf}
    ${If} $PrevDir == ""
      ${GetParent} "$PrevUninst" $PrevDir
    ${EndIf}
    ${If} $PrevDir != ""
      StrCpy $INSTDIR "$PrevDir"
    ${EndIf}
  ${EndIf}
FunctionEnd

; ---------------------------------------------------------------- upgrade
;
; Runs after the components and directory pages, so a person who gets this far
; and then cancels still has the version they started with.

Section -Upgrade
  ; $PrevDir has to be non-empty as well: it is the value of _?=, and an empty
  ; one turns a silent uninstall into a syntax error the person never sees.
  ${If} $PrevUninst != ""
  ${AndIf} $PrevDir != ""
  ${AndIf} ${FileExists} "$PrevUninst"
    DetailPrint "Removing BENCsnip $PrevVersion"
    ; _?= keeps the uninstaller where it is instead of copying itself to a
    ; temporary directory and returning immediately - without it ExecWait does
    ; not wait, and the removal races the installation that follows.
    ExecWait '"$PrevUninst" /S _?=$PrevDir' $0
    DetailPrint "  uninstaller returned $0"
    Delete "$PrevUninst"    ; _?= means it cannot delete itself
  ${EndIf}
SectionEnd

; ---------------------------------------------------------------- program

Section "BENCsnip (the program)" SEC_APP
  SectionIn RO
  SetOutPath "$INSTDIR"
  SetOverwrite on

  ; One executable. It carries its font, its icon, every licence text and its
  ; own ffmpeg inside itself, so there is no folder of libraries beside it and
  ; no way for it to stop working because a file went missing.
  File "${SRCDIR}/bencsnip.exe"
  File "${SRCDIR}/README.md"
  File "${SRCDIR}/ARCHITECTURE.md"
  File "${SRCDIR}/LICENSE"
  File "${SRCDIR}/NOTICE"
  File "${SRCDIR}/BINARY-LICENCE.txt"
  File /nonfatal "${SRCDIR}/ffmpeg-configure.txt"

  ; The Start Menu entry starts in Videos; the uninstaller starts where it
  ; lives. NSIS takes a shortcut's working directory from the last SetOutPath.
  CreateDirectory "$SMPROGRAMS\BENCsnip"
  SetOutPath "$WorkDir"
  CreateShortCut "$SMPROGRAMS\BENCsnip\BENCsnip.lnk" \
                 "$INSTDIR\bencsnip.exe" "" "$INSTDIR\bencsnip.exe" 0
  SetOutPath "$INSTDIR"
  CreateShortCut "$SMPROGRAMS\BENCsnip\Uninstall BENCsnip.lnk" \
                 "$INSTDIR\uninstall.exe"
SectionEnd

Section "Desktop shortcut" SEC_DESKTOP
  SetOutPath "$WorkDir"
  CreateShortCut "$DESKTOP\BENCsnip.lnk" \
                 "$INSTDIR\bencsnip.exe" "" "$INSTDIR\bencsnip.exe" 0
  SetOutPath "$INSTDIR"
SectionEnd

; ---------------------------------------------------------------- projects
;
; .bencsnip is this program's own extension and nothing else claims it, so
; taking it is not a land grab. The program already opens one handed to it on
; the command line, which is all a shell association is.
;
; Media files are deliberately not touched. Somebody who installs a video
; editor has not asked for every .mp4 on their machine to stop opening in
; whatever they were happy with.

Section "Open .bencsnip projects" SEC_ASSOC
  WriteRegStr HKCR ".bencsnip" "" "BENCsnip.Project"
  WriteRegStr HKCR "BENCsnip.Project" "" "BENCsnip project"
  WriteRegStr HKCR "BENCsnip.Project\DefaultIcon" "" "$INSTDIR\bencsnip.exe,0"
  WriteRegStr HKCR "BENCsnip.Project\shell\open\command" \
                   "" '"$INSTDIR\bencsnip.exe" "%1"'
  WriteRegDWORD HKLM "${REGKEY}" "Associated" 1

  ; Without this the association is real but Explorer goes on drawing the old
  ; icon and offering the old program until it is restarted.
  System::Call 'shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_APP} \
    "The editor itself. One executable with ffmpeg inside it."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_DESKTOP} \
    "A shortcut on the desktop as well as in the Start Menu."
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_ASSOC} \
    "Double-clicking a .bencsnip project opens it in BENCsnip. Video and audio files are left alone."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ---------------------------------------------------------------- post

Section -Post
  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr   HKLM "${REGKEY}" "DisplayName"     "BENCsnip"
  WriteRegStr   HKLM "${REGKEY}" "DisplayVersion"  "${VERSION}"
  WriteRegStr   HKLM "${REGKEY}" "Publisher"       "BENCO Holdings"
  WriteRegStr   HKLM "${REGKEY}" "URLInfoAbout"    "https://github.com/bropple/BENCsnip"
  WriteRegStr   HKLM "${REGKEY}" "DisplayIcon"     "$INSTDIR\bencsnip.exe"
  WriteRegStr   HKLM "${REGKEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr   HKLM "${REGKEY}" "QuietUninstallString" \
                                                   "$\"$INSTDIR\uninstall.exe$\" /S"
  WriteRegStr   HKLM "${REGKEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKLM "${REGKEY}" "NoModify" 1
  WriteRegDWORD HKLM "${REGKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKLM "${REGKEY}" "EstimatedSize" "$0"
SectionEnd

; ---------------------------------------------------------------- uninstall

Function un.onInit
  SetRegView 64
  SetShellVarContext all
FunctionEnd

Section "Uninstall"
  Delete "$INSTDIR\bencsnip.exe"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\ARCHITECTURE.md"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\NOTICE"
  Delete "$INSTDIR\BINARY-LICENCE.txt"
  Delete "$INSTDIR\ffmpeg-configure.txt"
  Delete "$INSTDIR\uninstall.exe"

  ; By name, not RMDir /r: a project somebody saved into the install directory
  ; is theirs, and the plain RMDir leaves the folder exactly when there is
  ; something in it worth leaving.
  RMDir "$INSTDIR"

  ; And the BENCO folder above it, when this went to the default place.
  ;
  ; RMDir without /r removes a directory only when it is empty, so BENCO goes
  ; when this was the last BENC program in it and stays when another is still
  ; installed beside it. RMDir /r there would uninstall the neighbours.
  ;
  ; Only for the default directory. Setup lets the directory be changed, and
  ; what sits above a path somebody typed themselves is not this uninstaller's
  ; to remove - an install into D:\Apps\BENCsnip should not take D:\Apps with
  ; it on the way out, however empty it happens to be.
  StrCmp $INSTDIR "$PROGRAMFILES64\BENCO\BENCsnip" 0 +2
    RMDir "$INSTDIR\.."

  Delete "$SMPROGRAMS\BENCsnip\BENCsnip.lnk"
  Delete "$SMPROGRAMS\BENCsnip\Uninstall BENCsnip.lnk"
  RMDir  "$SMPROGRAMS\BENCsnip"
  Delete "$DESKTOP\BENCsnip.lnk"

  ; Only what this install actually wrote. The flag is read back rather than
  ; the command string being searched for $INSTDIR: NSIS has no substring
  ; operator worth the name, and every version of that check written by hand
  ; is a new way to delete somebody else's association.
  ReadRegDWORD $0 HKLM "${REGKEY}" "Associated"
  ${If} $0 == 1
    DeleteRegKey HKCR "BENCsnip.Project"
    DeleteRegKey HKCR ".bencsnip"
    System::Call 'shell32::SHChangeNotify(i 0x8000000, i 0, i 0, i 0)'
  ${EndIf}

  DeleteRegKey HKLM "${REGKEY}"
SectionEnd
