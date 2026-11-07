# How it works

There are two parts.

**The overlay** (`utmtftm.exe`, or `utmtftm` on the Frame) is the dashboard page and the grab-to-place. SteamVR starts it along with itself. It sends offsets to the driver through a small shared file, `live.shm`, in `%LOCALAPPDATA%\UTMTFTM` or `~/.local/share/UTMTFTM`.

**The driver add-on** (`driver_utmtftm.dll`, or `driver_utmtftm.so` on the Frame) runs inside SteamVR and has no devices of its own. It hooks `IVRServerDriverHost::TrackedDevicePoseUpdated`, the call every SteamVR driver reports poses through, and moves the two hand controllers before anything else sees them. Every game then gets the moved controller, whichever API it uses, and nothing is loaded inside the game. Anything a game attaches to the controller moves with it.

Every driver that ships with SteamVR asks for `IVRServerDriverHost_006`, and so do Virtual Desktop's and `driver_cv`, which runs the Frame's headset and controllers. The add-on also hooks versions 004 and 005 for older drivers.

## The maths

Games attach their hands to the grip pose, so that's the frame an offset `O` is saved in: `grip' = grip * O`. The driver works on the device's raw pose, and `grip = raw * C`, where `C` is where the grip sits on that controller. So the device has to move by `C * O * C^-1`.

The driver applies that as the last transform in the pose: `world * pose * driverFromHead * offset`. SteamVR's prediction and velocities carry on as normal.

## Where the grip comes from

`C` comes from the render model SteamVR reports for the controller. SteamVR looks for an `openxr_grip` component, then `grip`, then `handgrip`, so UTMTFTM does the same, because Index and Vive wands don't have the first one.

It uses `IVRRenderModels::GetComponentState` rather than `GetComponentStateForDevicePath`, which answers no unless that controller is switched on at that moment.

The picture on the dashboard is the same model, built from its parts the way SteamVR draws it. The whole-model files can't be relied on: Pico and Focus 3 won't load whole, and the right Quest 2 file has a solid disc across its tracking ring.

## Never measure from moved poses

This caused the worst bug so far.

Once an offset is published, every pose SteamVR reports has that offset in it, a few updates late. If you measure anything from those poses and feed it into the next offset, it comes back larger each time. In the headset that looked like a shaking hand, which got flung out of sight after a couple of seconds, with NaNs in the log.

So while you're placing a hand, the driver also reports where each controller really is, before any offset, and the overlay works from only that. `test_session` runs this loop against a pretend SteamVR with late, mismatched poses.

Two checks stop a bad number getting far. The overlay ignores any offset that's broken or more than a metre out and keeps the last good one, and the driver won't pass on a broken offset at all.

## Cost

The hook runs at the tracking rate for every device, so that path has no allocation, file access or locks. Unchanged offsets come from a per-thread cache, checked against a sequence number in the shared file. `test_driver` measures it each run: about 10 ns for other devices, 20 ns for a hand with an offset, 45 ns while placing.

The overlay wakes ten times a second in the background and does almost nothing, and draws only when the dashboard is open or you're placing a hand.

## If the driver isn't running

The overlay falls back to writing SteamVR's own `device_offsets` into a binding file for the game. That only takes effect the next time the game launches, which is what "Applies next launch" on the page means. In practice you only see it if the add-on failed to load.

The driver counts the poses it sees for each hand. If it's running and a controller is on but it hasn't heard from either hand in 10 seconds, those poses are reaching SteamVR some way the hook can't see, so the overlay falls back the same way rather than claiming to be live, and says so in the log.

## On the Steam Frame

The Frame runs SteamVR natively on ARM64 Linux, from `/opt/steamvr`, so this is the same program built for that, and its controllers' poses come through the same hook. The differences are small:

- Overlay images go up as Vulkan textures rather than D3D11 ones.
- The text is Barlow, which ships with the program, because Linux has no Bahnschrift and a headset's fonts aren't something to count on.
- It uses the OpenVR loader SteamVR keeps at `/opt/steamvr/bin/linuxarm64`. The prebuilt ARM64 one in Valve's SDK looks for its settings in `/data/work`, an Android path, so it can't find SteamVR on a Frame.
- cairo, FreeType and Vulkan come from SteamOS, and everything else is built into the program. It only needs glibc 2.34, so it isn't tied to one SteamOS version.
- The Frame's SteamVR has the hand's skeleton and animations but no glove model, and that model isn't ours to ship. So `src/handskin.h` wraps a smooth skin around SteamVR's bones, and the hand on the page ends up with the same shape as on Windows.
- It's also listed in the dashboard's **+** menu, which shows whatever is in `~/.local/share/applications`. If it's already running, that just opens its page.

ARM also doesn't keep memory writes in order the way x86 does. The offsets in the shared file are guarded by a sequence number, and without an extra fence a reader on another core could take a half-written offset for a finished one. x86 hides that mistake, so it never showed up on Windows. `test_driver` writes and reads flat out on separate threads, and that test runs on ARM64 as well.

When a Frame streams from a PC, it runs Steam Link as its scene app, `vrlink.client`. Offsets only apply when the scene is a game, so UTMTFTM on the Frame stays out of the way of UTMTFTM on the PC.

## The bindings page

It's all read from files SteamVR already keeps, and most of the work is in `src/bindings.cpp`, which makes no VR calls.

- A controller's buttons come from its input profile, `*_profile.json`, in SteamVR's driver folders.
- Which controllers you can compare with is whichever of those have a model on disk. The Frame's SteamVR has the Index and Quest families, the PC's has those plus Vive, Pico and Focus 3. The picture is the controller's own render model, loaded through SteamVR in parts. Each line ends on the middle of what can be seen of that part. The Frame's D-pad is one part, so its four directions come from the buttons they mirror on the right controller.
- A game's bindings come from wherever SteamVR got them, in this order: the binding it's set to use for that game, what an OpenXR game asked for last time it ran (`config/openxr/<app>_<controller>_<n>_binding.json`, rewritten at each launch), bindings SteamVR ships for that game, and the game's own action file, which Steam lists in `steamapps.vrmanifest`.
- A game's names for its actions are in its action file. OpenXR games have no such file on disk, and Unity's and Unreal's plugins name their actions after the input anyway, like `oculustouch_right_a_click`, so those files can't say what a button does. For games like that there's `input/game_labels.json`, below.

### Game labels

`input/game_labels.json` lists what the buttons do in games that don't tell SteamVR. Each game says which controller its labels are for, what kind of source they came from and where to find it, and the page's heading says the kind. A game with mixed sources is filed under its weakest. A game's own words always win where it has any.

Some of it comes from the games themselves: Unity's Input System and older Unreal settings keep action names in plain text, like `Jump` on `<XRController>{RightHand}/primaryButton`, sometimes with the Frame listed separately. I read those from my own copies and wrote the labels from them, and encrypted packages stayed closed. The add-on doesn't read any game's files. The rest came from developers' guides and reviews, kept only where a source named the button. A source often describes another controller or doesn't name one, usually for something every controller has, like the trigger, so those are filed under Quest and `source_controller` says what it really described. Lots of games had nothing solid, and they're just not in the table.

A label moves to another controller only when the bindings prove it's the same action. Your button's action has to appear on a button of the controller the source described, on the same hand. That's what happens when SteamVR translates a game's Quest bindings for your Frame. Games can also give another controller its own layout, and in about a third of the games on my PC that name their actions, the same button did something different on Quest and Index. Unreal even names each controller's actions differently. So a button with the same name proves nothing, and it stays blank unless the table describes that controller too.

A button the game's binding doesn't have stays **Not used**, whatever a source says. These are default controls, so a setting, a left-handed mode or an update can change them. `headset_checked` in the table says whether anyone has confirmed that game's labels in the game.

What a line ends on comes from the model too. The picture is drawn once from in front and to the outside, so the trigger and grip show as well as the top, and each part's line goes to the middle of whatever can be seen of it. A hollow mark means the part is round the back from that angle.

The translation starts from SteamVR's own, `*_remapping.json`, read backwards to say which of their buttons each of yours acts as. When you point at a button, its match depends on what's known. If the game names its actions, it's the button with the same action on the other controller, on the same hand if there's one there. Otherwise, and when the game is on a layout, it's the button that's wired to it. Anything you rewire goes on top of that. Applying it copies the game's binding for the other controller, moves each input to your button and changes how it reads where it has to: a Touch trigger on a Frame bumper can't half-press, so it becomes a full pull. SteamVR leaves its last automatic translation in `auto-remapping_toBinding.json`, and `test_bindings` checks ours comes out the same.

The result is a binding file in `bindings/` in the data folder, with its `controller_type` set to yours and its `interaction_profile` left as theirs, so an OpenXR game goes on thinking it's talking to the other controller. Games written for SteamVR Input get `simulated_controller_type` instead, the same as SteamVR's own translation writes. It goes live through `<controller>_250820_CurrentURL_<category>` in that game's section of SteamVR's settings, the same way the offset fallback does, and SteamVR reads that when the game starts. Going back restores whatever was there.

None of it runs unless the page is open, except a string compare once a second, and on a game's start it applies a saved layout if there's one to apply. `test_bindings` prints what it costs. On my PC that's about 15 ms to read the controller files, 40 ms to find a game's bindings and 65 ms to draw the four controllers the first time, then about 30 ms to redraw the page when the laser moves.

## Combos

A combo is a chord in the game's binding file, the same thing SteamVR's own binding screen writes:

```json
{"output": "/actions/main/in/Jump", "inputs": [["/user/hand/right/input/a", "touch"], ["/user/hand/right/input/b", "touch"]]}
```

SteamVR does all the work, so nothing of ours runs while you play. VRChat players build these by hand, because touching a control doesn't press it, and that leaves room on a full controller. One control on its own goes in as an ordinary binding of its touch or press.

The chord goes in the set its action belongs to, on top of whatever that game's on: the developer's binding exactly as it was, a layout from the Bindings tab, or the translation SteamVR would have made. Each control in it gets a source in that set if it hasn't got one, with nothing on it. Every chord I could find on disk sits on a control that has one, and SteamVR's own files have empty ones. Removing the last combo puts the game back on what it had.

What it can set off is whatever's on-or-off: the game's action file if it has one, which also lists actions with no button at all, and otherwise what's in its binding. Sticks and half-pulls can't be set off by a combo, so they aren't on the list.

It goes live the same way a layout does, so it takes effect the next time the game launches. SteamVR doesn't say whether a chord stops its parts doing what they normally do, so the page warns about pressing a control that's already in use.

## Fine adjust

Each arrow changes one of the six numbers and saves it, like anything else on that page: `nudged()` in `src/placement.h`. Distance keeps to the limit unless the hand's already past it and the tap brings it closer. Rotation has no limit and wraps at 180.

Normally both hands stand aside while the dashboard's open, so its pointers point true. While you're fine adjusting, the hand that isn't holding the laser stays moved, which is what lets you watch it. `GetPrimaryDashboardDevice` says which that is, and if it says neither, both stand aside as usual. Whether you can see the hand behind the dashboard is up to the game.

## Buttons

The driver also hooks `IVRDriverInput`, the call every controller driver reports its buttons through. Valve's own drivers use `IVRDriverInput_005`, which isn't in the public SDK, but its first four functions line up with the SDK's `_003`, and those are the only ones hooked. To see them being set up and learn their names, the add-on has to load before the controllers do, so its settings ask for a high `loadPriority`.

This is what makes two things possible that SteamVR's bindings can't do:

- **Macros.** SteamVR's chords go many to one: hold these and that happens. A macro goes one to many: press this and a whole sequence plays, with its own timing.
- **Changes that work right away.** A running game keeps the bindings it launched with. If your A should now do what's on the game's B, the driver can send your A's press as B's, so it works without a restart. If nothing on the controller has that action right now, there's no press to send, so that waits for the next launch.

A VR button isn't one thing the way a mouse button is: there's a thumb resting on it, the press itself, and how far a trigger is pulled, each reported separately. Recording folds those into one step, and playing that step sets all of them, so a game reading any of them sees a press.

A controller also reports things that aren't controls. The Frame's `driver_cv` sends its raw hand sensor, capsense, with an `x` like a stick's, and it reads as pushed for as long as you're holding the controller. So recording only takes what the controller's input profile lists, and only takes `x` and `y` from a joystick or trackpad. Anything already down in the first 150 ms of listening was down before it started, like the grip you're holding on with, and isn't a step. A press only stops recording finishing if it's a button or a stick: a grip or trigger you're still squeezing doesn't.

Since a bug here could stick a button in every game, it's kept simple. The hook itself only remembers what the controller said, and passes it on unless that button's been taken over. Everything else runs in `RunFrame`, SteamVR's own loop for drivers, so there's no new thread. It lets go of anything it's holding, and tells SteamVR what your hands are really doing, whenever the dashboard opens, a hand's being placed, the saved table changes, or SteamVR shuts down. While recording, buttons are held back from SteamVR so the dashboard doesn't get them, but the overlay has to keep asking every second, so if it dies they come straight back. The Steam button is never held back.

In the tests the hook adds too little to measure against a fake's own lock and list, around a nanosecond either way, per button report, with nothing recorded on that button.

## See-through

The dream is a window around each hand showing the real room, with the game everywhere else. SteamVR doesn't allow it on the Frame:

- Passthrough is drawn by SteamVR's compositor, across the whole view. Overlays can only be drawn on top of it, with ordinary alpha, so they can't cut a hole through to it.
- `IVRTrackedCamera`, the public way to get the camera picture, gives nothing on the Frame, and asking for it can black out passthrough until it's toggled. That was measured by [frame-autopass](https://github.com/bod09/frame-autopass), and I haven't repeated it, because it would interrupt someone's view to find out.

So on the Frame, while you place a hand, this does the next best thing with what's public. It sets `camera/roomViewStyle` to translucent and double-presses the Steam button through the button hook, which is Room View's shortcut. Then a big dark overlay, drawn per eye like the ghost, covers everything except circles around your hands. Inside them you can see your real hands and the in-game ones together, and that's where your eye goes. All of it goes back how it was when you finish.

SteamVR doesn't say whether Room View is on, so that's a guess from `VREvent_CameraSettingsHaveChanged`, which fires when it's toggled. If it guesses wrong you get the opposite of what you wanted for one placement, and double-pressing the Steam button fixes it.

On a PC streaming to a Frame, the headset's own compositor adds the passthrough after the PC's picture arrives, so a dark overlay from the PC couldn't do this. There the panel just says how to turn it on.

Virtual Desktop is different, because it can turn one colour into the real room after the picture reaches the headset. Anything drawn in that exact colour is a real window, which is what the dream was. So when `VirtualDesktop.Server.exe` is running, the See-through button appears, off to begin with because without Virtual Desktop's own setting all you'd get is pink circles. Tapping it draws the globes in pure magenta, red 255, green 0, blue 255, on a plane just big enough to hold them, so the pixels stay fine and the edge can be hard. A soft edge would be neither the colour nor the game.

The catch is that an overlay sits on top of the game, so a solid window hides the in-game hand at the moment you're trying to line it up. Tapping again draws stripes instead, whole rows of pixels on and off. They're level so they look the same from both eyes, and you get the room in one and the game in the next. Stripes rather than a see-through colour because a colour that's half magenta is at the mercy of how Virtual Desktop's slider happens to be set.

## Tests

None of these need a headset or SteamVR. Run them with `build.cmd`, or `build.sh` on ARM64 Linux.

- `test_driver` loads the real driver library the way SteamVR does and sends poses through its hook from a fake host: which devices move, the maths, live updates, torn reads, cost per update and clean removal. It also drags a hand end to end with the offset arriving up to 12 updates late.
- `test_session` covers placing a hand: the ghost and offset maths, what the driver gets handed, saving and loading.
- `test_ghost` covers the 3D drawing and rotation freedom.
- `test_ui` renders every screen to `build\ui\*.png`, so you can look at them without a headset.
- `test_controllers` checks each binding file against SteamVR's own description of that controller, if SteamVR is installed: the button exists, it's on that hand, every stick and pad is blocked while placing, and the panel names the button that's really bound.
- `test_bindings` covers the bindings page against a small made-up SteamVR, including what it writes and what it puts back. If SteamVR's installed it does the same against the real files: every controller's model and buttons, the real games on that machine, and a comparison with SteamVR's own translation. It also renders the page to `build\ui`.
- `tests/test_frame_run.sh` is for the Frame build. It installs the packed download into a throwaway home folder and runs it against a simulated SteamVR (`tests/fake_steamvr.cpp`), which clicks Move, drags a hand and checks what comes out of the real driver. It also saves the pictures the program really sent as Vulkan textures. GitHub runs it on an ARM64 machine for every push. It shows the program works, not that the real SteamVR does what the simulated one does.

`dist\bind_sheet.exe <folder> [app key ...]` needs SteamVR running. It draws the bindings page from what the real SteamVR says, through the same calls the program makes, and only reads.

`dist\glove_sheet.exe <folder>` needs SteamVR installed, and starts its server if it isn't running. It loads every hand controller SteamVR can draw, the same way the overlay does, and writes `controllers.png` with the hand on each one. Go by the picture rather than its finger-depth numbers, because controller models are made of overlapping shells.

## Not tested yet

- A macro on any controller other than the Steam Frame's. On 7 November one played in Titan Isles on a real Frame, so `driver_cv` does report through `IVRDriverInput` and sets its controllers up after the add-on loads. `tests/real_steamvr/run.ps1` runs the hook against the real SteamVR on Windows with its null headset and a pretend controller, which shows the calls line up for the rest.
- Room View on the Frame. In the first try the dim appeared and Room View didn't come on, and why isn't known yet. So how the dim looks with the room behind it hasn't been seen either.
- Virtual Desktop's window in a headset: whether its colour setting takes the overlay as cleanly as `test_ghost`'s numbers suggest, what video compression does to the edge and the stripes, and whether stripes are any use.

- Any controller other than the Steam Frame's and the Quest 3's on real hardware.
- The game ignoring buttons while you place a hand. UTMTFTM asks for that with `k_nActionSetOverlayGlobalPriorityMin`.
- Any layout from the bindings page in a real game. What's written matches SteamVR's own translation and uses the settings route that's worked for offsets, but only a game can show it's right.
- A combo in a real game, and whether SteamVR's chords work the same for OpenXR games as for SteamVR Input ones.
- Fine adjust in a headset: which hand SteamVR says is holding the laser, and whether a game keeps drawing the hand behind the dashboard.
- Most of `input/game_labels.json`. I've checked Pavlov and Titan Isles on my Frame, and `headset_checked` says so for each game.
- Index's squeeze threshold. Its grip reads as fully closed just from holding the controller, so the binding goes by squeeze force. 0.6 is a judgement call.

## Known limits

- With the dashboard open or no game running, nothing is moved, so SteamVR's own pointers come from the real controller.
- Switching the add-on off in Manage Add-ons takes effect right away: the overlay closes, its tab goes and offsets stop. SteamVR still starts the program at each launch, and it exits straight away without showing anything. Switching back on needs a SteamVR restart, like any add-on.
- If you kill the overlay by hand, the driver keeps the last offsets it was given until SteamVR restarts.
