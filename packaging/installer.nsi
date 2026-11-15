; The UTMTFTM installer. It installs for the current Windows user, so it never asks for admin rights.
; make-release.ps1 builds it and passes VERSION, STAGE, OUTFILE and ICON.
Unicode true
!include "MUI2.nsh"
!include "FileFunc.nsh"

!define ARP "Software\Microsoft\Windows\CurrentVersion\Uninstall\UTMTFTM"

Name "UTMTFTM"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\UTMTFTM"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "UTMTFTM ${VERSION}, beta for Windows"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_FINISHPAGE_TITLE "UTMTFTM beta is installed"
!define MUI_FINISHPAGE_TEXT "Start SteamVR and open the dashboard. UTMTFTM is the yellow hand.$\r$\n$\r$\nStart a game first if you want to see your hands move while you place them.$\r$\n$\r$\nThe Windows version is a beta. If something feels off, please open an issue on GitHub and attach the logs from %LOCALAPPDATA%\UTMTFTM."
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

; SteamVR keeps the driver file locked while it runs, so it has to be closed before any of this.
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

; /UPDATE: started by UTMTFTM's own Update button, with SteamVR running and the add-on loaded. Windows lets a file that's
; in use be renamed, so the running ones move aside and the new ones go in where they were. SteamVR uses them from its next
; start, and nothing is closed. /NOREGISTER leaves SteamVR's lists alone, for the tests.
Var Update
Var NoRegister
Function .onInit
  StrCpy $Update ""
  StrCpy $NoRegister ""
  ${GetParameters} $R0
  ClearErrors
  ${GetOptions} $R0 "/UPDATE" $R1
  IfErrors +2
    StrCpy $Update "1"
  ClearErrors
  ${GetOptions} $R0 "/NOREGISTER" $R1
  IfErrors +2
    StrCpy $NoRegister "1"
  StrCmp $Update "1" +2
    Call WaitForSteamVR
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
  WriteUninstaller "$INSTDIR\uninstall.exe"

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
