; rPlayHub for Windows -- per-user NSIS installer.
;
; Built by scripts/package-windows.sh, which passes:
;   -DVERSION=0.1.3                  dotted version (client-c/Makefile RPLAYHUB_VERSION)
;   -DSTAGE=<dir>                    the staged layout from scripts/build-windows.sh
;   -DOUTFILE=<file.exe>             where to write the installer
;   -DUNINST_LIST=<file.nsh>         generated Delete/RMDir lines for exactly the staged files
;   -DUNINST_SIGN=<cmd "%1">         optional: signs the generated uninstaller (Authenticode)
;
; Per-user on purpose: rPlayHub has no driver and no service (the engine is a child process of
; the GUI, over a userspace TCP/IP stack), so nothing needs administrator rights and the
; installer never shows a UAC prompt. Installs to %LOCALAPPDATA%\Programs\rPlayHub and registers
; its uninstaller under HKCU.

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!ifndef VERSION
  !error "pass -DVERSION=x.y.z"
!endif
!ifndef STAGE
  !error "pass -DSTAGE=<staged layout dir>"
!endif
!ifndef OUTFILE
  !error "pass -DOUTFILE=<installer.exe>"
!endif
!ifndef UNINST_LIST
  !error "pass -DUNINST_LIST=<generated uninstall list>"
!endif

!define APPNAME     "rPlayHub"
!define PUBLISHER   "rPlayAI"
!define URL         "https://github.com/rPlayAI/rPlayHub"
!define DOCURL      "https://github.com/rPlayAI/rPlayHub/blob/main/doc/WINDOWS.md"
!define UNINSTKEY   "Software\Microsoft\Windows\CurrentVersion\Uninstall\rPlayHub"

!ifdef UNINST_SIGN
  !uninstfinalize '${UNINST_SIGN}' = 0     ; a failed signature fails the build
!endif

Name "${APPNAME}"
Caption "${APPNAME} ${VERSION} Setup"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\rPlayHub"
InstallDirRegKey HKCU "Software\rPlayHub" "InstallDir"
BrandingText "${APPNAME} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName"     "${APPNAME}"
VIAddVersionKey "CompanyName"     "${PUBLISHER}"
VIAddVersionKey "FileDescription" "${APPNAME} installer"
VIAddVersionKey "FileVersion"     "${VERSION}"
VIAddVersionKey "ProductVersion"  "${VERSION}"
VIAddVersionKey "LegalCopyright"  "GPL-3.0-or-later"

!include "MUI2.nsh"
!include "FileFunc.nsh"
!include "LogicLib.nsh"

!define MUI_ICON   "${STAGE}\rplayhub.ico"
!define MUI_UNICON "${STAGE}\rplayhub.ico"
!define MUI_ABORTWARNING

!define MUI_WELCOMEPAGE_TEXT "This installs ${APPNAME} ${VERSION}: mirror and control an iPhone from this PC.$\r$\n$\r$\nIt installs for the current user only and needs no administrator rights.$\r$\n$\r$\nClick Next to continue."
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE.txt"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES

!define MUI_FINISHPAGE_TITLE "${APPNAME} is installed"
!define MUI_FINISHPAGE_TEXT_LARGE
!define MUI_FINISHPAGE_TEXT "Before connecting a phone you need:$\r$\n$\r$\n\
  - Apple Devices (Microsoft Store) or iTunes installed -- it provides the USB driver and Apple Mobile Device Service.$\r$\n\
  - An iPhone on iOS 27 or later, with Developer Mode on (Settings > Privacy && Security).$\r$\n\
  - Apple's Developer Disk Image: run $\"Fetch Developer Disk Image$\" from the Start menu once.$\r$\n$\r$\n\
  Then plug the phone in, unlock it and tap Trust."
!define MUI_FINISHPAGE_RUN "$INSTDIR\rplay-gui.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start ${APPNAME}"
!define MUI_FINISHPAGE_LINK "Windows setup guide and troubleshooting"
!define MUI_FINISHPAGE_LINK_LOCATION "${DOCURL}"
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; A running executable cannot be opened for writing; detect that up front rather than failing
; file by file halfway through an upgrade.
!macro NotRunning EXE WHAT LABEL
  ${LABEL}:
  ${If} ${FileExists} "$INSTDIR\${EXE}"
    ClearErrors
    FileOpen $0 "$INSTDIR\${EXE}" a
    ${If} ${Errors}
      MessageBox MB_RETRYCANCEL|MB_ICONEXCLAMATION "${WHAT}. Close it, then click Retry." IDRETRY ${LABEL}
      Abort
    ${EndIf}
    FileClose $0
  ${EndIf}
!macroend

Section "${APPNAME} (required)" SecMain
  SectionIn RO
  SetOutPath "$INSTDIR"
  ; An upgrade over a running copy would fail file by file; say so once instead.
  !insertmacro NotRunning "rplay-gui.exe" "${APPNAME} is running" gui_check
  !insertmacro NotRunning "cdhost.exe" "The ${APPNAME} engine (cdhost.exe) is running" engine_check

  File /r "${STAGE}\*.*"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "Software\rPlayHub" "InstallDir" "$INSTDIR"

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortCut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\rplay-gui.exe" "" "$INSTDIR\rplayhub.ico" 0
  CreateShortCut "$SMPROGRAMS\${APPNAME}\Fetch Developer Disk Image.lnk" \
      "$SYSDIR\WindowsPowerShell\v1.0\powershell.exe" \
      '-NoProfile -ExecutionPolicy Bypass -NoExit -File "$INSTDIR\fetch-ddi.ps1"' \
      "$INSTDIR\rplayhub.ico" 0
  CreateShortCut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\uninstall.exe"

  ; Apps & features entry, per user.
  WriteRegStr   HKCU "${UNINSTKEY}" "DisplayName"          "${APPNAME}"
  WriteRegStr   HKCU "${UNINSTKEY}" "DisplayVersion"       "${VERSION}"
  WriteRegStr   HKCU "${UNINSTKEY}" "Publisher"            "${PUBLISHER}"
  WriteRegStr   HKCU "${UNINSTKEY}" "DisplayIcon"          "$INSTDIR\rplayhub.ico"
  WriteRegStr   HKCU "${UNINSTKEY}" "InstallLocation"      "$INSTDIR"
  WriteRegStr   HKCU "${UNINSTKEY}" "URLInfoAbout"         "${URL}"
  WriteRegStr   HKCU "${UNINSTKEY}" "HelpLink"             "${DOCURL}"
  WriteRegStr   HKCU "${UNINSTKEY}" "UninstallString"      '"$INSTDIR\uninstall.exe"'
  WriteRegStr   HKCU "${UNINSTKEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTKEY}" "NoRepair" 1
  ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
  IntFmt $0 "0x%08X" $0
  WriteRegDWORD HKCU "${UNINSTKEY}" "EstimatedSize" "$0"
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortCut "$DESKTOP\${APPNAME}.lnk" "$INSTDIR\rplay-gui.exe" "" "$INSTDIR\rplayhub.ico" 0
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain}    "The GUI (rplay-gui.exe), the engine (cdhost.exe) and their libraries."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut to ${APPNAME} on the desktop."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  ; Only the files this installer put there -- never a recursive delete of $INSTDIR, which
  ; could be a folder the user chose that holds other things.
  !include "${UNINST_LIST}"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"

  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Fetch Developer Disk Image.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"
  Delete "$DESKTOP\${APPNAME}.lnk"

  DeleteRegKey HKCU "${UNINSTKEY}"
  DeleteRegKey HKCU "Software\rPlayHub"
  ; %LOCALAPPDATA%\rplayhub (logs, the Developer Disk Image) and the GUI's settings are left in
  ; place: they are user data, and the DDI is a 16 MB download.
SectionEnd
