**Macros** are new, under **Advanced** on the **Combos** tab. One button plays back a whole sequence you did with your own hands, like a Logitech mouse macro. Tap **Record new**, close the dashboard, do your sequence, then choose the button that plays it. A sign in front of you counts down and shows what it's heard, and the dashboard opens again when it's done. Each press is one tile you can move or delete, and it works without restarting the game. I've used macros on my Frame and from my PC over Steam Link.

The Combos tab, combos and macros both, was co-authored with [MMeNDtal](https://github.com/MMeNDtal). They asked for macros, explained how they should work and found the bug where a tap on the page didn't show until the laser moved. Thank you.

**Not tested yet:** some changes now apply straight away, with no game restart, when they only mean sending one button's press as another's. That's rewiring a single button on the Bindings tab, and a new combo whose action is already on a button. Anything else, like switching to another controller's layout, still waits for the next launch, and the page says which you got. I haven't tried the straight-away part in a game myself, so if it doesn't take, restart the game and please tell me.

Also fixed: tapping something on the page often showed nothing until the laser moved or you tapped again.

**On a Frame:** turn on Developer Mode in Steam's System settings, open Konsole from the **+** on the bottom bar, and paste this:

```sh
curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
```

It restarts SteamVR at the end, so don't run it in the middle of a game. If you use FrameDrop, the **Install with FrameDrop** link in the [README](https://github.com/RxVreality/UTMTFTM#steam-frame) does the same.

**On Windows:** close SteamVR, run `UTMTFTM-Setup` and start SteamVR again. The installer isn't code-signed, so Windows may warn you: click **More info**, then **Run anyway**. Use this one if you stream to a Frame from your PC.

Either way, it's the yellow hand in the dashboard. If something goes wrong, please open an issue and attach the logs.
