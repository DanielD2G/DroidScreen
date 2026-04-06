; ─────────────────────────────────────────────────────────────
; DroidScreen NSIS Installer Script
;
; Builds an installer for DroidScreen Windows x64.
; Supports clean install and overwrite/upgrade installs.
;
; Expected staging layout (set via /DSTAGING_DIR=...):
;   staging/app/droidscreen_desktop.exe
;   staging/adb/adb.exe
;   staging/adb/AdbWinApi.dll
;   staging/adb/AdbWinUsbApi.dll
;   staging/icon.ico
; ─────────────────────────────────────────────────────────────

; ── Compiler flags ───────────────────────────────────────────
!include "MUI2.nsh"
!include "x64.nsh"
!include "nsDialogs.nsh"

; ── Defines (can be overridden from CLI with /D) ─────────────
!ifndef VERSION
  !define VERSION "0.0.0"
!endif

!ifndef STAGING_DIR
  !define STAGING_DIR "staging"
!endif

!ifndef OUTFILE
  !define OUTFILE "DroidScreen-Setup.exe"
!endif

; ── General settings ─────────────────────────────────────────
Name "DroidScreen ${VERSION}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\DroidScreen"
InstallDirRegKey HKCU "Software\DroidScreen" "InstallDir"
RequestExecutionLevel admin
Unicode True

; Allow overwriting files without prompting (handles upgrades).
SetOverwrite on

; ── Version information embedded in the .exe ─────────────────
VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "DroidScreen"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "DroidScreen Installer"
VIAddVersionKey "FileVersion" "${VERSION}"

; ── Variables ────────────────────────────────────────────────
Var DesktopShortcut
Var AutoStart

; ── Modern UI configuration ──────────────────────────────────
!define MUI_ABORTWARNING

; Use the DroidScreen icon for installer and uninstaller.
!define MUI_ICON "${STAGING_DIR}\icon.ico"
!define MUI_UNICON "${STAGING_DIR}\icon.ico"

; Finish page: offer to run DroidScreen
!define MUI_FINISHPAGE_RUN "$INSTDIR\droidscreen_desktop.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Run DroidScreen"

; ── Pages ────────────────────────────────────────────────────
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_DIRECTORY
Page custom OptionsPage OptionsPageLeave
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

; Uninstaller pages
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

; ── Language ─────────────────────────────────────────────────
!insertmacro MUI_LANGUAGE "English"

; ── Custom options page ──────────────────────────────────────
Function OptionsPage
  nsDialogs::Create 1018
  Pop $0
  ${If} $0 == error
    Abort
  ${EndIf}

  ${NSD_CreateCheckbox} 10u 10u 100% 12u "Create Desktop shortcut"
  Pop $DesktopShortcut
  ${NSD_SetState} $DesktopShortcut ${BST_CHECKED}

  ${NSD_CreateCheckbox} 10u 30u 100% 12u "Start DroidScreen when Windows starts"
  Pop $AutoStart
  ${NSD_SetState} $AutoStart ${BST_UNCHECKED}

  nsDialogs::Show
FunctionEnd

Function OptionsPageLeave
  ${NSD_GetState} $DesktopShortcut $DesktopShortcut
  ${NSD_GetState} $AutoStart $AutoStart
FunctionEnd

; ══════════════════════════════════════════════════════════════
; Installation section
; ══════════════════════════════════════════════════════════════
Section "DroidScreen (required)" SecMain
  SectionIn RO

  ; ── Kill running instance before overwriting ────────────────
  ; Silently kill any running DroidScreen process so files can
  ; be overwritten during upgrades. /F = force, /T = tree.
  nsExec::ExecToLog 'taskkill /F /IM droidscreen_desktop.exe /T'

  ; ── Main application + FFmpeg DLLs ──────────────────────────
  SetOutPath "$INSTDIR"
  File /oname=droidscreen_desktop.exe "${STAGING_DIR}\app\droidscreen_desktop.exe"
  File /oname=icon.ico "${STAGING_DIR}\icon.ico"
  ; FFmpeg shared libraries (bundled, may not all exist in every build)
  File /nonfatal "${STAGING_DIR}\app\avcodec*.dll"
  File /nonfatal "${STAGING_DIR}\app\avutil*.dll"
  File /nonfatal "${STAGING_DIR}\app\swscale*.dll"
  File /nonfatal "${STAGING_DIR}\app\swresample*.dll"
  File /nonfatal "${STAGING_DIR}\app\avformat*.dll"

  ; ── ADB (bundled platform-tools) ───────────────────────────
  ; Always overwrite — updates ADB to latest version.
  SetOutPath "$INSTDIR\adb"
  File /oname=adb.exe "${STAGING_DIR}\adb\adb.exe"
  File /oname=AdbWinApi.dll "${STAGING_DIR}\adb\AdbWinApi.dll"
  File /oname=AdbWinUsbApi.dll "${STAGING_DIR}\adb\AdbWinUsbApi.dll"

  ; ── Save install directory in registry ─────────────────────
  WriteRegStr HKCU "Software\DroidScreen" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\DroidScreen" "Version" "${VERSION}"

  ; ── Start Menu shortcuts ───────────────────────────────────
  CreateDirectory "$SMPROGRAMS\DroidScreen"
  CreateShortcut "$SMPROGRAMS\DroidScreen\DroidScreen.lnk" \
    "$INSTDIR\droidscreen_desktop.exe" "" "$INSTDIR\icon.ico"
  CreateShortcut "$SMPROGRAMS\DroidScreen\Uninstall DroidScreen.lnk" \
    "$INSTDIR\Uninstall.exe"

  ; ── Desktop shortcut (optional) ────────────────────────────
  ${If} $DesktopShortcut == ${BST_CHECKED}
    CreateShortcut "$DESKTOP\DroidScreen.lnk" \
      "$INSTDIR\droidscreen_desktop.exe" "" "$INSTDIR\icon.ico"
  ${EndIf}

  ; ── Auto-start (optional) ─────────────────────────────────
  ${If} $AutoStart == ${BST_CHECKED}
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" \
      "DroidScreen" '"$INSTDIR\droidscreen_desktop.exe"'
  ${EndIf}

  ; ── Uninstaller ────────────────────────────────────────────
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; ── Add/Remove Programs entry ──────────────────────────────
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroidScreen" \
    "DisplayName" "DroidScreen"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroidScreen" \
    "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroidScreen" \
    "DisplayIcon" '"$INSTDIR\icon.ico"'
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroidScreen" \
    "DisplayVersion" "${VERSION}"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroidScreen" \
    "Publisher" "DroidScreen"
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroidScreen" \
    "NoModify" 1
  WriteRegDWORD HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroidScreen" \
    "NoRepair" 1
SectionEnd

; ══════════════════════════════════════════════════════════════
; Parsec Virtual Display Driver — prompt user to install if missing
; ══════════════════════════════════════════════════════════════
Section "Parsec Virtual Display Driver" SecParsecVDD
  ; Check if the Parsec VDD driver is already installed by looking
  ; for its device in the registry.
  ClearErrors
  ReadRegStr $0 HKLM "SYSTEM\CurrentControlSet\Services\ParsecVDA" "ImagePath"
  IfErrors 0 parsec_already_installed

  ; Driver not found — ask user if they want to download it.
  MessageBox MB_YESNO|MB_ICONQUESTION \
    "DroidScreen requires the Parsec Virtual Display Driver to create$\n\
a virtual monitor for streaming.$\n$\n\
The driver is not currently installed. Would you like to open$\n\
the download page now?$\n$\n\
(You can also install it later — the app will prompt you.)" \
    IDYES parsec_open_download IDNO parsec_already_installed

  parsec_open_download:
    ExecShell "open" "https://github.com/nomi-san/parsec-vdd/releases"

  parsec_already_installed:
SectionEnd

; ══════════════════════════════════════════════════════════════
; Uninstaller
; ══════════════════════════════════════════════════════════════
Section "Uninstall"
  ; ── Kill running instance ──────────────────────────────────
  nsExec::ExecToLog 'taskkill /F /IM droidscreen_desktop.exe /T'

  ; ── Remove application files ───────────────────────────────
  Delete "$INSTDIR\droidscreen_desktop.exe"
  Delete "$INSTDIR\icon.ico"
  Delete "$INSTDIR\avcodec*.dll"
  Delete "$INSTDIR\avutil*.dll"
  Delete "$INSTDIR\swscale*.dll"
  Delete "$INSTDIR\swresample*.dll"
  Delete "$INSTDIR\avformat*.dll"
  Delete "$INSTDIR\adb\adb.exe"
  Delete "$INSTDIR\adb\AdbWinApi.dll"
  Delete "$INSTDIR\adb\AdbWinUsbApi.dll"
  RMDir "$INSTDIR\adb"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"

  ; ── Remove shortcuts ───────────────────────────────────────
  Delete "$SMPROGRAMS\DroidScreen\DroidScreen.lnk"
  Delete "$SMPROGRAMS\DroidScreen\Uninstall DroidScreen.lnk"
  RMDir "$SMPROGRAMS\DroidScreen"
  Delete "$DESKTOP\DroidScreen.lnk"

  ; ── Remove registry entries ────────────────────────────────
  DeleteRegValue HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "DroidScreen"
  DeleteRegKey HKCU "Software\DroidScreen"
  DeleteRegKey HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroidScreen"

  ; ── Remove log directory ───────────────────────────────────
  RMDir /r "$LOCALAPPDATA\DroidScreen"
SectionEnd
