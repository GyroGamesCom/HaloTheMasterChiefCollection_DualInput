# Dual Input for Halo: The Master Chief Collection

Gyro aiming and gamepad at the same time. Gyro moves the camera like a real mouse
(linear, no stick curve), and the game keeps showing gamepad buttons.

> **Offline only. The game must run with Easy Anti-Cheat OFF.**
> Start it with **Play without Anti-Cheat**. With EAC on, the mod does not load.

## What it does

- Gyro goes straight into the camera as native mouse look.
- The gamepad stays the active device (gamepad prompts).
- The right stick/trackpad has priority: while you push it, gyro is ignored.
- **F10** turns the mod on/off in game.

## Install

1. Close the game. Run `install.bat`.
2. Steam Input (controller layout for the game):
   - Gyro: **As Mouse**
   - Right trackpad / right stick: **Joystick**
3. Start the game with **Play without Anti-Cheat**.

**Manual install:** copy `xinput1_3.dll` and `xinput_mouse.ini` from `mod` to
`Halo The Master Chief Collection\MCC\Binaries\Win64\`.

## Uninstall

Run `uninstall.bat`, or delete `xinput1_3.dll`, `xinput_mouse.ini` and `mixed_input*.log`
from that folder.

## Settings

`xinput_mouse.ini` next to the DLL. Restart the game after a change.

| Key | Default | |
|---|---|---|
| `GyroSensitivity` | 1.0 | Gyro speed (on top of the in-game mouse sensitivity) |
| `InvertX` / `InvertY` | 0 | 1 = flip that gyro axis |
| `StickOverrideDeadzone` | 0.27 | How far the right stick must move to override gyro |
| `Enabled` | 1 | 0 = mod off |

## Problems

- **No effect:** check that the game runs without Anti-Cheat. After starting,
  `mixed_input.log` must appear next to the DLL.
- **Game updated, mod stopped working:** the mod turns itself off when the game code
  changes. Wait for an update.
- Gyro does not move the menu cursor; use the gamepad in menus.
- Tested on the Steam version, build 2025.08.16.178512, Halo 2.

Source code is in `src` (build with `src\build.bat`, MSVC x64).

Unofficial fan mod, not affiliated with Microsoft, Halo Studios or Valve.
Use at your own risk. License: `LICENSE.txt`.
