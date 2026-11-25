A better stick drift fix for Quest controllers, for the kind of drift my own Quest 3 has.

- **Stick drift that flicks (Windows).** My Quest 3 sticks rest in the middle, but as I move them the reading leaps a long way, faster than a thumb can move, and comes straight back a few hundredths of a second later. It looks like jitter, and it's why some directions never read 100%. 0.6.3's check only looked at where a stick rests, lands and reaches, so it couldn't see that. Now the check also counts those jumps while you roll and flick, and the page says how many it saw, like **Jumps away and straight back, 6 times in this check.** With **Fix drift** on, a jump like that is held at the last good reading wherever it starts, and let go the moment the stick is back, so the game doesn't see it. A real push or let-go goes straight through, and nothing is held for more than about a twentieth of a second. On the Sticks page the ring still shows every jump, and the solid dot is what games get. On the Decaf tab, tap **Sticks**, then **Check my sticks** (or **Check again** if you ran it in 0.6.3). It's only in the Windows version: the Steam Frame's sticks use magnets and don't wear this way.

**Updating from 0.6.3 or 0.6.2:** tap **Update** at the bottom of UTMTFTM's page. On Windows it hands `UTMTFTM-Setup-0.6.4.exe` to your web browser: open it when it's downloaded, and SteamVR can stay open. On a Frame it installs with one tap, while you play. Either way 0.6.4 starts the next time SteamVR does. I haven't seen the Windows Update button do a real update yet: my PC already had a 0.6.4 test build on it, so it had nothing to offer me. If it doesn't work for you, download the installer below instead. It isn't code-signed, so Windows may show a blue warning: click **More info**, then **Run anyway**.

**Updating from 0.6.1 or earlier on Windows:** download `UTMTFTM-Setup-0.6.4.exe` below with your web browser and run it, rather than using 0.6.1's own Update button, which does it out of sight.

**Installing it fresh on Windows:** close SteamVR, run `UTMTFTM-Setup-0.6.4.exe` from below, and start SteamVR again. It doesn't need admin rights.

**Installing it fresh on a Frame:** turn on Developer Mode in Steam's System settings, open Konsole from the **+** on the bottom bar, and paste this:

```sh
curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
```

It restarts SteamVR at the end, so don't run it in the middle of a game. If you use FrameDrop, the **Install with FrameDrop** link in the [README](https://github.com/RxVreality/UTMTFTM#steam-frame) does the same.

What I've tried this version on: on Windows, my PC, with my Quest 3 over Virtual Desktop. I ran the stick check on my own worn Quest 3 sticks, and this version's hold was tuned on recordings of those checks: replayed, it keeps most of the big jumps away from the game, though not every one. I haven't played a long session with **Fix drift** on in this version yet, so I'd love to hear how it does on your sticks. The Frame version passed the automated tests on an ARM64 machine and in a simulated SteamVR, but I haven't put 0.6.4 on my Frame. For the Frame nothing changes that you'd notice: it still has no stick drift.

**Hands dropping to the floor ([#5](https://github.com/RxVreality/UTMTFTM/issues/5), [#3](https://github.com/RxVreality/UTMTFTM/issues/3)).** A couple of people have had their hands in the game drop to the floor or vanish while placing them, in Half-Life: Alyx and Beanstalker, streaming to a PC. Nothing in 0.6.4 is aimed at that directly. But I've played Half-Life: Alyx on this version in PC VR on different headsets, a Steam Frame streaming to my PC among them, and I haven't been able to make it happen. So it may come down to something in those setups that I don't have. Hopefully it's gone for you too, but I can't promise that. If it still happens, please add a comment on #5 with your logs, your headset and how it connects to the PC. Both issues stay open until we hear back.

The Windows version stays a beta. There's more in between on a PC than on the Frame, like streaming apps and lots of different headsets, and I've used it less.

It's the purple hand in the dashboard on Windows, and the yellow one on a Frame. The full list of changes is in the [changelog](https://github.com/RxVreality/UTMTFTM/blob/main/CHANGELOG.md). If something goes wrong, please open an issue and attach the logs.
