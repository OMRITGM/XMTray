// DeviceWorker.cpp - owns libmdr (C ABI only) on one private thread.
// Nothing outside Impl::Run() and the functions it calls touches MDRConnectionWindows / MDRHeadphones.
#include "Core.h"

#include <mdr-c/Headphones.h>
#include <mdr-bt/ConnectionWindows.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <tuple>

namespace xm {

// ---- equality (found by ADL from std::vector / std::array comparisons) ---------------------
static bool operator==(const Battery& a, const Battery& b)
{
    return a.present == b.present && a.percent == b.percent && a.charging == b.charging;
}
static bool operator==(const Range& a, const Range& b) { return a.min == b.min && a.max == b.max && a.step == b.step; }
static bool operator==(const NamedInt& a, const NamedInt& b) { return a.value == b.value && a.label == b.label; }
static bool operator==(const PairedHost& a, const PairedHost& b)
{
    return a.mac == b.mac && a.name == b.name && a.connected == b.connected && a.playback == b.playback;
}
static bool operator==(const BtDevice& a, const BtDevice& b) { return a.mac == b.mac && a.name == b.name; }
static bool operator==(const Flag& a, const Flag& b) { return a.has == b.has && a.on == b.on; }
static bool operator==(const Choice& a, const Choice& b)
{
    return a.has == b.has && a.value == b.value && a.options == b.options;
}
static bool operator==(const GeneralItem& a, const GeneralItem& b)
{
    return a.index == b.index && a.label == b.label && a.isBool == b.isBool && a.on == b.on && a.value == b.value &&
           a.options == b.options && a.writable == b.writable;
}
static bool operator==(const AssignKey& a, const AssignKey& b)
{
    return a.label == b.label && a.action == b.action && a.options == b.options;
}
static bool operator==(const Snapshot& a, const Snapshot& b)
{
    const auto t4 = [](const Snapshot& s)
    {
        return std::tie(s.eqAvailable, s.adaptiveAmbient, s.adaptiveSensitivity, s.ncButton, s.listeningMode, s.bgmRoom,
                        s.voiceGuidance, s.hasVoiceGuidanceVolume, s.voiceGuidanceRange, s.voiceGuidanceVolume,
                        s.voiceAssistant, s.headGesture, s.wearingPowerOff, s.sourceSwitch, s.pairingMode);
    };
    const auto t5 = [](const Snapshot& s)
    {
        return std::tie(s.hasSafeListening, s.soundPressure, s.wearing, s.hasPlaybackControl, s.playState,
                        s.hasTrackInfo, s.trackTitle, s.trackArtist, s.trackAlbum, s.general, s.assignable);
    };
    if (!(t4(a) == t4(b) && t5(a) == t5(b)))
        return false;
    const auto t1 = [](const Snapshot& s)
    {
        return std::tie(s.link, s.mac, s.error, s.model, s.firmware, s.protocol, s.battSingle, s.battLeft, s.battRight,
                        s.battCase, s.hasCodec, s.codec, s.hasNc, s.hasAmbient, s.hasWind, s.hasVoiceFocus, s.nc,
                        s.ambientRange, s.ambientLevel, s.voiceFocus, s.hasVolume, s.volumeRange, s.volume);
    };
    const auto t2 = [](const Snapshot& s)
    {
        return std::tie(s.hasEq, s.eqPreset, s.eqPresets, s.eqBandCount, s.eqBands, s.eqBandHz, s.eqBandRange,
                        s.hasClearBass, s.clearBassRange, s.clearBass, s.hasDsee, s.dseeAvailable, s.dsee, s.dseeName,
                        s.hasSpeakToChat, s.speakToChat, s.stcSensitivity, s.stcTimeout, s.hasTouchPanel, s.touchPanel);
    };
    const auto t3 = [](const Snapshot& s)
    {
        return std::tie(s.hasAutoPowerOff, s.autoPowerOffMinutes, s.autoPowerOffOptions, s.hasPowerOff, s.hasMultipoint,
                        s.hosts, s.pendingAlert, s.alertText, s.hasConnectionPriority, s.connectionPriority,
                        s.hasAutoPause, s.autoPause);
    };
    return t1(a) == t1(b) && t2(a) == t2(b) && t3(a) == t3(b);
}

namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kAttemptTimeout = std::chrono::seconds(10);
constexpr auto kBatteryPeriod = std::chrono::seconds(60);
constexpr auto kListPeriod = std::chrono::seconds(5);
constexpr const char* kServices[2] = {MDR_SERVICE_UUID_V2, MDR_SERVICE_UUID_V1};

std::wstring Widen(const char* utf8)
{
    if (!utf8 || !*utf8)
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 1)
        return {};
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w.data(), n);
    w.resize(static_cast<size_t>(n) - 1);
    return w;
}
std::wstring Widen(const std::string& s) { return Widen(s.c_str()); }

// Last two octets only ("EE:FF") - the rest of a MAC never reaches the log.
std::wstring MacTail(const std::string& mac) { return Widen(mac.size() > 5 ? mac.substr(mac.size() - 5) : mac); }

bool ContainsNoCase(const std::string& hay, const char* needle)
{
    const size_t n = std::strlen(needle);
    for (size_t i = 0; i + n <= hay.size(); ++i)
        if (_strnicmp(hay.c_str() + i, needle, n) == 0)
            return true;
    return false;
}

bool LooksLikeSony(const std::string& name)
{
    for (const char* k : {"WH-", "WF-", "LinkBuds", "ULT ", "MDR-", "INZONE"})
        if (ContainsNoCase(name, k))
            return true;
    return false;
}

const wchar_t* PresetFallbackName(MDREqualizerPreset id)
{
    static const wchar_t* const kNames[] = {
        L"Off",    L"Rock",    L"Pop",    L"Jazz",   L"Dance",  L"EDM",    L"R&B / Hip-Hop", L"Acoustic",
        L"Bright", L"Excited", L"Mellow", L"Relaxed", L"Vocal", L"Treble", L"Bass",          L"Speech",
        L"Heavy",  L"Clear",   L"Hard",   L"Soft",   L"Gaming", L"FPS 1",  L"FPS 2",         L"FPS 3",
        L"Custom", L"User 1",  L"User 2", L"User 3", L"User 4", L"User 5"};
    return id < ARRAYSIZE(kNames) ? kNames[id] : L"Preset";
}

constexpr auto kPressurePeriod = std::chrono::seconds(1);
// Sound pressure and (on some models) track changes are only reported when asked for.
constexpr auto kLivePeriod = std::chrono::seconds(5);
// An ACK confirms delivery of a playback command, not the new state: look again a little later.
constexpr auto kPlaybackRecheck = std::chrono::milliseconds(600);

NamedInt Opt(uint32_t value, std::wstring label) { return {static_cast<int>(value), std::move(label)}; }

// Keeps the current value selectable when the device does not (or no longer) list it.
void EnsureOption(std::vector<NamedInt>& options, int value, const wchar_t* label)
{
    if (std::none_of(options.begin(), options.end(), [value](const NamedInt& o) { return o.value == value; }))
        options.push_back({value, label});
}

// "TAP_SENSITIVITY_SETTING_ELEM_LOW" -> "Low", "TAP_SENSITIVITY_SETTING" -> "Tap Sensitivity"
std::wstring Humanize(std::string key)
{
    if (const size_t elem = key.rfind("_ELEM_"); elem != std::string::npos)
        key.erase(0, elem + 6);
    else if (key.size() > 8 && key.compare(key.size() - 8, 8, "_SETTING") == 0)
        key.erase(key.size() - 8);
    bool wordStart = true;
    for (char& c : key)
    {
        if (c == '_')
        {
            c = ' ';
            wordStart = true;
            continue;
        }
        const auto u = static_cast<unsigned char>(c);
        c = static_cast<char>(wordStart ? std::toupper(u) : std::tolower(u));
        wordStart = false;
    }
    return Widen(key);
}

// Label for a general setting / option text; `isKey` = the device sent an identifier, not display text.
std::wstring GeneralLabel(const std::string& text, bool isKey)
{
    static const struct
    {
        const char* key;
        const wchar_t* label;
    } kKnown[] = {{"MULTIPOINT_SETTING", L"Connect to 2 devices simultaneously"},
                  {"SIDETONE_SETTING", L"Capture voice during a phone call"},
                  {"TOUCH_PANEL_SETTING", L"Touch sensor control panel"},
                  {"ASSIGNABLE_KEY_SETTING", L"Custom button function"}};
    if (!isKey)
        return Widen(text);
    for (const auto& k : kKnown)
        if (text == k.key)
            return k.label;
    return Humanize(text);
}

const wchar_t* AssistantName(MDRVoiceAssistant a)
{
    switch (a)
    {
    case MDR_VOICE_ASSISTANT_NONE: return L"Do not use";
    case MDR_VOICE_ASSISTANT_VOICE_RECOGNITION: return L"Phone's voice assist";
    case MDR_VOICE_ASSISTANT_GOOGLE_ASSISTANT: return L"Google Assistant";
    case MDR_VOICE_ASSISTANT_AMAZON_ALEXA: return L"Amazon Alexa";
    case MDR_VOICE_ASSISTANT_TENCENT_XIAOWEI: return L"Tencent Xiaowei";
    case MDR_VOICE_ASSISTANT_SONY_VOICE_CONTROL: return L"Voice Control (Sony)";
    case MDR_VOICE_ASSISTANT_OTHER_DEVICE: return L"Enabled on another device";
    default: return L"Unknown";
    }
}

const wchar_t* AssignableActionName(MDRAssignableAction a)
{
    static const wchar_t* const kNames[] = {L"No function",
                                            L"Playback control",
                                            L"Ambient sound control",
                                            L"Ambient sound control / Quick Access",
                                            L"Track control",
                                            L"Phone's voice assist",
                                            L"Google Assistant",
                                            L"Amazon Alexa",
                                            L"Tencent Xiaowei",
                                            L"Microsoft Cortana",
                                            L"Quick Access",
                                            L"Volume control",
                                            L"Playback control (voice assistant limited)",
                                            L"Tencent Xiaowei (Q-MSC)",
                                            L"Microsoft Teams",
                                            L"Google Assistant (Bluetooth Classic only)",
                                            L"Amazon Alexa (Bluetooth Classic only)",
                                            L"Tencent Xiaowei (Bluetooth Classic only)",
                                            L"Quick Access (Bluetooth Classic only)",
                                            L"Ambient sound control / Quick Access (Bluetooth Classic only)",
                                            L"Tencent Xiaowei Q-MSC (Bluetooth Classic only)",
                                            L"Ambient sound control / Mic",
                                            L"Listening mode / Quick Access",
                                            L"Ambient sound control / Listening mode",
                                            L"Chat mix",
                                            L"Custom 1",
                                            L"Custom 2"};
    return a < ARRAYSIZE(kNames) ? kNames[a] : L"Unknown";
}

std::wstring AssignableKeyName(const MDRAssignableControl& c)
{
    const wchar_t* kind = c.type == MDR_ASSIGNABLE_ACTION_KEY_TYPE_TOUCH_SENSOR ? L" (touch sensor)"
                          : c.type == MDR_ASSIGNABLE_ACTION_KEY_TYPE_FACE_TAP   ? L" (face tap)"
                          : c.type == MDR_ASSIGNABLE_ACTION_KEY_TYPE_BUTTON     ? L" (button)"
                                                                                : L"";
    switch (c.location)
    {
    case MDR_ASSIGNABLE_ACTION_KEY_LEFT: return std::wstring(L"Left") + kind;
    case MDR_ASSIGNABLE_ACTION_KEY_RIGHT: return std::wstring(L"Right") + kind;
    case MDR_ASSIGNABLE_ACTION_KEY_CUSTOM: return L"Custom button";
    case MDR_ASSIGNABLE_ACTION_KEY_C: return L"C button";
    case MDR_ASSIGNABLE_ACTION_KEY_NC_AMB: return L"NC/AMB button";
    case MDR_ASSIGNABLE_ACTION_KEY_NC_AMBIENT: return L"NC/Ambient button";
    default: return std::wstring(L"Key") + kind;
    }
}

} // namespace

struct DeviceWorker::Impl {
    explicit Impl(HWND hwnd) : notify(hwnd) {}

    // ---- shared, guarded by m ----
    HWND notify;
    mutable std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    bool wantConnect = false, wantDisconnect = false;
    std::string reqMac;
    std::vector<Command> queue;
    Snapshot snap;
    std::vector<BtDevice> devices;
    std::thread th;

    // ---- worker thread only ----
    MDRConnectionWindows* cw = nullptr;
    MDRConnection* conn = nullptr;
    MDRHeadphones* hp = nullptr;
    Link link = Link::Idle;
    std::string mac;
    std::wstring error;
    int service = 0; // index into kServices
    Clock::time_point deadline{}, lastBattery{}, lastList{};
    bool listedOnce = false;
    MDRHeadphonesSyncFlags pendingSync = MDR_HEADPHONES_SYNC_NONE;
    bool alertPending = false;
    std::wstring alertText;
    bool dirty = false; // snapshot needs a rebuild
    // Sound pressure is published at most once per kPressurePeriod (it can change with every device report).
    int shownPressure = -1;
    Clock::time_point pressureAt{};
    bool pressureLate = false; // a newer value is being held back
    Clock::time_point lastLive{};        // last periodic sound-pressure / playback sync
    Clock::time_point playbackRecheck{}; // when to sync playback again after a command; {} = not scheduled
    // Last timed auto-power-off seen outside "when removed" mode (libmdr reads 0 minutes while in it); 0 = none.
    uint32_t lastTimedMinutes = 0;
    mutable bool assignWarned = false, ncButtonWarned = false; // log once per link

    // ---- small libmdr helpers ----
    std::vector<MDRAssignableAction> AssignableActions(MDRAssignableActionKeyLocation key) const
    {
        return List<MDRAssignableAction>([this, key](MDRAssignableAction* o, uint32_t* c)
                                         { return mdrHeadphonesGetAssignableControlActions(hp, key, o, c); });
    }
    bool AssignableOffers(const MDRAssignableControl& ctl, MDRAssignableAction action) const
    {
        const auto actions = AssignableActions(ctl.location);
        return action < MDR_ASSIGNABLE_UNKNOWN && std::find(actions.begin(), actions.end(), action) != actions.end();
    }
    // libmdr validates the whole key array on every write: one key it cannot write back blocks all of them.
    // Returns the position of the first such key, or -1.
    int BadAssignKey(const std::vector<MDRAssignableControl>& controls) const
    {
        for (size_t i = 0; i < controls.size(); ++i)
            if (controls[i].location == MDR_ASSIGNABLE_ACTION_KEY_UNKNOWN ||
                controls[i].type == MDR_ASSIGNABLE_ACTION_KEY_TYPE_UNKNOWN ||
                !AssignableOffers(controls[i], controls[i].action))
                return static_cast<int>(i);
        return -1;
    }
    std::vector<MDRGeneralSettingInfo> GeneralInfos() const
    {
        return List<MDRGeneralSettingInfo>(
            [this](MDRGeneralSettingInfo* o, uint32_t* c) { return mdrHeadphonesGetGeneralSettingInfo(hp, o, c); });
    }
    std::vector<MDRAssignableControl> AssignableControls() const
    {
        return List<MDRAssignableControl>([this](MDRAssignableControl* o, uint32_t* c)
                                          { return mdrHeadphonesGetAssignableControls(hp, MDR_PROPERTY_DESIRED, o, c); });
    }
    bool Has(MDRFeature f) const
    {
        MDRFeatureAvailability a = MDR_AVAILABILITY_UNKNOWN;
        return mdrHeadphonesGetFeature(hp, f, &a) == MDR_RESULT_OK && a == MDR_AVAILABILITY_AVAILABLE;
    }
    std::string Text(MDRText text, uint32_t index = 0) const
    {
        uint32_t size = 0;
        if (mdrHeadphonesGetText(hp, text, index, nullptr, &size) != MDR_RESULT_OK || size == 0)
            return {};
        std::vector<char> buf(static_cast<size_t>(size) + 1, '\0');
        if (mdrHeadphonesGetText(hp, text, index, buf.data(), &size) != MDR_RESULT_OK)
            return {};
        return buf.data();
    }
    bool Value(MDRValue key, uint32_t index, int& out) const
    {
        int32_t v = 0;
        if (mdrHeadphonesGetValue(hp, key, index, &v) != MDR_RESULT_OK)
            return false;
        out = v;
        return true;
    }
    bool GetRange(MDRValue lo, MDRValue hi, MDRValue step, uint32_t index, Range& out) const
    {
        Range r;
        if (!Value(lo, index, r.min) || !Value(hi, index, r.max) || !Value(step, index, r.step) || r.min > r.max ||
            r.step <= 0)
            return false;
        out = r;
        return true;
    }
    template <typename T, typename Query>
    std::vector<T> List(Query query) const
    {
        uint32_t count = 0;
        if (query(static_cast<T*>(nullptr), &count) != MDR_RESULT_OK || count == 0)
            return {};
        std::vector<T> v(count);
        if (query(v.data(), &count) != MDR_RESULT_OK)
            return {};
        v.resize(std::min<size_t>(count, v.size()));
        return v;
    }
    // Index of the boolean general setting whose subject is TOUCH_PANEL_SETTING, or -1.
    int TouchPanelIndex() const
    {
        if (!Has(MDR_FEATURE_GENERAL_SETTINGS))
            return -1;
        const auto infos = List<MDRGeneralSettingInfo>(
            [this](MDRGeneralSettingInfo* o, uint32_t* c) { return mdrHeadphonesGetGeneralSettingInfo(hp, o, c); });
        for (const MDRGeneralSettingInfo& info : infos)
            if (info.type == MDR_GENERAL_SETTING_BOOLEAN &&
                Text(MDR_TEXT_GENERAL_SETTING_SUBJECT, info.index) == "TOUCH_PANEL_SETTING")
                return static_cast<int>(info.index);
        return -1;
    }

    // ---- link state machine ----
    void Teardown()
    {
        if (hp)
            mdrHeadphonesDestroy(hp);
        hp = nullptr;
        if (conn)
            mdrConnectionDisconnect(conn);
        pendingSync = MDR_HEADPHONES_SYNC_NONE;
        alertPending = false;
        alertText.clear();
        shownPressure = -1;
        pressureLate = false;
        playbackRecheck = {};
        lastTimedMinutes = 0;
        assignWarned = ncButtonWarned = false;
        dirty = true;
    }
    void Fail(const std::wstring& why)
    {
        Teardown();
        link = Link::Failed;
        error = why.empty() ? L"Unknown error" : why;
        Log(L"Link failed (..%ls): %ls", MacTail(mac).c_str(), error.c_str());
    }
    std::wstring ConnError(MDRResult r) const
    {
        const char* e = conn ? mdrConnectionGetLastError(conn) : nullptr;
        return Widen(e && *e ? e : mdrResultString(r));
    }
    // Reason for a fatal libmdr result. Call BEFORE Teardown().
    std::wstring DeviceError(MDRResult r) const
    {
        const std::string t = Text(MDR_TEXT_LAST_ERROR);
        if (!t.empty())
            return Widen(t);
        if (r == MDR_RESULT_ERROR_NET)
            return ConnError(r);
        return Widen(mdrResultString(r));
    }
    void StartAttempt(int index, std::wstring lastError)
    {
        for (service = index; service < 2; ++service)
        {
            Log(L"Connecting to ..%ls (protocol V%d)", MacTail(mac).c_str(), service == 0 ? 2 : 1);
            const MDRResult r = mdrConnectionConnect(conn, mac.c_str(), kServices[service]);
            if (r == MDR_RESULT_OK || r == MDR_RESULT_INPROGRESS)
            {
                deadline = Clock::now() + kAttemptTimeout;
                link = Link::Connecting;
                dirty = true;
                return;
            }
            lastError = ConnError(r);
            Log(L"Connect attempt failed: %ls", lastError.c_str());
            mdrConnectionDisconnect(conn);
        }
        Fail(lastError);
    }
    void BeginConnect(const std::string& target)
    {
        Teardown();
        mac = target;
        error.clear();
        if (!conn)
        {
            Fail(L"Bluetooth backend unavailable");
            return;
        }
        StartAttempt(0, {});
    }
    void PollConnect()
    {
        const MDRResult r = mdrConnectionPoll(conn, 0);
        if (r == MDR_RESULT_OK)
        {
            const MDRProtocolVersion proto = service == 0 ? MDR_PROTOCOL_V2 : MDR_PROTOCOL_V1;
            MDRResult cr = mdrHeadphonesCreate(MDR_ABI_VERSION, conn, proto, &hp);
            if (cr != MDR_RESULT_OK)
            {
                hp = nullptr;
                Fail(Widen(mdrResultString(cr)));
                return;
            }
            cr = mdrHeadphonesRequestInit(hp, MDR_HEADPHONES_INIT_DEFAULT);
            if (cr != MDR_RESULT_OK)
            {
                Fail(DeviceError(cr));
                return;
            }
            link = Link::Initializing;
            dirty = true;
            return;
        }
        const bool waiting = r == MDR_RESULT_INPROGRESS || r == MDR_RESULT_ERROR_TIMEOUT;
        if (waiting && Clock::now() < deadline)
            return;
        // The connection's own text is stale ("Connecting...") on a timeout, so name it ourselves.
        const std::wstring why = waiting ? Widen(mdrResultString(MDR_RESULT_ERROR_TIMEOUT)) : ConnError(r);
        Log(L"Connect attempt failed: %ls", why.c_str());
        mdrConnectionDisconnect(conn);
        StartAttempt(service + 1, why);
    }

    // Returns false if the link died.
    bool PollDevice()
    {
        for (int i = 0; i < 64; ++i)
        {
            MDREvent ev = MDR_EVENT_NONE;
            const MDRResult r = mdrHeadphonesPoll(hp, &ev);
            if (r != MDR_RESULT_OK)
            {
                Fail(DeviceError(r));
                return false;
            }
            if (ev == MDR_EVENT_NONE)
                break;
            dirty = true;
            switch (ev)
            {
            case MDR_EVENT_INITIALIZE_COMPLETE:
                pendingSync |= MDR_HEADPHONES_SYNC_DEFAULT;
                lastBattery = Clock::now();
                link = Link::Ready;
                Log(L"Ready: model=%ls firmware=%ls protocol=V%d", Widen(Text(MDR_TEXT_MODEL_NAME)).c_str(),
                    Widen(Text(MDR_TEXT_FIRMWARE_VERSION)).c_str(), service == 0 ? 2 : 1);
                break;
            case MDR_EVENT_NEED_SYNC:
            {
                MDRHeadphonesSyncFlags flags = MDR_HEADPHONES_SYNC_NONE;
                if (mdrHeadphonesGetSync(hp, &flags) == MDR_RESULT_OK)
                    pendingSync |= flags;
                break;
            }
            case MDR_EVENT_ALERT:
                alertText = Widen(Text(MDR_TEXT_LAST_ALERT));
                alertPending = true;
                break;
            default:
                break;
            }
        }
        return true;
    }

    void Check(const wchar_t* what, MDRResult r)
    {
        if (r != MDR_RESULT_OK)
            Log(L"%ls refused: %ls", what, Widen(mdrResultString(r)).c_str());
        dirty = true;
    }

    void Ignored(const Command& c) { Log(L"SetFeature %d ignored: value %d, index %d", c.a, c.b, c.c); }

    void ApplyFeature(const Command& c)
    {
        const Feature f = static_cast<Feature>(c.a);
        const MDRBoolean on = c.b ? MDR_TRUE : MDR_FALSE;
        switch (f)
        {
        case Feature::AdaptiveAmbient:
        case Feature::AdaptiveSensitivity:
        case Feature::NcButton: // V2 only; libmdr refuses all three on V1
        {
            MDRNoiseControl n{};
            if (!Has(f == Feature::NcButton ? MDR_FEATURE_NOISE_CONTROL_BUTTON : MDR_FEATURE_ADAPTIVE_AMBIENT_SOUND) ||
                mdrHeadphonesGetNoiseControl(hp, MDR_PROPERTY_DESIRED, &n) != MDR_RESULT_OK)
                return Ignored(c);
            if (f == Feature::AdaptiveAmbient)
                n.adaptive_ambient = on;
            else if (f == Feature::AdaptiveSensitivity)
            {
                if (c.b < static_cast<int>(MDR_ADAPTIVE_SENSITIVITY_LOW) ||
                    c.b > static_cast<int>(MDR_ADAPTIVE_SENSITIVITY_HIGH))
                    return Ignored(c);
                n.adaptive_sensitivity = static_cast<MDRAdaptiveSensitivity>(c.b);
            }
            else
            {
                // NONE also stands for a device value libmdr cannot map, so it is neither offered nor overwritten.
                if (n.button_mode == MDR_NOISE_BUTTON_NONE || c.b <= static_cast<int>(MDR_NOISE_BUTTON_NONE) ||
                    c.b > static_cast<int>(MDR_NOISE_BUTTON_AMBIENT_OFF))
                    return Ignored(c);
                n.button_mode = static_cast<MDRNoiseButtonMode>(c.b);
            }
            n.changing_asm_level = MDR_FALSE;
            Check(L"NoiseControl", mdrHeadphonesSetNoiseControl(hp, &n));
            break;
        }
        case Feature::ListeningMode:
        case Feature::BgmRoom:
        {
            MDRListening l{};
            if (!Has(MDR_FEATURE_LISTENING_MODE) ||
                mdrHeadphonesGetListening(hp, MDR_PROPERTY_DESIRED, &l) != MDR_RESULT_OK)
                return Ignored(c);
            if (f == Feature::ListeningMode)
            {
                if (c.b < 0 || c.b > static_cast<int>(MDR_LISTENING_SOUND_LEAKAGE_REDUCTION))
                    return Ignored(c);
                l.mode = static_cast<MDRListeningMode>(c.b);
                // Background music needs a room; the device may not have reported one yet.
                if (l.mode == MDR_LISTENING_BACKGROUND_MUSIC && l.background_room == MDR_ROOM_UNKNOWN)
                    l.background_room = MDR_ROOM_SMALL;
            }
            else
            {
                if (c.b < static_cast<int>(MDR_ROOM_SMALL) || c.b > static_cast<int>(MDR_ROOM_LARGE))
                    return Ignored(c);
                l.background_room = static_cast<MDRRoomSize>(c.b);
            }
            Check(L"Listening", mdrHeadphonesSetListening(hp, &l));
            break;
        }
        case Feature::VoiceGuidance:
        case Feature::VoiceGuidanceVolume:
        {
            const bool vol = f == Feature::VoiceGuidanceVolume;
            MDRVoiceGuidance v{};
            Range r;
            if (!Has(vol ? MDR_FEATURE_VOICE_GUIDANCE_VOLUME : MDR_FEATURE_VOICE_GUIDANCE) ||
                mdrHeadphonesGetVoiceGuidance(hp, MDR_PROPERTY_DESIRED, &v) != MDR_RESULT_OK)
                return Ignored(c);
            if (!vol)
                v.enabled = on;
            else if (GetRange(MDR_VALUE_VOICE_GUIDANCE_VOLUME_MIN, MDR_VALUE_VOICE_GUIDANCE_VOLUME_MAX,
                              MDR_VALUE_VOICE_GUIDANCE_VOLUME_STEP, 0, r))
                v.volume = std::clamp(c.b, r.min, r.max);
            else
                return Ignored(c);
            Check(L"VoiceGuidance", mdrHeadphonesSetVoiceGuidance(hp, &v));
            break;
        }
        case Feature::VoiceAssistant: // libmdr checks the value against the advertised, currently allowed options
            if (c.b < 0)
                return Ignored(c);
            Check(L"VoiceAssistant", mdrHeadphonesSetVoiceAssistant(hp, static_cast<MDRVoiceAssistant>(c.b)));
            break;
        case Feature::HeadGesture:
        case Feature::WearingPowerOff:
        {
            MDRPower p{};
            if (!Has(f == Feature::HeadGesture ? MDR_FEATURE_HEAD_GESTURE : MDR_FEATURE_AUTO_POWER_OFF) ||
                mdrHeadphonesGetPower(hp, MDR_PROPERTY_DESIRED, &p) != MDR_RESULT_OK)
                return Ignored(c);
            if (f == Feature::HeadGesture)
                p.head_gesture = on;
            else
            {
                if (p.wearing_power == MDR_WEARING_POWER_UNAVAILABLE)
                    return Ignored(c);
                p.wearing_power = on ? MDR_WEARING_POWER_WHEN_REMOVED : MDR_WEARING_POWER_DISABLED;
                // Leaving the mode goes back to the timer used before it (GetPower reads 0 minutes while in it),
                // else to what libmdr reports; libmdr refuses minutes the device does not offer.
                int count = 0, first = -1;
                bool offered = false, lastOffered = false;
                Value(MDR_VALUE_AUTO_POWER_OFF_OPTION_COUNT, 0, count);
                for (int i = 0; i < count; ++i)
                {
                    int minutes = 0;
                    if (!Value(MDR_VALUE_AUTO_POWER_OFF_OPTION, static_cast<uint32_t>(i), minutes))
                        continue;
                    if (first < 0)
                        first = minutes;
                    offered = offered || minutes == static_cast<int>(p.auto_power_off_minutes);
                    lastOffered = lastOffered || (minutes > 0 && minutes == static_cast<int>(lastTimedMinutes));
                }
                if (!on && lastOffered)
                    p.auto_power_off_minutes = lastTimedMinutes;
                else if (!on && !offered && first >= 0)
                    p.auto_power_off_minutes = static_cast<uint32_t>(first);
            }
            Check(L"Power", mdrHeadphonesSetPower(hp, &p));
            break;
        }
        case Feature::SourceSwitch: // V2 only
            Check(L"SourceSwitch", mdrHeadphonesSetSourceSwitchControl(hp, on));
            break;
        case Feature::PairingMode:
        {
            const MDRPairing p{on};
            Check(L"Pairing", mdrHeadphonesSetPairing(hp, &p));
            break;
        }
        case Feature::General:
        {
            const auto infos = GeneralInfos();
            const auto it = std::find_if(infos.begin(), infos.end(), [&c](const MDRGeneralSettingInfo& i)
                                         { return c.c >= 0 && i.index == static_cast<uint32_t>(c.c); });
            MDRGeneralSetting g{};
            if (it == infos.end() || it->writable == MDR_FALSE ||
                mdrHeadphonesGetGeneralSetting(hp, MDR_PROPERTY_DESIRED, it->index, &g) != MDR_RESULT_OK)
                return Ignored(c);
            if (it->type == MDR_GENERAL_SETTING_BOOLEAN)
                g.boolean_value = on;
            else if (c.b >= 0 && static_cast<uint32_t>(c.b) < it->option_count)
                g.list_value = static_cast<uint32_t>(c.b);
            else
                return Ignored(c);
            Check(L"GeneralSetting", mdrHeadphonesSetGeneralSetting(hp, &g));
            break;
        }
        case Feature::Assignable: // the device takes all keys at once
        {
            auto controls = AssignableControls();
            if (c.c < 0 || static_cast<size_t>(c.c) >= controls.size() || c.b < 0)
                return Ignored(c);
            if (const int bad = BadAssignKey(controls); bad >= 0)
            {
                Log(L"Assignable ignored: key %d (%ls) holds action %u that cannot be written back; set it in the Sony app",
                    bad, AssignableKeyName(controls[static_cast<size_t>(bad)]).c_str(),
                    controls[static_cast<size_t>(bad)].action);
                break;
            }
            if (!AssignableOffers(controls[static_cast<size_t>(c.c)], static_cast<MDRAssignableAction>(c.b)))
            {
                Log(L"Assignable ignored: key %d (%ls) does not offer action %d", c.c,
                    AssignableKeyName(controls[static_cast<size_t>(c.c)]).c_str(), c.b);
                break;
            }
            controls[static_cast<size_t>(c.c)].action = static_cast<MDRAssignableAction>(c.b);
            Check(L"AssignableControls",
                  mdrHeadphonesSetAssignableControls(hp, controls.data(), static_cast<uint32_t>(controls.size())));
            break;
        }
        default:
            Ignored(c);
            break;
        }
    }

    void Apply(const Command& c)
    {
        switch (c.kind)
        {
        case Command::SetFeature:
            ApplyFeature(c);
            break;
        case Command::Playback:
        {
            static const MDRPlaybackAction kActions[] = {MDR_PLAYBACK_PLAY, MDR_PLAYBACK_PAUSE, MDR_PLAYBACK_NEXT,
                                                         MDR_PLAYBACK_PREVIOUS};
            if (c.a < 0 || c.a > 3 || !Has(MDR_FEATURE_PLAYBACK_CONTROL))
                break;
            const MDRPlaybackCommand pc{kActions[c.a]};
            Check(L"Playback", mdrHeadphonesSendPlaybackCommand(hp, &pc));
            // Some models (WH-1000XM5) do not notify playback changes; Flush() commits first, then syncs.
            pendingSync |= MDR_HEADPHONES_SYNC_PLAYBACK;
            playbackRecheck = Clock::now() + kPlaybackRecheck;
            break;
        }
        case Command::SetNc:
        case Command::SetAmbientLevel:
        case Command::SetVoiceFocus:
        {
            MDRNoiseControl n{};
            if (mdrHeadphonesGetNoiseControl(hp, MDR_PROPERTY_DESIRED, &n) != MDR_RESULT_OK)
                break;
            Range amb;
            const bool haveAmb =
                GetRange(MDR_VALUE_AMBIENT_LEVEL_MIN, MDR_VALUE_AMBIENT_LEVEL_MAX, MDR_VALUE_AMBIENT_LEVEL_STEP, 0, amb);
            if (c.kind == Command::SetNc)
            {
                switch (static_cast<NcMode>(c.a))
                {
                case NcMode::NoiseCancelling: n.mode = MDR_NOISE_MODE_CANCELLING; break;
                case NcMode::Ambient:
                    n.mode = MDR_NOISE_MODE_AMBIENT;
                    // Outside ambient mode the device reports level 0, which is not a valid ambient level.
                    if (haveAmb && static_cast<int>(n.ambient_level) < amb.min)
                        n.ambient_level = static_cast<uint32_t>(amb.max);
                    break;
                case NcMode::Wind:
                    if (!Has(MDR_FEATURE_WIND_NOISE_REDUCTION))
                        return;
                    n.mode = MDR_NOISE_MODE_WIND_NOISE_REDUCTION;
                    break;
                default: n.mode = MDR_NOISE_MODE_OFF; break;
                }
            }
            else if (c.kind == Command::SetAmbientLevel)
                n.ambient_level = static_cast<uint32_t>(haveAmb ? std::clamp(c.a, amb.min, amb.max) : std::max(c.a, 0));
            else
                n.focus_on_voice = c.a ? MDR_TRUE : MDR_FALSE;
            n.changing_asm_level = MDR_FALSE;
            Check(L"NoiseControl", mdrHeadphonesSetNoiseControl(hp, &n));
            break;
        }
        case Command::SetVolume:
            Check(L"Volume", mdrHeadphonesSetPlaybackVolume(hp, static_cast<uint32_t>(std::max(c.a, 0))));
            break;
        case Command::SetEqPreset:
            Check(L"EqPreset", mdrHeadphonesSetEqualizerPreset(hp, static_cast<MDREqualizerPreset>(c.a)));
            break;
        case Command::SetEqBands:
        {
            MDREqualizer eq{};
            if (mdrHeadphonesGetEqualizer(hp, MDR_PROPERTY_DESIRED, &eq) != MDR_RESULT_OK)
                break;
            if (c.a <= 0 || c.a > 10 || static_cast<uint32_t>(c.a) != eq.band_count)
            {
                Log(L"EqBands ignored: %d bands sent, device has %u", c.a, eq.band_count);
                break;
            }
            int32_t bands[10]{};
            for (int i = 0; i < c.a; ++i)
                bands[i] = c.bands[static_cast<size_t>(i)];
            Check(L"EqBands", mdrHeadphonesSetEqualizerBands(hp, bands, eq.band_count));
            break;
        }
        case Command::SetClearBass:
            Check(L"ClearBass", mdrHeadphonesSetEqualizerClearBass(hp, c.a));
            break;
        case Command::SetDsee:
            Check(L"DSEE", mdrHeadphonesSetDSEE(hp, c.a ? MDR_TRUE : MDR_FALSE));
            break;
        case Command::SetConnectionPriority:
        {
            MDRConnectionMode cm{};
            cm.audio_priority = c.a ? MDR_AUDIO_PRIORITY_STABILITY : MDR_AUDIO_PRIORITY_QUALITY;
            Check(L"ConnectionMode", mdrHeadphonesSetConnectionMode(hp, &cm));
            break;
        }
        case Command::SetAutoPause:
        {
            MDRPower p{};
            if (mdrHeadphonesGetPower(hp, MDR_PROPERTY_DESIRED, &p) != MDR_RESULT_OK)
                break;
            p.auto_pause = c.a ? MDR_TRUE : MDR_FALSE;
            Check(L"AutoPause", mdrHeadphonesSetPower(hp, &p));
            break;
        }
        case Command::SetSpeakToChat:
        {
            MDRSpeakToChat s{};
            if (mdrHeadphonesGetSpeakToChat(hp, MDR_PROPERTY_DESIRED, &s) != MDR_RESULT_OK)
                break;
            static const MDRSpeechSensitivity kSens[] = {MDR_SPEECH_SENSITIVITY_AUTO, MDR_SPEECH_SENSITIVITY_HIGH,
                                                         MDR_SPEECH_SENSITIVITY_LOW};
            static const MDRSpeakTimeout kTimeout[] = {MDR_SPEAK_TIMEOUT_SHORT, MDR_SPEAK_TIMEOUT_MEDIUM,
                                                       MDR_SPEAK_TIMEOUT_LONG, MDR_SPEAK_TIMEOUT_MANUAL};
            s.enabled = c.a ? MDR_TRUE : MDR_FALSE;
            if (c.b >= 0 && c.b < 3)
                s.sensitivity = kSens[c.b];
            if (c.c >= 0 && c.c < 4)
                s.timeout = kTimeout[c.c];
            Check(L"SpeakToChat", mdrHeadphonesSetSpeakToChat(hp, &s));
            break;
        }
        case Command::SetTouchPanel:
        {
            const int index = TouchPanelIndex();
            MDRGeneralSetting g{};
            if (index < 0 || mdrHeadphonesGetGeneralSetting(hp, MDR_PROPERTY_DESIRED, static_cast<uint32_t>(index),
                                                            &g) != MDR_RESULT_OK)
                break;
            g.boolean_value = c.a ? MDR_TRUE : MDR_FALSE;
            Check(L"TouchPanel", mdrHeadphonesSetGeneralSetting(hp, &g));
            break;
        }
        case Command::SetAutoPowerOff:
        case Command::PowerOff:
        {
            MDRPower p{};
            if (mdrHeadphonesGetPower(hp, MDR_PROPERTY_DESIRED, &p) != MDR_RESULT_OK)
                break;
            if (c.kind == Command::PowerOff)
                p.shutdown_requested = MDR_TRUE;
            else
            {
                p.auto_power_off_minutes = static_cast<uint32_t>(std::max(c.a, 0));
                // In "power off when removed" mode libmdr ignores the minutes value; leave that mode first.
                if (p.wearing_power == MDR_WEARING_POWER_WHEN_REMOVED)
                    p.wearing_power = MDR_WEARING_POWER_DISABLED;
            }
            Check(L"Power", mdrHeadphonesSetPower(hp, &p));
            break;
        }
        case Command::HostConnect:
        case Command::HostDisconnect:
        case Command::HostSelectPlayback:
        {
            MDRPairedDeviceAction a{};
            a.command = c.kind == Command::HostConnect      ? MDR_PAIRED_DEVICE_CONNECT
                        : c.kind == Command::HostDisconnect ? MDR_PAIRED_DEVICE_DISCONNECT
                                                            : MDR_PAIRED_DEVICE_SELECT_PLAYBACK;
            a.device_id = c.text.c_str();
            a.device_id_size = static_cast<uint32_t>(c.text.size());
            Check(L"PairedDevice", mdrHeadphonesSetPairedDevice(hp, &a));
            break;
        }
        case Command::AnswerAlert:
        {
            const MDRResult r =
                mdrHeadphonesSetAlertResponse(hp, c.a ? MDR_ALERT_ACTION_POSITIVE : MDR_ALERT_ACTION_NEGATIVE);
            if (r != MDR_RESULT_INPROGRESS) // answered, or the device no longer asks
            {
                alertPending = false;
                alertText.clear();
            }
            Check(L"AlertResponse", r);
            break;
        }
        case Command::RefreshBattery:
            pendingSync |= MDR_HEADPHONES_SYNC_BATTERY;
            break;
        }
    }

    // Returns false if the link died.
    bool Flush()
    {
        if (!mdrHeadphonesIsReady(hp))
            return true;
        MDRResult r = MDR_RESULT_OK;
        if (mdrHeadphonesIsDirty(hp))
            r = mdrHeadphonesRequestCommit(hp);
        else if (pendingSync != MDR_HEADPHONES_SYNC_NONE)
        {
            const MDRHeadphonesSyncFlags flags = pendingSync;
            pendingSync = MDR_HEADPHONES_SYNC_NONE;
            r = mdrHeadphonesRequestSync(hp, flags);
        }
        if (r == MDR_RESULT_OK)
            return true;
        Fail(DeviceError(r));
        return false;
    }

    // ---- snapshot ----
    void FillDevice(Snapshot& s) const
    {
        s.model = Widen(Text(MDR_TEXT_MODEL_NAME));
        s.firmware = Widen(Text(MDR_TEXT_FIRMWARE_VERSION));

        MDRModel model{};
        if (Has(MDR_FEATURE_IDENTITY) && mdrHeadphonesGetModel(hp, &model) == MDR_RESULT_OK)
        {
            s.hasCodec = true;
            switch (model.audio_codec)
            {
            case MDR_AUDIO_CODEC_UNKNOWN: s.codec = Codec::Unknown; break;
            case MDR_AUDIO_CODEC_SBC: s.codec = Codec::SBC; break;
            case MDR_AUDIO_CODEC_AAC: s.codec = Codec::AAC; break;
            case MDR_AUDIO_CODEC_LDAC: s.codec = Codec::LDAC; break;
            case MDR_AUDIO_CODEC_APTX: s.codec = Codec::AptX; break;
            case MDR_AUDIO_CODEC_APTX_HD: s.codec = Codec::AptXHD; break;
            case MDR_AUDIO_CODEC_LC3: s.codec = Codec::LC3; break;
            default: s.codec = Codec::Other; break;
            }
        }

        const bool single = Has(MDR_FEATURE_BATTERY_SINGLE), lr = Has(MDR_FEATURE_BATTERY_LEFT_RIGHT),
                   cs = Has(MDR_FEATURE_BATTERY_CASE);
        for (const MDRBattery& b :
             List<MDRBattery>([this](MDRBattery* o, uint32_t* c) { return mdrHeadphonesGetBatteries(hp, o, c); }))
        {
            Battery* out = b.part == MDR_BATTERY_MAIN && single ? &s.battSingle
                           : b.part == MDR_BATTERY_LEFT && lr   ? &s.battLeft
                           : b.part == MDR_BATTERY_RIGHT && lr  ? &s.battRight
                           : b.part == MDR_BATTERY_CASE && cs   ? &s.battCase
                                                                : nullptr;
            if (!out)
                continue;
            out->present = true;
            out->percent = static_cast<int>(std::min<uint32_t>(b.level_percent, 100u));
            out->charging = b.charging == MDR_CHARGING_YES        ? Charging::Yes
                            : b.charging == MDR_CHARGING_COMPLETE ? Charging::Full
                                                                  : Charging::No;
        }

        MDRNoiseControl n{};
        if (mdrHeadphonesGetNoiseControl(hp, MDR_PROPERTY_DESIRED, &n) == MDR_RESULT_OK)
        {
            s.hasNc = Has(MDR_FEATURE_NOISE_CANCELLING);
            s.hasAmbient = Has(MDR_FEATURE_AMBIENT_SOUND);
            s.hasWind = Has(MDR_FEATURE_WIND_NOISE_REDUCTION);
            s.hasVoiceFocus = s.hasAmbient;
            s.nc = n.mode == MDR_NOISE_MODE_CANCELLING             ? NcMode::NoiseCancelling
                   : n.mode == MDR_NOISE_MODE_AMBIENT              ? NcMode::Ambient
                   : n.mode == MDR_NOISE_MODE_WIND_NOISE_REDUCTION ? NcMode::Wind
                                                                   : NcMode::Off;
            GetRange(MDR_VALUE_AMBIENT_LEVEL_MIN, MDR_VALUE_AMBIENT_LEVEL_MAX, MDR_VALUE_AMBIENT_LEVEL_STEP, 0,
                     s.ambientRange);
            s.ambientLevel = static_cast<int>(n.ambient_level);
            s.voiceFocus = n.focus_on_voice != MDR_FALSE;
        }

        MDRPlayback pb{};
        if (Has(MDR_FEATURE_PLAYBACK_VOLUME) &&
            mdrHeadphonesGetPlayback(hp, MDR_PROPERTY_DESIRED, &pb) == MDR_RESULT_OK &&
            GetRange(MDR_VALUE_PLAYBACK_VOLUME_MIN, MDR_VALUE_PLAYBACK_VOLUME_MAX, MDR_VALUE_PLAYBACK_VOLUME_STEP, 0,
                     s.volumeRange))
        {
            s.hasVolume = true;
            s.volume = static_cast<int>(pb.volume);
        }

        MDREqualizer eq{};
        const bool haveEq = mdrHeadphonesGetEqualizer(hp, MDR_PROPERTY_DESIRED, &eq) == MDR_RESULT_OK;
        if (haveEq && Has(MDR_FEATURE_EQUALIZER))
        {
            s.hasEq = true;
            s.eqAvailable = eq.available != MDR_FALSE;
            s.eqPreset = static_cast<int>(eq.preset);
            const auto ids = List<MDREqualizerPreset>(
                [this](MDREqualizerPreset* o, uint32_t* c) { return mdrHeadphonesGetEqualizerPresets(hp, o, c); });
            for (uint32_t i = 0; i < ids.size(); ++i)
            {
                if (ids[i] == MDR_EQ_UNKNOWN)
                    continue;
                std::wstring label = Widen(Text(MDR_TEXT_EQUALIZER_PRESET_NAME, i));
                if (label.empty())
                    label = PresetFallbackName(ids[i]);
                s.eqPresets.push_back({static_cast<int>(ids[i]), std::move(label)});
            }
            const auto bands = List<int32_t>(
                [this](int32_t* o, uint32_t* c) { return mdrHeadphonesGetEqualizerBands(hp, MDR_PROPERTY_DESIRED, o, c); });
            if (bands.size() <= s.eqBands.size() && bands.size() == eq.band_count)
            {
                s.eqBandCount = static_cast<int>(bands.size());
                for (size_t i = 0; i < bands.size(); ++i)
                {
                    s.eqBands[i] = bands[i];
                    Value(MDR_VALUE_EQUALIZER_BAND_FREQUENCY, static_cast<uint32_t>(i), s.eqBandHz[i]);
                }
                GetRange(MDR_VALUE_EQUALIZER_BAND_MIN, MDR_VALUE_EQUALIZER_BAND_MAX, MDR_VALUE_EQUALIZER_BAND_STEP,
                         eq.band_count, s.eqBandRange);
            }
            // No range (NOT_SUPPORTED) when the current layout has no Clear Bass, e.g. 10 bands.
            s.hasClearBass = GetRange(MDR_VALUE_EQUALIZER_CLEAR_BASS_MIN, MDR_VALUE_EQUALIZER_CLEAR_BASS_MAX,
                                      MDR_VALUE_EQUALIZER_CLEAR_BASS_STEP, 0, s.clearBassRange);
            s.clearBass = eq.clear_bass;
        }
        if (haveEq && Has(MDR_FEATURE_DSEE))
        {
            s.hasDsee = true;
            s.dseeAvailable = eq.dsee_available != MDR_FALSE;
            s.dsee = eq.dsee_enabled != MDR_FALSE;
            s.dseeName = eq.dsee_type == MDR_DSEE_HX         ? L"DSEE HX"
                         : eq.dsee_type == MDR_DSEE_HX_AI    ? L"DSEE Extreme"
                         : eq.dsee_type == MDR_DSEE_ULTIMATE ? L"DSEE Ultimate"
                                                             : L"DSEE";
        }

        MDRSpeakToChat stc{};
        if (Has(MDR_FEATURE_SPEAK_TO_CHAT) &&
            mdrHeadphonesGetSpeakToChat(hp, MDR_PROPERTY_DESIRED, &stc) == MDR_RESULT_OK)
        {
            s.hasSpeakToChat = true;
            s.speakToChat = stc.enabled != MDR_FALSE;
            s.stcSensitivity = stc.sensitivity == MDR_SPEECH_SENSITIVITY_HIGH ? 1
                               : stc.sensitivity == MDR_SPEECH_SENSITIVITY_LOW ? 2
                                                                               : 0;
            s.stcTimeout = stc.timeout >= MDR_SPEAK_TIMEOUT_SHORT && stc.timeout <= MDR_SPEAK_TIMEOUT_MANUAL
                               ? static_cast<int>(stc.timeout - MDR_SPEAK_TIMEOUT_SHORT)
                               : 0;
        }

        MDRGeneralSetting touch{};
        const int touchIndex = TouchPanelIndex();
        if (touchIndex >= 0 && mdrHeadphonesGetGeneralSetting(hp, MDR_PROPERTY_DESIRED,
                                                              static_cast<uint32_t>(touchIndex), &touch) == MDR_RESULT_OK)
        {
            s.hasTouchPanel = true;
            s.touchPanel = touch.boolean_value != MDR_FALSE;
        }

        MDRPower power{};
        const bool havePower = mdrHeadphonesGetPower(hp, MDR_PROPERTY_DESIRED, &power) == MDR_RESULT_OK;
        if (havePower && Has(MDR_FEATURE_AUTO_POWER_OFF))
        {
            s.hasAutoPowerOff = true;
            // "When removed" replaces the timer (libmdr reads 0 minutes then): no timed option is selected.
            s.autoPowerOffMinutes = power.wearing_power == MDR_WEARING_POWER_WHEN_REMOVED
                                        ? -1
                                        : static_cast<int>(power.auto_power_off_minutes);
            int count = 0;
            Value(MDR_VALUE_AUTO_POWER_OFF_OPTION_COUNT, 0, count);
            for (int i = 0; i < count; ++i)
            {
                int minutes = 0;
                if (!Value(MDR_VALUE_AUTO_POWER_OFF_OPTION, static_cast<uint32_t>(i), minutes))
                    continue;
                s.autoPowerOffOptions.push_back(
                    {minutes, minutes <= 0        ? std::wstring(L"Never")
                              : minutes % 60 == 0 ? std::to_wstring(minutes / 60) + L" h"
                                                  : std::to_wstring(minutes) + L" min"});
            }
        }
        s.hasPowerOff = havePower && Has(MDR_FEATURE_SHUTDOWN);
        {
            MDRPower p{};
            s.hasAutoPause = Has(MDR_FEATURE_AUTO_PAUSE) &&
                             mdrHeadphonesGetPower(hp, MDR_PROPERTY_DESIRED, &p) == MDR_RESULT_OK;
            s.autoPause = s.hasAutoPause && p.auto_pause != MDR_FALSE;
            MDRConnectionMode cm{};
            s.hasConnectionPriority = Has(MDR_FEATURE_CONNECTION_MODE) &&
                                      mdrHeadphonesGetConnectionMode(hp, MDR_PROPERTY_DESIRED, &cm) == MDR_RESULT_OK &&
                                      cm.audio_priority != MDR_AUDIO_PRIORITY_UNKNOWN;
            s.connectionPriority = cm.audio_priority == MDR_AUDIO_PRIORITY_STABILITY ? 1 : 0;
        }

        if (service == 0 && Has(MDR_FEATURE_PAIRED_DEVICE_MANAGEMENT)) // host actions are V2-only in libmdr
        {
            s.hasMultipoint = true;
            for (MDRPairedDevice& d : List<MDRPairedDevice>(
                     [this](MDRPairedDevice* o, uint32_t* c) { return mdrHeadphonesGetPairedDevices(hp, o, c); }))
            {
                d.macAddress[sizeof(d.macAddress) - 1] = '\0';
                d.name[sizeof(d.name) - 1] = '\0';
                s.hosts.push_back(
                    {d.macAddress, Widen(d.name), d.connected != MDR_FALSE, d.playback_device != MDR_FALSE});
            }
        }

        FillExtras(s);

        s.pendingAlert = alertPending;
        s.alertText = alertText;
    }

    // The long tail: every item is gated on the feature flag and on its getter succeeding.
    void FillExtras(Snapshot& s) const
    {
        MDRNoiseControl n{};
        if (mdrHeadphonesGetNoiseControl(hp, MDR_PROPERTY_DESIRED, &n) == MDR_RESULT_OK)
        {
            if (Has(MDR_FEATURE_ADAPTIVE_AMBIENT_SOUND))
            {
                s.adaptiveAmbient = {true, n.adaptive_ambient != MDR_FALSE};
                s.adaptiveSensitivity = {true,
                                         static_cast<int>(n.adaptive_sensitivity),
                                         {Opt(MDR_ADAPTIVE_SENSITIVITY_LOW, L"Low"),
                                          Opt(MDR_ADAPTIVE_SENSITIVITY_STANDARD, L"Standard"),
                                          Opt(MDR_ADAPTIVE_SENSITIVITY_HIGH, L"High")}};
                EnsureOption(s.adaptiveSensitivity.options, s.adaptiveSensitivity.value, L"Not set");
            }
            // NONE is also what libmdr reads for a device value it cannot map (patches/libmdr-nc-button-preserve
            // keeps that raw value on noise-control writes): never offer it, and leave such a device alone.
            if (Has(MDR_FEATURE_NOISE_CONTROL_BUTTON) && n.button_mode != MDR_NOISE_BUTTON_NONE)
                s.ncButton = {true,
                              static_cast<int>(n.button_mode),
                              {Opt(MDR_NOISE_BUTTON_NOISE_AMBIENT_OFF, L"Noise cancelling / Ambient / Off"),
                               Opt(MDR_NOISE_BUTTON_NOISE_AMBIENT, L"Noise cancelling / Ambient"),
                               Opt(MDR_NOISE_BUTTON_NOISE_OFF, L"Noise cancelling / Off"),
                               Opt(MDR_NOISE_BUTTON_AMBIENT_OFF, L"Ambient / Off")}};
            else if (Has(MDR_FEATURE_NOISE_CONTROL_BUTTON) && !ncButtonWarned)
            {
                ncButtonWarned = true;
                Log(L"NC/AMB button function reads as none/unmapped; not offering it");
            }
        }

        MDRListening l{};
        if (Has(MDR_FEATURE_LISTENING_MODE) && mdrHeadphonesGetListening(hp, MDR_PROPERTY_DESIRED, &l) == MDR_RESULT_OK)
        {
            static const struct
            {
                MDRListeningMode mode;
                MDRFeature feature;
                const wchar_t* label;
            } kModes[] = {
                {MDR_LISTENING_STANDARD, MDR_FEATURE_LISTENING_MODE, L"Standard"},
                {MDR_LISTENING_BACKGROUND_MUSIC, MDR_FEATURE_LISTENING_BACKGROUND_MUSIC, L"Background music"},
                {MDR_LISTENING_CINEMA, MDR_FEATURE_LISTENING_CINEMA, L"Cinema"},
                {MDR_LISTENING_VOICE_BOOST, MDR_FEATURE_LISTENING_VOICE_BOOST, L"Voice boost"},
                {MDR_LISTENING_SOUND_LEAKAGE_REDUCTION, MDR_FEATURE_LISTENING_SOUND_LEAKAGE_REDUCTION,
                 L"Sound leakage reduction"}};
            s.listeningMode.has = true;
            s.listeningMode.value = static_cast<int>(l.mode);
            for (const auto& lm : kModes)
            {
                // libmdr refuses a mode the device currently reports unavailable (call, LE Audio, voice assistant).
                MDRListeningStatus st{MDR_TRUE, MDR_LISTENING_BLOCKER_NONE};
                if (!Has(lm.feature))
                    continue;
                mdrHeadphonesGetListeningStatus(hp, lm.mode, &st);
                if (st.available != MDR_FALSE || lm.mode == l.mode)
                    s.listeningMode.options.push_back(Opt(lm.mode, lm.label));
            }
            if (Has(MDR_FEATURE_LISTENING_BACKGROUND_MUSIC))
            {
                s.bgmRoom = {true,
                             static_cast<int>(l.background_room),
                             {Opt(MDR_ROOM_SMALL, L"My room (small)"), Opt(MDR_ROOM_MEDIUM, L"Living room (medium)"),
                              Opt(MDR_ROOM_LARGE, L"Cafe (large)")}};
                EnsureOption(s.bgmRoom.options, s.bgmRoom.value, L"Not set");
            }
        }

        MDRVoiceGuidance vg{};
        if (Has(MDR_FEATURE_VOICE_GUIDANCE) &&
            mdrHeadphonesGetVoiceGuidance(hp, MDR_PROPERTY_DESIRED, &vg) == MDR_RESULT_OK)
        {
            s.voiceGuidance = {true, vg.enabled != MDR_FALSE};
            s.hasVoiceGuidanceVolume =
                Has(MDR_FEATURE_VOICE_GUIDANCE_VOLUME) &&
                GetRange(MDR_VALUE_VOICE_GUIDANCE_VOLUME_MIN, MDR_VALUE_VOICE_GUIDANCE_VOLUME_MAX,
                         MDR_VALUE_VOICE_GUIDANCE_VOLUME_STEP, 0, s.voiceGuidanceRange);
            if (s.hasVoiceGuidanceVolume)
                s.voiceGuidanceVolume = vg.volume;
        }

        MDRVoiceAssistantSettings va{};
        if (Has(MDR_FEATURE_VOICE_ASSISTANT) &&
            mdrHeadphonesGetVoiceAssistant(hp, MDR_PROPERTY_DESIRED, &va) == MDR_RESULT_OK)
        {
            s.voiceAssistant.has = true;
            s.voiceAssistant.value = static_cast<int>(va.assistant);
            if (va.available != MDR_FALSE) // otherwise the device refuses every change right now
                for (const MDRVoiceAssistantOption& o :
                     List<MDRVoiceAssistantOption>([this](MDRVoiceAssistantOption* out, uint32_t* c)
                                                   { return mdrHeadphonesGetVoiceAssistantOptions(hp, out, c); }))
                    if (o.available != MDR_FALSE && o.assistant <= MDR_VOICE_ASSISTANT_SONY_VOICE_CONTROL)
                        EnsureOption(s.voiceAssistant.options, static_cast<int>(o.assistant),
                                     AssistantName(o.assistant));
            EnsureOption(s.voiceAssistant.options, s.voiceAssistant.value, AssistantName(va.assistant));
        }

        MDRPower power{};
        if (mdrHeadphonesGetPower(hp, MDR_PROPERTY_DESIRED, &power) == MDR_RESULT_OK)
        {
            if (Has(MDR_FEATURE_HEAD_GESTURE))
                s.headGesture = {true, power.head_gesture != MDR_FALSE};
            if (Has(MDR_FEATURE_AUTO_POWER_OFF) && power.wearing_power != MDR_WEARING_POWER_UNAVAILABLE)
                s.wearingPowerOff = {true, power.wearing_power == MDR_WEARING_POWER_WHEN_REMOVED};
        }

        MDRBoolean autoSwitch = MDR_FALSE;
        if (Has(MDR_FEATURE_SOURCE_SWITCH_CONTROL) &&
            mdrHeadphonesGetSourceSwitchControl(hp, MDR_PROPERTY_DESIRED, &autoSwitch) == MDR_RESULT_OK)
            s.sourceSwitch = {true, autoSwitch != MDR_FALSE};

        MDRPairing pairing{};
        if (Has(MDR_FEATURE_PAIRING_MODE) && mdrHeadphonesGetPairing(hp, MDR_PROPERTY_DESIRED, &pairing) == MDR_RESULT_OK)
            s.pairingMode = {true, pairing.enabled != MDR_FALSE};

        MDRSafeListening safe{};
        if (Has(MDR_FEATURE_SAFE_LISTENING) &&
            mdrHeadphonesGetSafeListening(hp, MDR_PROPERTY_CURRENT, &safe) == MDR_RESULT_OK)
        {
            s.hasSafeListening = true;
            s.soundPressure = static_cast<int>(safe.sound_pressure);
        }

        MDRWearingStatus wearing = MDR_WEARING_STATUS_UNKNOWN;
        if (Has(MDR_FEATURE_WEARING_STATUS) && mdrHeadphonesGetWearingStatus(hp, &wearing) == MDR_RESULT_OK)
            s.wearing = static_cast<int>(wearing); // same numbering as Snapshot::wearing

        MDRPlayback pb{};
        if (Has(MDR_FEATURE_PLAYBACK_CONTROL) && mdrHeadphonesGetPlayback(hp, MDR_PROPERTY_CURRENT, &pb) == MDR_RESULT_OK)
        {
            s.hasPlaybackControl = true;
            s.playState = pb.status == MDR_PLAYBACK_PLAYING   ? 1
                          : pb.status == MDR_PLAYBACK_UNKNOWN ? 0
                                                              : 2; // paused or stopped
        }
        if (Has(MDR_FEATURE_PLAYBACK_METADATA))
        {
            s.hasTrackInfo = true;
            s.trackTitle = Widen(Text(MDR_TEXT_TRACK_TITLE));
            s.trackArtist = Widen(Text(MDR_TEXT_TRACK_ARTIST));
            s.trackAlbum = Widen(Text(MDR_TEXT_TRACK_ALBUM));
        }

        if (Has(MDR_FEATURE_GENERAL_SETTINGS))
        {
            const int touchIndex = TouchPanelIndex();
            for (const MDRGeneralSettingInfo& info : GeneralInfos())
            {
                MDRGeneralSetting g{};
                if (static_cast<int>(info.index) == touchIndex ||
                    (info.type != MDR_GENERAL_SETTING_BOOLEAN && info.type != MDR_GENERAL_SETTING_LIST) ||
                    mdrHeadphonesGetGeneralSetting(hp, MDR_PROPERTY_DESIRED, info.index, &g) != MDR_RESULT_OK)
                    continue;
                GeneralItem item;
                item.index = static_cast<int>(info.index);
                item.label = GeneralLabel(Text(MDR_TEXT_GENERAL_SETTING_SUBJECT, info.index), info.text_is_key != MDR_FALSE);
                if (item.label.empty())
                    item.label = L"Setting " + std::to_wstring(info.index + 1);
                item.isBool = info.type == MDR_GENERAL_SETTING_BOOLEAN;
                item.on = g.boolean_value != MDR_FALSE;
                item.value = static_cast<int>(g.list_value);
                item.writable = info.writable != MDR_FALSE;
                for (uint32_t i = 0; !item.isBool && i < info.option_count; ++i)
                {
                    const uint32_t oi = MDR_GENERAL_SETTING_OPTION_INDEX(info.index, i);
                    int isKey = 0;
                    Value(MDR_VALUE_GENERAL_SETTING_OPTION_TEXT_IS_KEY, oi, isKey);
                    std::wstring label = GeneralLabel(Text(MDR_TEXT_GENERAL_SETTING_OPTION_SUBJECT, oi), isKey != 0);
                    if (label.empty())
                        label = L"Option " + std::to_wstring(i + 1);
                    item.options.push_back(Opt(i, std::move(label)));
                }
                s.general.push_back(std::move(item));
            }
        }

        if (Has(MDR_FEATURE_ASSIGNABLE_CONTROLS))
        {
            const auto controls = AssignableControls();
            // One key libmdr cannot write back blocks every write: show all keys read-only (current action only).
            const int bad = BadAssignKey(controls);
            if (bad >= 0 && !assignWarned)
            {
                assignWarned = true;
                Log(L"Assignable keys are read-only: key %d (%ls) holds action %u that cannot be written back", bad,
                    AssignableKeyName(controls[static_cast<size_t>(bad)]).c_str(),
                    controls[static_cast<size_t>(bad)].action);
            }
            for (const MDRAssignableControl& ctl : controls) // one entry per key: Command index = position
            {
                AssignKey key;
                key.label = AssignableKeyName(ctl);
                key.action = static_cast<int>(ctl.action);
                for (const MDRAssignableAction a : AssignableActions(ctl.location))
                    if (bad < 0 && a < MDR_ASSIGNABLE_UNKNOWN)
                        EnsureOption(key.options, static_cast<int>(a), AssignableActionName(a));
                EnsureOption(key.options, key.action,
                             ctl.action < MDR_ASSIGNABLE_UNKNOWN ? AssignableActionName(ctl.action)
                                                                 : L"Unknown (set in Sony app)");
                s.assignable.push_back(std::move(key));
            }
        }
    }

    void Publish()
    {
        Snapshot s;
        s.link = link;
        s.mac = mac;
        if (link == Link::Failed)
            s.error = error;
        if (hp)
        {
            s.protocol = service == 0 ? 2 : 1;
            MDRPower power{};
            if (mdrHeadphonesGetPower(hp, MDR_PROPERTY_DESIRED, &power) == MDR_RESULT_OK &&
                power.wearing_power != MDR_WEARING_POWER_WHEN_REMOVED && power.auto_power_off_minutes != 0)
                lastTimedMinutes = power.auto_power_off_minutes;
            FillDevice(s);
            if (s.hasSafeListening)
            {
                const auto now = Clock::now();
                pressureLate = shownPressure >= 0 && s.soundPressure != shownPressure && now - pressureAt < kPressurePeriod;
                if (pressureLate)
                    s.soundPressure = shownPressure;
                else if (s.soundPressure != shownPressure)
                {
                    shownPressure = s.soundPressure;
                    pressureAt = now;
                }
            }
        }
        {
            std::lock_guard lk(m);
            if (s == snap)
                return;
            snap = std::move(s);
        }
        if (notify)
            PostMessageW(notify, WM_XM_STATE, 0, 0);
    }

    // ---- Windows' connected-device list (separate, short-lived connection object) ----
    void RefreshList()
    {
        MDRConnectionWindows* tmp = mdrConnectionWindowsCreate();
        if (!tmp)
            return;
        MDRConnection* c = mdrConnectionWindowsGet(tmp);
        MDRDeviceInfo* list = nullptr;
        int count = 0;
        const bool ok = mdrConnectionGetDevicesList(c, &list, &count) == MDR_RESULT_OK;
        std::vector<BtDevice> found;
        bool linkedPresent = false;
        for (int i = 0; ok && list && i < count; ++i)
        {
            list[i].szDeviceName[sizeof(list[i].szDeviceName) - 1] = '\0';
            list[i].szDeviceMacAddress[sizeof(list[i].szDeviceMacAddress) - 1] = '\0';
            std::string devMac = list[i].szDeviceMacAddress;
            for (char& ch : devMac)
                if (ch >= 'a' && ch <= 'f')
                    ch = static_cast<char>(ch - 'a' + 'A');
            if (_stricmp(devMac.c_str(), mac.c_str()) == 0)
                linkedPresent = true;
            std::string name = list[i].szDeviceName;
            if (_strnicmp(name.c_str(), "LE_", 3) == 0)
                name.erase(0, 3);
            if (LooksLikeSony(name))
                found.push_back({std::move(devMac), Widen(name)});
        }
        if (list)
            mdrConnectionFreeDevicesList(c, &list);
        mdrConnectionWindowsDestroy(tmp);
        if (!ok)
            return;

        if (hp && !linkedPresent)
        {
            Log(L"Device ..%ls left Windows' connected list; dropping link", MacTail(mac).c_str());
            Teardown();
            link = Link::Idle;
        }
        bool changed = false;
        {
            std::lock_guard lk(m);
            if (!(found == devices))
            {
                devices = std::move(found);
                changed = true;
            }
        }
        if (changed && notify)
            PostMessageW(notify, WM_XM_STATE, 0, 0);
    }

    // ---- thread ----
    void Tick(std::vector<Command>& cmds, bool doConnect, bool doDisconnect, const std::string& target)
    {
        if (doDisconnect)
        {
            if (link != Link::Idle)
                Log(L"Disconnect ..%ls", MacTail(mac).c_str());
            Teardown();
            link = Link::Idle;
            mac.clear();
            error.clear();
        }
        else if (doConnect)
            BeginConnect(target);

        const auto now = Clock::now();
        // Enumeration can block; never do it while a libmdr request is in flight (its ACK timer keeps running).
        if (!listedOnce || (now - lastList >= kListPeriod && (!hp || mdrHeadphonesIsReady(hp))))
        {
            listedOnce = true;
            lastList = now;
            RefreshList();
        }

        if (link == Link::Connecting)
            PollConnect();
        if (hp && PollDevice())
        {
            if (link == Link::Ready)
            {
                for (const Command& c : cmds)
                    Apply(c);
                if (now - lastBattery >= kBatteryPeriod)
                {
                    lastBattery = now;
                    pendingSync |= MDR_HEADPHONES_SYNC_BATTERY;
                }
                // These only set pending flags: Flush() still commits first and never overlaps a request.
                const bool playback = Has(MDR_FEATURE_PLAYBACK_CONTROL) || Has(MDR_FEATURE_PLAYBACK_METADATA);
                if (now - lastLive >= kLivePeriod)
                {
                    lastLive = now;
                    if (Has(MDR_FEATURE_SAFE_LISTENING))
                        pendingSync |= MDR_HEADPHONES_SYNC_SAFE_LISTENING;
                    if (playback)
                        pendingSync |= MDR_HEADPHONES_SYNC_PLAYBACK;
                }
                if (playbackRecheck != Clock::time_point{} && now >= playbackRecheck)
                {
                    playbackRecheck = {};
                    if (playback)
                        pendingSync |= MDR_HEADPHONES_SYNC_PLAYBACK;
                }
            }
            Flush();
        }

        if (pressureLate && Clock::now() - pressureAt >= kPressurePeriod)
            dirty = true; // publish the held-back sound pressure

        if (dirty)
        {
            dirty = false;
            Publish();
        }
    }

    void Run()
    {
        cw = mdrConnectionWindowsCreate();
        conn = cw ? mdrConnectionWindowsGet(cw) : nullptr;
        for (;;)
        {
            std::vector<Command> cmds;
            bool doConnect = false, doDisconnect = false;
            std::string target;
            {
                std::unique_lock lk(m);
                const auto wait = std::chrono::milliseconds(link == Link::Idle || link == Link::Failed ? 250 : 25);
                cv.wait_for(lk, wait, [this] { return stop || wantConnect || wantDisconnect || !queue.empty(); });
                if (stop)
                    break;
                cmds.swap(queue);
                std::swap(doConnect, wantConnect);
                std::swap(doDisconnect, wantDisconnect);
                target = reqMac;
            }
            try
            {
                Tick(cmds, doConnect, doDisconnect, target);
            }
            catch (...)
            {
                try
                {
                    Fail(L"Internal error");
                    dirty = false;
                    Publish();
                }
                catch (...)
                {
                }
            }
        }
        Teardown();
        if (cw)
            mdrConnectionWindowsDestroy(cw);
        cw = nullptr;
        conn = nullptr;
    }
};

DeviceWorker::DeviceWorker(HWND notify) : impl_(new Impl(notify))
{
    impl_->th = std::thread([p = impl_] { p->Run(); });
}

DeviceWorker::~DeviceWorker()
{
    {
        std::lock_guard lk(impl_->m);
        impl_->stop = true;
    }
    impl_->cv.notify_all();
    if (impl_->th.joinable())
        impl_->th.join();
    delete impl_;
}

void DeviceWorker::Connect(const std::string& mac)
{
    {
        std::lock_guard lk(impl_->m);
        impl_->reqMac = mac;
        impl_->wantConnect = true;
        impl_->wantDisconnect = false;
        impl_->queue.clear();
    }
    impl_->cv.notify_all();
}

void DeviceWorker::Disconnect()
{
    {
        std::lock_guard lk(impl_->m);
        impl_->wantDisconnect = true;
        impl_->wantConnect = false;
        impl_->queue.clear();
    }
    impl_->cv.notify_all();
}

void DeviceWorker::Post(const Command& cmd)
{
    {
        std::lock_guard lk(impl_->m);
        if (impl_->snap.link != Link::Ready || impl_->wantConnect || impl_->wantDisconnect)
            return;
        impl_->queue.push_back(cmd);
    }
    impl_->cv.notify_all();
}

Snapshot DeviceWorker::Get() const
{
    std::lock_guard lk(impl_->m);
    return impl_->snap;
}

std::vector<BtDevice> DeviceWorker::ListConnected() const
{
    std::lock_guard lk(impl_->m);
    return impl_->devices;
}

} // namespace xm
