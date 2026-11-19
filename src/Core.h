// Core.h - the shared contract between all modules. Plain types only; no libmdr, no COM headers.
// Threading: everything here is used on the UI thread, except that DeviceWorker and Audio run
// their own threads internally and talk to the UI ONLY by PostMessage(hwnd, WM_XM_*, ...).
#pragma once
#include <windows.h>
#include <array>
#include <string>
#include <vector>

namespace xm {

// ---- window messages posted to the main (hidden) window ---------------------------------
constexpr UINT WM_XM_TRAY = WM_APP + 1;   // Shell_NotifyIcon v4 callback
constexpr UINT WM_XM_STATE = WM_APP + 10; // DeviceWorker published a new Snapshot
constexpr UINT WM_XM_AUDIO = WM_APP + 11; // audio endpoints / default endpoint changed
constexpr UINT WM_XM_SHOW = WM_APP + 12;  // second instance asked us to show the flyout

// ---- device state ------------------------------------------------------------------------
enum class Link { Idle, Connecting, Initializing, Ready, Failed };
enum class NcMode { Off, NoiseCancelling, Ambient, Wind };
enum class Charging { No, Yes, Full };
enum class Codec { Unknown, SBC, AAC, LDAC, AptX, AptXHD, LC3, Other };

struct Battery {
    bool present = false;
    int percent = 0;
    Charging charging = Charging::No;
};

struct Range {
    int min = 0, max = 0, step = 1;
};

struct NamedInt { // an option in a list: value to send + label to show
    int value = 0;
    std::wstring label;
};

// Generic building blocks for the long tail of per-model settings. `has` = supported by the connected model.
struct Flag {
    bool has = false;
    bool on = false;
};
struct Choice { // one value out of `options` (value to send + label to show)
    bool has = false;
    int value = 0;
    std::vector<NamedInt> options;
};
struct GeneralItem { // a model-defined "general setting" (multipoint, sidetone, touch panel, ...)
    int index = 0;      // id to send back
    std::wstring label; // human readable
    bool isBool = true;
    bool on = false;                // when isBool
    int value = 0;                  // option index when !isBool
    std::vector<NamedInt> options;  // when !isBool
    bool writable = true;
};
struct AssignKey { // a programmable button / touch surface
    std::wstring label;             // e.g. "Left earbud (touch)", "NC/AMB button"
    int action = 0;                 // current value out of `options`
    std::vector<NamedInt> options;  // actions this key accepts
};

struct PairedHost { // a source device (phone/PC) the headphones know (multipoint)
    std::string mac;
    std::wstring name;
    bool connected = false;
    bool playback = false;
};

// Everything the UI needs. `has*` = the connected model supports it (hide the control otherwise).
// Values are what the user last asked for when a change is pending, else what the device reported.
struct Snapshot {
    Link link = Link::Idle;
    std::string mac;      // "AA:BB:CC:DD:EE:FF" of the device we are (trying to be) connected to
    std::wstring error;   // human readable, set when link == Failed
    std::wstring model;   // e.g. "WH-1000XM5"; empty until known
    std::wstring firmware;
    int protocol = 0; // 1 or 2

    Battery battSingle, battLeft, battRight, battCase;

    bool hasCodec = false;
    Codec codec = Codec::Unknown;

    bool hasNc = false, hasAmbient = false, hasWind = false, hasVoiceFocus = false;
    NcMode nc = NcMode::Off;
    Range ambientRange;
    int ambientLevel = 0;
    bool voiceFocus = false;

    bool hasVolume = false;
    Range volumeRange;
    int volume = 0;

    bool hasEq = false;
    int eqPreset = 0;               // value from eqPresets
    std::vector<NamedInt> eqPresets; // may be empty (device accepts any / no choice)
    int eqBandCount = 0;            // 0, 5 or 10
    std::array<int, 10> eqBands{};  // first eqBandCount entries valid
    std::array<int, 10> eqBandHz{}; // centre frequency per band, 0 if unknown
    Range eqBandRange;
    bool hasClearBass = false;
    Range clearBassRange;
    int clearBass = 0;

    bool hasDsee = false, dseeAvailable = false, dsee = false;
    std::wstring dseeName; // "DSEE", "DSEE HX", "DSEE Extreme", "DSEE Ultimate"

    bool hasSpeakToChat = false, speakToChat = false;
    int stcSensitivity = 0; // 0 auto, 1 high, 2 low
    int stcTimeout = 0;     // 0 short, 1 medium, 2 long, 3 manual

    bool hasTouchPanel = false, touchPanel = false;

    bool hasAutoPowerOff = false;
    int autoPowerOffMinutes = 0;             // 0 = never
    std::vector<NamedInt> autoPowerOffOptions; // value = minutes
    bool hasPowerOff = false;

    bool hasMultipoint = false;
    std::vector<PairedHost> hosts;

    bool hasConnectionPriority = false;
    int connectionPriority = 0; // 0 = sound quality, 1 = stable connection

    bool hasAutoPause = false, autoPause = false; // pause when the headphones are taken off

    // ---- everything else the protocol exposes (all optional per model) ----
    bool eqAvailable = true;         // false = the device currently refuses EQ changes (e.g. during a call)
    Flag adaptiveAmbient;            // ambient level follows the surroundings
    Choice adaptiveSensitivity;
    Choice ncButton;                 // what the NC/AMB button cycles through
    Choice listeningMode;            // Standard / Background music / Cinema ...
    Choice bgmRoom;                  // room size for background-music mode
    Flag voiceGuidance;
    bool hasVoiceGuidanceVolume = false;
    Range voiceGuidanceRange;
    int voiceGuidanceVolume = 0;
    Choice voiceAssistant;
    Flag headGesture;                // nod / shake to answer calls
    Flag wearingPowerOff;            // power off when the headphones are removed
    Flag sourceSwitch;               // automatic source switching between connected devices
    Flag pairingMode;                // Bluetooth pairing mode on/off
    bool hasSafeListening = false;
    int soundPressure = 0;           // current dB(A), 0 if unknown
    int wearing = 0;                 // 0 unknown, 1 worn, 2 left removed, 3 right removed, 4 removed
    bool hasPlaybackControl = false;
    int playState = 0;               // 0 unknown, 1 playing, 2 paused
    bool hasTrackInfo = false;
    std::wstring trackTitle, trackArtist, trackAlbum;
    std::vector<GeneralItem> general;   // excludes the touch panel item (that one is `touchPanel` above)
    std::vector<AssignKey> assignable;

    bool pendingAlert = false; // device asks for confirmation of the last change
    std::wstring alertText;
};

// Targets of Command::SetFeature (a = (int)Feature, b = new value, c = index for General / Assignable).
enum class Feature {
    AdaptiveAmbient,     // b = 0/1
    AdaptiveSensitivity, // b = option value
    NcButton,            // b = option value
    ListeningMode,       // b = option value
    BgmRoom,             // b = option value
    VoiceGuidance,       // b = 0/1
    VoiceGuidanceVolume, // b = volume
    VoiceAssistant,      // b = option value
    HeadGesture,         // b = 0/1
    WearingPowerOff,     // b = 0/1
    SourceSwitch,        // b = 0/1
    PairingMode,         // b = 0/1
    General,             // c = GeneralItem::index, b = 0/1 or option value
    Assignable,          // c = position in Snapshot::assignable, b = action value
};

// ---- commands UI -> DeviceWorker ---------------------------------------------------------
struct Command {
    enum Kind {
        SetNc,           // a = (int)NcMode
        SetAmbientLevel, // a = level
        SetVoiceFocus,   // a = 0/1
        SetVolume,       // a = volume
        SetEqPreset,     // a = preset value
        SetEqBands,      // bands[0..count) , count = a
        SetClearBass,    // a = value
        SetDsee,         // a = 0/1
        SetSpeakToChat,  // a = 0/1, b = sensitivity, c = timeout
        SetTouchPanel,   // a = 0/1
        SetAutoPowerOff, // a = minutes (0 never)
        PowerOff,
        HostConnect,     // text = host mac
        HostDisconnect,  // text = host mac
        HostSelectPlayback, // text = host mac
        AnswerAlert,     // a = 0 negative / 1 positive
        SetConnectionPriority, // a = 0 sound quality / 1 stable connection
        SetAutoPause,    // a = 0/1
        SetFeature,      // a = (int)Feature, b = value, c = index (see Feature)
        Playback,        // a = 0 play, 1 pause, 2 next, 3 previous
        RefreshBattery,
    } kind = RefreshBattery;
    int a = 0, b = 0, c = 0;
    std::array<int, 10> bands{};
    std::string text;
};

// A Sony headphone Windows currently has connected (candidate for control).
struct BtDevice {
    std::string mac; // "AA:BB:CC:DD:EE:FF", upper case
    std::wstring name;
};

// ---- DeviceWorker (DeviceWorker.cpp) -----------------------------------------------------
// Owns libmdr on a private thread. All public methods are thread-safe and non-blocking.
class DeviceWorker {
public:
    explicit DeviceWorker(HWND notify); // posts WM_XM_STATE to `notify` whenever Get() would differ
    ~DeviceWorker();                    // disconnects and joins
    DeviceWorker(const DeviceWorker&) = delete;
    DeviceWorker& operator=(const DeviceWorker&) = delete;

    void Connect(const std::string& mac); // drops any current link first; tries protocol V2 then V1
    void Disconnect();
    void Post(const Command& cmd); // ignored unless link == Ready
    Snapshot Get() const;
    std::vector<BtDevice> ListConnected() const; // cached; refreshed by the worker every few seconds

private:
    struct Impl;
    Impl* impl_;
};

// ---- Audio (Audio.cpp) -------------------------------------------------------------------
struct Endpoint {
    std::wstring id;   // IMMDevice id
    std::wstring name; // friendly name
    std::string btMac; // "AA:BB:CC:DD:EE:FF" if this endpoint is a Bluetooth A2DP device, else ""
    bool isDefault = false;
    int sampleRate = 0, bits = 0, channels = 0; // shared-mode device format, 0 if unknown
};

class Audio {
public:
    explicit Audio(HWND notify); // posts WM_XM_AUDIO on endpoint add/remove/state/default change
    ~Audio();
    Audio(const Audio&) = delete;
    Audio& operator=(const Audio&) = delete;

    std::vector<Endpoint> RenderEndpoints() const;   // active render endpoints, stereo (not hands-free)
    bool SetDefault(const std::wstring& endpointId); // console + multimedia + communications roles

private:
    struct Impl;
    Impl* impl_;
};

// ---- Settings + log (Settings.cpp) -------------------------------------------------------
struct EqPreset { // a user-saved curve, stored on the PC
    std::wstring name;
    int bandCount = 0;
    std::array<int, 10> bands{};
    int clearBass = 0;
};

struct Settings {
    bool autostart = false;
    bool notifyConnect = true;
    bool notifyLowBattery = true;
    int lowBatteryPercent = 20;
    bool routeAudioOnSelect = true;  // picking a headphone makes it the Windows output
    bool followWindowsOutput = true; // Windows output changed to a known headphone -> control that one
    bool disconnectWhenHidden = true; // drop the control link shortly after the window closes
    bool resetEqOnDolbyMode = true;   // picking a Dolby mode flattens the headphone EQ (no stacked tunings)
    bool dolbyAutomation = false;     // experimental: switch Dolby sound modes by driving the Dolby Access window
    std::string lastMac;
    std::vector<EqPreset> eqPresets;
};

Settings LoadSettings();                 // %LOCALAPPDATA%\XMTray\settings.ini ; defaults if missing
void SaveSettings(const Settings& s);    // also applies/removes the HKCU Run entry per s.autostart
std::wstring DataDir();                  // %LOCALAPPDATA%\XMTray (created)
void Log(const wchar_t* fmt, ...);       // thread-safe, appends to DataDir()\xmtray.log, rotates at 1 MB

// ---- Dolby (Dolby.cpp) -------------------------------------------------------------------
// Spatial sound / Dolby Atmos for Headphones on a Windows render endpoint. UI thread only.
struct DolbyState {
    bool appInstalled = false;   // Dolby Access is installed
    bool statusKnown = false;    // `spatialOn` / `atmosOn` / `formatName` are meaningful
    bool spatialOn = false;      // some spatial sound format is active on the endpoint
    bool atmosOn = false;        // ... and it is Dolby Atmos for Headphones
    std::wstring formatName;     // e.g. "Dolby Atmos for Headphones", "Windows Sonic for Headphones", "Off"
    bool canToggle = false;      // the output supports spatial sound: offer its Windows settings page
    bool canSetMode = false;     // SetMode() is implemented on this system
    int mode = -1;               // current value from `modes`, -1 if unknown
    std::vector<NamedInt> modes; // Dynamic / Movie / Music / Game / Voice / Personalize when canSetMode
    bool modeBusy = false;       // a mode switch is in progress (takes a few seconds)
    std::wstring modeError;      // why the last mode switch failed; empty if it worked or none was tried
    std::wstring note;           // one line for the user explaining a limitation, may be empty
    std::wstring endpointName;   // the Windows output this state describes (filled by the app), may be empty
};

// Sound modes have no public interface. With SetAutomation(true) (experimental, opt-in) SetMode drives the
// Dolby Access window through UI Automation on a worker thread; it is asynchronous: watch modeBusy / modeError.
class Dolby {
public:
    explicit Dolby(HWND notify); // posts WM_XM_AUDIO when an asynchronous mode switch finishes
    ~Dolby();
    Dolby(const Dolby&) = delete;
    Dolby& operator=(const Dolby&) = delete;

    DolbyState Get(const std::wstring& endpointId) const; // endpointId = IMMDevice id; "" = default output
    void SetAutomation(bool enabled); // experimental UI Automation of Dolby Access
    bool SetMode(int mode);           // true = switch started; result arrives via modeBusy/modeError/mode
    bool OpenApp() const; // launch Dolby Access (or its Store page if not installed)

private:
    struct Impl;
    Impl* impl_;
};

// ---- Tray (Tray.cpp) ---------------------------------------------------------------------
class Tray {
public:
    enum MenuCmd { None = 0, Open = 1, Quit = 2, ToggleAutostart = 3, OpenLogFolder = 4, DeviceBase = 100 };

    explicit Tray(HWND owner); // adds the icon, callback message WM_XM_TRAY, NOTIFYICON_VERSION_4
    ~Tray();
    Tray(const Tray&) = delete;
    Tray& operator=(const Tray&) = delete;

    void Readd();                       // call on the "TaskbarCreated" registered message
    void Update(const Snapshot& s);     // redraws icon (battery %) + tooltip
    void Notify(const std::wstring& title, const std::wstring& text); // balloon/toast
    RECT IconRect() const;              // screen rect of the icon (for anchoring the flyout)
    // Shows the right-click menu at the cursor; returns a MenuCmd or DeviceBase + index into `devices`.
    int ShowMenu(const std::vector<BtDevice>& devices, const std::string& activeMac, bool autostart);

private:
    struct Impl;
    Impl* impl_;
};

// ---- Main window (Flyout.cpp; the class keeps its historical name) -----------------------
// What the window needs from the application (implemented in main.cpp).
struct AppHost {
    virtual ~AppHost() = default;
    virtual Snapshot State() = 0;
    virtual std::vector<BtDevice> Devices() = 0;
    virtual std::vector<Endpoint> Endpoints() = 0;
    virtual void SelectDevice(const std::string& mac) = 0; // connect + (optionally) route audio
    virtual void Send(const Command& c) = 0;
    virtual DolbyState DolbyStatus() = 0;        // for the Windows output of the selected headphone
    virtual void OpenSpatialSettings() = 0;     // Windows Settings page of that output (spatial sound format)
    virtual void SetDolbyMode(int mode) = 0;     // also flattens the headphone EQ per Config().resetEqOnDolbyMode
    virtual void OpenDolbyAccess() = 0;
    virtual Settings& Config() = 0;
    virtual void ConfigChanged() = 0; // persist
    virtual void OpenLogFolder() = 0;
    virtual void Quit() = 0;
};

class Flyout {
public:
    Flyout(HINSTANCE inst, AppHost& host);
    ~Flyout();
    Flyout(const Flyout&) = delete;
    Flyout& operator=(const Flyout&) = delete;

    void Toggle(const RECT& anchor); // show (restoring the last position; `anchor` = tray icon) or hide if visible
    void Hide();
    bool Visible() const;
    void Refresh(); // state/devices/endpoints changed -> repaint if visible

private:
    struct Impl;
    Impl* impl_;
};

} // namespace xm
