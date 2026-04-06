; ─────────────────────────────────────────────────────────────
; DroidScreen NSIS Installer Script
;
; Builds an installer for DroidScreen Windows x64.
; Expected staging layout (set via /DSTAGING_DIR=...):
;   staging/app/droidscreen_desktop.exe
;   staging/adb/adb.exe
;   staging/adb/AdbWinApi.dll
;   staging/adb/AdbWinUsbApi.dll
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
Name "DroidScreen Setup"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\DroidScreen"
InstallDirRegKey HKCU "Software\DroidScreen" "InstallDir"
RequestExecutionLevel admin
Unicode True

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
!define MUI_ICON "${NSISDIR}\Contrib\Graphics\Icons\modern-install.ico"
!define MUI_UNICON "${NSISDIR}\Contrib\Graphics\Icons\modern-uninstall.ico"

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

  ; ── Main application ───────────────────────────────────────
  SetOutPath "$INSTDIR"
  File "${STAGING_DIR}\app\droidscreen_desktop.exe"

  ; ── ADB (bundled platform-tools) ───────────────────────────
  SetOutPath "$INSTDIR\adb"
  File "${STAGING_DIR}\adb\adb.exe"
  File "${STAGING_DIR}\adb\AdbWinApi.dll"
  File "${STAGING_DIR}\adb\AdbWinUsbApi.dll"

  ; ── Save install directory in registry ─────────────────────
  WriteRegStr HKCU "Software\DroidScreen" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "Software\DroidScreen" "Version" "${VERSION}"

  ; ── Start Menu shortcuts ───────────────────────────────────
  CreateDirectory "$SMPROGRAMS\DroidScreen"
  CreateShortcut "$SMPROGRAMS\DroidScreen\DroidScreen.lnk" "$INSTDIR\droidscreen_desktop.exe"
  CreateShortcut "$SMPROGRAMS\DroidScreen\Uninstall DroidScreen.lnk" "$INSTDIR\Uninstall.exe"

  ; ── Desktop shortcut (optional) ────────────────────────────
  ${If} $DesktopShortcut == ${BST_CHECKED}
    CreateShortcut "$DESKTOP\DroidScreen.lnk" "$INSTDIR\droidscreen_desktop.exe"
  ${EndIf}

  ; ── Auto-start (optional) ─────────────────────────────────
  ${If} $AutoStart == ${BST_CHECKED}
    WriteRegStr HKCU "Software\Microsoft\Windows\CurrentVersion\Run" "DroidScreen" '"$INSTDIR\droidscreen_desktop.exe"'
  ${EndIf}

  ; ── Uninstaller ────────────────────────────────────────────
  WriteUninstaller "$INSTDIR\Uninstall.exe"

  ; ── Add/Remove Programs entry ──────────────────────────────
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroidScreen" \
    "DisplayName" "DroidScreen"
  WriteRegStr HKLM "Software\Microsoft\Windows\CurrentVersion\Uninstall\DroidScreen" \
    "UninstallString" '"$INSTDIR\Uninstall.exe"'
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
; Uninstaller
; ══════════════════════════════════════════════════════════════
Section "Uninstall"
  ; ── Remove application files ───────────────────────────────
  Delete "$INSTDIR\droidscreen_desktop.exe"
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
SectionEnd
