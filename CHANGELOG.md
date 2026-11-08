# Changelog

## Not released yet

Macros, under Advanced on the Combos tab: one button plays back a sequence you did with your own hands, the way a Logitech mouse macro does. Press Record, do it, then pick the button. Each press is one tile, which you can move around or delete, and it works with no game restart. It plays at the pace you did it unless you pick Quick. There's an Interview switch that asks one question at a time instead, and a Capacitive touches switch for when resting a thumb should count too. Thanks to u/MMeNDtal, who asked for this.

Rewiring a button now works straight away in the game you're playing, when it only means sending one button's press as another's. So does a new combo, when what it sets off is already on a button. Anything else, like switching to another controller's layout, still waits for the next launch, and the page says which you got.

Fixed: tapping something on the page often showed nothing until the laser moved or you tapped again, and pointing at a button could light up the one you'd been on before.

The driver add-on has to be there from the moment SteamVR starts to hear your buttons, so this version needs one SteamVR restart after installing.

## 0.4.0

A Combos tab: pick controls on a picture of your controllers, tap once for touch or twice for press, then pick what they set off in the game you're in. It's what SteamVR's own binding screen makes, without the binding screen. Touching doesn't press, so a combo of touches won't set off what's already on those buttons. New combos start working the next time the game launches. This is the newest part and it hasn't had much testing, so please tell me how it goes.

Fine adjust on the Hands tab: arrows either side of each number, for when a hand's nearly right and you'd like it 1 degree to the left. They go 1 mm and 1 degree a tap, or 1 cm and 5 degrees. The hand you aren't pointing with moves in the game as you tap, with the dashboard still open. It's new and hasn't had much use yet.

Yaw and roll now read positive to the right, so all six numbers go the way their names say. Saved offsets don't change, only how they read.

On the Bindings tab, pointing at a button now lights up the one that does the same thing on the other controller, in games that name their buttons. In No Man's Sky, Jump on your Frame's A used to light up the Quest's A, and now it lights up the Quest's B.

The Bindings tab now says what buttons do in 47 games that don't tell SteamVR, like Pavlov, Forefront and Titan Isles. I looked them up, from the game's own files where I could and otherwise from developers' guides and reviews, and the top of the page says which. A label only moves to another controller if the game's own bindings prove it's the same action, so you'll see some blanks on purpose. The Combos tab uses the same words. I've checked Pavlov and Titan Isles in the game on my Frame, and the rest haven't been confirmed in a headset.

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
