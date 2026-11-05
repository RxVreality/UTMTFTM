@echo off
rem Removes the driver add-on and the SteamVR registration. Saved offsets stay in %LOCALAPPDATA%\UTMTFTM.
setlocal
cd /d "%~dp0"
start /wait "" utmtftm.exe --uninstall
if %ERRORLEVEL%==0 (
  echo Removed. If SteamVR is running, restart it to finish.
) else if %ERRORLEVEL%==2 (
  echo The driver add-on is gone, but SteamVR couldn't be reached. Start SteamVR and run this again to remove the overlay too.
) else (
  echo Something failed, code %ERRORLEVEL%. See %LOCALAPPDATA%\UTMTFTM\utmtftm.log
)
pause
