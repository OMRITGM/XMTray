// Tray.cpp - notification-area icon (battery digits / headphone glyph), tooltip, toast, context menu.
#include "Core.h"
#include "resource.h"
#include <shellapi.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "advapi32.lib")

namespace xm {
namespace {

enum class Glyph { Headphones, Battery };

struct IconKey {
    Glyph glyph = Glyph::Headphones;
    int percent = 0;
    bool charging = false;
    bool light = false;
    int size = 0;
    bool operator==(const IconKey&) const = default;
};

bool TaskbarIsLight() {
    DWORD v = 0, sz = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) != ERROR_SUCCESS)
        return false;
    return v != 0;
}

// 32bpp top-down DIB wrapper.
struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    void* bits = nullptr;
    int n = 0;
    explicit Dib(int size) : n(size) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = size;
        bi.bmiHeader.biHeight = -size;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (bmp) old = (HBITMAP)SelectObject(dc, bmp);
        if (bits) memset(bits, 0, (size_t)size * size * 4);
    }
    ~Dib() {
        if (old) SelectObject(dc, old);
        if (bmp) DeleteObject(bmp);
        if (dc) DeleteDC(dc);
    }
    Dib(const Dib&) = delete;
    Dib& operator=(const Dib&) = delete;
    bool ok() const { return bmp && bits; }
    BYTE* px() const { return static_cast<BYTE*>(bits); }
};

void DrawHeadphones(HDC dc, int s) {
    // white on black mask
    int pw = std::max(1, s / 8);
    HPEN pen = CreatePen(PS_SOLID, pw, RGB(255, 255, 255));
    HGDIOBJ op = SelectObject(dc, pen);
    HGDIOBJ ob = SelectObject(dc, GetStockObject(NULL_BRUSH));
    int l = s / 8, r = s - s / 8, t = s / 12 + pw / 2, b = s - s / 6;
    Arc(dc, l, t, r, t + (b - t) * 2 - s / 4, r, t + (b - t) - s / 12, l, t + (b - t) - s / 12); // headband
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(pen);
    HBRUSH br = CreateSolidBrush(RGB(255, 255, 255));
    HGDIOBJ obr = SelectObject(dc, br);
    HGDIOBJ opn = SelectObject(dc, GetStockObject(NULL_PEN));
    int cw = std::max(3, s * 3 / 16), ch = s * 7 / 16;
    int cy = s - s / 12 - ch;
    RoundRect(dc, l - pw / 2, cy, l - pw / 2 + cw + 1, cy + ch + 1, 2, 2);
    RoundRect(dc, r + pw / 2 - cw, cy, r + pw / 2 + 1, cy + ch + 1, 2, 2);
    SelectObject(dc, opn);
    SelectObject(dc, obr);
    DeleteObject(br);
}

void DrawDigits(HDC dc, int s, int percent) {
    wchar_t txt[8];
    swprintf(txt, 8, L"%d", std::clamp(percent, 0, 100));
    int len = (int)wcslen(txt);
    HFONT font = nullptr;
    SIZE ext{};
    int h = len >= 3 ? s * 3 / 5 : s * 7 / 8;
    for (; h >= 6; --h) {
        if (font) DeleteObject(font);
        font = CreateFontW(-h, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
        HGDIOBJ o = SelectObject(dc, font);
        GetTextExtentPoint32W(dc, txt, len, &ext);
        SelectObject(dc, o);
        if (ext.cx <= s) break;
    }
    HGDIOBJ o = SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(255, 255, 255));
    TextOutW(dc, (s - ext.cx) / 2, (s - ext.cy) / 2, txt, len);
    SelectObject(dc, o);
    DeleteObject(font);
}

void DrawBolt(HDC dc, int s, COLORREF c, int grow) {
    int bw = std::max(5, s * 5 / 16), bh = std::max(7, s * 7 / 16);
    int x = s - bw - 1, y = s - bh - 1;
    POINT p[6] = {{x + bw * 2 / 3, y}, {x, y + bh * 11 / 20}, {x + bw * 9 / 20, y + bh * 11 / 20},
                  {x + bw / 3, y + bh}, {x + bw, y + bh * 9 / 20}, {x + bw * 11 / 20, y + bh * 9 / 20}};
    HBRUSH br = CreateSolidBrush(c);
    HPEN pen = CreatePen(PS_SOLID, grow, c);
    HGDIOBJ ob = SelectObject(dc, br), op = SelectObject(dc, pen);
    Polygon(dc, p, 6);
    SelectObject(dc, ob);
    SelectObject(dc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

HICON MakeIcon(const IconKey& k) {
    const int s = k.size;
    Dib mask(s);
    if (!mask.ok()) return nullptr;
    HDC dc = mask.dc;
    if (k.glyph == Glyph::Battery) DrawDigits(dc, s, k.percent);
    else DrawHeadphones(dc, s);

    BYTE fr, fg, fb;
    if (k.glyph == Glyph::Battery && k.percent <= 20) {
        fr = k.light ? 200 : 255; fg = k.light ? 30 : 80; fb = k.light ? 30 : 70;
    } else {
        fr = fg = fb = k.light ? 0 : 255;
    }

    Dib color(s);
    Dib bolt(s), halo(s);
    if (!color.ok() || !bolt.ok() || !halo.ok()) return nullptr;
    if (k.charging) {
        DrawBolt(bolt.dc, s, RGB(255, 255, 255), 1);
        DrawBolt(halo.dc, s, RGB(255, 255, 255), 3);
    }
    for (int i = 0; i < s * s; ++i) {
        const BYTE* m = mask.px() + (size_t)i * 4; // BGRA; coverage = green channel
        int a = m[1];
        int r = fr, g = fg, b = fb;
        if (k.charging) {
            if (halo.px()[(size_t)i * 4 + 1] > 40) a = a * (255 - halo.px()[(size_t)i * 4 + 1]) / 255;
            int ba = bolt.px()[(size_t)i * 4 + 1];
            if (ba) {
                // bolt over (possibly cut-out) glyph, straight alpha composite in premultiplied space
                int pr = r * a / 255, pg = g * a / 255, pb = b * a / 255;
                const int br = 255, bg = 205, bb = 0;
                pr = pr * (255 - ba) / 255 + br * ba / 255;
                pg = pg * (255 - ba) / 255 + bg * ba / 255;
                pb = pb * (255 - ba) / 255 + bb * ba / 255;
                a = a + (255 - a) * ba / 255;
                BYTE* d = color.px() + (size_t)i * 4;
                d[0] = (BYTE)pb; d[1] = (BYTE)pg; d[2] = (BYTE)pr; d[3] = (BYTE)a;
                continue;
            }
        }
        BYTE* d = color.px() + (size_t)i * 4;
        d[0] = (BYTE)(b * a / 255); d[1] = (BYTE)(g * a / 255); d[2] = (BYTE)(r * a / 255); d[3] = (BYTE)a;
    }

    const std::vector<BYTE> zeros(size_t(((s + 15) / 16) * 2) * size_t(s), 0);
    HBITMAP andMask = CreateBitmap(s, s, 1, 1, zeros.data()); // ignored when colour bitmap has alpha
    ICONINFO ii{};
    ii.fIcon = TRUE;
    ii.hbmMask = andMask;
    ii.hbmColor = color.bmp;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(andMask);
    return icon;
}

const wchar_t* NcText(NcMode m) {
    switch (m) {
    case NcMode::NoiseCancelling: return L"Noise cancelling";
    case NcMode::Ambient: return L"Ambient sound";
    case NcMode::Wind: return L"Wind noise reduction";
    default: return L"Noise control off";
    }
}

} // namespace

struct Tray::Impl {
    HWND owner = nullptr;
    NOTIFYICONDATAW nid{};
    HICON icon = nullptr;
    HICON appIcon = nullptr; // shared, not destroyed
    IconKey key;
    std::wstring tip;

    void FillTip(const std::wstring& t) {
        wcsncpy_s(nid.szTip, ARRAYSIZE(nid.szTip), t.c_str(), _TRUNCATE);
    }
    void Add() {
        Shell_NotifyIconW(NIM_ADD, &nid);
        nid.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &nid);
    }
    int IconSize() const {
        UINT dpi = GetDpiForWindow(owner);
        if (!dpi) dpi = GetDpiForSystem();
        return GetSystemMetricsForDpi(SM_CXSMICON, dpi);
    }
};

Tray::Tray(HWND owner) : impl_(new Impl) {
    impl_->owner = owner;
    impl_->appIcon = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
    NOTIFYICONDATAW& n = impl_->nid;
    n.cbSize = sizeof(n);
    n.hWnd = owner;
    n.uID = 1;
    n.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    n.uCallbackMessage = WM_XM_TRAY;
    impl_->key = {Glyph::Headphones, 0, false, TaskbarIsLight(), impl_->IconSize()};
    impl_->icon = MakeIcon(impl_->key);
    n.hIcon = impl_->icon ? impl_->icon : impl_->appIcon;
    impl_->tip = L"XMTray — no headphones connected";
    impl_->FillTip(impl_->tip);
    impl_->Add();
}

Tray::~Tray() {
    Shell_NotifyIconW(NIM_DELETE, &impl_->nid);
    if (impl_->icon) DestroyIcon(impl_->icon);
    delete impl_;
}

void Tray::Readd() { impl_->Add(); }

void Tray::Update(const Snapshot& s) {
    IconKey k{Glyph::Headphones, 0, false, TaskbarIsLight(), impl_->IconSize()};
    std::wstring tip;

    if (s.link == Link::Ready) {
        int low = 101;
        bool charging = false;
        for (const Battery* b : {&s.battSingle, &s.battLeft, &s.battRight}) {
            if (!b->present) continue;
            low = std::min(low, b->percent);
            if (b->charging == Charging::Yes) charging = true;
        }
        tip = s.model.empty() ? L"Sony headphones" : s.model;
        if (low <= 100) {
            k.glyph = Glyph::Battery;
            k.percent = low;
            k.charging = charging;
            tip += L" — " + std::to_wstring(low) + L"%";
        }
        if (s.hasNc) tip += std::wstring(L" — ") + NcText(s.nc);
    } else if (s.link == Link::Connecting || s.link == Link::Initializing) {
        tip = L"XMTray — connecting…";
    } else if (s.link == Link::Failed) {
        tip = L"XMTray — " + (s.error.empty() ? std::wstring(L"connection failed") : s.error);
    } else {
        tip = L"XMTray — no headphones connected";
    }
    if (tip.size() > 127) tip.resize(127);

    bool iconChanged = !(k == impl_->key);
    if (!iconChanged && tip == impl_->tip) return;

    HICON old = nullptr;
    if (iconChanged) {
        if (HICON ni = MakeIcon(k)) {
            old = impl_->icon;
            impl_->icon = ni;
            impl_->nid.hIcon = ni;
            impl_->key = k;
        }
    }
    impl_->tip = tip;
    impl_->FillTip(tip);
    Shell_NotifyIconW(NIM_MODIFY, &impl_->nid);
    if (old) DestroyIcon(old);
}

void Tray::Notify(const std::wstring& title, const std::wstring& text) {
    NOTIFYICONDATAW n{};
    n.cbSize = sizeof(n);
    n.hWnd = impl_->owner;
    n.uID = impl_->nid.uID;
    n.uFlags = NIF_INFO | NIF_SHOWTIP;
    wcsncpy_s(n.szInfoTitle, ARRAYSIZE(n.szInfoTitle), title.c_str(), 63);
    wcsncpy_s(n.szInfo, ARRAYSIZE(n.szInfo), text.c_str(), 255);
    n.dwInfoFlags = NIIF_RESPECT_QUIET_TIME;
    if (impl_->appIcon) {
        n.dwInfoFlags |= NIIF_USER | NIIF_LARGE_ICON;
        n.hBalloonIcon = impl_->appIcon;
    } else {
        n.dwInfoFlags |= NIIF_INFO;
    }
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

RECT Tray::IconRect() const {
    NOTIFYICONIDENTIFIER id{};
    id.cbSize = sizeof(id);
    id.hWnd = impl_->owner;
    id.uID = impl_->nid.uID;
    RECT rc{};
    if (SUCCEEDED(Shell_NotifyIconGetRect(&id, &rc)) && rc.right > rc.left) return rc;
    RECT wa{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    return RECT{wa.right - 32, wa.bottom - 32, wa.right, wa.bottom};
}

int Tray::ShowMenu(const std::vector<BtDevice>& devices, const std::string& activeMac, bool autostart) {
    HMENU m = CreatePopupMenu();
    if (!m) return None;
    auto add = [&](UINT id, const std::wstring& text, UINT type, UINT state) {
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_FTYPE | MIIM_STATE | MIIM_ID | MIIM_STRING;
        mi.fType = MFT_STRING | type;
        mi.fState = state;
        mi.wID = id;
        mi.dwTypeData = const_cast<LPWSTR>(text.c_str());
        InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
    };
    auto sep = [&] {
        MENUITEMINFOW mi{};
        mi.cbSize = sizeof(mi);
        mi.fMask = MIIM_FTYPE;
        mi.fType = MFT_SEPARATOR;
        InsertMenuItemW(m, GetMenuItemCount(m), TRUE, &mi);
    };

    if (devices.empty()) {
        add(0, L"No Sony headphones connected", 0, MFS_DISABLED);
    } else {
        for (size_t i = 0; i < devices.size(); ++i) {
            std::wstring name;
            for (wchar_t c : devices[i].name) {
                name += c;
                if (c == L'&') name += L'&';
            }
            add((UINT)(DeviceBase + i), name, MFT_RADIOCHECK, devices[i].mac == activeMac ? MFS_CHECKED : 0);
        }
    }
    sep();
    add(Open, L"Open", 0, 0);
    add(ToggleAutostart, L"Start with Windows", 0, autostart ? MFS_CHECKED : 0);
    add(OpenLogFolder, L"Open log folder", 0, 0);
    sep();
    add(Quit, L"Quit", 0, 0);

    POINT pt{};
    GetCursorPos(&pt);
    SetForegroundWindow(impl_->owner);
    int cmd = (int)TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, pt.x, pt.y, 0,
                                  impl_->owner, nullptr);
    PostMessageW(impl_->owner, WM_NULL, 0, 0);
    DestroyMenu(m);
    return cmd > 0 ? cmd : None;
}

} // namespace xm
