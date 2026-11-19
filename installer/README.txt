XMTray
======
Unofficial desktop control for Sony Bluetooth headphones on Windows 10/11:
noise cancelling, ambient sound, equalizer, battery, codec info and more,
from a tray app. No network access, no account, no telemetry.

Not affiliated with or endorsed by Sony or Dolby. Trademarks belong to their owners.

Status: built and UI-tested with simulated headphones only.
Not yet verified against real hardware.

Supported models are whatever libmdr supports (mos9527/SonyHeadphonesClient);
features vary by model and firmware.

LDAC: an app cannot add a Bluetooth codec to Windows. XMTray only shows the
codec the headphones report and can set connection priority to sound quality.

Dolby: XMTray shows the active spatial sound format. Switching Dolby Access
sound modes is experimental, off by default and has never been run end to end.

Files and settings
------------------
Settings and log: %LOCALAPPDATA%\XMTray\settings.ini, xmtray.log
(MAC addresses are never logged in full).
Autostart: "Start with Windows" (in XMTray settings) writes
HKCU\Software\Microsoft\Windows\CurrentVersion\Run\XMTray for that user only.

Uninstall
---------
Uninstalling closes a running XMTray and removes the program files.
It does not touch per-user data: %LOCALAPPDATA%\XMTray stays, and a Run value
left by "Start with Windows" is not removed for any user. Turn "Start with
Windows" off in XMTray before uninstalling, or delete the XMTray value under
HKCU\Software\Microsoft\Windows\CurrentVersion\Run afterwards.

Switching the Windows output device uses the undocumented IPolicyConfig
interface, as EarTrumpet and SoundSwitch do.

MIT licensed, see LICENSE.txt.
Project: https://github.com/OMRITGM/XMTray
