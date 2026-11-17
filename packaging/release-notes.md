A fix for worn sticks on Quest controllers, and a purple Windows version so you can tell it apart from the Frame's.

- **Stick drift (Windows).** On the Decaf tab, tap **Sticks**, then **Check my sticks**. It takes about half a minute: let go of both sticks, roll each one round its edge, then flick each one up, right, down and left. If a stick drifts, the game sees the middle when you let go from then on, and a full push still goes all the way. A stick that comes back to the middle is left alone. It's saved per controller, and **Fix drift** turns it off and on. It's only in the Windows version: the Steam Frame's sticks use magnets and don't wear this way.
- **Windows is purple now.** The pages, the icon and the hand you're placing are purple on Windows, and the Frame version stays yellow. If you have both and stream from your PC to a Frame, you can tell which one you're in. While the Frame streams, its own copy doesn't move your hands, so place them with the purple one.
- **Fixed:** on Windows, SteamVR's dashboard showed a question mark where UTMTFTM's hand should be. Thanks to @MMeNDtal for spotting it.

**Updating from 0.6.2:** tap **Update** at the bottom of UTMTFTM's page. On Windows it hands `UTMTFTM-Setup-0.6.3.exe` to your web browser: open it when it's downloaded, and SteamVR can stay open. On a Frame it installs with one tap, while you play. Either way 0.6.3 starts the next time SteamVR does. This is the first time the Update button has had a version to give on Windows, so if it doesn't work for you, download the installer below instead. It isn't code-signed, so Windows may show a blue warning: click **More info**, then **Run anyway**.

**Updating from 0.6.1 or earlier on Windows:** download `UTMTFTM-Setup-0.6.3.exe` below with your web browser and run it, rather than using 0.6.1's own Update button, which does it out of sight.

**Installing it fresh on Windows:** close SteamVR, run `UTMTFTM-Setup-0.6.3.exe` from below, and start SteamVR again. It doesn't need admin rights.

**Installing it fresh on a Frame:** turn on Developer Mode in Steam's System settings, open Konsole from the **+** on the bottom bar, and paste this:

```sh
curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
```

It restarts SteamVR at the end, so don't run it in the middle of a game. If you use FrameDrop, the **Install with FrameDrop** link in the [README](https://github.com/RxVreality/UTMTFTM#steam-frame) does the same.

What I've tried this version on: on Windows, my PC, streaming from my Steam Frame over Steam Link with the Frame version running too. I placed and saved hands in Half-Life: Alyx and Beanstalker, and everything was purple except the hand in the dashboard bar, which still showed yellow over Steam Link. I haven't worked out why yet. With my Quest 3 over Virtual Desktop I ran the stick check on my own sticks, which don't drift. That caught a bug: a quick roll that didn't go all the way out made a healthy stick look worn. That's fixed here, and the automated tests cover it, but I haven't run the check in the headset again since, and it hasn't met a really drifting controller yet. The Frame version passed the automated tests on an ARM64 machine and in a simulated SteamVR, but I haven't tried 0.6.3 on my Frame yet. For the Frame the changes are small: no stick drift, and the same yellow.

The Windows version stays a beta. There's more in between on a PC than on the Frame, like streaming apps and lots of different headsets, and I've used it less.

It's the purple hand in the dashboard on Windows, and the yellow one on a Frame. The full list of changes is in the [changelog](https://github.com/RxVreality/UTMTFTM/blob/main/CHANGELOG.md). If something goes wrong, please open an issue and attach the logs.
