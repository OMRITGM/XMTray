// Dolby.cpp - spatial sound status (read only) and an experimental sound-mode switch that drives the Dolby Access
// window through UI Automation on a worker thread. Windows lets only the app that owns a spatial format set it
// (SetDefaultSpatialAudioFormatAsync), so turning Atmos on/off is left to Windows Settings / Dolby Access.
#include "Core.h"

#include <ole2.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include <uiautomation.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.Devices.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>

#pragma comment(lib, "oleaut32.lib") // BSTR / VARIANT helpers used with UI Automation

namespace xm {

namespace {

namespace wma = winrt::Windows::Media::Audio;
using winrt::com_ptr;

constexpr wchar_t kOffGuid[] = L"{00000000-0000-0000-0000-000000000000}";
constexpr wchar_t kAtmosHeadphones[] = L"{1459AC38-3875-49BF-BB59-0FE80F4D395D}";
constexpr wchar_t kAtmosHomeTheater[] = L"{A289735D-FA3E-4E35-9D7D-B6F896ACB2E7}";
constexpr wchar_t kAtmosSpeakers[] = L"{4C81E564-C8EF-4AD9-9F2C-9EF995533790}";
constexpr wchar_t kPackageFamily[] = L"DolbyLaboratories.DolbyAccess_rz1tebttyb220";
constexpr wchar_t kAppId[] = L"DolbyLaboratories.DolbyAccess_rz1tebttyb220!App";
constexpr wchar_t kAumid[] = L"shell:AppsFolder\\DolbyLaboratories.DolbyAccess_rz1tebttyb220!App";
constexpr ULONGLONG kCacheMs = 1000;
constexpr ULONGLONG kModeCacheMs = 3000;
constexpr ULONGLONG kJobBudgetMs = 20000;    // whole UI Automation job
constexpr ULONGLONG kStepTimeoutMs = 12000;  // waiting for the window / settings tab
constexpr ULONGLONG kConfirmMs = 4000;
constexpr ULONGLONG kLateWindowMs = 3000;   // cleanup: wait for a launched window that appeared after a cancel
// Destructor wait: covers the worst cleanup after a cancel (one in-flight UIA call, ≤3 s transaction timeout, plus
// either the late-window wait or one restore UIA call, ≤3 s) with margin.
constexpr DWORD kShutdownWaitMs = 10000;
constexpr ULONGLONG kLogTail = 256 * 1024;

struct KnownFormat { const wchar_t* guid; const wchar_t* name; };
constexpr KnownFormat kFormats[] = {
    {L"{B53D940C-B846-4831-9F76-D102B9B725A0}", L"Windows Sonic for Headphones"},
    {kAtmosHeadphones, L"Dolby Atmos for Headphones"},
    {kAtmosHomeTheater, L"Dolby Atmos for home theater"},
    {kAtmosSpeakers, L"Dolby Atmos for built-in speakers"},
    {L"{4444ACB0-8DC0-4C2C-A0D8-2C76DB470F86}", L"DTS Headphone:X"},
    {L"{ADAFD3C6-AC4C-404A-836A-9615E9060564}", L"DTS:X Ultra"},
    {L"{10201B4A-3322-4967-BF40-2CAA9BAFCA44}", L"DTS:X for home theater"},
};

// UI text of the modes in Dolby Access (English UI) and their index in DolbyState::modes.
constexpr const wchar_t* kModeNames[] = {L"Dynamic", L"Movie", L"Music", L"Game", L"Voice", L"Personalize"};
constexpr int kModeCount = 6;

bool EqualNoCase(const std::wstring& a, const wchar_t* b) {
    return CompareStringOrdinal(a.c_str(), -1, b, -1, TRUE) == CSTR_EQUAL;
}
bool StartsNoCase(const std::wstring& a, const wchar_t* prefix) {
    const int n = static_cast<int>(wcslen(prefix));
    return static_cast<int>(a.size()) >= n && CompareStringOrdinal(a.c_str(), n, prefix, n, TRUE) == CSTR_EQUAL;
}

// ---- installed check ---------------------------------------------------------------------------------------
bool KeyExists(const wchar_t* path) {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, path, 0, KEY_READ, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

// Per-user package repository: one subkey per installed package full name, e.g.
// DolbyLaboratories.DolbyAccess_3.27.12250.0_x64__rz1tebttyb220
bool PackageRegistered() {
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Classes\\Local Settings\\Software\\Microsoft\\Windows\\CurrentVersion\\AppModel\\Repository\\Packages",
            0, KEY_READ, &k) != ERROR_SUCCESS)
        return false;
    bool found = false;
    wchar_t name[256];
    for (DWORD i = 0; !found; ++i) {
        DWORD len = static_cast<DWORD>(std::size(name));
        if (RegEnumKeyExW(k, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        const std::wstring n(name, len);
        found = StartsNoCase(n, L"DolbyLaboratories.DolbyAccess_") && n.size() > 15 &&
                EqualNoCase(n.substr(n.size() - 15), L"__rz1tebttyb220");
    }
    RegCloseKey(k);
    return found;
}

bool AppInstalled() { return KeyExists(L"Software\\Classes\\dl-dolbyaccess") || PackageRegistered(); }

// ---- WinRT spatial audio -------------------------------------------------------------------------------------
std::wstring WinRtDeviceId(const std::wstring& endpointId) {
    if (endpointId.empty())
        return std::wstring(winrt::Windows::Media::Devices::MediaDevice::GetDefaultAudioRenderId(
            winrt::Windows::Media::Devices::AudioDeviceRole::Default));
    return L"\\\\?\\SWD#MMDEVAPI#" + endpointId + L"#{e6327cad-dcec-4949-ae8a-991e976a79d2}";
}

wma::SpatialAudioDeviceConfiguration ConfigFor(const std::wstring& endpointId) {
    return wma::SpatialAudioDeviceConfiguration::GetForDeviceId(winrt::hstring(WinRtDeviceId(endpointId)));
}

// True when the WinRT class itself can be activated (false on Windows versions without it). Checked once.
bool ActivateSpatialApi() {
    try {
        winrt::get_activation_factory<wma::SpatialAudioDeviceConfiguration, wma::ISpatialAudioDeviceConfigurationStatics>();
        return true;
    } catch (const winrt::hresult_error& e) {
        Log(L"Dolby: SpatialAudioDeviceConfiguration activation failed 0x%08X", static_cast<unsigned>(e.code().value));
    } catch (...) {
        Log(L"Dolby: SpatialAudioDeviceConfiguration activation failed (unknown)");
    }
    return false;
}
bool SpatialApiAvailable() {
    static const bool available = ActivateSpatialApi();
    return available;
}

// ---- Dolby Access log (current sound mode without any UI) ----------------------------------------------------
std::wstring LogDir() {
    wchar_t buf[MAX_PATH];
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(buf) + L"\\Packages\\" + kPackageFamily + L"\\LocalCache\\";
}

// Full paths of DolbyAccess_YYYYMMDD.log, newest first.
std::vector<std::wstring> LogFiles() {
    std::vector<std::wstring> out;
    const std::wstring dir = LogDir();
    if (dir.empty()) return out;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"DolbyAccess_*.log").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch,
                                nullptr, 0);
    if (f == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(dir + fd.cFileName);
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    std::sort(out.begin(), out.end(), std::greater<>());
    return out;
}

// Reads the file from `from` (but at most the last kLogTail bytes) while Dolby Access keeps writing it.
// UTF-8 (what the app writes) or UTF-16LE. `size` receives the file size. False if it cannot be opened.
bool ReadLogText(const std::wstring& path, ULONGLONG from, std::wstring& text, ULONGLONG* size) {
    text.clear();
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz{};
    bool ok = GetFileSizeEx(h, &sz) != FALSE;
    const ULONGLONG total = ok ? static_cast<ULONGLONG>(sz.QuadPart) : 0;
    if (size) *size = total;
    unsigned char head[4]{};
    DWORD got = 0;
    if (ok) ok = ReadFile(h, head, sizeof head, &got, nullptr) != FALSE;
    const bool utf16 = got >= 2 && head[0] == 0xFF && head[1] == 0xFE;
    ULONGLONG start = std::max(from, total > kLogTail ? total - kLogTail : 0);
    if (utf16) start = std::max<ULONGLONG>(start, 2) & ~1ULL;
    else if (start == 0 && got >= 3 && head[0] == 0xEF && head[1] == 0xBB && head[2] == 0xBF) start = 3;
    if (ok && start < total) {
        std::string buf(static_cast<size_t>(total - start), '\0');
        LARGE_INTEGER pos{};
        pos.QuadPart = static_cast<LONGLONG>(start);
        DWORD read = 0;
        ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) && ReadFile(h, buf.data(), static_cast<DWORD>(buf.size()), &read, nullptr);
        buf.resize(ok ? read : 0);
        if (utf16) {
            text.assign(reinterpret_cast<const wchar_t*>(buf.data()), buf.size() / 2);
        } else if (!buf.empty()) {
            const int n = MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), nullptr, 0);
            text.resize(static_cast<size_t>(n));
            MultiByteToWideChar(CP_UTF8, 0, buf.data(), static_cast<int>(buf.size()), text.data(), n);
        }
    }
    CloseHandle(h);
    return ok;
}

// Profile name as logged -> index into kModeNames (Personalize is logged as "Custom1"), -1 if unknown.
int ProfileToMode(const std::wstring& name) {
    for (int i = 0; i < 5; ++i)
        if (EqualNoCase(name, kModeNames[i])) return i;
    if (StartsNoCase(name, L"Custom") || StartsNoCase(name, L"Personaliz")) return 5;
    return -1;
}

// Mode named by one log line, -1 if it is not a profile line or names an unknown profile. Authoritative lines:
//   ...::Selected profile: Music                                                  (a switch, and app start)
//   ...::Initial configuration restored from AppService. Active profile: Music    (app start; `withActive` only)
// "Profile <X> set or updated" is not used: it also repeats while a profile's settings are edited.
int ModeInLine(const std::wstring& line, bool withActive) {
    for (const wchar_t* key : {L"Selected profile: ", L"Active profile: "}) {
        if (!withActive && key[0] == L'A') continue;
        const size_t p = line.rfind(key);
        if (p == std::wstring::npos) continue;
        size_t at = p + wcslen(key), end = at;
        while (end < line.size() && !iswspace(line[end])) ++end;
        return ProfileToMode(line.substr(at, end - at));
    }
    return -1;
}

// Calls fn(line) for each complete line of `text`, newest first (a final line without '\n' may still be being
// written and is skipped). Stops when fn returns true.
template <class Fn> bool EachLineBackward(const std::wstring& text, Fn fn) {
    size_t end = text.rfind(L'\n');
    while (end != std::wstring::npos && end > 0) {
        const size_t nl = text.rfind(L'\n', end - 1);
        const size_t begin = nl == std::wstring::npos ? 0 : nl + 1;
        if (fn(text.substr(begin, end - begin))) return true;
        end = nl;
    }
    return false;
}

int ReadCurrentMode() {
    const auto files = LogFiles();
    std::wstring text;
    int mode = -1;
    for (size_t i = 0; i < files.size() && i < 3 && mode < 0; ++i)
        if (ReadLogText(files[i], 0, text, nullptr))
            EachLineBackward(text, [&](const std::wstring& l) { return (mode = ModeInLine(l, true)) >= 0; });
    return mode;
}

// True if a "Selected profile: <mode>" line was written after byte `from` of `path`.
bool LogConfirms(const std::wstring& path, ULONGLONG from, int mode) {
    std::wstring text;
    if (!ReadLogText(path, from, text, nullptr)) return false;
    return EachLineBackward(text, [&](const std::wstring& l) { return ModeInLine(l, false) == mode; });
}

// ---- state shared by the Dolby object and its worker thread --------------------------------------------------
struct Shared {
    HWND notify = nullptr;
    std::atomic<bool> cancel{false};    // the Dolby object is being destroyed
    std::atomic<bool> jobCancel{false}; // the running mode job should stop (SetAutomation(false)); reset per job
    std::atomic<bool> dirty{false};     // Get() must drop its cache
    std::mutex m;
    std::condition_variable cv;
    bool running = false;    // a job thread is active
    bool modeBusy = false;
    std::wstring modeError;
    int mode = -1;           // parsed from the Dolby Access log
    ULONGLONG modeAt = 0;    // when `mode` was read successfully; 0 = read again
};

void FinishJob(const std::shared_ptr<Shared>& sh, const std::function<void(Shared&)>& apply) {
    {
        std::lock_guard<std::mutex> lk(sh->m);
        apply(*sh);
        sh->running = false;
    }
    sh->cv.notify_all();
    sh->dirty = true;
    if (sh->notify) PostMessageW(sh->notify, WM_XM_AUDIO, 0, 0);
}

struct ComInit {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

// ---- job: switch the Dolby sound mode through the Dolby Access window ----------------------------------------
bool IsDolbyProcess(DWORD pid) {
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t path[MAX_PATH];
    DWORD len = MAX_PATH;
    const bool ok = QueryFullProcessImageNameW(p, 0, path, &len) != FALSE;
    CloseHandle(p);
    if (!ok) return false;
    const wchar_t* slash = wcsrchr(path, L'\\');
    return CompareStringOrdinal(slash ? slash + 1 : path, -1, L"DolbyAccess.exe", -1, TRUE) == CSTR_EQUAL;
}

std::set<DWORD> DolbyPids() {
    std::set<DWORD> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{sizeof pe};
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe))
        if (CompareStringOrdinal(pe.szExeFile, -1, L"DolbyAccess.exe", -1, TRUE) == CSTR_EQUAL) out.insert(pe.th32ProcessID);
    CloseHandle(snap);
    return out;
}

DWORD DolbyPidOf(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid && IsDolbyProcess(pid) ? pid : 0;
}

bool HasClass(HWND w, const wchar_t* cls) {
    wchar_t buf[64];
    return GetClassNameW(w, buf, 64) && wcscmp(buf, cls) == 0;
}

// The ApplicationFrameWindow whose Windows.UI.Core.CoreWindow child belongs to DolbyAccess.exe (`pid` = that
// process). While the app is minimised the CoreWindow is detached from its frame and becomes top-level; then the
// frame is the one without a CoreWindow child whose title equals that CoreWindow's title (compared, not
// hard-coded: it is localised).
HWND FindDolbyFrame(DWORD* pid) {
    struct Ctx { HWND frame = nullptr; DWORD pid = 0; HWND looseCore = nullptr; DWORD loosePid = 0; std::vector<HWND> emptyFrames; } ctx;
    EnumWindows([](HWND top, LPARAM lp) -> BOOL {
        Ctx& c = *reinterpret_cast<Ctx*>(lp);
        if (HasClass(top, L"Windows.UI.Core.CoreWindow")) {
            if (!c.looseCore && (c.loosePid = DolbyPidOf(top)) != 0) c.looseCore = top;
            return TRUE;
        }
        if (!HasClass(top, L"ApplicationFrameWindow")) return TRUE;
        bool hasCore = false;
        for (HWND w = nullptr; (w = FindWindowExW(top, w, L"Windows.UI.Core.CoreWindow", nullptr)) != nullptr;) {
            hasCore = true;
            if ((c.pid = DolbyPidOf(w)) != 0) {
                c.frame = top;
                return FALSE;
            }
        }
        if (!hasCore) c.emptyFrames.push_back(top);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    *pid = ctx.pid;
    if (ctx.frame || !ctx.looseCore) return ctx.frame;
    wchar_t want[256] = L"", have[256];
    if (GetWindowTextW(ctx.looseCore, want, 256) == 0) return nullptr;
    for (HWND f : ctx.emptyFrames)
        if (GetWindowTextW(f, have, 256) > 0 && wcscmp(have, want) == 0) {
            *pid = ctx.loosePid;
            return f;
        }
    return nullptr;
}

struct Bstr {
    BSTR b = nullptr;
    Bstr() = default;
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    ~Bstr() { SysFreeString(b); }
    std::wstring str() const { return b ? std::wstring(b, SysStringLen(b)) : std::wstring(); }
};

struct Node {
    com_ptr<IUIAutomationElement> el;
    int depth = 0;
    CONTROLTYPEID type = 0;
    std::wstring id, name;
    bool sel = false, inv = false, tog = false; // patterns (Invoke / Toggle only for the layout dump)
};

const wchar_t* TypeName(CONTROLTYPEID t) {
    switch (t) {
    case UIA_ButtonControlTypeId: return L"Button";
    case UIA_CheckBoxControlTypeId: return L"CheckBox";
    case UIA_ComboBoxControlTypeId: return L"ComboBox";
    case UIA_CustomControlTypeId: return L"Custom";
    case UIA_GroupControlTypeId: return L"Group";
    case UIA_HyperlinkControlTypeId: return L"Hyperlink";
    case UIA_ImageControlTypeId: return L"Image";
    case UIA_ListControlTypeId: return L"List";
    case UIA_ListItemControlTypeId: return L"ListItem";
    case UIA_PaneControlTypeId: return L"Pane";
    case UIA_RadioButtonControlTypeId: return L"RadioButton";
    case UIA_SliderControlTypeId: return L"Slider";
    case UIA_TabControlTypeId: return L"Tab";
    case UIA_TabItemControlTypeId: return L"TabItem";
    case UIA_TextControlTypeId: return L"Text";
    case UIA_WindowControlTypeId: return L"Window";
    default: return nullptr;
    }
}

constexpr wchar_t kLayoutErr[] = L"Dolby Access layout not recognised (app updated?)";

// One mode switch. The destructor always puts the window back (and closes the app if this job started it), so
// no exception or early return can leave Dolby Access off-screen.
class ModeJob {
public:
    ModeJob(std::shared_ptr<Shared> sh, int mode) : sh_(std::move(sh)), mode_(mode) {}
    ModeJob(const ModeJob&) = delete;
    ModeJob& operator=(const ModeJob&) = delete;
    ~ModeJob() {
        try { Restore(); } catch (...) {}
    }

    // Returns "" on success, else a short reason for the user.
    std::wstring Run() {
        std::wstring err = LaunchOrFind();
        if (err.empty()) err = PrepareWindow();
        if (err.empty()) err = StartUia();
        if (err.empty()) err = SelectSettingsTab();
        com_ptr<IUIAutomationSelectionItemPattern> target;
        if (err.empty()) err = FindModeControl(target);
        if (err.empty()) err = ActivateAndConfirm(target);
        return err;
    }

private:
    std::shared_ptr<Shared> sh_;
    int mode_;
    ULONGLONG deadline_ = GetTickCount64() + kJobBudgetMs;
    HWND fg_ = GetForegroundWindow();
    std::set<DWORD> before_;    // DolbyAccess.exe processes running before we did anything
    HWND frame_ = nullptr;
    bool existed_ = false;      // the window was there before
    bool launched_ = false;     // we called ActivateApplication
    bool ours_ = false;         // ... and it started a new process: close it afterwards
    bool moved_ = false;        // the window has been moved / un-minimised and must be put back
    bool wasIconic_ = false;
    RECT rect_{};               // pre-existing normal window: exact rect (placement loses snapped positions)
    WINDOWPLACEMENT wp_{sizeof(WINDOWPLACEMENT)};
    com_ptr<IUIAutomation> uia_;
    com_ptr<IUIAutomationElement> root_;
    com_ptr<IUIAutomationSelectionItemPattern> prevNav_; // nav item to re-select in a pre-existing window

    bool Cancelled() const { return sh_->cancel || sh_->jobCancel; }

    // Poll `done` every `stepMs` until it returns true, `timeoutMs` passes, the job budget ends or we are cancelled.
    std::wstring Poll(ULONGLONG timeoutMs, DWORD stepMs, const std::function<bool()>& done, const wchar_t* timeoutErr) {
        const ULONGLONG end = std::min(GetTickCount64() + timeoutMs, deadline_);
        for (;;) {
            if (Cancelled()) return L"Cancelled";
            if (done()) return {};
            if (GetTickCount64() >= end) return GetTickCount64() >= deadline_ ? L"Timed out" : timeoutErr;
            Sleep(stepMs);
        }
    }

    // Hand the focus back only if Dolby Access took it and the window that had it is still visible.
    void RestoreForeground() {
        const HWND cur = GetForegroundWindow();
        if (!cur || !fg_ || cur == fg_ || !IsWindow(fg_) || !IsWindowVisible(fg_)) return;
        const bool dolby = (frame_ && (cur == frame_ || GetAncestor(cur, GA_ROOT) == frame_)) || DolbyPidOf(cur) != 0;
        if (dolby && !SetForegroundWindow(fg_)) Log(L"Dolby mode: could not give the focus back");
    }

    // A window we launched: remember where it opened, then push it behind and off-screen at once.
    void ParkLaunched() {
        GetWindowPlacement(frame_, &wp_);
        moved_ = SetWindowPos(frame_, HWND_BOTTOM, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOACTIVATE) != FALSE;
        RestoreForeground();
    }

    // 1. Reuse the Dolby Access window, or launch the app and wait for its window.
    std::wstring LaunchOrFind() {
        before_ = DolbyPids();
        DWORD pid = 0;
        frame_ = FindDolbyFrame(&pid);
        existed_ = frame_ != nullptr;
        if (!existed_) {
            if (Cancelled()) return L"Cancelled";
            Log(L"Dolby mode: launching Dolby Access");
            com_ptr<IApplicationActivationManager> aam;
            DWORD started = 0;
            HRESULT hr = CoCreateInstance(__uuidof(ApplicationActivationManager), nullptr, CLSCTX_LOCAL_SERVER,
                                          __uuidof(IApplicationActivationManager), aam.put_void());
            if (SUCCEEDED(hr)) hr = aam->ActivateApplication(kAppId, nullptr, AO_NONE, &started); // cannot be cancelled
            if (FAILED(hr)) {
                Log(L"Dolby mode: ActivateApplication failed 0x%08X", static_cast<unsigned>(hr));
                return L"Could not start Dolby Access";
            }
            launched_ = true;
            ours_ = !before_.count(started);
            std::wstring err = Poll(kStepTimeoutMs, 50, [&] {
                if (!(frame_ = FindDolbyFrame(&pid))) return false;
                ParkLaunched();
                return true;
            }, L"Dolby Access did not open a window");
            if (!err.empty()) return err;
        }
        if (before_.count(pid)) ours_ = false; // the user's own instance: restore, never close
        Log(L"Dolby mode: window %p (%ls%ls)", static_cast<void*>(frame_), existed_ ? L"reused" : L"launched",
            launched_ && !ours_ ? L", process was already running" : L"");
        return {};
    }

    // 2. A minimised UWP app is suspended and has no UI Automation tree: un-minimise it straight to an off-screen
    //    normal position without activating; a normal window is moved off-screen. Both stay alive there.
    std::wstring PrepareWindow() {
        if (launched_) return {}; // parked as soon as it appeared
        if (!GetWindowPlacement(frame_, &wp_)) return L"Could not read the Dolby Access window position";
        wasIconic_ = IsIconic(frame_) != FALSE;
        if (wasIconic_) {
            WINDOWPLACEMENT off = wp_;
            OffsetRect(&off.rcNormalPosition, -32000 - off.rcNormalPosition.left, -32000 - off.rcNormalPosition.top);
            off.flags &= ~static_cast<UINT>(WPF_RESTORETOMAXIMIZED);
            off.showCmd = SW_SHOWNOACTIVATE;
            moved_ = SetWindowPlacement(frame_, &off) != FALSE;
        } else {
            if (!GetWindowRect(frame_, &rect_)) return L"Could not read the Dolby Access window position";
            moved_ = SetWindowPos(frame_, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        }
        RestoreForeground();
        return moved_ ? std::wstring() : L"Could not move the Dolby Access window";
    }

    std::wstring StartUia() {
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                      uia_.put_void());
        if (FAILED(hr))
            hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                  uia_.put_void());
        if (FAILED(hr)) return L"UI Automation is unavailable";
        if (auto u2 = uia_.try_as<IUIAutomation2>()) { // a hung app cannot block us (or shutdown) for long
            u2->put_ConnectionTimeout(2000);
            u2->put_TransactionTimeout(3000);
        }
        if (FAILED(uia_->ElementFromHandle(frame_, root_.put())) || !root_) return L"Dolby Access window not accessible";
        return {};
    }

    com_ptr<IUIAutomationElement> ById(IUIAutomationElement* under, const wchar_t* id) {
        com_ptr<IUIAutomationElement> el;
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_BSTR;
        v.bstrVal = SysAllocString(id);
        com_ptr<IUIAutomationCondition> cond;
        if (v.bstrVal && SUCCEEDED(uia_->CreatePropertyCondition(UIA_AutomationIdPropertyId, v, cond.put())))
            under->FindFirst(TreeScope_Descendants, cond.get(), el.put());
        VariantClear(&v);
        return el;
    }

    // In a pre-existing window: the nav item that is selected now, to put back afterwards.
    void RememberNav() {
        com_ptr<IUIAutomationElement> nav = ById(root_.get(), L"NavigationPanel_navigationView");
        com_ptr<IUIAutomationSelectionPattern> sel;
        com_ptr<IUIAutomationElementArray> items;
        com_ptr<IUIAutomationElement> item;
        int n = 0;
        if (nav && SUCCEEDED(nav->GetCurrentPatternAs(UIA_SelectionPatternId, __uuidof(IUIAutomationSelectionPattern), sel.put_void())) && sel &&
            SUCCEEDED(sel->GetCurrentSelection(items.put())) && items && SUCCEEDED(items->get_Length(&n)) && n > 0 &&
            SUCCEEDED(items->GetElement(0, item.put())) && item)
            item->GetCurrentPatternAs(UIA_SelectionItemPatternId, __uuidof(IUIAutomationSelectionItemPattern), prevNav_.put_void());
        if (!prevNav_) Log(L"Dolby mode: could not read the selected nav item");
    }

    // 3. Settings tab (NavigationView item); select it if needed.
    std::wstring SelectSettingsTab() {
        com_ptr<IUIAutomationElement> tab;
        std::wstring err = Poll(kStepTimeoutMs, 100, [&] { return (tab = ById(root_.get(), L"NavigationPanel_settingsTabLabel")) != nullptr; },
                                L"Dolby Access settings tab not found");
        if (!err.empty()) return err;
        com_ptr<IUIAutomationSelectionItemPattern> sel;
        tab->GetCurrentPatternAs(UIA_SelectionItemPatternId, __uuidof(IUIAutomationSelectionItemPattern), sel.put_void());
        if (!sel) return L"Dolby Access settings tab cannot be selected";
        BOOL selected = FALSE;
        sel->get_CurrentIsSelected(&selected);
        if (!selected) {
            if (existed_) RememberNav();
            if (FAILED(sel->Select())) return L"Could not open the Dolby Access settings tab";
        }
        Log(L"Dolby mode: settings tab %ls", selected ? L"already selected" : L"selected");
        return {};
    }

    // Control-view subtree of `under` (not including it), depth-first, with the properties we need cached.
    std::vector<Node> Walk(IUIAutomationElement* under) {
        std::vector<Node> out;
        com_ptr<IUIAutomationTreeWalker> walker;
        com_ptr<IUIAutomationCacheRequest> req;
        if (FAILED(uia_->get_ControlViewWalker(walker.put())) || FAILED(uia_->CreateCacheRequest(req.put()))) return out;
        for (PROPERTYID p : {UIA_ControlTypePropertyId, UIA_AutomationIdPropertyId, UIA_NamePropertyId,
                             UIA_IsSelectionItemPatternAvailablePropertyId, UIA_IsInvokePatternAvailablePropertyId,
                             UIA_IsTogglePatternAvailablePropertyId})
            req->AddProperty(p);
        auto flag = [](IUIAutomationElement* e, PROPERTYID p) {
            VARIANT v;
            VariantInit(&v);
            const bool on = SUCCEEDED(e->GetCachedPropertyValue(p, &v)) && v.vt == VT_BOOL && v.boolVal != VARIANT_FALSE;
            VariantClear(&v);
            return on;
        };
        std::function<void(IUIAutomationElement*, int)> visit = [&](IUIAutomationElement* parent, int depth) {
            if (depth > 40 || out.size() >= 2000 || Cancelled()) return;
            com_ptr<IUIAutomationElement> c;
            walker->GetFirstChildElementBuildCache(parent, req.get(), c.put());
            while (c && out.size() < 2000) {
                Node n;
                n.el = c;
                n.depth = depth;
                c->get_CachedControlType(&n.type);
                Bstr id, name;
                c->get_CachedAutomationId(&id.b);
                c->get_CachedName(&name.b);
                n.id = id.str();
                n.name = name.str();
                n.sel = flag(c.get(), UIA_IsSelectionItemPatternAvailablePropertyId);
                n.inv = flag(c.get(), UIA_IsInvokePatternAvailablePropertyId);
                n.tog = flag(c.get(), UIA_IsTogglePatternAvailablePropertyId);
                out.push_back(n);
                visit(c.get(), depth + 1);
                com_ptr<IUIAutomationElement> next;
                walker->GetNextSiblingElementBuildCache(c.get(), req.get(), next.put());
                c = next;
            }
        };
        visit(under, 0);
        return out;
    }

    // The mode control, strictly: Name equal to the mode name (case-insensitive), SelectionItem pattern, and a
    // ListItem / RadioButton / TabItem. The deepest one wins if several match.
    const Node* Pick(const std::vector<Node>& nodes) const {
        const Node* best = nullptr;
        for (const Node& n : nodes) {
            const bool type = n.type == UIA_ListItemControlTypeId || n.type == UIA_RadioButtonControlTypeId ||
                              n.type == UIA_TabItemControlTypeId;
            if (n.sel && type && EqualNoCase(n.name, kModeNames[mode_]) && (!best || n.depth > best->depth)) best = &n;
        }
        return best;
    }

    // Names are replaced by their length: they can contain the signed-in account's name.
    static void DumpTree(const std::vector<Node>& nodes) {
        Log(L"Dolby mode: PcSoundSettingsPage dump (%zu elements):", nodes.size());
        size_t lines = 0;
        for (const Node& n : nodes) {
            if (++lines > 80) { Log(L"  ..."); break; }
            const wchar_t* t = TypeName(n.type);
            wchar_t num[16];
            if (!t) { swprintf_s(num, L"%d", n.type); t = num; }
            Log(L"  %*ls%ls id='%ls' nameLen=%zu%ls%ls%ls", n.depth * 2, L"", t, n.id.c_str(), n.name.size(),
                n.sel ? L" [SelectionItem]" : L"", n.inv ? L" [Invoke]" : L"", n.tog ? L" [Toggle]" : L"");
        }
    }

    // 4 + 5. Fail on the "enable Dolby Atmos" overlay; otherwise discover the mode control under PcSoundSettingsPage
    //        (its ids are unknown).
    std::wstring FindModeControl(com_ptr<IUIAutomationSelectionItemPattern>& target) {
        bool overlay = false, page = false;
        std::vector<Node> last;
        std::wstring err = Poll(6000, 250, [&] {
            if (ById(root_.get(), L"SoundOverlay_setupOverlayTitle")) return overlay = true;
            com_ptr<IUIAutomationElement> p = ById(root_.get(), L"PcSoundSettingsPage");
            if (!(page = p != nullptr)) return false;
            last = Walk(p.get());
            const Node* n = Pick(last);
            if (!n) return false;
            n->el->GetCurrentPatternAs(UIA_SelectionItemPatternId, __uuidof(IUIAutomationSelectionItemPattern), target.put_void());
            Log(L"Dolby mode: control %ls id='%ls'", TypeName(n->type), n->id.c_str());
            return target != nullptr;
        }, kLayoutErr);
        if (overlay) {
            Log(L"Dolby mode: setup overlay shown (Atmos off on the current output)");
            return L"Dolby Atmos is not enabled on the current output";
        }
        if (err == kLayoutErr) {
            if (page) DumpTree(last);
            else Log(L"Dolby mode: PcSoundSettingsPage not found");
        }
        return err;
    }

    // 6. Select it; success only when Dolby Access logs "Selected profile: <mode>" after this point.
    std::wstring ActivateAndConfirm(const com_ptr<IUIAutomationSelectionItemPattern>& sel) {
        const auto files = LogFiles();
        const std::wstring logPath = files.empty() ? std::wstring() : files.front();
        ULONGLONG logSize = 0;
        std::wstring ignored;
        if (!logPath.empty()) ReadLogText(logPath, ~0ULL, ignored, &logSize);
        const HRESULT hr = sel->Select();
        if (FAILED(hr)) {
            Log(L"Dolby mode: Select failed 0x%08X", static_cast<unsigned>(hr));
            return L"Dolby Access did not accept the change";
        }
        Log(L"Dolby mode: control selected");
        const std::wstring err = Poll(kConfirmMs, 200, [&] {
            const auto now = LogFiles();
            return !now.empty() && LogConfirms(now.front(), now.front() == logPath ? logSize : 0, mode_);
        }, L"Dolby Access did not confirm the change");
        if (err.empty()) Log(L"Dolby mode: confirmed by the Dolby Access log");
        return err;
    }

    // 7. Put everything back. Waits here are cleanup: bounded, and they ignore cancel on purpose.
    void Restore() {
        if (launched_ && !frame_) { // stopped before its window appeared: wait briefly so we can close it
            DWORD pid = 0;
            const ULONGLONG end = GetTickCount64() + kLateWindowMs;
            while (!(frame_ = FindDolbyFrame(&pid)) && GetTickCount64() < end) Sleep(100);
            if (frame_ && before_.count(pid)) ours_ = false;
            if (frame_ && ours_) {
                PostMessageW(frame_, WM_CLOSE, 0, 0);
                Log(L"Dolby mode: closed Dolby Access (WM_CLOSE, window appeared late)");
            } else if (ours_) {
                Log(L"Dolby mode: Dolby Access window never appeared; it may stay open");
            }
            RestoreForeground();
            return;
        }
        if (!frame_ || !IsWindow(frame_) || !moved_) return;
        if (prevNav_ && FAILED(prevNav_->Select())) Log(L"Dolby mode: could not re-select the previous page");
        if (ours_) {
            WINDOWPLACEMENT wp = wp_;
            wp.showCmd = SW_HIDE; // keep its position for next time without flashing it on screen
            com_ptr<IUIAutomationWindowPattern> win;
            if (root_) root_->GetCurrentPatternAs(UIA_WindowPatternId, __uuidof(IUIAutomationWindowPattern), win.put_void());
            SetWindowPlacement(frame_, &wp);
            const bool closed = win && SUCCEEDED(win->Close());
            if (!closed) PostMessageW(frame_, WM_CLOSE, 0, 0);
            Log(L"Dolby mode: closed Dolby Access (%ls)", closed ? L"WindowPattern" : L"WM_CLOSE");
        } else if (launched_) { // an already running process we brought up: leave the window where it opened
            WINDOWPLACEMENT wp = wp_;
            wp.showCmd = SW_SHOWNOACTIVATE;
            SetWindowPlacement(frame_, &wp);
            Log(L"Dolby mode: window left open at its own position");
        } else if (wasIconic_) {
            WINDOWPLACEMENT wp = wp_;
            wp.showCmd = SW_SHOWMINNOACTIVE;
            SetWindowPlacement(frame_, &wp);
            Log(L"Dolby mode: window minimised again");
        } else {
            SetWindowPos(frame_, nullptr, rect_.left, rect_.top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            Log(L"Dolby mode: window moved back");
        }
        RestoreForeground();
    }
};

void RunMode(std::shared_ptr<Shared> sh, int mode) {
    const ULONGLONG t0 = GetTickCount64();
    std::wstring err;
    {
        ComInit com;
        try {
            ModeJob job(sh, mode); // restores in its destructor, still inside the COM apartment
            err = job.Run();
        } catch (const winrt::hresult_error& e) {
            Log(L"Dolby mode: failed 0x%08X", static_cast<unsigned>(e.code().value));
            err = L"Dolby Access automation failed";
        } catch (...) {
            Log(L"Dolby mode: failed (unknown)");
            err = L"Dolby Access automation failed";
        }
    }
    int now = -1;
    try { now = ReadCurrentMode(); } catch (...) {}
    Log(L"Dolby mode: %ls -> %ls in %llu ms", kModeNames[mode], err.empty() ? L"ok" : err.c_str(), GetTickCount64() - t0);
    FinishJob(sh, [&](Shared& s) {
        s.modeBusy = false;
        s.modeError = err;
        s.mode = err.empty() ? mode : now;
        s.modeAt = s.mode >= 0 ? GetTickCount64() : 0;
    });
}

struct BaseState { // the slow-to-read part of DolbyState, cached ~1 s per endpoint
    ULONGLONG at = 0;
    DolbyState s;
    bool supported = false;
    bool atmosActive = false; // some Dolby Atmos format is active on the endpoint
};

} // namespace

struct Dolby::Impl {
    std::shared_ptr<Shared> sh = std::make_shared<Shared>();
    std::thread worker;
    bool automation = false;
    std::map<std::wstring, BaseState> cache;

    // Starts `job` on the worker thread unless one is running.
    bool Start(std::function<void()> job, const std::function<void(Shared&)>& mark) {
        {
            std::lock_guard<std::mutex> lk(sh->m);
            if (sh->running) return false;
            sh->running = true;
            sh->jobCancel = false;
            mark(*sh);
        }
        try {
            if (worker.joinable()) worker.join(); // previous job already finished
            worker = std::thread(std::move(job));
        } catch (...) {
            std::lock_guard<std::mutex> lk(sh->m);
            sh->running = sh->modeBusy = false;
            return false;
        }
        cache.clear();
        return true;
    }
};

Dolby::Dolby(HWND notify) : impl_(new Impl) { impl_->sh->notify = notify; }

Dolby::~Dolby() {
    try {
        impl_->sh->cancel = true;
        bool done;
        {
            std::unique_lock<std::mutex> lk(impl_->sh->m);
            done = impl_->sh->cv.wait_for(lk, std::chrono::milliseconds(kShutdownWaitMs), [&] { return !impl_->sh->running; });
        }
        if (impl_->worker.joinable()) {
            if (done) impl_->worker.join();
            else { // stuck in a call it cannot cancel; it only touches Shared, which it co-owns
                Log(L"Dolby: worker did not stop in time; detaching");
                impl_->worker.detach();
            }
        }
    } catch (...) {}
    delete impl_;
}

DolbyState Dolby::Get(const std::wstring& endpointId) const {
    try {
        Impl& im = *impl_;
        if (im.sh->dirty.exchange(false)) im.cache.clear();
        const ULONGLONG now = GetTickCount64();
        auto it = im.cache.find(endpointId);
        if (it == im.cache.end() || now - it->second.at >= kCacheMs) {
            BaseState b;
            b.at = now;
            DolbyState& s = b.s;
            s.appInstalled = AppInstalled();
            if (!SpatialApiAvailable()) {
                s.note = L"Spatial sound status is unavailable on this Windows version.";
            } else {
                try {
                    auto cfg = ConfigFor(endpointId);
                    const std::wstring fmt(cfg.ActiveSpatialAudioFormat());
                    b.supported = cfg.IsSpatialAudioSupported();
                    s.statusKnown = true;
                    if (fmt.empty() || EqualNoCase(fmt, kOffGuid)) {
                        s.formatName = L"Off";
                    } else {
                        s.spatialOn = true;
                        s.formatName = L"Other spatial format";
                        for (const auto& f : kFormats)
                            if (EqualNoCase(fmt, f.guid)) { s.formatName = f.name; break; }
                        s.atmosOn = EqualNoCase(fmt, kAtmosHeadphones);
                        b.atmosActive = s.atmosOn || EqualNoCase(fmt, kAtmosHomeTheater) || EqualNoCase(fmt, kAtmosSpeakers);
                    }
                } catch (const winrt::hresult_error& e) {
                    Log(L"Dolby: status read failed 0x%08X", static_cast<unsigned>(e.code().value));
                    s.note = L"Spatial sound status could not be read for this output.";
                }
            }
            it = im.cache.insert_or_assign(endpointId, std::move(b)).first;
        }
        const BaseState& b = it->second;
        DolbyState s = b.s;

        bool refreshMode;
        {
            std::lock_guard<std::mutex> lk(im.sh->m);
            s.modeBusy = im.sh->modeBusy;
            s.modeError = im.sh->modeError;
            refreshMode = im.automation && !im.sh->modeBusy && (im.sh->modeAt == 0 || now - im.sh->modeAt >= kModeCacheMs);
            s.mode = im.sh->mode;
        }
        if (refreshMode && s.appInstalled) { // file read only, no UI; a failed read is retried on the next call
            s.mode = ReadCurrentMode();
            std::lock_guard<std::mutex> lk(im.sh->m);
            im.sh->mode = s.mode;
            im.sh->modeAt = s.mode >= 0 ? now : 0;
        }

        // Stable while a job runs: progress is reported through modeBusy and the note, not by hiding controls.
        s.canToggle = s.statusKnown && b.supported;
        s.canSetMode = im.automation && s.appInstalled && b.atmosActive;
        if (im.automation && s.appInstalled) {
            for (int i = 0; i < kModeCount; ++i) s.modes.push_back({i, kModeNames[i]});
        } else {
            s.mode = -1;
        }

        std::wstring note;
        if (!s.appInstalled) note += L"Dolby Access is not installed. ";
        if (!s.note.empty()) note += s.note + L" ";
        if (!im.automation)
            note += L"Sound modes (Dynamic, Movie, Music, Game, Voice) can only be changed in Dolby Access: Windows and "
                    L"Dolby provide no interface for other apps. An experimental option on this page can switch them by "
                    L"driving the Dolby Access window.";
        else if (s.appInstalled && !b.atmosActive)
            note += L"Turn on Dolby Atmos for this output first — Dolby Access hides its sound modes until then.";
        while (!note.empty() && note.back() == L' ') note.pop_back();
        s.note = std::move(note);
        return s;
    } catch (...) {
        return {};
    }
}

void Dolby::SetAutomation(bool enabled) {
    try {
        impl_->automation = enabled;
        if (!enabled) {
            std::lock_guard<std::mutex> lk(impl_->sh->m);
            if (impl_->sh->modeBusy) impl_->sh->jobCancel = true; // the job restores the window and reports "Cancelled"
        }
        impl_->cache.clear();
    } catch (...) {}
}

bool Dolby::SetMode(int mode) {
    try {
        if (!impl_->automation || mode < 0 || mode >= kModeCount) return false;
        auto sh = impl_->sh;
        const bool started = impl_->Start([sh, mode] { RunMode(sh, mode); },
                                          [](Shared& s) { s.modeBusy = true; s.modeError.clear(); });
        if (started) Log(L"Dolby mode: switching to %ls", kModeNames[mode]);
        return started;
    } catch (...) {
        return false;
    }
}

bool Dolby::OpenApp() const {
    try {
        const bool installed = AppInstalled();
        const HINSTANCE r = ShellExecuteW(nullptr, L"open",
            installed ? kAumid : L"ms-windows-store://search/?query=Dolby%20Access",
            nullptr, nullptr, SW_SHOWNORMAL);
        const bool ok = reinterpret_cast<INT_PTR>(r) > 32;
        if (!ok) Log(L"Dolby: ShellExecute failed (%d)", static_cast<int>(reinterpret_cast<INT_PTR>(r)));
        return ok;
    } catch (...) {
        return false;
    }
}

} // namespace xm
