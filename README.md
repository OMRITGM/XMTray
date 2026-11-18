# XMTray

Unofficial desktop control for Sony Bluetooth headphones on Windows 10/11: noise cancelling, ambient sound, equalizer, battery, codec info and more, from a tray app. Single portable exe, standard user, no network access.

> Not affiliated with or endorsed by Sony or Dolby. Trademarks belong to their owners.

**Status: built and UI-tested with simulated headphones only. Not yet verified against real hardware.**

![Home](docs/ui-home.png)

## Features

- Noise cancelling / ambient (level, focus on voice) / off
- Equalizer with 5 or 10 bands (per model), device presets, Clear Bass, your own saved curves
- Battery (single or left / right / case) on the tray icon, charging state
- Volume, Speak-to-Chat, touch sensor, DSEE, auto power-off, pause on removal, connection priority, power off
- Codec reported by the headphones (SBC / AAC / LDAC / aptX) and the Windows output format
- Several headphones: pick one; optionally route Windows audio to it and follow the Windows output device
- Multipoint: connected sources and playback switching (protocol v2 models)
- Spatial sound status (Off / Windows Sonic / Dolby Atmos for Headphones) and a shortcut to Dolby Access
- Notifications on connect, disconnect and low battery
- Connects on demand; the control link is dropped shortly after the window is hidden

Supported models are whatever [libmdr](https://github.com/mos9527/SonyHeadphonesClient) supports (WH-1000XM3…XM6, WF-1000XM4…XM6 and relatives); features vary by model and firmware.

### Limits

- LDAC / aptX on Windows need a third-party A2DP driver. XMTray only reports the codec.
- Dolby Access sound modes have no public interface. See "Dolby" below.

## Build

Requirements: Visual Studio 2022 Build Tools (C++ workload, CMake ≥ 3.31, Ninja) and git.

```bat
git clone --recurse-submodules <this repo>
cd XMTray
build.cmd
```

Output: `b\XMTray.exe`. `build.cmd` builds through a short `subst` drive because libmdr's object paths can exceed `MAX_PATH`, and applies `patches\libmdr-bt-handle-leak.patch` to the submodule (a radio-handle leak in device enumeration).

`XMTray.exe --demo` runs with a simulated headphone and no Bluetooth; `tools\shot.ps1` uses it to capture screenshots.

## Layout

| Path | What |
|---|---|
| `src/Core.h` | The contract shared by all modules |
| `src/DeviceWorker.cpp` | libmdr on a private thread: connect, poll, commands, snapshot |
| `src/Flyout.cpp` | The window (Direct2D / DirectWrite) |
| `src/Audio.cpp` | Audio endpoints, Bluetooth mapping, default-device switch |
| `src/Dolby.cpp` | Spatial sound status, Dolby Access |
| `src/Tray.cpp` | Tray icon, menu, notifications |
| `src/Settings.cpp` | `%LOCALAPPDATA%\XMTray\settings.ini`, autostart, log |
| `src/main.cpp` | Wiring and the connect-on-demand policy |
| `third_party/libmdr` | Sony protocol library (submodule, MIT) |

## Footprint

- No network access, no account, no telemetry.
- Files: `%LOCALAPPDATA%\XMTray\settings.ini`, `xmtray.log` (MAC addresses are never logged in full).
- Registry: `HKCU\Software\Microsoft\Windows\CurrentVersion\Run\XMTray`, only when "Start with Windows" is on.
- Switching the Windows output device uses the undocumented `IPolicyConfig` interface, as EarTrumpet and SoundSwitch do.

## Credits

Protocol work: [mos9527/SonyHeadphonesClient](https://github.com/mos9527/SonyHeadphonesClient) (libmdr, MIT) and its predecessors. XMTray's own code is MIT, see `LICENSE`.
