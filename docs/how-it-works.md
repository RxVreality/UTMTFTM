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
- The Frame's SteamVR has the hand's skeleton and animations but no glove model, and that model isn't ours to ship, so the page draws a plain hand of its own.
- It's also listed in the dashboard's **+** menu, which shows whatever is in `~/.local/share/applications`. If it's already running, that just opens its page.

ARM also doesn't keep memory writes in order the way x86 does. The offsets in the shared file are guarded by a sequence number, and without an extra fence a reader on another core could take a half-written offset for a finished one. x86 hides that mistake, so it never showed up on Windows. `test_driver` writes and reads flat out on separate threads, and that test runs on ARM64 as well.

When a Frame streams from a PC, it runs Steam Link as its scene app, `vrlink.client`. Offsets only apply when the scene is a game, so UTMTFTM on the Frame stays out of the way of UTMTFTM on the PC.

## The bindings page

It's all read from files SteamVR already keeps, and most of the work is in `src/bindings.cpp`, which makes no VR calls.

- A controller's buttons come from its input profile, `*_profile.json`, in SteamVR's driver folders.
- Which controllers you can compare with is whichever of those have a model on disk. The Frame's SteamVR has the Index and Quest families, the PC's has those plus Vive, Pico and Focus 3. The picture is the controller's own render model, loaded through SteamVR in parts. Each line ends on the middle of what can be seen of that part. The Frame's D-pad is one part, so its four directions come from the buttons they mirror on the right controller.
- A game's bindings come from wherever SteamVR got them, in this order: the binding it's set to use for that game, what an OpenXR game asked for last time it ran (`config/openxr/<app>_<controller>_<n>_binding.json`, rewritten at each launch), bindings SteamVR ships for that game, and the game's own action file, which Steam lists in `steamapps.vrmanifest`.
- A game's names for its actions are in its action file. OpenXR games have no such file on disk, and Unity's and Unreal's plugins name their actions after the input anyway, like `oculustouch_right_a_click`, so all the page can say is that the button's in use.

The translation starts from SteamVR's own, `*_remapping.json`, read backwards to say which of their buttons each of yours acts as. Anything you rewire goes on top of that. Applying it copies the game's binding for the other controller, moves each input to your button and changes how it reads where it has to: a Touch trigger on a Frame bumper can't half-press, so it becomes a full pull. SteamVR leaves its last automatic translation in `auto-remapping_toBinding.json`, and `test_bindings` checks ours comes out the same.

The result is a binding file in `bindings/` in the data folder, with its `controller_type` set to yours and its `interaction_profile` left as theirs, so an OpenXR game goes on thinking it's talking to the other controller. Games written for SteamVR Input get `simulated_controller_type` instead. It goes live through `<controller>_250820_CurrentURL_<category>` in that game's section of SteamVR's settings, the same way the offset fallback does, and SteamVR reads that when the game starts. Going back restores whatever was there.

None of it runs unless the page is open, except a string compare once a second, and on a game's start it applies a saved layout if there's one to apply. `test_bindings` prints what it costs. On my PC that's about 15 ms to read the controller files, 40 ms to find a game's bindings and 65 ms to draw the four controllers the first time, then about 30 ms to redraw the page when the laser moves.

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

- Any controller other than the Steam Frame's and the Quest 3's on real hardware.
- The game ignoring buttons while you place a hand. UTMTFTM asks for that with `k_nActionSetOverlayGlobalPriorityMin`.
- Any layout from the bindings page in a real game. What's written matches SteamVR's own translation and uses the settings route that's worked for offsets, but only a game can show it's right.
- Index's squeeze threshold. Its grip reads as fully closed just from holding the controller, so the binding goes by squeeze force. 0.6 is a judgement call.

## Known limits

- With the dashboard open or no game running, nothing is moved, so SteamVR's own pointers come from the real controller.
- Switching the add-on off in Manage Add-ons takes effect right away: the overlay closes, its tab goes and offsets stop. SteamVR still starts the program at each launch, and it exits straight away without showing anything. Switching back on needs a SteamVR restart, like any add-on.
- If you kill the overlay by hand, the driver keeps the last offsets it was given until SteamVR restarts.
