# LDAC on Windows with a WH-1000XM5

Compiled 2026-11-19. XMTray reports the codec the headphones say they use and the Windows output format. It cannot enable LDAC, does not include or install any driver, and does not change Windows Bluetooth settings.

## (a) Native Windows Bluetooth codecs (A2DP)

| Codec | Native in Windows 10/11? | Source |
|---|---|---|
| SBC | Yes | all sources below |
| AAC | Windows 11 yes; Windows 10 no | bluetoothgoodies.com/a2dp comparison table |
| aptX (classic) | Yes, but "depends on your adapter and drivers" | MS Q&A 2025-09-20 |
| aptX HD | No | MS Q&A; How-To Geek 2026-07-31 |
| aptX Adaptive | Reported not native (MS Q&A). One third-party blog claims 24H2 with Qualcomm radios; **unverified** (no Microsoft or Qualcomm source found) | see sources |
| aptX Lossless | How-To Geek says 24H2+ with compatible Qualcomm hardware; **unverified** by Microsoft | How-To Geek |
| **LDAC** | **No, not native** | MS Q&A ("not natively supported"), How-To Geek ("does not support ... LDAC ... natively at all") |
| LE Audio / LC3 | Yes since Windows 11 22H2 (KB5026446), if the PC's Bluetooth adapter and driver support it | Microsoft Learn LE Audio page (ms.date 2025-05-05) |

- Microsoft's A2DP codec statement is a forum answer by a "Microsoft External Staff" moderator (thread posted 2025-09-19, answered 2025-09-20). I found no official Microsoft documentation that lists the inbox A2DP codecs. Treat the list as reporting, not a specification.
- Microsoft's Windows Experience post "Cutting the wire without cutting the audio quality" (2025-08-26, updated 2025-08-27) covers LE Audio "super wideband stereo": Windows 11 24H2 with latest updates plus an OEM Bluetooth audio driver update. It says nothing about A2DP codecs.
- LE Audio is a separate path (LC3 instead of A2DP). Enabling it does not give LDAC. WH-1000XM5 does **not** support LE Audio per its Sony guide (SBC, AAC, LDAC only). WH-1000XM6 and WF-1000XM5/XM6 do (LC3).
- What Microsoft could change later is unknown; the dates above are the latest I found.

## (b) Third-party options

- **Alternative A2DP Driver** (Bluetooth Goodies, https://www.bluetoothgoodies.com/a2dp): replaces the Windows 10/11 inbox A2DP driver. Per the vendor's table it adds LDAC, aptX HD and aptX LL, and (on Windows 10) AAC with an extra AAC license. It has a "Try Free" download and a paid license. How-To Geek (2026-07-31) says about $6; the vendor page I read gave no price (unverified). Install: select the audio device in its app, switch the driver from "Windows Standard Driver" to "Alternative A2DP Driver", confirm the system change; codec parameters apply after reconnect.
- It is a separate, kernel-mode driver. Installing it needs administrator rights and a system change prompt (the vendor page does not discuss Secure Boot or signing, unverified). One guide advises a restore point first. Microsoft's Q&A calls it experimental (secondary report).
- Avoid "cracked" copies; they are a malware and stability risk.
- A USB Bluetooth transmitter that encodes LDAC itself is a driver-free alternative (How-To Geek names Creative BT-W6 and Sennheiser BTD 700 for codec support, but I did not verify that those two do LDAC).
- **XMTray does not include, bundle, install or configure any A2DP driver.**

## (c) Headphone-side settings

### Sony help-guide statements

| Item | WH-1000XM5 | WH-1000XM4 | WH-1000XM6 | WF-1000XM4/XM5/XM6 |
|---|---|---|---|---|
| A2DP codecs | SBC, AAC, LDAC | SBC, AAC, LDAC | SBC, AAC, LDAC; LC3 on LE Audio | same, plus LC3 on XM5/XM6 |
| Setting name | Sound quality mode | Sound quality mode | Sound quality mode | Bluetooth connection quality / sound quality mode |
| Factory default | Priority on sound quality | Priority on sound quality | Prioritize Sound Quality | **Stable connection / High Reliability**, which blocks LDAC (XM4, XM5, XM6) |
| Alternatives | Priority on stable connection | Priority on stable connection | Prioritize Stable Connection, Low latency (LE Audio) | Low latency on XM5/XM6 |
| App | "Sony \| Headphones Connect" in the 2022-era guide; the live index says it was renamed "Sound Connect" (Oct 2024) | Headphones Connect | Sound Connect | Headphones Connect / Sound Connect |

- WH-1000XM5 "Supported codecs": the headset switches to whichever codec the source sends. The source device may need to be set to LDAC first.
- WH-1000XM5 "Sound quality mode" page says the default is "Priority on sound quality" and "Priority on stable connection" for dropouts. It does not mention LDAC or bitrates.
- A Sony support article (sony-mea.com article 00264224, could not be fetched) is summarized by search as: when you play with LDAC the data volume increases, so set Bluetooth Connection Quality to "Priority on Stable Connection" if interruptions bother you.
- Bitrate figures per mode (990 / 660 / 330 kbps) come from third-party pages; **unverified** against Sony.

### Multipoint and DSEE interaction (rules by model)

| Model | LDAC vs multipoint | LDAC vs DSEE Extreme |
|---|---|---|
| WH-1000XM4 | Sony guide (TP0002935899): turning on 2-device simultaneous connection in the app **disables LDAC**; the headset uses AAC or SBC while multipoint is on. Guide version/firmware not stated. | The DSEE Extreme guide page I read is silent on LDAC. The model's print guide includes DSEE Extreme in an LDAC operating-time table (subagent report, not individually verified). |
| WH-1000XM5 | Sony multipoint page (TP1000541784) says nothing about LDAC. Press (Notebookcheck, 2023-07-27) reports firmware **2.0.2** "lets you use LDAC and multipoint together", that before it you had to choose. Sony release notes could not be fetched. Third-party sources say full 990 kbps LDAC may still be limited with multipoint; **unverified**. | Sony DSEE Extreme page: DSEE Extreme "may be disabled" when a compressed source is sent over LDAC, depending on the playback device. |
| WH-1000XM6 | Sony multipoint page (TP1001863603) states no LDAC restriction. | Same wording as XM5 (TP1001861254). |
| WF-1000XM4 | No restriction stated in the guide. Notebookcheck says it gained LDAC plus multipoint before the XM5 headset did (unverified). | Not stated. |
| MDR-1000X | Not stated. | A footnote says DSEE HX only works over Bluetooth with a non-LDAC codec (subagent reading; unverified). |

In practice for the XM5: if LDAC does not engage, check firmware (2.0.2 or later per the press report), the sound quality mode, and whether "Connect to 2 devices simultaneously" is on.

### What the headphones report

- libmdr can report the active codec (`MDR_AUDIO_CODEC_SBC/AAC/LDAC/APTX/APTX_HD/LC3/OTHER` in `Headphones.h`). The WH-1000XM3 doc lists "Codec Indicator" as working. The WH-1000XM5 doc does not list it, so whether the XM5 sends it to libmdr is **unverified**.
- Sony's app shows the codec in use ("By using the 'Sound Connect' app, you can check which codec is used", WF-1000XM6 guide).
- LDAC bitrate mode (990/660/330) is separate from the codec name; I did not find a libmdr feature that reports it (libmdr has `MDR_FEATURE_CONNECTION_MODE`, but its LDAC semantics are not documented in what I read).

## (d) Read-only detection on Windows

On this machine (Windows 11 Pro 26H2, build 10.0.26300): `Get-PnpDevice -Class Bluetooth` returned nothing (no Bluetooth radio), the Bluetooth Support Service (`bthserv`) is Stopped, the audio endpoints are USB/HDMI devices (none Bluetooth), and `pnputil /enum-drivers` matched nothing for "a2dp" or "Bluetooth". **So the queries below are untested against a real driver.**

```powershell
# 1. Bluetooth radio and A2DP/AVRCP devices (no admin)
Get-PnpDevice -Class Bluetooth | Select Status,FriendlyName,InstanceId
Get-PnpDevice -Class AudioEndpoint | Where FriendlyName -match 'Headphones|WH-1000' | Select Status,FriendlyName

# 2. Driver bound to a Bluetooth audio device (look for a non-Microsoft Provider)
Get-PnpDevice -Class Bluetooth -PresentOnly |
  ForEach-Object { Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName DEVPKEY_Device_DriverProvider,DEVPKEY_Device_DriverInfPath,DEVPKEY_Device_DriverVersion }

# 3. Third-party driver packages
pnputil /enum-drivers | Select-String -Pattern 'a2dp|Bluetooth' -Context 2,6
```

- The Alternative A2DP Driver's INF name, service name and registry keys are **not documented** on the vendor page I read; I did not guess them. Pragmatic check: a driver provider other than "Microsoft" on the headphones' Bluetooth A2DP node, or a non-inbox OEM INF in the `pnputil` list, would indicate a replacement. Both are unverified.
- Indirect hint: the audio endpoint's shared-mode format. The vendor says the inbox SBC driver is limited to 44.1 kHz (48 kHz content is resampled), whereas the Alternative driver supports every sample rate the headphones accept. A third-party blog claims LDAC at 96 kHz on Windows; **unverified**. A 96 kHz endpoint format is only a weak hint: the shared-mode format is whatever the user chose, not proof of the codec. It cannot tell you LDAC from aptX HD or SBC. The only reliable indicator is what the headphones report.
- XMTray reads the endpoint format already (`src/Audio.cpp`, `src/Flyout.cpp`); this is the "Windows output format" shown in the tray.

## (e) Checklist: LDAC on Windows with a Sony 1000X headset

1. Confirm the model supports LDAC (XM2 to XM6 and WF-1000XM4/XM5/XM6: yes; WF-1000X and WF-1000XM3: no per Sony guides).
2. Windows alone gives SBC/AAC/(aptX). LDAC needs a third-party A2DP driver (Alternative A2DP Driver) or an LDAC USB transmitter. XMTray does not provide either.
3. If you install a driver: it is a paid, admin-level kernel driver replacing the Windows A2DP driver; create a restore point first and use only the vendor's download.
4. Headphones: set sound quality / Bluetooth connection quality to "Priority on sound quality" (WF: not the factory default).
5. If you use two devices at once on XM4: LDAC is disabled by Sony's design. On XM5: update firmware (2.0.2 reported to allow both) and re-test.
6. DSEE Extreme may switch itself off for compressed sources over LDAC (XM5/XM6 guides).
7. Reconnect the headphones after any change; check the codec in the Sony app or XMTray's codec readout (XM5 reporting unverified).
8. If audio stutters over LDAC, switch to "Priority on stable connection" or reduce LDAC bitrate in the driver.
9. LE Audio/LC3 is a different path and needs a PC with LE Audio support (22H2+, adapter and driver); XM5 does not use it.

## Not verified

- Whether the XM5 sends its codec to libmdr; XM5 Sony support article 00264224 content (403); Sony release notes for firmware 2.0.2; Microsoft official statement on aptX Adaptive/Lossless; Alternative A2DP Driver price, INF/service/registry names, signing requirements; every driver-detection command on a machine that has the driver.

## Sources

- https://learn.microsoft.com/en-us/answers/questions/5560825/bluetooth-codecs-supported-in-windows-11 (2025-09-20)
- https://www.howtogeek.com/why-windows-cant-use-the-best-bluetooth-codecs-and-what-to-do-about-it/ (2026-07-31)
- https://learn.microsoft.com/en-us/windows-hardware/drivers/bluetooth/bluetooth-low-energy-audio (ms.date 2025-05-05)
- https://techcommunity.microsoft.com/blog/windowsosplatform/cutting-the-wire-without-cutting-the-audio-quality/4447942 (2025-08-26)
- https://www.bluetoothgoodies.com/a2dp
- Sony WH-1000XM5 guide: https://helpguide.sony.net/mdr/wh1000xm5/v1/en/contents/TP1000534600.html, `TP1000534501.html`, `TP1000541784.html`, `TP1000539063.html`
- Sony WH-1000XM4 guide: `https://helpguide.sony.net/mdr/wh1000xm4/v1/en/contents/TP0002752840.html`, `TP0002935899.html`
- Sony WH-1000XM6 guide: `https://helpguide.sony.net/mdr/2984/v1/en/contents/TP1001856790.html`, `TP1001856706.html`, `TP1001863603.html`, `TP1001861254.html`
- Sony WF-1000XM4/XM5/XM6, WF-1000X, WF-1000XM3, MDR-1000X guides (see `docs/SUPPORT.md`)
- https://www.notebookcheck.net/Sony-WH-1000XM5-receives-Multipoint-LDAC-and-head-tracking-support-via-new-update.736850.0.html (2023-07-27)
- Local: `third_party/libmdr/libmdr/include/mdr-c/Headphones.h`, `third_party/libmdr/docs/device-support/WH-1000XM3.md`, `WH-1000XM5.md`
