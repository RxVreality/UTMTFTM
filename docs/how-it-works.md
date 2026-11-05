# How it works

There are two parts.

**The overlay** (`utmtftm.exe`) is the dashboard page and the grab-to-place. SteamVR starts it along with itself. It sends offsets to the driver through a small shared file, `%LOCALAPPDATA%\UTMTFTM\live.shm`.

**The driver add-on** (`driver_utmtftm.dll`) runs inside SteamVR and has no devices of its own. It hooks `IVRServerDriverHost::TrackedDevicePoseUpdated`, the call every SteamVR driver reports poses through, and moves the two hand controllers before anything else sees them. Every game then gets the moved controller, whichever API it uses, and nothing is loaded inside the game. Anything a game attaches to the controller moves with it.

Every driver that ships with SteamVR asks for `IVRServerDriverHost_006`, and so does Virtual Desktop's. The add-on also hooks versions 004 and 005 for older drivers.

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

## Tests

None of these need a headset or SteamVR. Run them with `build.cmd`.

- `test_driver` loads the real driver DLL the way SteamVR does and sends poses through its hook from a fake host: which devices move, the maths, live updates, torn reads, cost per update and clean removal. It also drags a hand end to end with the offset arriving up to 12 updates late.
- `test_session` covers placing a hand: the ghost and offset maths, what the driver gets handed, saving and loading.
- `test_ghost` covers the 3D drawing and rotation freedom.
- `test_ui` renders every screen to `build\ui\*.png`, so you can look at them without a headset.
- `test_controllers` checks each binding file against SteamVR's own description of that controller, if SteamVR is installed: the button exists, it's on that hand, every stick and pad is blocked while placing, and the panel names the button that's really bound.

`dist\glove_sheet.exe <folder>` needs SteamVR installed, and starts its server if it isn't running. It loads every hand controller SteamVR can draw, the same way the overlay does, and writes `controllers.png` with the hand on each one. Go by the picture rather than its finger-depth numbers, because controller models are made of overlapping shells.

## Not tested yet

- Any controller other than the Steam Frame's and the Quest 3's on real hardware.
- The game ignoring buttons while you place a hand. UTMTFTM asks for that with `k_nActionSetOverlayGlobalPriorityMin`.
- Index's squeeze threshold. Its grip reads as fully closed just from holding the controller, so the binding goes by squeeze force. 0.6 is a judgement call.

## Known limits

- With the dashboard open or no game running, nothing is moved, so SteamVR's own pointers come from the real controller.
- Switching the add-on off in Manage Add-ons takes effect right away: the overlay closes, its tab goes and offsets stop. SteamVR still starts `utmtftm.exe` at each launch, and it exits straight away without showing anything. Switching back on needs a SteamVR restart, like any add-on.
- If you kill the overlay by hand, the driver keeps the last offsets it was given until SteamVR restarts.
