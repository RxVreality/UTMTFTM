; The UTMTFTM installer. It installs for the current Windows user, so it never asks for admin rights.
; make-release.ps1 builds it and passes VERSION, STAGE, OUTFILE and ICON.
Unicode true
!include "MUI2.nsh"

!define ARP "Software\Microsoft\Windows\CurrentVersion\Uninstall\UTMTFTM"

Name "UTMTFTM"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\UTMTFTM"
RequestExecutionLevel user
SetCompressor /SOLID lzma
BrandingText "UTMTFTM ${VERSION}"

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_FINISHPAGE_TITLE "UTMTFTM is installed"
!define MUI_FINISHPAGE_TEXT "Start SteamVR and open the dashboard. UTMTFTM is the yellow hand.$\r$\n$\r$\nStart a game first if you want to see your hands move while you place them."
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

Function .onInit
  Call WaitForSteamVR
FunctionEnd

Function un.onInit
  Call un.WaitForSteamVR
FunctionEnd

Section
  SetOutPath "$INSTDIR"
  File /r /x install.cmd /x uninstall.cmd "${STAGE}\*.*"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  WriteRegStr HKCU "${ARP}" "DisplayName" "UTMTFTM"
  WriteRegStr HKCU "${ARP}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${ARP}" "Publisher" "The UTMTFTM contributors"
  WriteRegStr HKCU "${ARP}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${ARP}" "DisplayIcon" "$INSTDIR\uninstall.exe"
  WriteRegStr HKCU "${ARP}" "UninstallString" '"$INSTDIR\uninstall.exe"'
  WriteRegStr HKCU "${ARP}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
  WriteRegDWORD HKCU "${ARP}" "NoModify" 1
  WriteRegDWORD HKCU "${ARP}" "NoRepair" 1

  ExecWait '"$INSTDIR\utmtftm.exe" --install' $0
  ; --install starts SteamVR's server to register with it. SteamVR was closed when we began, so that copy can go.
  nsExec::Exec 'taskkill /f /im vrserver.exe'
  Pop $1
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
