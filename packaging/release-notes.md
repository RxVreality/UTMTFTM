A small one. The Steam Frame version came out first, and now the Windows beta too, after I tried it on my own PC.

- On Windows, 0.6.1's Update button downloaded the new version and ran its installer out of sight, and every look for a new version started a hidden copy of curl. That's how a Trojan downloader behaves, and @MMeNDtal got an antivirus warning on 0.6.1. Now UTMTFTM only asks GitHub which version is newest, through Windows' own web library, and the Update page hands the download to your web browser. You open the installer yourself, and it updates in place, so SteamVR can stay open.
- On a Frame, UTMTFTM starts while the headset boots, before its Wi-Fi is back, so its first look for a new version failed and the next one was six hours later. A look that fails now tries again after a minute, then after 2, 5 and 15 minutes, then every 30 until it gets through.

**Updating from 0.6.1 on Windows:** download `UTMTFTM-Setup-0.6.2.exe` below with your web browser and run it. SteamVR can stay open, and 0.6.2 starts the next time you start SteamVR. 0.6.1's own Update button would work too, but it does it the old way, out of sight, so this one time please don't use it. The installer isn't code-signed, so Windows may show a blue warning: click **More info**, then **Run anyway**.

**Updating from 0.6.1 on a Frame:** this is the first version the **Update** button can install. Tap it at the bottom of the page. It installs while you play, and 0.6.2 starts the next time SteamVR or the headset does. If the button isn't there yet, that's the bug above: close UTMTFTM from its page and open it again from the **+** on the bottom bar, and it looks again.

**Installing it fresh on Windows:** close SteamVR, run `UTMTFTM-Setup-0.6.2.exe` from below, and start SteamVR again. It doesn't need admin rights.

**Installing it fresh on a Frame:** turn on Developer Mode in Steam's System settings, open Konsole from the **+** on the bottom bar, and paste this:

```sh
curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
```

It restarts SteamVR at the end, so don't run it in the middle of a game. If you use FrameDrop, the **Install with FrameDrop** link in the [README](https://github.com/RxVreality/UTMTFTM#steam-frame) does the same.

**If Virtual Desktop stops finding your headset:** while testing this week, Virtual Desktop's Launch SteamVR button on my PC stopped working. As far as I can tell it wasn't UTMTFTM: its installer only adds its own folder to SteamVR's list of add-ons, through Valve's own tool, and leaves the others alone. If it happens to you, check that Virtual Desktop is still switched on in SteamVR's Manage Add-ons, then quit and reopen Steam and the Virtual Desktop Streamer.

What I've tried: on Windows, my PC with a Quest 3 over Virtual Desktop. I installed it and placed, saved and reset both hands in Half-Life: Alyx, and everything worked. I haven't seen the new Update page hand a download to the browser yet, because that needs a version newer than this one. On my Steam Frame, running natively on the headset, it installed and started with SteamVR running, found the add-on, and I went through the menu pages. I haven't played a game on the Frame with it yet.

The Windows version stays a beta. There's more in between on a PC than on the Frame, like streaming apps and lots of different headsets, and I've used it less.

It's the yellow hand in the dashboard. The full list of changes is in the [changelog](https://github.com/RxVreality/UTMTFTM/blob/main/CHANGELOG.md). If something goes wrong, please open an issue and attach the logs.
