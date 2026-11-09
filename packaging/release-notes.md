**Macros** are new, under **Advanced** on the **Combos** tab. One button plays back a whole sequence you record yourself, like a Logitech mouse macro. Tap **Record new** and close the dashboard. A sign in front of you counts down, then shows what it's heard while you do your sequence, and the dashboard opens again when it's done. Then choose the button that plays it. Each press is one tile you can move or delete, and it works without restarting the game. I've used macros on my Frame and from my PC over Steam Link.

[MMeNDtal](https://github.com/MMeNDtal) co-authored the Combos tab, both combos and macros. They asked for macros, explained how they should work and found the bug where a tap on the page didn't show until the laser moved. Thank you.

**Not tested in a game yet:** rewiring one button on the Bindings tab, or adding a combo whose action is already on a button, now takes effect without restarting the game. Anything else, like switching to another controller's layout, still waits for the next launch, and the page says which you got. If one doesn't take, restart the game and please tell me.

Also fixed: tapping something on the page often showed nothing until the laser moved or you tapped again.

**On a Frame:** turn on Developer Mode in Steam's System settings, open Konsole from the **+** on the bottom bar, and paste this:

```sh
curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
```

It restarts SteamVR at the end, so don't run it in the middle of a game. If you use FrameDrop, the **Install with FrameDrop** link in the [README](https://github.com/RxVreality/UTMTFTM#steam-frame) does the same.

**On Windows:** close SteamVR, run `UTMTFTM-Setup` and start SteamVR again. The installer isn't code-signed, so Windows may warn you: click **More info**, then **Run anyway**. Use this one if you stream to a Frame from your PC.

Either way, it's the yellow hand in the dashboard. If something goes wrong, please open an issue and attach the logs.
