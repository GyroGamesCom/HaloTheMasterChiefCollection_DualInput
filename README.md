# Dual Input mod for Halo: The Master Chief Collection

Use **gyro aiming and a gamepad at the same time**, with native mouse-look feel and
gamepad button prompts.

Out of the box, MCC switches between "mouse" and "gamepad" whenever either one moves.
Steam Input sends gyro as mouse, so on Steam Deck, Steam Controller, DualSense,
Switch Pro and similar controllers you either lose the gamepad glyphs or fight the
stick response curve. This mod fixes that:

- **Gyro is fed to the game as native, linear mouse look** (no stick curve, no
  acceleration), while MCC keeps the **gamepad as the active device**, so gamepad
  button prompts/glyphs stay.
- **The right stick/trackpad wins.** Once you push it past a threshold, gyro is
  ignored until you let go, so big turns stay clean and gyro handles fine aim.
- **F10 toggles** the mod on and off at runtime.
- Everything else (left stick, buttons, triggers, vibration) passes through untouched.

Unofficial fan mod. Not affiliated with Microsoft, Xbox Game Studios, 343/Halo Studios
or Valve. Use at your own risk.

## Requirements

- Halo: The Master Chief Collection, **Steam version** (not Microsoft Store / Game Pass).
  Tested on build `2025.08.16.178512` (exe size 67179480 bytes).
- **Must be launched WITHOUT Easy Anti-Cheat.** In Steam, launch the game and choose
  **"Play without Anti-Cheat"** (or the matching launch option). With EAC running, the
  DLL is not loaded, and online play requires EAC, so this is a
  campaign / offline-only setup.
- A controller with gyro, used through **Steam Input**.

## Install

Download the latest ZIP from the [Releases](https://github.com/GyroGamesCom/MCC_DualInput/releases) page.

**Automatic:** unzip anywhere and run `install.bat`. It finds the game through
Steam (or asks for the folder), backs up any `xinput1_3.dll` it would replace, and
keeps your existing settings if you update.

**Manual:** copy the 2 files from the `mod` folder (`xinput1_3.dll`, `xinput_mouse.ini`)
next to `MCC-Win64-Shipping.exe`, which is in
`Halo The Master Chief Collection\MCC\Binaries\Win64\`
(Steam: right-click the game, then Manage > Browse local files).

**Uninstall:** run `uninstall.bat`, or delete `xinput1_3.dll`, `xinput_mouse.ini`,
`mixed_input.log` and `mixed_input.old.log` from that folder.

## Steam Input setup

In the game's controller configuration:

| Input              | Set to                                 |
|--------------------|----------------------------------------|
| Gyro               | **As Mouse** (any activation you like) |
| Right trackpad / stick | **Joystick** (right stick output)  |
| Everything else    | Normal gamepad layout                  |

Tune each input separately:

- **Gyro speed:** the in-game mouse sensitivity, `GyroSensitivity` in
  `xinput_mouse.ini`, or the Steam Input gyro settings.
- **Trackpad speed:** Steam Input trackpad settings, or the in-game controller
  sensitivity.

If vertical gyro is inverted, change `InvertY` in the ini (or invert in Steam,
but not both, or they cancel out).

## Settings (`xinput_mouse.ini`)

Changes take effect after restarting the game. Every option is explained in the file.

| Key            | Default | Meaning |
|----------------|---------|---------|
| `Mode`         | 2   | 2 = native mouse look (recommended), 1 = legacy: mouse converted to right-stick deflection |
| `GyroSensitivity` | 1.0 | Mode 2: multiplier on top of the game's own mouse sensitivity |
| `TrackpadOverride` | 1 | Mode 2: 1 = real right stick/trackpad above `StickOverrideDeadzone` discards gyro |
| `Enabled`      | 1   | 1 = mod on, 0 = pass-through. F10 toggles at runtime |
| `PadIndex`     | 0   | XInput user index that receives the mouse (0 = first pad) |
| `Sensitivity`  | 0.3 | Mode 1: mouse speed to stick (1.0 = about 1000 counts/s gives full deflection) |
| `Curve`        | 1.0 | Mode 1: below 1 boosts small moves, above 1 damps them |
| `SmoothingMs`  | 30  | Mode 1: averaging window (5-200 ms) |
| `AntiDeadzone` | 0.30 | Mode 1: added to any non-zero deflection to jump the game's stick dead zone (0-0.9) |
| `StickOverrideDeadzone` | 0.27 | Right-stick deflection above which gyro is ignored |
| `InvertY`, `InvertX` | 0 | 1 = invert that axis |
| `RawSuppress`  | 1   | Raw mouse packets: 0 = leave to the game, 1 = zero movement (buttons/wheel kept), 2 = drop movement-only packets |
| `BlockMouseMove` | 1 | Swallow window mouse-move messages during gameplay |
| `MenuDetect`   | 0   | 0 = always gameplay, 1 = flag byte in the exe (`FlagRva`), 2 = hidden-cursor heuristic |
| `FlagRva`      | 0x4001b63 | Address of the "mouse locked" byte (valid for the tested build only) |
| `Log`          | 1   | 1 = write `mixed_input.log` next to the DLL |

## Troubleshooting

- **Nothing changes:** make sure the game was started **without Easy Anti-Cheat**.
  Then check that `mixed_input.log` appears in `MCC\Binaries\Win64` after launching.
  If not, the DLL is in the wrong folder or was not loaded.
- **Log says "GetInput hook NOT installed" / "FALLBACK":** the game build differs from
  the tested one. The mod switches itself off and MCC behaves as usual (no mixing).
  See Limitations.
- **Antivirus warning:** proxy DLLs next to a game exe are a common false positive
  (ReShade and many fixes work the same way). The full source code is in `src`.
- **Conflict with another mod** that also uses `xinput1_3.dll`: the installer backs it
  up to `DualInput_backup` and restores it on uninstall. The two cannot run at the
  same time.

## Limitations

- Tested only on **Halo 2** (the only title installed during development). The logic
  lives in the shared MCC host, so the other games probably work too, but this is
  unverified.
- Gyro does **not** move the cursor in menus. Use the gamepad in menus.
- Several addresses are specific to the tested game build. After a game update the
  mod checks the code before hooking it and **disables itself** (MCC falls back to
  normal behavior) until the mod is updated.
- No Easy Anti-Cheat, so no EAC-protected online play. Steam version only.

## How it works

One small proxy DLL, `xinput1_3.dll`, sits between the game and Windows. MCC imports
XInput 1.3 by ordinal, so the DLL placed next to the exe is loaded instead of the
system one, and it forwards everything to the real XInput. It then hooks three things:

- **`XInputGetState`**, through the exe's import table. Steam's overlay detours the
  exported function, so the mod points the game's import entry at its own function,
  which calls Steam's version first and then post-processes the result (bypassing
  Steam's hook).
- **`GetRawInputData`**, to hide mouse movement from MCC so it does not switch to
  keyboard and mouse. Buttons and wheel are kept.
- **MCC's per-player input builder.** The mod writes the mouse yaw/pitch (in radians
  per frame) into the input packet, exactly as if the game had read a real mouse,
  while the device type stays "gamepad".

Before patching, the mod verifies the function prologue and slot addresses. If they
do not match, it installs nothing.

Build: run `src\build.bat` (needs Visual Studio Build Tools, 64-bit MSVC).

## Contact

Bug reports, questions and permission requests: open an issue at
https://github.com/GyroGamesCom/MCC_DualInput/issues

## License

Free for personal use. Redistribution, re-uploading or any commercial use requires prior written
permission from GyroGamesCom, and GyroGamesCom must always be credited. Showing the mod in
videos and streams is fine with credit. See `LICENSE.txt` for the full terms.
