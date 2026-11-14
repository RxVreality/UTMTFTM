The **Bindings** and **Combos** tabs now start with a simple mode that asks one thing at a time, on a picture of your own controllers. On Bindings every button flashes: tap the one you want to change, then tap the job you'd like it to have on another button, and the two swap. Combos asks whether you want a combo or a macro and takes you through either. **Advanced**, at the top right, opens the full pages from before.

Most changes should now work without restarting the game, including swaps, stick directions and combos. A few still wait for the next launch, like giving a button a job that wasn't on any button, and the page tells you which. Swaps are done by the add-on, because games like Titan Isles tie a job to one hand and a binding file can't move it to the other.

**Decaffeinate** is new, on its own **Decaf** tab. It takes the little shakes out of your hands while you hold them still, like when you're aiming at something far away, and should let go the moment you move. It's off until you turn it on. I've only had a quick try of Maximum, in Pavlov Shack and Titan Isles. It felt good, but I haven't tried the other steps in a game, so I'd really like to hear how it feels for you.

@MMeNDtal asked for stick directions on the Bindings tab, gave me the idea for Decaffeinate, and co-authored the Combos tab. Thank you.

**Tested on my Steam Frame only.** I haven't tried this version on PC VR yet, so if something's off there, please open an issue.

**On a Frame:** turn on Developer Mode in Steam's System settings, open Konsole from the **+** on the bottom bar, and paste this:

```sh
curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
```

It restarts SteamVR at the end, so don't run it in the middle of a game. If you use FrameDrop, the **Install with FrameDrop** link in the [README](https://github.com/RxVreality/UTMTFTM#steam-frame) does the same.

**On Windows:** close SteamVR, run `UTMTFTM-Setup` and start SteamVR again. The installer isn't code-signed, so Windows may warn you: click **More info**, then **Run anyway**. Use this one if you stream to a Frame from your PC.

Either way, it's the yellow hand in the dashboard. The full list of changes is in the [changelog](https://github.com/RxVreality/UTMTFTM/blob/main/CHANGELOG.md). If something goes wrong, please open an issue and attach the logs.
