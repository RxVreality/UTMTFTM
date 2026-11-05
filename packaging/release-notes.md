There's a **Bindings** tab now. It shows your controllers under any other controller you pick, with a line from each button to its spot. If the game says what its buttons do, you see that too. You can put a game on another controller's layout or change single buttons. I wrote it because I find Steam's binding screen hard to use, and there are pictures and the details in the [README](https://github.com/RxVreality/UTMTFTM#bindings).

I've used the page on my Frame, but I haven't played a game on a translated layout yet, so if one misbehaves, tap **Developer's** on that tab to put the game back, restart it and please open an issue.

On a Frame, the hand on the page now looks like the Windows one. It used to come out mangled.

**On a Frame:** turn on Developer Mode in Steam's System settings, open Konsole from the **+** on the bottom bar, and paste this:

```sh
curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
```

It restarts SteamVR at the end, so don't run it in the middle of a game.

**On Windows:** close SteamVR, run `UTMTFTM-Setup` and start SteamVR again. The installer isn't code-signed, so Windows may warn you: click **More info**, then **Run anyway**. Use this one if you stream to a Frame from your PC.

Either way, it's the yellow hand in the dashboard. If something goes wrong, please open an issue and attach the logs.
