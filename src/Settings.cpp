// Settings.cpp - settings.ini (UTF-16LE), HKCU Run autostart entry, and the rotating log.
#include "Core.h"

#include <windows.h>
#include <knownfolders.h>
#include <objbase.h>
#include <shlobj.h>

#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <iterator>
#include <string>
#include <vector>

namespace xm {
namespace {

constexpr int kMaxPresets = 32;
constexpr size_t kMaxPresetName = 40;
constexpr int kMaxBands = 10;
constexpr ULONGLONG kMaxLogBytes = 1024ull * 1024ull;
constexpr const wchar_t* kRunKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* kRunValue = L"XMTray";

SRWLOCK g_logLock = SRWLOCK_INIT;

struct LogLock {
    LogLock() { AcquireSRWLockExclusive(&g_logLock); }
    ~LogLock() { ReleaseSRWLockExclusive(&g_logLock); }
    LogLock(const LogLock&) = delete;
    LogLock& operator=(const LogLock&) = delete;
};

std::string ToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

std::wstring IniPath() { return DataDir() + L"\\settings.ini"; }

int ReadInt(const std::wstring& sec, const wchar_t* key, int def, const std::wstring& path) {
    return static_cast<int>(GetPrivateProfileIntW(sec.c_str(), key, def, path.c_str()));
}

// bufChars = buffer size including the terminator, so at most bufChars-1 characters are read.
std::wstring ReadStr(const std::wstring& sec, const wchar_t* key, size_t bufChars, const std::wstring& path) {
    std::wstring buf(bufChars, L'\0');
    const DWORD n = GetPrivateProfileStringW(sec.c_str(), key, L"", buf.data(), static_cast<DWORD>(bufChars), path.c_str());
    buf.resize(n);
    return buf;
}

void WriteStr(const std::wstring& sec, const wchar_t* key, const std::wstring& value, const std::wstring& path) {
    WritePrivateProfileStringW(sec.c_str(), key, value.c_str(), path.c_str());
}

void WriteInt(const std::wstring& sec, const wchar_t* key, int value, const std::wstring& path) {
    WriteStr(sec, key, std::to_wstring(value), path);
}

// Profile API only treats the file as Unicode when it starts with a UTF-16LE BOM; a new file would be ANSI.
void EnsureUtf16(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        unsigned char head[2] = {0, 0};
        DWORD got = 0;
        const BOOL ok = ReadFile(h, head, 2, &got, nullptr);
        CloseHandle(h);
        if (ok && got == 2 && head[0] == 0xFF && head[1] == 0xFE) return;
        // Exists without a BOM (hand-edited): keep it aside instead of silently wiping it.
        MoveFileExW(path.c_str(), (path + L".bak").c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    const unsigned char bom[2] = {0xFF, 0xFE};
    DWORD written = 0;
    WriteFile(h, bom, 2, &written, nullptr);
    CloseHandle(h);
}

// Comma-separated ints into bands[0..10); stops at the first token that is not a number.
void ParseBands(const std::wstring& text, std::array<int, kMaxBands>& bands) {
    const wchar_t* p = text.c_str();
    for (size_t i = 0; i < bands.size() && *p; ++i) {
        wchar_t* end = nullptr;
        const long v = std::wcstol(p, &end, 10);
        if (end == p) break;
        bands[i] = static_cast<int>(v);
        p = end;
        if (*p != L',') break;
        ++p;
    }
}

std::wstring SanitizeName(const std::wstring& n) {
    std::wstring name = n.substr(0, kMaxPresetName);
    for (wchar_t& c : name)
        if (c == L'\r' || c == L'\n') c = L' ';
    return name;
}

std::wstring ExePath() {
    std::vector<wchar_t> buf(32768);
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
    return n == 0 ? std::wstring() : std::wstring(buf.data(), n);
}

// HKCU only. Writes the current exe path on, deletes the value off.
void ApplyAutostart(bool on) {
    const std::wstring exePath = on ? ExePath() : std::wstring();
    const std::wstring wanted = exePath.empty() ? std::wstring() : L"\"" + exePath + L"\"";
    wchar_t current[1024] = L"";
    DWORD bytes = sizeof(current) - sizeof(wchar_t);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kRunValue, RRF_RT_REG_SZ, nullptr, current, &bytes) != ERROR_SUCCESS)
        current[0] = L'\0';
    if (wanted == current) return; // already in the desired state: no registry write
    HKEY key = nullptr;
    const LSTATUS open = RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &key);
    if (open != ERROR_SUCCESS) {
        Log(L"autostart: cannot open HKCU Run key (%ld)", open);
        return;
    }
    if (on) {
        const std::wstring exe = ExePath();
        if (!exe.empty()) {
            const std::wstring cmd = L"\"" + exe + L"\"";
            const LSTATUS rc = RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(cmd.c_str()),
                                              static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
            if (rc != ERROR_SUCCESS) Log(L"autostart: RegSetValueEx failed (%ld)", rc);
        }
    } else {
        const LSTATUS rc = RegDeleteValueW(key, kRunValue);
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) Log(L"autostart: RegDeleteValue failed (%ld)", rc);
    }
    RegCloseKey(key);
}

} // namespace

Settings LoadSettings() {
    const std::wstring path = IniPath();
    const std::wstring gen = L"general";
    Settings s;

    s.autostart = ReadInt(gen, L"autostart", s.autostart, path) != 0;
    s.notifyConnect = ReadInt(gen, L"notifyConnect", s.notifyConnect, path) != 0;
    s.notifyLowBattery = ReadInt(gen, L"notifyLowBattery", s.notifyLowBattery, path) != 0;
    s.lowBatteryPercent = std::clamp(ReadInt(gen, L"lowBatteryPercent", s.lowBatteryPercent, path), 5, 50);
    s.routeAudioOnSelect = ReadInt(gen, L"routeAudioOnSelect", s.routeAudioOnSelect, path) != 0;
    s.followWindowsOutput = ReadInt(gen, L"followWindowsOutput", s.followWindowsOutput, path) != 0;
    s.disconnectWhenHidden = ReadInt(gen, L"disconnectWhenHidden", s.disconnectWhenHidden, path) != 0;
    s.resetEqOnDolbyMode = ReadInt(gen, L"resetEqOnDolbyMode", s.resetEqOnDolbyMode, path) != 0;
    s.dolbyAutomation = ReadInt(gen, L"dolbyAutomation", s.dolbyAutomation, path) != 0;
    s.lastMac = ToUtf8(ReadStr(gen, L"lastMac", 64, path));

    const int count = std::clamp(ReadInt(L"eq", L"count", 0, path), 0, kMaxPresets);
    for (int i = 0; i < count; ++i) {
        const std::wstring sec = L"eq" + std::to_wstring(i);
        EqPreset p;
        p.name = ReadStr(sec, L"name", kMaxPresetName + 1, path);
        p.bandCount = std::clamp(ReadInt(sec, L"bandCount", 0, path), 0, kMaxBands);
        ParseBands(ReadStr(sec, L"bands", 256, path), p.bands);
        p.clearBass = ReadInt(sec, L"clearBass", 0, path);
        s.eqPresets.push_back(std::move(p));
    }

    // Refresh the stored exe path in case the program has moved.
    if (s.autostart) ApplyAutostart(true);
    return s;
}

void SaveSettings(const Settings& s) {
    const std::wstring path = IniPath();
    EnsureUtf16(path);
    const std::wstring gen = L"general";

    WriteInt(gen, L"autostart", s.autostart ? 1 : 0, path);
    WriteInt(gen, L"notifyConnect", s.notifyConnect ? 1 : 0, path);
    WriteInt(gen, L"notifyLowBattery", s.notifyLowBattery ? 1 : 0, path);
    WriteInt(gen, L"lowBatteryPercent", std::clamp(s.lowBatteryPercent, 5, 50), path);
    WriteInt(gen, L"routeAudioOnSelect", s.routeAudioOnSelect ? 1 : 0, path);
    WriteInt(gen, L"followWindowsOutput", s.followWindowsOutput ? 1 : 0, path);
    WriteInt(gen, L"disconnectWhenHidden", s.disconnectWhenHidden ? 1 : 0, path);
    WriteInt(gen, L"resetEqOnDolbyMode", s.resetEqOnDolbyMode ? 1 : 0, path);
    WriteInt(gen, L"dolbyAutomation", s.dolbyAutomation ? 1 : 0, path);
    WriteStr(gen, L"lastMac", ToWide(s.lastMac), path);

    const int oldCount = std::clamp(ReadInt(L"eq", L"count", 0, path), 0, kMaxPresets);
    const int count = std::min(static_cast<int>(s.eqPresets.size()), kMaxPresets);
    WriteInt(L"eq", L"count", count, path);
    for (int i = 0; i < count; ++i) {
        const EqPreset& p = s.eqPresets[static_cast<size_t>(i)];
        const std::wstring sec = L"eq" + std::to_wstring(i);
        const int bc = std::clamp(p.bandCount, 0, kMaxBands);
        std::wstring bands;
        for (int b = 0; b < bc; ++b) {
            if (b) bands += L',';
            bands += std::to_wstring(p.bands[static_cast<size_t>(b)]);
        }
        WriteStr(sec, L"name", SanitizeName(p.name), path);
        WriteInt(sec, L"bandCount", bc, path);
        WriteStr(sec, L"bands", bands, path);
        WriteInt(sec, L"clearBass", p.clearBass, path);
    }
    for (int i = count; i < oldCount; ++i) {
        const std::wstring stale = L"eq" + std::to_wstring(i);
        WritePrivateProfileStringW(stale.c_str(), nullptr, nullptr, path.c_str());
    }

    ApplyAutostart(s.autostart);
}

std::wstring DataDir() {
    std::wstring dir;
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base)) && base) dir = base;
    CoTaskMemFree(base);
    dir += L"\\XMTray";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void Log(const wchar_t* fmt, ...) {
    try {
        wchar_t msg[1024];
        va_list ap;
        va_start(ap, fmt);
        _vsnwprintf_s(msg, std::size(msg), _TRUNCATE, fmt, ap);
        va_end(ap);

        SYSTEMTIME st{};
        GetLocalTime(&st);
        wchar_t stamp[64];
        _snwprintf_s(stamp, std::size(stamp), _TRUNCATE, L"%04u-%02u-%02u %02u:%02u:%02u.%03u ",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        const std::string line = ToUtf8(std::wstring(stamp) + msg + L"\r\n");

        LogLock lock;
        const std::wstring dir = DataDir();
        const std::wstring logPath = dir + L"\\xmtray.log";

        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(logPath.c_str(), GetFileExInfoStandard, &fa)) {
            ULARGE_INTEGER bytes{};
            bytes.HighPart = fa.nFileSizeHigh;
            bytes.LowPart = fa.nFileSizeLow;
            if (bytes.QuadPart > kMaxLogBytes)
                MoveFileExW(logPath.c_str(), (dir + L"\\xmtray.old.log").c_str(), MOVEFILE_REPLACE_EXISTING);
        }

        HANDLE h = CreateFileW(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            DWORD written = 0;
            WriteFile(h, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
            CloseHandle(h);
        }
    } catch (...) {
        // Logging must never throw or take the app down.
    }
}

} // namespace xm
