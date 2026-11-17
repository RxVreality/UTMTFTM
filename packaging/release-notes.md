A small one, and only the Steam Frame version for now. The Windows version of 0.6.2 follows once I've tried it myself.

- On a Frame, UTMTFTM starts while the headset boots, before its Wi-Fi is back, so its first look for a new version failed and the next one was six hours later. A look that fails now tries again after a minute, then after 2, 5 and 15 minutes, then every 30 until it gets through.

**Updating from 0.6.1 on a Frame:** this is the first version the **Update** button can install. Tap it at the bottom of the page. It installs while you play, and 0.6.2 starts the next time SteamVR or the headset does. If the button isn't there yet, that's the bug above: close UTMTFTM from its page and open it again from the **+** on the bottom bar, and it looks again.

**Installing it fresh on a Frame:** turn on Developer Mode in Steam's System settings, open Konsole from the **+** on the bottom bar, and paste this:

```sh
curl -fsSL https://github.com/RxVreality/UTMTFTM/releases/latest/download/install-frame.sh | sh
```

It restarts SteamVR at the end, so don't run it in the middle of a game. If you use FrameDrop, the **Install with FrameDrop** link in the [README](https://github.com/RxVreality/UTMTFTM#steam-frame) does the same.

**On Windows (beta):** for now, get 0.6.1 from [its release page](https://github.com/RxVreality/UTMTFTM/releases/tag/v0.6.1). If 0.6.1 on Windows offers you an update to 0.6.2 in the meantime, it'll say there's no Windows installer yet, which is expected until it's out. The Windows side of 0.6.2 is the bigger change: after @MMeNDtal got an antivirus warning on 0.6.1, the Windows version only asks GitHub which version is newest and leaves the download to your web browser, with nothing running out of sight. I want to try that on my own PC before it goes out.

I've tried this version on my Steam Frame, running natively on the headset: it installed and started with SteamVR running, found the add-on, and I went through the menu pages. I haven't played a game on it yet, and I haven't watched the retry after a slow Wi-Fi start happen on the headset; the tests cover that part.

It's the yellow hand in the dashboard. The full list of changes is in the [changelog](https://github.com/RxVreality/UTMTFTM/blob/main/CHANGELOG.md). If something goes wrong, please open an issue and attach the logs.
