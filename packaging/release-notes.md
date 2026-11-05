The buttons on the **Bindings** tab now say what they do in 47 games that don't tell SteamVR, like Pavlov, Forefront and Titan Isles. I looked them up, from the game's own files where I could and otherwise from developers' guides and reviews, and the top of the page says which. I've checked Pavlov and Titan Isles in the game on my Frame. The rest haven't been confirmed in a headset, so if you spot a wrong one, please tell me. You'll see some blanks on purpose, where I couldn't prove a label applies to your controller.

There's a **Combos** tab too. A combo is a macro for your controller: hold two or more controls together and they do one thing in the game you're in, and just resting your thumb on a button counts. **It hasn't had much testing yet**, so expect some rough edges. A new combo starts working the next time the game launches. If one misbehaves, tap **Remove**, restart the game and please open an issue with the game's name. There's more in the [README](https://github.com/RxVreality/UTMTFTM#combos).

**Fine adjust** on the Hands tab puts arrows next to each number, for when a hand's nearly right and you'd like it 1 degree to the left. This one's new and hasn't had much use yet.

Two smaller changes. Pointing at a button on the Bindings tab now lights up the one that does the same thing on the other controller, in games that name their buttons. Yaw and roll now read positive to the right, and saved offsets don't change, only how they read.

**On a Frame:** turn on Developer Mode in Steam's System settings, open Konsole from the **+** on the bottom bar, and paste this:

```sh
curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
```

It restarts SteamVR at the end, so don't run it in the middle of a game.

**On Windows:** close SteamVR, run `UTMTFTM-Setup` and start SteamVR again. The installer isn't code-signed, so Windows may warn you: click **More info**, then **Run anyway**. Use this one if you stream to a Frame from your PC.

Either way, it's the yellow hand in the dashboard. If something goes wrong, please open an issue and attach the logs.
