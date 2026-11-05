# Changelog

## 0.3.0

A Bindings tab: your controllers under any other one you pick, with where each button is and what it does in the game you're in. You can put a game on another controller's layout, rewire single buttons, and set either as the default for all games. It reads and writes SteamVR's own binding files, and changes apply the next time the game launches.

Changes on the Steam Frame:

- The hand on the page looks like the one on Windows now, with a smooth skin around SteamVR's own bones. It used to come out mangled.

Fixed in the fallback for games the add-on can't reach:

- It was spelling SteamVR's setting wrong for SteamVR Input games.
- On a Frame it wrote and read file paths with a slash too many or too few.

## 0.2.0

Runs on the Steam Frame itself now, for games running on the headset, as well as on Windows.

Changes on both:

- Offsets only apply in games. They used to also apply in other SteamVR scenes, like SteamVR Home, and on a Frame that would have included Steam Link.
- If the add-on is running but can't see your controllers, the page says "Applies next launch" instead of wrongly saying "Live in game".

## 0.1.0

First public version. Windows only.
