; The UTMTFTM installer. It installs for the current Windows user, so it never asks for admin rights.
; make-release.ps1 builds it and passes VERSION, VINUMBERS, STAGE, OUTFILE and ICON.
;
; It's made in two goes so the uninstaller can be code-signed like everything else: makensis can't wait while GitHub's
; build sends a file off to be signed. First with EXPORT_UNINST=<path>, which only writes the uninstaller to that path,
; then, once it's signed, with IMPORT_UNINST=<path>, which puts that signed copy in instead of writing a new one.
Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"

!define ARP "Software\Microsoft\Windows\CurrentVersion\Uninstall\UTMTFTM"

!ifdef EXPORT_UNINST & IMPORT_UNINST
  !error "EXPORT_UNINST and IMPORT_UNINST are two separate goes"
!endif
!ifndef EXPORT_UNINST & IMPORT_UNINST
  !error "make-release.ps1 builds this: pass EXPORT_UNINST, then IMPORT_UNINST"
!endif

; Windows' Details tab, for the installer and the uninstaller alike. Code signing checks the name and version.
VIProductVersion "${VINUMBERS}"
VIFileVersion "${VINUMBERS}"
VIAddVersionKey /LANG=1033 "ProductName" "UTMTFTM"
VIAddVersionKey /LANG=1033 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=1033 "FileDescription" "UTMTFTM installer"
VIAddVersionKey /LANG=1033 "CompanyName" "The UTMTFTM contributors"
VIAddVersionKey /LANG=1033 "LegalCopyright" "Copyright (c) 2026 The UTMTFTM contributors. MIT License."

Name "UTMTFTM"
!ifdef EXPORT_UNINST
  ; Only the uninstaller is wanted from this go. makensis runs this on it once it's written; the installer itself is
  ; thrown away.
  OutFile "${OUTFILE}.export-only.exe"
  !uninstfinalize 'copy /Y "%1" "${EXPORT_UNINST}"'
!else
  OutFile "${OUTFILE}"
!endif
InstallDir "$LOCALAPPDATA\Programs\UTMTFTM"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "UTMTFTM ${VERSION}, beta for Windows"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_FINISHPAGE_TITLE "UTMTFTM beta is installed"
; What to do next: start SteamVR, or after an update in place, restart it (.onInit says which).
Var Finish
!define MUI_FINISHPAGE_TEXT "$Finish$\r$\n$\r$\nThe Windows version is a beta. If something feels off, please open an issue on GitHub and attach the logs from %LOCALAPPDATA%\UTMTFTM."
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; $0 is "0" when SteamVR's server is running.
Function SteamVRRunning
  nsExec::ExecToStack 'cmd /c tasklist /nh /fi "imagename eq vrserver.exe" | find /i "vrserver.exe"'
  Pop $0
  Pop $1
FunctionEnd

; SteamVR keeps the driver file locked while it runs, so a first install or an uninstall needs it closed.
!macro WaitForSteamVR un
Function ${un}WaitForSteamVR
  retry:
    nsExec::ExecToStack 'cmd /c tasklist /nh /fi "imagename eq vrserver.exe" | find /i "vrserver.exe"'
    Pop $0
    Pop $1
    StrCmp $0 "0" 0 done
    MessageBox MB_RETRYCANCEL|MB_ICONINFORMATION "SteamVR is running. Close it, then click Retry." /SD IDCANCEL IDRETRY retry
    Abort
  done:
FunctionEnd
!macroend
!insertmacro WaitForSteamVR ""
!insertmacro WaitForSteamVR "un."

; Updating with SteamVR running and the add-on loaded: Windows lets a file that's in use be renamed, so the running ones
; move aside and the new ones go in where they were. SteamVR uses them from its next start, and nothing is closed.
; From 0.6.2 that's what happens when you open this yourself while SteamVR runs and UTMTFTM is already installed: UTMTFTM's
; Update page only hands this file to the web browser, and you open it, SteamVR open or not (J4, 15 November).
; /UPDATE: 0.6.1's Update button, which started this itself, out of sight. Kept so 0.6.1 can still update to this.
; /NOREGISTER leaves SteamVR's lists alone, for the tests.
Var Update
Var NoRegister
Function .onInit
  StrCpy $Update ""
  StrCpy $NoRegister ""
  StrCpy $Finish "Start SteamVR and open the dashboard. UTMTFTM is the purple hand.$\r$\n$\r$\nStart a game first if you want to see your hands move while you place them."
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/UPDATE" $R1
  IfErrors +2
    StrCpy $Update "1"
  ClearErrors
  ${GetOptions} $R0 "/NOREGISTER" $R1
  IfErrors +2
    StrCpy $NoRegister "1"
  StrCmp $Update "1" inplace
  Call SteamVRRunning
  StrCmp $0 "0" 0 done
  IfFileExists "$INSTDIR\utmtftm.exe" 0 wait
    StrCpy $Update "1"
    Goto inplace
  wait:
    Call WaitForSteamVR
    Goto done
  inplace:
    StrCpy $Finish "UTMTFTM ${VERSION} is in, next to the one SteamVR is using now. Restart SteamVR when you're ready, and ${VERSION} starts with it.$\r$\n$\r$\nYour settings, layouts and macros stay as they are."
  done:
FunctionEnd

; Moves a file that may be in use out of the way, under a name no earlier update used, after clearing earlier ones that
; nothing holds any more. $2 is the clock when this install started.
!macro MoveAside FILE
  Delete "${FILE}.old*"
  IfFileExists "${FILE}" 0 +2
    Rename "${FILE}" "${FILE}.old$2"
!macroend

Function un.onInit
  Call un.WaitForSteamVR
FunctionEnd

Section
  StrCmp $Update "1" 0 fresh
    System::Call 'kernel32::GetTickCount() i .r2'
    !insertmacro MoveAside "$INSTDIR\utmtftm.exe"
    !insertmacro MoveAside "$INSTDIR\openvr_api.dll"
    !insertmacro MoveAside "$INSTDIR\driver\bin\win64\driver_utmtftm.dll"
    Goto placed
  fresh:
  ; SteamVR is shut, so whatever an update moved aside last time can go now.
  Delete "$INSTDIR\utmtftm.exe.old*"
  Delete "$INSTDIR\openvr_api.dll.old*"
  Delete "$INSTDIR\driver\bin\win64\driver_utmtftm.dll.old*"
  placed:
  SetOutPath "$INSTDIR"
  File /r /x install.cmd /x uninstall.cmd "${STAGE}\*.*"
  !ifdef EXPORT_UNINST
    WriteUninstaller "$INSTDIR\uninstall.exe"
  !else
    ; The uninstaller the first go wrote, signed if this is a release.
    File "/oname=uninstall.exe" "${IMPORT_UNINST}"
  !endif

  WriteRegStr HKCU "${ARP}" "DisplayName" "UTMTFTM (Windows beta)"
  WriteRegStr HKCU "${ARP}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${ARP}" "Publisher" "The UTMTFTM contributors"
  WriteRegStr HKCU "${ARP}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${ARP}" "DisplayIcon" "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "${ARP}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${ARP}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${ARP}" "NoModify" 1
  WriteRegDWORD HKCU "${ARP}" "NoRepair" 1

  StrCpy $0 "0"
  StrCmp $NoRegister "1" registered
    ExecWait '"$INSTDIR\utmtftm.exe" --install' $0
  registered:
  ; --install starts SteamVR's server to register with it. SteamVR was closed when we began, so that copy can go. Not when
  ; updating: that's the SteamVR you're in.
  StrCmp $Update "1" kept
    nsExec::Exec 'taskkill /f /im vrserver.exe'
    Pop $1
  kept:
  StrCmp $0 "0" installed
    MessageBox MB_ICONEXCLAMATION "UTMTFTM couldn't register with SteamVR (code $0). Check SteamVR is installed, start it once, close it and run this again.$\r$\n$\r$\nDetails are in %LOCALAPPDATA%\UTMTFTM\utmtftm.log" /SD IDOK
    SetErrorLevel 3
  installed:
SectionEnd

Section "Uninstall"
  ExecWait '"$INSTDIR\utmtftm.exe" --uninstall'
  nsExec::Exec 'taskkill /f /im vrserver.exe'
  Pop $0
  RMDir /r "$INSTDIR"
  DeleteRegKey HKCU "${ARP}"
  MessageBox MB_YESNO|MB_ICONQUESTION|MB_DEFBUTTON2 "Delete your saved hand offsets too?" /SD IDNO IDNO keep
    RMDir /r "$LOCALAPPDATA\UTMTFTM"
  keep:
SectionEnd
