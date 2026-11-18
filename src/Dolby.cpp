// Dolby.cpp - spatial sound status (read), asynchronous Atmos toggle, and an experimental sound-mode switch that
// drives the Dolby Access window through UI Automation on a worker thread.
#include "Core.h"

#include <ole2.h>
#include <shellapi.h>
#include <shobjidl.h>
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
#include <optional>
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
constexpr ULONGLONG kSetTimeoutMs = 15000;   // spatial format switch
constexpr ULONGLONG kJobBudgetMs = 20000;    // whole UI Automation job
constexpr ULONGLONG kStepTimeoutMs = 12000;  // waiting for the window / settings tab
constexpr ULONGLONG kConfirmMs = 4000;
constexpr DWORD kShutdownWaitMs = 5000;
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
bool ContainsNoCase(const std::wstring& hay, const wchar_t* needle) {
    return FindNLSStringEx(LOCALE_NAME_INVARIANT, FIND_FROMSTART | LINGUISTIC_IGNORECASE, hay.c_str(),
                           static_cast<int>(hay.size()), needle, -1, nullptr, nullptr, nullptr, 0) >= 0;
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

// Profile name from one log line, if it is one of:
//   ...::Selected profile: Music
//   ...::Profile Music set or updated
//   ...::Initial configuration restored from AppService. Active profile: Music
std::optional<std::wstring> ProfileInLine(const std::wstring& line) {
    auto token = [&](size_t at) {
        size_t end = at;
        while (end < line.size() && !iswspace(line[end])) ++end;
        return line.substr(at, end - at);
    };
    for (const wchar_t* key : {L"Selected profile: ", L"Active profile: "}) {
        const size_t p = line.rfind(key);
        if (p != std::wstring::npos) return token(p + wcslen(key));
    }
    const size_t tail = line.rfind(L" set or updated");
    if (tail != std::wstring::npos) {
        const size_t p = line.rfind(L"Profile ", tail);
        if (p != std::wstring::npos) return line.substr(p + 8, tail - (p + 8));
    }
    return std::nullopt;
}

// Last profile line in `text` -> mode (-1 if that profile is unknown); nullopt if there is none.
std::optional<int> LastProfile(const std::wstring& text) {
    size_t end = text.size();
    while (end > 0) {
        const size_t nl = text.rfind(L'\n', end - 1);
        const size_t begin = nl == std::wstring::npos ? 0 : nl + 1;
        if (auto p = ProfileInLine(text.substr(begin, end - begin))) return ProfileToMode(*p);
        if (nl == std::wstring::npos) break;
        end = nl;
    }
    return std::nullopt;
}

int ReadCurrentMode() {
    const auto files = LogFiles();
    std::wstring text;
    for (size_t i = 0; i < files.size() && i < 3; ++i)
        if (ReadLogText(files[i], 0, text, nullptr))
            if (auto m = LastProfile(text)) return *m;
    return -1;
}

// True if a profile line for `mode` was written after byte `from` of `path`.
bool LogConfirms(const std::wstring& path, ULONGLONG from, int mode) {
    std::wstring text;
    if (!ReadLogText(path, from, text, nullptr)) return false;
    size_t begin = 0;
    while (begin < text.size()) {
        size_t nl = text.find(L'\n', begin);
        if (nl == std::wstring::npos) nl = text.size();
        if (auto p = ProfileInLine(text.substr(begin, nl - begin)); p && ProfileToMode(*p) == mode) return true;
        begin = nl + 1;
    }
    return false;
}

// ---- state shared by the Dolby object and its worker thread --------------------------------------------------
struct Shared {
    HWND notify = nullptr;
    std::atomic<bool> cancel{false};
    std::atomic<bool> dirty{false}; // Get() must drop its cache
    std::mutex m;
    std::condition_variable cv;
    bool running = false;    // a job thread is active
    bool toggleBusy = false;
    bool modeBusy = false;
    bool denied = false;     // Windows explicitly refused a spatial format switch this session
    std::wstring toggleNote; // why the last spatial format switch failed
    std::wstring modeError;
    int mode = -1;           // parsed from the Dolby Access log
    ULONGLONG modeAt = 0;    // when `mode` was read; 0 = never
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

// ---- job: switch the spatial sound format --------------------------------------------------------------------
void RunToggle(std::shared_ptr<Shared> sh, std::wstring endpointId, bool on) {
    bool denied = false;
    std::wstring note;
    {
        ComInit com;
        try {
            auto cfg = ConfigFor(endpointId);
            if (on) // informational only: returned false for every format on a licensed test machine
                Log(L"Dolby: IsSpatialAudioFormatSupported(Atmos for Headphones) = %d",
                    cfg.IsSpatialAudioFormatSupported(winrt::hstring(kAtmosHeadphones)) ? 1 : 0);
            auto op = cfg.SetDefaultSpatialAudioFormatAsync(winrt::hstring(on ? kAtmosHeadphones : kOffGuid));
            const ULONGLONG end = GetTickCount64() + kSetTimeoutMs;
            auto st = winrt::Windows::Foundation::AsyncStatus::Started;
            while ((st = op.wait_for(std::chrono::milliseconds(200))) == winrt::Windows::Foundation::AsyncStatus::Started &&
                   !sh->cancel && GetTickCount64() < end) {}
            if (st == winrt::Windows::Foundation::AsyncStatus::Started) {
                op.Cancel();
                Log(L"Dolby: SetEnabled timed out or cancelled");
                note = L"Windows did not finish switching spatial sound in time; try again or use Windows sound settings.";
            } else if (st == winrt::Windows::Foundation::AsyncStatus::Completed) {
                const auto status = op.GetResults().Status();
                Log(L"Dolby: SetEnabled(%d) status %d", on ? 1 : 0, static_cast<int>(status));
                if (status != wma::SetDefaultSpatialAudioFormatStatus::Succeeded) {
                    denied = true;
                    note = L"Windows did not allow XMTray to switch spatial sound; use Dolby Access or Windows sound settings.";
                }
            } else {
                Log(L"Dolby: SetEnabled failed 0x%08X", static_cast<unsigned>(op.ErrorCode().value));
                note = L"Switching spatial sound failed; try again or use Windows sound settings.";
            }
        } catch (const winrt::hresult_error& e) {
            Log(L"Dolby: SetEnabled failed 0x%08X", static_cast<unsigned>(e.code().value));
            note = L"Switching spatial sound failed; try again or use Windows sound settings.";
        } catch (...) {
            Log(L"Dolby: SetEnabled failed (unknown)");
            note = L"Switching spatial sound failed; try again or use Windows sound settings.";
        }
    }
    FinishJob(sh, [&](Shared& s) {
        s.toggleBusy = false;
        s.toggleNote = note;
        if (denied) s.denied = true;
    });
}

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

bool IsDolbyWindow(HWND w) {
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    return pid && IsDolbyProcess(pid);
}

bool HasClass(HWND w, const wchar_t* cls) {
    wchar_t buf[64];
    return GetClassNameW(w, buf, 64) && wcscmp(buf, cls) == 0;
}

// The ApplicationFrameWindow whose Windows.UI.Core.CoreWindow child belongs to DolbyAccess.exe. While the app is
// minimised the CoreWindow is detached from its frame and becomes top-level; then the frame is the one without a
// CoreWindow child whose title equals that CoreWindow's title (compared, not hard-coded: it is localised).
HWND FindDolbyFrame() {
    struct Ctx { HWND frame = nullptr; HWND looseCore = nullptr; std::vector<HWND> emptyFrames; } ctx;
    EnumWindows([](HWND top, LPARAM lp) -> BOOL {
        Ctx& c = *reinterpret_cast<Ctx*>(lp);
        if (HasClass(top, L"Windows.UI.Core.CoreWindow")) {
            if (!c.looseCore && IsDolbyWindow(top)) c.looseCore = top;
            return TRUE;
        }
        if (!HasClass(top, L"ApplicationFrameWindow")) return TRUE;
        bool hasCore = false;
        for (HWND w = nullptr; (w = FindWindowExW(top, w, L"Windows.UI.Core.CoreWindow", nullptr)) != nullptr;) {
            hasCore = true;
            if (IsDolbyWindow(w)) {
                c.frame = top;
                return FALSE;
            }
        }
        if (!hasCore) c.emptyFrames.push_back(top);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&ctx));
    if (ctx.frame || !ctx.looseCore) return ctx.frame;
    wchar_t want[256] = L"", have[256];
    if (GetWindowTextW(ctx.looseCore, want, 256) == 0) return nullptr;
    for (HWND f : ctx.emptyFrames)
        if (GetWindowTextW(f, have, 256) > 0 && wcscmp(have, want) == 0) return f;
    return nullptr;
}

struct Bstr {
    BSTR b = nullptr;
    Bstr() = default;
    explicit Bstr(const wchar_t* s) : b(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    std::wstring str() const { return b ? std::wstring(b, SysStringLen(b)) : std::wstring(); }
};

struct Node {
    com_ptr<IUIAutomationElement> el;
    int depth = 0;
    CONTROLTYPEID type = 0;
    std::wstring id, name;
    bool sel = false, inv = false, tog = false;
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

class ModeJob {
public:
    ModeJob(std::shared_ptr<Shared> sh, int mode) : sh_(std::move(sh)), mode_(mode) {}

    // Returns "" on success, else a short reason for the user.
    std::wstring Run() {
        std::wstring err = LaunchOrFind();
        if (err.empty()) err = PrepareWindow();
        if (err.empty()) err = StartUia();
        if (err.empty()) err = SelectSettingsTab();
        com_ptr<IUIAutomationElement> target;
        if (err.empty()) err = FindModeControl(target);
        if (err.empty()) err = ActivateAndConfirm(target);
        RestoreWindow();
        return err;
    }

private:
    std::shared_ptr<Shared> sh_;
    int mode_;
    ULONGLONG deadline_ = GetTickCount64() + kJobBudgetMs;
    HWND frame_ = nullptr;
    bool existed_ = false, launched_ = false, moved_ = false;
    WINDOWPLACEMENT wp_{sizeof(WINDOWPLACEMENT)};
    HWND fg_ = GetForegroundWindow();
    com_ptr<IUIAutomation> uia_;
    com_ptr<IUIAutomationElement> root_;

    // Poll `done` every `stepMs` until it returns true, `timeoutMs` passes, the job budget ends or we are cancelled.
    std::wstring Poll(ULONGLONG timeoutMs, DWORD stepMs, const std::function<bool()>& done, const wchar_t* timeoutErr) {
        const ULONGLONG end = std::min(GetTickCount64() + timeoutMs, deadline_);
        for (;;) {
            if (sh_->cancel) return L"Cancelled";
            if (done()) return {};
            if (GetTickCount64() >= end) return GetTickCount64() >= deadline_ ? L"Timed out" : timeoutErr;
            Sleep(stepMs);
        }
    }

    // 1. Reuse the Dolby Access window, or launch the app and wait for its window.
    std::wstring LaunchOrFind() {
        frame_ = FindDolbyFrame();
        existed_ = frame_ != nullptr;
        if (!existed_) {
            Log(L"Dolby mode: launching Dolby Access");
            com_ptr<IApplicationActivationManager> aam;
            DWORD pid = 0;
            HRESULT hr = CoCreateInstance(__uuidof(ApplicationActivationManager), nullptr, CLSCTX_LOCAL_SERVER,
                                          __uuidof(IApplicationActivationManager), aam.put_void());
            if (SUCCEEDED(hr)) hr = aam->ActivateApplication(kAppId, nullptr, AO_NONE, &pid);
            if (FAILED(hr)) {
                Log(L"Dolby mode: ActivateApplication failed 0x%08X", static_cast<unsigned>(hr));
                return L"Could not start Dolby Access";
            }
            launched_ = true;
            std::wstring err = Poll(kStepTimeoutMs, 100, [&] { return (frame_ = FindDolbyFrame()) != nullptr; },
                                    L"Dolby Access did not open a window");
            if (!err.empty()) return err;
        }
        if (!GetWindowPlacement(frame_, &wp_)) return L"Could not read the Dolby Access window position";
        Log(L"Dolby mode: window %p (%ls), showCmd %u", static_cast<void*>(frame_), existed_ ? L"reused" : L"launched",
            wp_.showCmd);
        return {};
    }

    // 2. A minimised UWP app is suspended and has no UI Automation tree: un-minimise without activating, then park
    //    the window off-screen so it stays alive but out of the way, and give the focus back.
    std::wstring PrepareWindow() {
        if (IsIconic(frame_)) ShowWindow(frame_, SW_SHOWNOACTIVATE);
        moved_ = SetWindowPos(frame_, nullptr, -32000, -32000, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        RestoreForeground();
        return {};
    }

    void RestoreForeground() {
        if (fg_ && IsWindow(fg_) && GetForegroundWindow() != fg_ && !SetForegroundWindow(fg_))
            Log(L"Dolby mode: could not give the focus back");
    }

    std::wstring StartUia() {
        HRESULT hr = CoCreateInstance(__uuidof(CUIAutomation8), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                      uia_.put_void());
        if (FAILED(hr))
            hr = CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, __uuidof(IUIAutomation),
                                  uia_.put_void());
        if (FAILED(hr)) return L"UI Automation is unavailable";
        if (auto u2 = uia_.try_as<IUIAutomation2>()) { // keep a hung app from blocking us (and shutdown) for long
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
        if (!selected && FAILED(sel->Select())) return L"Could not open the Dolby Access settings tab";
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
            if (depth > 40 || out.size() >= 2000 || sh_->cancel) return;
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

    // Best element for the requested mode in `nodes`, or nullptr. Rank: pattern (SelectionItem > Invoke > Toggle),
    // then a typical control type, then depth.
    const Node* Pick(const std::vector<Node>& nodes) const {
        const wchar_t* want = kModeNames[mode_];
        const Node* best = nullptr;
        int bestScore = -1;
        for (const Node& n : nodes) {
            if (!(n.sel || n.inv || n.tog)) continue;
            if (!EqualNoCase(n.name, want) && !ContainsNoCase(n.id, want)) continue;
            const bool typical = n.type == UIA_ListItemControlTypeId || n.type == UIA_RadioButtonControlTypeId ||
                                 n.type == UIA_TabItemControlTypeId || n.type == UIA_ButtonControlTypeId;
            const int score = (n.sel ? 3 : n.inv ? 2 : 1) * 10000 + (typical ? 1000 : 0) + n.depth;
            if (score > bestScore) { bestScore = score; best = &n; }
        }
        return best;
    }

    void DumpTree(const std::vector<Node>& nodes) const {
        Log(L"Dolby mode: layout dump (%zu elements):", nodes.size());
        size_t lines = 0;
        for (const Node& n : nodes) {
            if (++lines > 80) { Log(L"  ..."); break; }
            const wchar_t* t = TypeName(n.type);
            wchar_t num[16];
            if (!t) { swprintf_s(num, L"%d", n.type); t = num; }
            Log(L"  %*ls%ls id='%ls' name='%ls'%ls%ls%ls", n.depth * 2, L"", t, n.id.c_str(), n.name.substr(0, 60).c_str(),
                n.sel ? L" [SelectionItem]" : L"", n.inv ? L" [Invoke]" : L"", n.tog ? L" [Toggle]" : L"");
        }
    }

    // 4 + 5. Fail on the "enable Dolby Atmos" overlay; otherwise discover the mode control (ids unknown).
    std::wstring FindModeControl(com_ptr<IUIAutomationElement>& target) {
        bool overlay = false;
        std::vector<Node> last;
        std::wstring err = Poll(6000, 250, [&] {
            if (ById(root_.get(), L"SoundOverlay_setupOverlayTitle")) return overlay = true;
            com_ptr<IUIAutomationElement> page = ById(root_.get(), L"PcSoundSettingsPage");
            last = Walk(page ? page.get() : root_.get());
            if (const Node* n = Pick(last)) {
                target = n->el;
                Log(L"Dolby mode: control %ls id='%ls' name='%ls'", TypeName(n->type) ? TypeName(n->type) : L"?",
                    n->id.c_str(), n->name.c_str());
                return true;
            }
            return false;
        }, L"Dolby Access layout not recognised (app updated?)");
        if (overlay) {
            Log(L"Dolby mode: setup overlay shown (Atmos off on the current output)");
            return L"Dolby Atmos is not enabled on the current output";
        }
        if (err == L"Dolby Access layout not recognised (app updated?)") DumpTree(last);
        return err;
    }

    // 6. Select / invoke / toggle, then confirm by a new log line or by the element's own state.
    std::wstring ActivateAndConfirm(const com_ptr<IUIAutomationElement>& el) {
        const auto files = LogFiles();
        const std::wstring logPath = files.empty() ? std::wstring() : files.front();
        ULONGLONG logSize = 0;
        std::wstring ignored;
        if (!logPath.empty()) ReadLogText(logPath, ~0ULL, ignored, &logSize);

        com_ptr<IUIAutomationSelectionItemPattern> sel;
        com_ptr<IUIAutomationInvokePattern> inv;
        com_ptr<IUIAutomationTogglePattern> tog;
        HRESULT hr = E_FAIL;
        if (SUCCEEDED(el->GetCurrentPatternAs(UIA_SelectionItemPatternId, __uuidof(IUIAutomationSelectionItemPattern), sel.put_void())) && sel) {
            hr = sel->Select();
        } else if (SUCCEEDED(el->GetCurrentPatternAs(UIA_InvokePatternId, __uuidof(IUIAutomationInvokePattern), inv.put_void())) && inv) {
            hr = inv->Invoke();
        } else if (SUCCEEDED(el->GetCurrentPatternAs(UIA_TogglePatternId, __uuidof(IUIAutomationTogglePattern), tog.put_void())) && tog) {
            ToggleState ts = ToggleState_Off;
            tog->get_CurrentToggleState(&ts);
            hr = ts == ToggleState_On ? S_OK : tog->Toggle();
        }
        if (FAILED(hr)) {
            Log(L"Dolby mode: activating the control failed 0x%08X", static_cast<unsigned>(hr));
            return L"Dolby Access did not accept the change";
        }
        Log(L"Dolby mode: control activated");

        const std::wstring err = Poll(kConfirmMs, 200, [&] {
            const auto now = LogFiles();
            if (now.empty()) return false;
            return LogConfirms(now.front(), now.front() == logPath ? logSize : 0, mode_);
        }, L"no log line");
        if (err.empty()) {
            Log(L"Dolby mode: confirmed by the Dolby Access log");
            return {};
        }
        if (err != L"no log line") return err;
        BOOL selected = FALSE;
        ToggleState ts = ToggleState_Off;
        if ((sel && SUCCEEDED(sel->get_CurrentIsSelected(&selected)) && selected) ||
            (tog && SUCCEEDED(tog->get_CurrentToggleState(&ts)) && ts == ToggleState_On)) {
            Log(L"Dolby mode: confirmed by the control state");
            return {};
        }
        return L"Dolby Access did not confirm the change";
    }

    // 7. Put the window back; close the app if we started it.
    void RestoreWindow() {
        if (launched_ && !frame_) { // cancelled / timed out before its window appeared: wait briefly so we can close it
            const ULONGLONG end = GetTickCount64() + 3000;
            while (!(frame_ = FindDolbyFrame()) && GetTickCount64() < end) Sleep(100);
            if (frame_) {
                PostMessageW(frame_, WM_CLOSE, 0, 0);
                Log(L"Dolby mode: closed Dolby Access (WM_CLOSE, window appeared late)");
            } else {
                Log(L"Dolby mode: Dolby Access window never appeared; it may stay open");
            }
            RestoreForeground();
            return;
        }
        if (!frame_ || !IsWindow(frame_)) { RestoreForeground(); return; }
        WINDOWPLACEMENT wp = wp_;
        if (!existed_) {
            wp.showCmd = SW_HIDE; // remember the position for next time without flashing the window on screen
            com_ptr<IUIAutomationWindowPattern> win;
            if (root_) root_->GetCurrentPatternAs(UIA_WindowPatternId, __uuidof(IUIAutomationWindowPattern), win.put_void());
            if (moved_) SetWindowPlacement(frame_, &wp);
            const bool closed = win && SUCCEEDED(win->Close());
            if (!closed) PostMessageW(frame_, WM_CLOSE, 0, 0);
            Log(L"Dolby mode: closed Dolby Access (%ls)", closed ? L"WindowPattern" : L"WM_CLOSE");
        } else {
            wp.showCmd = wp_.showCmd == SW_SHOWMINIMIZED ? SW_SHOWMINNOACTIVE
                       : wp_.showCmd == SW_SHOWMAXIMIZED ? SW_SHOWMAXIMIZED : SW_SHOWNOACTIVATE;
            SetWindowPlacement(frame_, &wp);
            Log(L"Dolby mode: window restored");
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
            err = ModeJob(sh, mode).Run();
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
        s.modeAt = GetTickCount64();
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
            mark(*sh);
        }
        try {
            if (worker.joinable()) worker.join(); // previous job already finished
            worker = std::thread(std::move(job));
        } catch (...) {
            std::lock_guard<std::mutex> lk(sh->m);
            sh->running = sh->toggleBusy = sh->modeBusy = false;
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
            else { // it only touches Shared, which it co-owns
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

        bool running, toggleBusy, denied, refreshMode;
        std::wstring toggleNote;
        {
            std::lock_guard<std::mutex> lk(im.sh->m);
            running = im.sh->running;
            toggleBusy = im.sh->toggleBusy;
            denied = im.sh->denied;
            toggleNote = im.sh->toggleNote;
            s.modeBusy = im.sh->modeBusy;
            s.modeError = im.sh->modeError;
            refreshMode = im.automation && !im.sh->modeBusy && (im.sh->modeAt == 0 || now - im.sh->modeAt >= kModeCacheMs);
            s.mode = im.sh->mode;
        }
        if (refreshMode && s.appInstalled) { // file read only, no UI
            s.mode = ReadCurrentMode();
            std::lock_guard<std::mutex> lk(im.sh->m);
            im.sh->mode = s.mode;
            im.sh->modeAt = now;
        }

        s.canToggle = s.appInstalled && s.statusKnown && b.supported && !denied && !running;
        s.canSetMode = im.automation && s.appInstalled && b.atmosActive && !running;
        if (im.automation && s.appInstalled) {
            for (int i = 0; i < kModeCount; ++i) s.modes.push_back({i, kModeNames[i]});
        } else {
            s.mode = -1;
        }

        std::wstring note;
        if (!s.appInstalled) note += L"Dolby Access is not installed. ";
        if (!s.note.empty()) note += s.note + L" ";
        if (toggleBusy) note += L"Switching spatial sound… ";
        else if (!toggleNote.empty()) note += toggleNote + L" ";
        if (denied && toggleNote.empty())
            note += L"Windows did not allow XMTray to switch spatial sound; use Dolby Access or Windows sound settings. ";
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

bool Dolby::SetEnabled(const std::wstring& endpointId, bool atmosOn) {
    try {
        auto sh = impl_->sh;
        const bool started = impl_->Start([sh, endpointId, atmosOn] { RunToggle(sh, endpointId, atmosOn); },
                                          [](Shared& s) { s.toggleBusy = true; s.toggleNote.clear(); });
        if (!started) Log(L"Dolby: SetEnabled ignored, a job is running");
        return started;
    } catch (...) {
        Log(L"Dolby: SetEnabled failed to start");
        return false;
    }
}

void Dolby::SetAutomation(bool enabled) {
    impl_->automation = enabled;
    impl_->cache.clear();
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
