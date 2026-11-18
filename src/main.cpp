// main.cpp - wires DeviceWorker, Audio, Tray, Flyout and Settings together.
#include "Core.h"
#include <shellapi.h>
#include <objbase.h>
#include <algorithm>
#include <memory>

using namespace xm;

namespace {

constexpr wchar_t kClass[] = L"XMTrayMain";
constexpr wchar_t kMutex[] = L"Local\\XMTray-6f1d2c5e-single-instance";
constexpr UINT_PTR kTimer = 1;
constexpr ULONGLONG kGraceMs = 30'000;          // keep the link this long after the flyout closes
constexpr ULONGLONG kRefreshEveryMs = 15 * 60'000; // background battery refresh while hidden
constexpr ULONGLONG kRefreshWindowMs = 25'000;
constexpr ULONGLONG kRetryMs = 15'000;

bool Contains(const std::vector<BtDevice>& v, const std::string& mac) {
    return std::any_of(v.begin(), v.end(), [&](const BtDevice& d) { return d.mac == mac; });
}

struct App final : AppHost {
    HWND hwnd = nullptr;
    HINSTANCE inst = nullptr;
    UINT taskbarCreated = 0;
    Settings cfg;
    std::unique_ptr<DeviceWorker> worker;
    std::unique_ptr<Audio> audio;
    std::unique_ptr<Dolby> dolby;
    std::unique_ptr<Tray> tray;
    std::unique_ptr<Flyout> flyout;

    Snapshot snap;       // latest from the worker
    Snapshot lastReady;  // last Ready snapshot (shown in the tray while disconnected on demand)
    std::vector<BtDevice> devices;
    std::vector<Endpoint> endpoints; // cached; refreshed only on WM_XM_AUDIO
    bool devicesKnown = false;
    ULONGLONG startedAt = GetTickCount64();
    std::string target;  // headphone we control
    bool wasVisible = false;
    ULONGLONG graceUntil = 0, refreshUntil = 0, lastReadyAt = 0, lastAttemptAt = 0, lastRefreshAt = 0;
    bool lowNotified = false;
    bool demo = false; // --demo: no Bluetooth, a fake headphone, commands applied locally (UI testing)

    void StartDemo() {
        demo = true;
        Snapshot& s = snap;
        s.link = Link::Ready;
        s.mac = "00:00:00:00:00:01";
        s.model = L"WH-1000XM6 (demo)";
        s.firmware = L"0.0.0";
        s.protocol = 2;
        s.battSingle = {true, 72, Charging::No};
        s.hasCodec = true;
        s.codec = Codec::LDAC;
        s.hasNc = s.hasAmbient = s.hasVoiceFocus = true;
        s.nc = NcMode::Ambient;
        s.ambientRange = {1, 20, 1};
        s.ambientLevel = 12;
        s.hasVolume = true;
        s.volumeRange = {0, 30, 1};
        s.volume = 14;
        s.hasEq = true;
        s.eqPresets = {{0, L"Off"}, {16, L"Heavy"}, {17, L"Clear"}, {18, L"Hard"}, {19, L"Soft"}, {24, L"Custom"}};
        s.eqPreset = 24;
        s.eqBandCount = 10;
        s.eqBands = {3, 2, 0, -1, -2, 0, 1, 2, 4, 5};
        s.eqBandHz = {31, 63, 125, 250, 500, 1000, 2000, 4000, 8000, 16000};
        s.eqBandRange = {-6, 6, 1};
        s.hasDsee = s.dseeAvailable = s.dsee = true;
        s.dseeName = L"DSEE Extreme";
        s.hasSpeakToChat = true;
        s.hasTouchPanel = s.touchPanel = true;
        s.hasAutoPowerOff = s.hasPowerOff = true;
        s.autoPowerOffMinutes = 30;
        s.autoPowerOffOptions = {{0, L"Never"}, {15, L"15 min"}, {30, L"30 min"}, {60, L"1 h"}, {180, L"3 h"}};
        s.hasMultipoint = true;
        s.hosts = {{"00:00:00:00:00:0A", L"This PC", true, true}, {"00:00:00:00:00:0B", L"Phone", true, false}};
        s.hasConnectionPriority = true;
        s.hasAutoPause = s.autoPause = true;
        s.adaptiveAmbient = {true, false};
        s.adaptiveSensitivity = {true, 0, {{0, L"Standard"}, {1, L"High"}, {2, L"Low"}}};
        s.ncButton = {true, 0, {{0, L"NC / Ambient / Off"}, {1, L"NC / Ambient"}, {2, L"Ambient / Off"}}};
        s.listeningMode = {true, 0, {{0, L"Standard"}, {1, L"Background music"}, {2, L"Cinema"}}};
        s.bgmRoom = {true, 1, {{0, L"Small"}, {1, L"Medium"}, {2, L"Large"}}};
        s.voiceGuidance = {true, true};
        s.hasVoiceGuidanceVolume = true;
        s.voiceGuidanceRange = {-2, 2, 1};
        s.voiceAssistant = {true, 1, {{0, L"None"}, {1, L"Voice assist (phone)"}, {2, L"Google Assistant"}, {3, L"Amazon Alexa"}}};
        s.headGesture = {true, true};
        s.wearingPowerOff = {true, false};
        s.sourceSwitch = {true, true};
        s.pairingMode = {true, false};
        s.hasSafeListening = true;
        s.soundPressure = 68;
        s.wearing = 1;
        s.hasPlaybackControl = s.hasTrackInfo = true;
        s.playState = 1;
        s.trackTitle = L"Demo track with a rather long title to test ellipsis";
        s.trackArtist = L"Demo artist";
        s.trackAlbum = L"Demo album";
        s.general = {{1, L"Connect to 2 devices simultaneously", true, true, 0, {}, true},
                     {2, L"Sidetone", false, false, 1, {{0, L"Off"}, {1, L"Low"}, {2, L"High"}}, true}};
        s.assignable = {{L"Touch sensor", 1, {{0, L"None"}, {1, L"Playback control"}, {2, L"Ambient sound control"}}},
                        {L"NC/AMB button", 2, {{2, L"Ambient sound control"}, {3, L"Quick access"}}}};
        devices = {{s.mac, L"WH-1000XM6 (demo)"}, {"00:00:00:00:00:02", L"WF-1000XM5 (demo)"}};
        devicesKnown = true;
        target = s.mac;
    }

    void DemoApply(const Command& c) {
        Snapshot& s = snap;
        switch (c.kind) {
        case Command::SetNc: s.nc = NcMode(c.a); break;
        case Command::SetAmbientLevel: s.ambientLevel = c.a; break;
        case Command::SetVoiceFocus: s.voiceFocus = c.a != 0; break;
        case Command::SetVolume: s.volume = c.a; break;
        case Command::SetEqPreset: s.eqPreset = c.a; break;
        case Command::SetEqBands: s.eqBands = c.bands; break;
        case Command::SetClearBass: s.clearBass = c.a; break;
        case Command::SetDsee: s.dsee = c.a != 0; break;
        case Command::SetSpeakToChat: s.speakToChat = c.a != 0; s.stcSensitivity = c.b; s.stcTimeout = c.c; break;
        case Command::SetTouchPanel: s.touchPanel = c.a != 0; break;
        case Command::SetAutoPowerOff: s.autoPowerOffMinutes = c.a; break;
        case Command::SetConnectionPriority: s.connectionPriority = c.a; break;
        case Command::SetAutoPause: s.autoPause = c.a != 0; break;
        case Command::Playback:
            if (c.a == 0) s.playState = 1;
            else if (c.a == 1) s.playState = 2;
            break;
        case Command::SetFeature:
            switch (Feature(c.a)) {
            case Feature::AdaptiveAmbient: s.adaptiveAmbient.on = c.b != 0; break;
            case Feature::AdaptiveSensitivity: s.adaptiveSensitivity.value = c.b; break;
            case Feature::NcButton: s.ncButton.value = c.b; break;
            case Feature::ListeningMode: s.listeningMode.value = c.b; break;
            case Feature::BgmRoom: s.bgmRoom.value = c.b; break;
            case Feature::VoiceGuidance: s.voiceGuidance.on = c.b != 0; break;
            case Feature::VoiceGuidanceVolume: s.voiceGuidanceVolume = c.b; break;
            case Feature::VoiceAssistant: s.voiceAssistant.value = c.b; break;
            case Feature::HeadGesture: s.headGesture.on = c.b != 0; break;
            case Feature::WearingPowerOff: s.wearingPowerOff.on = c.b != 0; break;
            case Feature::SourceSwitch: s.sourceSwitch.on = c.b != 0; break;
            case Feature::PairingMode: s.pairingMode.on = c.b != 0; break;
            case Feature::General:
                for (GeneralItem& g : s.general)
                    if (g.index == c.c) (g.isBool ? g.on = c.b != 0, 0 : g.value = c.b);
                break;
            case Feature::Assignable:
                if (c.c >= 0 && size_t(c.c) < s.assignable.size()) s.assignable[size_t(c.c)].action = c.b;
                break;
            }
            break;
        default: break;
        }
        if (tray) tray->Update(s);
        flyout->Refresh();
    }

    // ---- AppHost ----
    Snapshot State() override { return snap; }
    std::vector<BtDevice> Devices() override { return devices; }
    std::vector<Endpoint> Endpoints() override { return endpoints; }
    void Send(const Command& c) override {
        if (demo) DemoApply(c);
        else worker->Post(c);
    }
    // The Windows output that belongs to the selected headphone ("" = the default output).
    std::wstring TargetEndpoint() const {
        for (const Endpoint& e : endpoints)
            if (!target.empty() && e.btMac == target) return e.id;
        return {};
    }
    // Spatial sound is per output device: only ever look at / change the selected headphone's own endpoint.
    // (Demo mode has no such endpoint; it shows this PC's default output read-only so the page looks real.)
    DolbyState DolbyStatus() override {
        const std::wstring id = TargetEndpoint();
        DolbyState s = dolby->Get(id);
        if (id.empty()) {
            s.canToggle = false;
            if (!demo) {
                s.statusKnown = s.spatialOn = s.atmosOn = s.canSetMode = false;
                s.formatName.clear();
                s.note = L"The selected headphones are not a Windows audio output right now. Connect them in Windows "
                         L"to see and change spatial sound.";
            }
        } else {
            for (const Endpoint& e : endpoints)
                if (e.id == id) s.endpointName = e.name;
        }
        return s;
    }
    void SetDolbyEnabled(bool on) override {
        const std::wstring id = TargetEndpoint();
        if (demo || id.empty()) return; // never touch the default output or the system from demo mode
        if (!dolby->SetEnabled(id, on)) Log(L"dolby: could not start the spatial sound switch");
        flyout->Refresh();
    }
    void SetDolbyMode(int mode) override {
        const bool ok = !demo && !TargetEndpoint().empty() && dolby->SetMode(mode);
        if (!ok) Log(L"dolby: mode switch %d not started", mode);
        if (ok && cfg.resetEqOnDolbyMode && snap.link == Link::Ready && snap.hasEq && snap.eqBandCount > 0) {
            Command c; // flat curve, so the Dolby tuning and the headphone EQ do not stack
            c.kind = Command::SetEqBands;
            c.a = snap.eqBandCount;
            Send(c);
            if (snap.hasClearBass) {
                Command b;
                b.kind = Command::SetClearBass;
                Send(b);
            }
        }
        flyout->Refresh();
    }
    void OpenDolbyAccess() override {
        if (!demo && !dolby->OpenApp()) Log(L"dolby: could not open Dolby Access");
    }
    Settings& Config() override { return cfg; }
    void ConfigChanged() override {
        if (dolby) dolby->SetAutomation(cfg.dolbyAutomation);
        if (!demo) SaveSettings(cfg); // demo mode never touches the real settings file
    }
    void OpenLogFolder() override {
        ShellExecuteW(nullptr, L"open", DataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    void Quit() override { DestroyWindow(hwnd); }

    void SelectDevice(const std::string& mac) override {
        if (mac.empty() || demo) return;
        target = mac;
        if (cfg.lastMac != mac) {
            cfg.lastMac = mac;
            if (!demo) SaveSettings(cfg);
        }
        if (cfg.routeAudioOnSelect) {
            for (const Endpoint& e : endpoints)
                if (e.btMac == mac && !e.isDefault) {
                    if (!audio->SetDefault(e.id)) Log(L"audio: could not make the headphone the default output");
                    break;
                }
        }
        graceUntil = GetTickCount64() + kGraceMs;
        lastAttemptAt = GetTickCount64();
        worker->Connect(mac);
    }

    // ---- policy ----
    void PickTarget() {
        if (!target.empty() && Contains(devices, target)) return;
        target.clear();
        if (devices.empty()) return;
        if (Contains(devices, cfg.lastMac)) {
            target = cfg.lastMac;
            return;
        }
        if (cfg.followWindowsOutput)
            for (const Endpoint& e : endpoints)
                if (e.isDefault && Contains(devices, e.btMac)) {
                    target = e.btMac;
                    return;
                }
        target = devices.front().mac;
    }

    void Reconcile() {
        if (demo) return;
        const ULONGLONG now = GetTickCount64();
        const bool visible = flyout->Visible();
        if (wasVisible && !visible) graceUntil = now + kGraceMs;
        wasVisible = visible;

        const bool present = !target.empty() && Contains(devices, target);
        if (present && !visible && cfg.disconnectWhenHidden && now >= refreshUntil &&
            now - std::max(lastReadyAt, lastRefreshAt) > kRefreshEveryMs) {
            lastRefreshAt = now; // one window per period, whether or not it reaches Ready
            refreshUntil = now + kRefreshWindowMs;
        }

        const bool want =
            present && (!cfg.disconnectWhenHidden || visible || now < graceUntil || now < refreshUntil);
        const bool linkedToTarget = snap.link != Link::Idle && snap.mac == target;

        if (!want) {
            if (snap.link != Link::Idle) worker->Disconnect();
            return;
        }
        if (linkedToTarget && snap.link != Link::Failed) return;
        if (now - lastAttemptAt < (snap.link == Link::Failed ? kRetryMs : 2'000)) return;
        lastAttemptAt = now;
        worker->Connect(target);
    }

    static int LowestBattery(const Snapshot& s, bool* charging) {
        int low = -1;
        *charging = false;
        for (const Battery* b : {&s.battSingle, &s.battLeft, &s.battRight}) {
            if (!b->present) continue;
            if (b->charging != Charging::No) *charging = true;
            if (low < 0 || b->percent < low) low = b->percent;
        }
        return low;
    }

    void OnState() {
        const std::vector<BtDevice> now = worker->ListConnected();
        if (devicesKnown && cfg.notifyConnect && GetTickCount64() - startedAt > 8'000) {
            for (const BtDevice& d : now)
                if (!Contains(devices, d.mac)) tray->Notify(d.name, L"Connected");
            for (const BtDevice& d : devices)
                if (!Contains(now, d.mac)) tray->Notify(d.name, L"Disconnected");
        }
        devices = now;
        devicesKnown = true;

        snap = worker->Get();
        PickTarget();

        if (snap.link == Link::Ready) {
            lastReady = snap;
            lastReadyAt = GetTickCount64();
            if (GetTickCount64() < refreshUntil && !flyout->Visible()) {
                // background refresh got what it came for; let the link drop shortly
                refreshUntil = 0;
                graceUntil = std::max(graceUntil, GetTickCount64() + 4'000);
            }
            bool charging = false;
            const int low = LowestBattery(snap, &charging);
            if (low >= 0) {
                if (charging || low > cfg.lowBatteryPercent + 5) lowNotified = false;
                else if (low <= cfg.lowBatteryPercent && !lowNotified && cfg.notifyLowBattery) {
                    lowNotified = true;
                    wchar_t text[64];
                    swprintf_s(text, L"Battery low: %d%%", low);
                    tray->Notify(snap.model.empty() ? L"Headphones" : snap.model, text);
                }
            }
        }
        if (lastReady.link == Link::Ready && !Contains(devices, lastReady.mac)) lastReady = Snapshot{};

        UpdateTray();
        flyout->Refresh();
        Reconcile();
    }

    // Tray shows the last known state while we are disconnected on demand.
    void UpdateTray() {
        if (!tray) return;
        const bool showLast = snap.link != Link::Ready && lastReady.link == Link::Ready &&
                              lastReady.mac == target && Contains(devices, target);
        tray->Update(showLast ? lastReady : snap);
    }

    void OnAudio() {
        MSG drop; // one enumeration covers every queued notification (a default change fires several)
        while (PeekMessageW(&drop, hwnd, WM_XM_AUDIO, WM_XM_AUDIO, PM_REMOVE)) {}
        endpoints = audio->RenderEndpoints();
        if (cfg.followWindowsOutput) {
            for (const Endpoint& e : endpoints)
                if (e.isDefault && !e.btMac.empty() && e.btMac != target && Contains(devices, e.btMac)) {
                    target = e.btMac;
                    cfg.lastMac = target;
                    if (!demo) SaveSettings(cfg);
                    lastAttemptAt = 0;
                    break;
                }
        }
        flyout->Refresh();
        Reconcile();
    }

    void OnTray(WPARAM, LPARAM lp) {
        switch (LOWORD(lp)) {
        case NIN_KEYSELECT: // Enter delivers this twice: show only, never toggle
            if (!flyout->Visible()) flyout->Toggle(tray->IconRect());
            Reconcile();
            break;
        case NIN_SELECT:
            flyout->Toggle(tray->IconRect());
            Reconcile();
            break;
        case WM_CONTEXTMENU: {
            const std::vector<BtDevice> shown = devices; // the menu loop can deliver WM_XM_STATE and change `devices`
            const int cmd = tray->ShowMenu(shown, target, cfg.autostart);
            if (cmd == Tray::Open) {
                if (!flyout->Visible()) flyout->Toggle(tray->IconRect());
            } else if (cmd == Tray::Quit) {
                Quit();
                return;
            } else if (cmd == Tray::ToggleAutostart) {
                cfg.autostart = !cfg.autostart;
                ConfigChanged();
                flyout->Refresh();
            } else if (cmd == Tray::OpenLogFolder) {
                OpenLogFolder();
            } else if (cmd >= Tray::DeviceBase && size_t(cmd - Tray::DeviceBase) < shown.size()) {
                SelectDevice(shown[size_t(cmd - Tray::DeviceBase)].mac);
            }
            Reconcile();
            break;
        }
        default:
            break;
        }
    }
};

App* g_app = nullptr;

LRESULT CALLBACK WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    App* a = g_app;
    if (a && a->hwnd == h) {
        if (m == a->taskbarCreated && m != 0) {
            if (a->tray) a->tray->Readd();
            a->UpdateTray();
            return 0;
        }
        switch (m) {
        case WM_XM_TRAY:
            a->OnTray(w, l);
            return 0;
        case WM_XM_STATE:
            a->OnState();
            return 0;
        case WM_XM_AUDIO:
            a->OnAudio();
            return 0;
        case WM_XM_SHOW:
            if (!a->flyout->Visible()) a->flyout->Toggle(a->tray->IconRect());
            a->Reconcile();
            return 0;
        case WM_TIMER:
            if (w == kTimer) a->Reconcile();
            return 0;
        case WM_SETTINGCHANGE: // taskbar theme changed: the drawn icon must follow
        case WM_DPICHANGED:
            a->UpdateTray();
            break;
        case WM_DESTROY:
            KillTimer(h, kTimer);
            a->tray.reset(); // remove the icon while its owner window still exists
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
    }
    return DefWindowProcW(h, m, w, l);
}

} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR cmdLine, int) {
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32);
    const bool demo = cmdLine && wcsstr(cmdLine, L"--demo") != nullptr;
    HANDLE mutex = CreateMutexW(nullptr, TRUE, kMutex);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND other = FindWindowW(kClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(other, &pid);
            AllowSetForegroundWindow(pid); // let the running instance activate its flyout
            PostMessageW(other, WM_XM_SHOW, 0, 0);
        }
        CloseHandle(mutex);
        return 0;
    }
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClass;
    if (!RegisterClassExW(&wc)) return 1;

    int rc = 1;
    {
        App app;
        app.inst = inst;
        if (!demo) app.cfg = LoadSettings(); // demo: defaults only, nothing read from or written to disk/registry
        app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
        // A real (never shown) top-level window: tray callbacks and TaskbarCreated need one.
        app.hwnd = CreateWindowExW(0, kClass, L"XMTray", WS_OVERLAPPED, 0, 0, 0, 0, nullptr, nullptr, inst, nullptr);
        if (app.hwnd) {
            Log(L"XMTray starting");
            app.audio = std::make_unique<Audio>(app.hwnd);
            app.endpoints = app.audio->RenderEndpoints();
            app.dolby = std::make_unique<Dolby>(app.hwnd);
            app.dolby->SetAutomation(app.cfg.dolbyAutomation);
            app.tray = std::make_unique<Tray>(app.hwnd);
            app.flyout = std::make_unique<Flyout>(inst, app);
            if (demo) app.StartDemo();
            else app.worker = std::make_unique<DeviceWorker>(app.hwnd);
            g_app = &app;
            if (demo) PostMessageW(app.hwnd, WM_XM_SHOW, 0, 0);
            app.tray->Update(app.snap);
            SetTimer(app.hwnd, kTimer, 1000, nullptr);

            MSG msg;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            rc = int(msg.wParam);
            g_app = nullptr;
            app.worker.reset(); // stop the device thread before the windows it posts to go away
            app.flyout.reset();
            app.tray.reset();
            app.dolby.reset();
            app.audio.reset();
            Log(L"XMTray exiting");
        }
    }
    CoUninitialize();
    if (mutex) {
        ReleaseMutex(mutex);
        CloseHandle(mutex);
    }
    return rc;
}
