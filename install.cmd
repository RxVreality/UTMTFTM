@echo off
rem Adds the driver add-on for this Windows user and registers the overlay with SteamVR.
rem No admin rights needed. Run it by double-clicking it, so it sees the same system as your games do.
setlocal
cd /d "%~dp0"
start /wait "" utmtftm.exe --install
if %ERRORLEVEL%==0 (
  echo Installed. If SteamVR is running, restart it. Then open the dashboard and look for UTMTFTM.
) else if %ERRORLEVEL%==2 (
  echo The driver add-on is in, but SteamVR couldn't be reached. Start SteamVR and run this again.
) else (
  echo Something failed, code %ERRORLEVEL%. See %LOCALAPPDATA%\UTMTFTM\utmtftm.log
)
pause
