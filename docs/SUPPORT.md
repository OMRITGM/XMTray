# XMTray model support (Sony 1000X family and relatives)

Compiled 2026-11-19. XMTray supports whatever the vendored libmdr supports (`third_party/libmdr`). Nothing here comes from testing XMTray on hardware.

Status legend (per cell):

| Code | Meaning |
|---|---|
| **R** | Reported working in libmdr docs (`third_party/libmdr/docs/device-support/<model>.md`) |
| **H** | Supported by the hardware per Sony's help guide; untested in libmdr (no doc, or doc marks it untested) |
| **N** | Not supported by the hardware (per Sony guide) or reported unsupported in libmdr docs |
| **?** | Unknown / not stated in any source I could read |

## 1. Protocol generation

libmdr has two protocol families, selected by Bluetooth SDP service UUID (`third_party/libmdr/libmdr/include/mdr-c/Base.h`):

- `MDR_SERVICE_UUID_V1` `96CC203E-5068-46AD-B32D-E316F5E069BA`, commented "MDR V1, XM4s and older".
- `MDR_SERVICE_UUID_V2` `956C7B26-D49A-4BA8-B03F-B17D393CB6E2`, commented "MDR V2, XM5s and newer".
- XMTray tries V2 first, then V1 (`src/DeviceWorker.cpp`, `kServices`).
- V2 has "table 1 / table 2" command tables. The libmdr docs mention them only for V2 devices.

| Model (year) | Family | Where established | In libmdr docs? |
|---|---|---|---|
| MDR-1000X (2016) | **unknown / no evidence** | See section 3 | no |
| WH-1000XM2 (2017) | V1 (inferred) | Base.h comment ("XM4s and older"); README roadmap lists it as a V1/legacy target. Upstream issue #61 (closed 2026-11-16): reporter says it works with all features and battery. | no doc file ("support matrix pending") |
| WH-1000XM3 (2018) | **V1**, protocol `0x4010`, table 1 (19 functions) | `docs/device-support/WH-1000XM3.md`, FW 4.5.2, Linux; replay capture `tests/WH-1000XM3-4.5.2/` | yes |
| WH-1000XM4 (2020) | V1 | README roadmap ("tested by @Amrsatrio"), Base.h comment. PR #75 (closed, unmerged) claims XM4 notes; its text says the connection used either UUID, so the family is not independently confirmed here. | no doc file in this vendored copy |
| WH-1000XM5 (2022) | V2 (inferred) | Base.h comment ("XM5s and newer"). Doc cites issue #4. | yes |
| WH-1000XM6 (2025) | V2 (inferred) | Same. Doc cites PR #48, FW 1.0.5 / 3.0.0 | yes |
| WF-1000X (2017) | ? | not in libmdr docs or in Sony's Headphones Connect compatibility list as a libmdr device | no |
| WF-1000XM3 (2019) | ? | not in libmdr docs | no |
| WF-1000XM4 (2021) | ? (the "XM4s and older" comment suggests V1, but it names no WF model) | not in libmdr docs. Gadgetbridge lists it "highly supported", FW 1.4.2 (no protocol stated). | no |
| WF-1000XM5 (2023) | V2 (inferred) | doc + capture `tests/WF-1000XM5-6.1.0/` (FW 6.1.0, replay test). The protocol number is not stated in the doc. | yes |
| WF-1000XM6 (2026) | V2 (inferred) | `WF-1000XM6.md`, upstream issue #33 | yes |
| WF-C700N | V2, `0x03000015`, FW 2.0.5/2.0.7, both tables | `WF-C700N.md` | yes |
| WF-LC900 LinkBuds Clip | V2, `0x03003015`, FW 2.0.3, series `LINK_BUDS` | `WF-LC900.md` | yes |
| WF-LS900N LinkBuds S | V2, `0x03000007`, FW 2.0.2 (35 table-1, 2 table-2 functions) | upstream issue #74 (V1 UUID lookup fails, V2 resolves). The doc itself is all "?" | yes (matrix untested) |
| WF-L910 LinkBuds Open | ? (not stated) | `WF-L910.md`, FW 1.4.1, Linux | yes |
| WF-C510 | ? | `WF-C510.md` (issue #5) | yes |
| WH-CH720N | ? | `WH-CH720N.md` (issue #7) | yes |

"Inferred" means derived only from the Base.h comment, not from a capture or doc line that names the protocol number.

## 2. Feature matrix, 1000X family

Hardware facts come from Sony's help guides (`print.pdf` / topic pages, see Sources). A hardware fact only appears as **H** if the guide lists the feature. Where the guide is silent the cell is **?**. libmdr status overrides: R if the doc says working, N if the doc says unsupported.

| Feature | MDR-1000X | WH-1000XM2 | WH-1000XM3 | WH-1000XM4 | WH-1000XM5 | WH-1000XM6 | WF-1000X | WF-1000XM3 | WF-1000XM4 | WF-1000XM5 | WF-1000XM6 |
|---|---|---|---|---|---|---|---|---|---|---|---|
| Noise cancelling / ambient on-off | H (hw only; protocol ?) | H | R | H | R | R | H | H | H | R | R |
| Ambient sound level | ? | ? | R | H | R (via NC/AMB) | R (incl. auto level) | ? | ? | ? | R (via NC/AMB) | R (via NC/AMB) |
| Adaptive ambient / Adaptive Sound Control | ? | ? | H (libmdr: advertised, not requested) | H | H | H | ? | H | H | H | H |
| Wind noise reduction | ? | ? | libmdr code treats a "single" mode as WNR on XM3 | ? | ? | ? | ? | ? | H (automatic) | ? | ? |
| NC Optimizer | H ("Personal NC Optimizer") | H | H (libmdr: advertised, no UI) | H | H (auto) | H (auto) | ? | ? | ? | ? | ? |
| Equalizer, bands/presets | ? | H | R (preset) | H | R (no preset choice) | R (Heavy/Clear/Hard/Soft + Custom, FW 3.0.0) | ? | H | H | R | partial (doc: in progress) |
| Clear Bass | ? | ? | ? | ? | ? | ? | ? | ? | H | H | ? |
| DSEE variant | HX (H) | HX (H) | HX, R | Extreme (H) | Extreme (H) | Extreme (H) | none stated | HX (H) | Extreme (H) | Extreme (H) | Extreme, R |
| Speak-to-Chat | N | N | N (doc) | H | doc: untested | R | N | N | H | R | R |
| Touch sensor on/off | H | H | R | H | R | R | ? | H | H | R (gesture) | R (gesture) |
| Auto power off | ? | ? | R (5/30/60/180 min or off; no 15 min) | H | R (Power Off) | R (Power Off) | ? | ? | H | R (Power Off) | R (Power Off) |
| Multipoint | H (guide has multipoint topics) | H | N (doc: device does not advertise it) | H | doc: untested | R | N (guide) | N (guide says unsupported) | H | R | R |
| LDAC (hardware) | yes | yes | yes | yes | yes | yes | no | no | yes | yes | yes |
| Other A2DP codecs (hardware) | SBC, AAC, aptX | SBC, AAC, aptX, aptX HD | SBC, AAC, aptX, aptX HD | SBC, AAC | SBC, AAC | SBC, AAC; LC3 (LE Audio) | SBC, AAC | SBC, AAC | SBC, AAC | SBC, AAC; LC3 (LE Audio) | SBC, AAC; LC3 (LE Audio) |
| Codec indicator read by libmdr | ? | ? | R | ? | ? (not in doc) | ? | ? | ? | ? | ? | ? |
| Bluetooth connection quality | H (set on the headset) | H | H (libmdr: advertised, no UI) | H | H | H (includes Low latency) | ? | H | H | H | H |
| Voice guidance | H | H | R (on/off; no volume) | H | R on/off; volume N | R incl. volume | H | H | H | R | R |
| Head gestures | ? | ? | ? | ? | ? | H | ? | ? | ? | H | H |
| Battery reporting | headset only (H) | headset only | R (single level) | headset only (H) | R (single level) | R | headset (H) | L/R + case (H) | L/R + case (H) | R (layout not stated in doc) | R |

Notes:

- Sony's guides do not state ambient-level step counts, so "ambient level" is **H** only where libmdr R says so or the guide names the function.
- "Head gestures" for WF-1000XM5/XM6 and WH-1000XM6: the guides describe nod/shake for calls. libmdr docs list "Mapping headphone gestures to Shell Commands" as untested (XM6, `?`). That is a different feature from the hardware gesture.
- WF-1000XM6 `Pause when removed`: libmdr doc says unsupported (N in libmdr).
- WH-1000XM3 libmdr notes: `Ambient level` works. `Speak to Chat`, `Multipoint`, `Pause when removed` are unsupported on the device.
- WH-1000XM6 voice assistant: Background Music reboots the headphones while Voice Control is on (upstream issue #76, per the doc).

## 3. MDR-1000X

**There is no evidence that XMTray or libmdr works with the MDR-1000X. Status: unknown, not supported.**

- libmdr docs (`docs/device-support/`), README roadmap and DEVELOPER.md never mention it.
- Plutoberth/SonyHeadphonesClient README lists WH-1000XM3 (works), WH-1000XM4 (partial), MDR-XB950BT (works). The MDR-1000X is not mentioned. Its "Headset Reports" thread (#29) has no MDR-1000X entry in the content I could read.
- Gadgetbridge's Sony page and changelog do not list the MDR-1000X (WH-1000XM2/XM3/XM4/XM5/XM6, WF-1000XM3/4/5 and others are listed).
- AndreasOlofsson/mdr-protocol (the protocol write-up) names no MDR-1000X.
- Sony's own "Headphones Connect" compatibility table (as of Sept 2024) lists WF-1000X, WI-1000X and the XM series, but **not** the MDR-1000X. So the app that speaks the MDR protocol does not list it, which is a reason to doubt it speaks the same control protocol.
- It does have Sony app-free controls (the MDR-1000X help guide describes "Priority on sound quality" set on the headset, plus multipoint, DSEE HX and a Personal NC Optimizer).
- Unverified: whether it ever answered the V1 service UUID. Nobody has reported a connection attempt in anything I read. I could not search GitHub issues beyond the API queries I ran (no results for "MDR-1000X" in mos9527 or Plutoberth).

If you own one: XMTray would just fail to find the service. A packet capture via libmdr's Protocol Debugger would settle it.

## 4. Other models with a libmdr doc

| Model | Status summary from its doc |
|---|---|
| WF-1000XM5 | Everything R: current playing, sound pressure, battery, volume, NC/AMB, voice guidance volume, track controls, multipoint, speak-to-chat, EQ, touch gestures, power off, capture voice during call |
| WF-1000XM6 | As XM5 except EQ in progress (🔄); DSEE R; mapping gestures to shell commands and listening mode untested; pause when removed N |
| WH-1000XM6 | Everything R incl. listening mode and a voice assistant (see note); EQ limited to Heavy/Clear/Hard/Soft/Custom |
| WH-1000XM5 | NC/AMB, battery, volume, track, EQ (no preset choice), touch sensor, NC/AMB button, power off R; voice guidance volume and capture voice N; multipoint and speak-to-chat untested |
| WH-1000XM3 | See section 2 plus `docs/device-support/WH-1000XM3.md` |
| WF-C510 | Most R; sound pressure, speak-to-chat, capture voice, gesture mapping N |
| WH-CH720N | NC/AMB, battery, volume, track, power off R; EQ R (no preset choice); touch gesture N; multipoint documented as officially supported but pending |
| WF-C700N | V2; most R (battery L/R+case, DSEE, connection quality, auto power off); sound pressure, listening modes, speak-to-chat N; adaptive sound control pending |
| WF-LC900 | V2; most R; NC/AMB N |
| WF-L910 (LinkBuds Open) | Power off, battery, speak-to-chat, EQ, DSEE, adaptive volume, track controls R; connection quality (SBC/AAC) and LC3 setting N |
| WF-LS900N (LinkBuds S) | Matrix not filled in (all "?"); issue #74 says codec indicator and connection quality answered during initialization |

## Sources

Local:
- `third_party/libmdr/libmdr/include/mdr-c/Base.h` (service UUIDs; `MDR_AUDIO_CODEC_*` in `Headphones.h`)
- `third_party/libmdr/docs/device-support/*.md`, `third_party/libmdr/README.md`, `third_party/libmdr/tests/`
- `src/DeviceWorker.cpp` (V2 then V1 service order)

Upstream and related projects:
- https://github.com/mos9527/SonyHeadphonesClient (issues #4, #5, #7, #32, #33, #48, #61, #64, #66, #74, #75, #76)
- https://github.com/Plutoberth/SonyHeadphonesClient (README, issue #29, issue #53)
- https://gadgetbridge.org/gadgets/headphones/sony/ and https://raw.githubusercontent.com/Freeyourgadget/Gadgetbridge/master/CHANGELOG.md
- https://github.com/AndreasOlofsson/mdr-protocol

Sony:
- https://helpguide.sony.net/mdr/hpc/v1/en/contents/TP0001548861.html (Headphones Connect compatible devices, as of Sept 2024)
- Help guides: `https://helpguide.sony.net/mdr/<id>/v1/en/print.pdf` for wh1000xm2, wh1000xm3, wh1000xm4, wh1000xm5, 2984 (WH-1000XM6), wf1000x, wf1000xm3, wf1000xm4, 2963 (WF-1000XM5), 2985 (WF-1000XM6). The MDR-1000X guide (`.../mdr/1000x/v1/en/contents/TP0001176186.html` for codecs, `TP0001176179.html` for sound quality mode, others) has no `print.pdf`.
- XM4 multipoint topic: https://helpguide.sony.net/mdr/wh1000xm4/v1/en/contents/TP0002935899.html
- XM5 codecs: https://helpguide.sony.net/mdr/wh1000xm5/v1/en/contents/TP1000534600.html

Caveats: the per-model hardware row was compiled from help-guide text read through a summarizing fetcher, not read line by line. Treat individual cells as "per Sony guide, unverified against the device".
