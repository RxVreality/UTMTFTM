Mostly fixes, from @MMeNDtal's testing of 0.6.0 on PC VR (#2). Thank you.

- Placing a hand while streaming, like with Virtual Desktop, no longer leaves the dragged hand trailing behind, and the instructions sit above the bubble instead of inside it.
- On Windows, the menu no longer lags in heavy games like Pavlov, and a placed hand no longer sticks to the other one after you let go.
- The Steam Frame's D-pad left and right no longer point at each other's arm on the Bindings pictures.
- The **Guided** button no longer covers the last controller on the advanced Bindings page.
- Setting one way of a stick to **Nothing** now really does nothing in the game. The add-on holds that way at the middle while you play.
- A macro recorded on the advanced Macros page takes you back to choose its button, not to the Combos page.
- The Decaf tab says why the hand pointing at the menu isn't steadied (so the laser stays sharp).

**Strength** on the Decaf tab, also MMeNDtal's idea: a slider from 25% to 150% of the step you picked. If Tuned pushes back on small aiming corrections, try 50%.

Some games give your hands weight of their own or smooth your aim themselves, and Decaf adds to that. The Decaf page now says when you're in one of them, and starts lighter where the sources are solid.

**From this version on, UTMTFTM updates itself.** When there's a newer version, the bottom of the page shows an **Update** button. It installs while you play and starts with SteamVR next time. This one you'll install by hand, as before.

Some changes still need a game restart. SteamVR can reload a game's buttons while it runs, but only for its own screens, behind a password it doesn't share, so the page says when you need one. The README explains more.

I've tried this version on my PC with a Quest 3 over Virtual Desktop, in Pavlov: placing hands, the menu pages and Decaf. I haven't tried it on a Frame yet.

**On a Frame:** turn on Developer Mode in Steam's System settings, open Konsole from the **+** on the bottom bar, and paste this:

```sh
curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
```

It restarts SteamVR at the end, so don't run it in the middle of a game. If you use FrameDrop, the **Install with FrameDrop** link in the [README](https://github.com/RxVreality/UTMTFTM#steam-frame) does the same.

**On Windows (beta):** close SteamVR, run `UTMTFTM-Setup` and start SteamVR again. The Windows version is a beta: there's more between your hands and the game on a PC, so expect a few rough edges, and please tell me about them. The installer isn't code-signed, so Windows may warn you: click **More info**, then **Run anyway**. Use this one if you stream to a Frame from your PC.

Either way, it's the yellow hand in the dashboard. The full list of changes is in the [changelog](https://github.com/RxVreality/UTMTFTM/blob/main/CHANGELOG.md). If something goes wrong, please open an issue and attach the logs.
