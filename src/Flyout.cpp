// Flyout.cpp - the XMTray main window (the class keeps its historical name): a resizable top-level window
// drawn with Direct2D + DirectWrite in an immediate style. Every paint rebuilds a list of hit-testable items;
// input is resolved against the last list. Cards are measured with a "dry" pass (no drawing) and then drawn.
#include "Core.h"
#include "resource.h"

#include <windowsx.h>
#include <shellapi.h>
#include <d2d1.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <functional>
#include <map>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "shcore.lib")
#pragma comment(lib, "shell32.lib")

using Microsoft::WRL::ComPtr;

namespace xm {
namespace {

// ---- constants (DIP) ----------------------------------------------------------------------
constexpr wchar_t kClass[] = L"XMTrayFlyout";
constexpr wchar_t kVersion[] = L"0.3.0";
constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW;
constexpr int kDefW = 940, kDefH = 640, kMinW = 820, kMinH = 560;
constexpr float SB = 200.f;     // sidebar width
constexpr float TOPH = 76.f;    // top bar height
constexpr float GUT = 28.f;     // content side gutter
constexpr float GAP = 16.f;     // gap between cards
constexpr float CP = 20.f;      // card padding
constexpr float RAD = 10.f;     // card radius
constexpr float MAXW = 1040.f;  // content column max width
constexpr UINT WM_XM_QUIT = WM_APP + 50;
constexpr UINT_PTR TIMER_DRAG = 1, TIMER_ANIM = 2;
constexpr ULONGLONG kSendMs = 80;
constexpr size_t kMaxPresets = 32, kMaxName = 32;

enum : int {
    ID_BLOCK = 1, ID_NAV = 10, ID_DEVSEL = 20, ID_DD = 100, ID_DD_BT = 190,
    ID_NC = 200, ID_AMB = 220, ID_VF, ID_VOL, ID_STC, ID_STC_SENS = 230, ID_STC_TO = 240,
    ID_TOUCH = 250, ID_DSEE, ID_PAUSE, ID_APO = 260, ID_PRIO = 280,
    ID_POWER = 290, ID_POWER_OK, ID_POWER_NO, ID_RETRY, ID_BTSET,
    ID_HOST = 300, ID_EQP = 400, ID_BAND = 450, ID_CB = 470, ID_FLAT, ID_NAME, ID_SAVE,
    ID_UP = 500, ID_RENAME = 700, ID_REN_OK, ID_REN_NO, ID_DEL_OK, ID_DEL_NO,
    ID_DOLBY_TOG = 720, ID_DOLBY_OPEN, ID_DOLBY_RESET, ID_DOLBY_AUTO, ID_DMODE = 740,
    ID_SET = 800, ID_STEP_DN = 820, ID_STEP_UP, ID_LOG, ID_QUIT, ID_ALERT_NO = 850, ID_ALERT_OK,
    ID_ADAPT = 900, ID_ADAPT_SENS = 910, ID_PREV = 950, ID_PLAYPAUSE, ID_NEXT,
    ID_NCBTN = 1000, ID_LMODE = 1050, ID_ROOM = 1100, ID_VA = 1150,
    ID_VG = 1200, ID_VGVOL, ID_HG, ID_WPO, ID_SRC, ID_PAIR, ID_PAUSE2,
    ID_ASSIGN = 1300, ID_GEN = 2400, ID_PICK = 5000, // ASSIGN / GEN: 50 ids per row
};
constexpr size_t kMaxAssign = 20, kMaxGeneral = 50;
enum Style { BtnNormal, BtnPrimary, BtnDanger, BtnDangerOutline, BtnSubtle };
enum Align { AlLeft, AlCenter, AlRight };
enum Wt { Reg, Semi };
enum Glyph { GNone, GHome, GEq, GSpatial, GGear, GInfo, GHeadphones, GSpeaker, GBolt, GCheck, GChevron,
             GLaptop, GPhone, GWarn, GPower, GBluetooth, GControls, GMusic, GPlay, GPause, GNext, GPrev };

using C = D2D1_COLOR_F;
constexpr float F(int v) { return static_cast<float>(v); }
float F(size_t v) { return static_cast<float>(v); }
C Rgb(UINT32 rgb, float a = 1.f) { return D2D1::ColorF(rgb, a); }
C Alpha(C c, float a) { c.a = a; return c; }
C Mix(C a, C b, float t) { return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, 1.f}; }
float Lin(float c) { return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f); }
float RelLum(C c) { return 0.2126f * Lin(c.r) + 0.7152f * Lin(c.g) + 0.0722f * Lin(c.b); }
float Contrast(C a, C b) {
    float x = RelLum(a) + 0.05f, y = RelLum(b) + 0.05f;
    return x > y ? x / y : y / x;
}
C Ensure(C c, C against, float ratio, C toward) {
    for (int i = 0; i < 14 && Contrast(c, against) < ratio; ++i) c = Mix(c, toward, 0.1f);
    return c;
}
COLORREF Ref(C c) {
    return RGB(static_cast<BYTE>(c.r * 255.f + 0.5f), static_cast<BYTE>(c.g * 255.f + 0.5f),
               static_cast<BYTE>(c.b * 255.f + 0.5f));
}
D2D1_RECT_F Inflate(D2D1_RECT_F r, float d) { return {r.left - d, r.top - d, r.right + d, r.bottom + d}; }
D2D1_RECT_F Offset(D2D1_RECT_F r, float dx, float dy) { return {r.left + dx, r.top + dy, r.right + dx, r.bottom + dy}; }
std::wstring Widen(const std::string& s) { return std::wstring(s.begin(), s.end()); }

struct Palette {
    bool dark = true;
    C bg, card, popup, inset, btn, hover, press, border, divider, text, text2, text3, track;
    C accent, accentText, onAccent, accentSoft, navSel, good, warn, bad, danger, overlay;
};

struct Item {
    D2D1_RECT_F rc{}, hit{};
    int id = 0;
    bool focus = true, content = false, slider = false, vertical = false;
    std::wstring* text = nullptr; // editable text field buffer
    int mn = 0, mx = 0, step = 1, val = 0;
    std::function<void()> click;
    std::function<void(int, bool)> change; // (value, final)
    size_t key = 0; // what the item addresses; a mouse release fires only if it still matches the press
};

struct Col {
    float x, w;
};

std::wstring CodecName(Codec c) {
    switch (c) {
    case Codec::SBC: return L"SBC";
    case Codec::AAC: return L"AAC";
    case Codec::LDAC: return L"LDAC";
    case Codec::AptX: return L"aptX";
    case Codec::AptXHD: return L"aptX HD";
    case Codec::LC3: return L"LC3";
    case Codec::Other: return L"Other";
    default: return L"Unknown";
    }
}

struct CodecInfo {
    const wchar_t *bitrate, *format, *desc;
};
CodecInfo InfoFor(Codec c) {
    switch (c) {
    case Codec::SBC: return {L"Up to 328 kbps", L"16-bit / 48 kHz", L"The baseline Bluetooth codec that every device supports."};
    case Codec::AAC: return {L"Up to 256 kbps (typ.)", L"16-bit / 44.1–48 kHz", L"An efficient lossy codec, common on phones and tablets."};
    case Codec::AptX: return {L"352 kbps", L"16-bit / 48 kHz", L"A fixed-rate codec with steady quality and modest latency."};
    case Codec::AptXHD: return {L"576 kbps", L"24-bit / 48 kHz", L"A higher-bitrate aptX variant that carries 24-bit audio."};
    case Codec::LDAC: return {L"Up to 990 kbps", L"24-bit / up to 96 kHz", L"A high-bitrate codec for high-resolution audio over Bluetooth."};
    case Codec::LC3: return {L"Up to 345 kbps (typ.)", L"Up to 24-bit / 48 kHz", L"The LE Audio codec: good quality at low bitrates."};
    default: return {L"—", L"—", L"The headphones did not report a codec this app recognises."};
    }
}

std::wstring HzLabel(int hz, int i) {
    wchar_t b[16];
    if (hz <= 0) swprintf_s(b, L"B%d", i + 1);
    else if (hz < 1000) swprintf_s(b, L"%d", hz);
    else if (hz % 1000 == 0) swprintf_s(b, L"%dk", hz / 1000);
    else swprintf_s(b, L"%.1fk", hz / 1000.0);
    return b;
}

std::wstring Signed(int v) { return (v > 0 ? L"+" : v < 0 ? L"−" : L"") + std::to_wstring(std::abs(v)); }

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L' '), e = s.find_last_not_of(L' ');
    return b == std::wstring::npos ? std::wstring() : s.substr(b, e - b + 1);
}

D2D1_POINT_2F Polar(float cx, float cy, float r, float deg) {
    float a = deg * 3.14159265f / 180.f;
    return {cx + r * std::cos(a), cy + r * std::sin(a)};
}

bool ReadAccent(bool dark, C& out) {
    BYTE pal[32];
    DWORD sz = sizeof pal;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &sz) == ERROR_SUCCESS && sz >= 32) {
        int i = dark ? 1 : 4; // Light2 in dark mode, Dark1 in light mode (what Windows uses for accent text)
        out = D2D1::ColorF(pal[i * 4] / 255.f, pal[i * 4 + 1] / 255.f, pal[i * 4 + 2] / 255.f);
        return true;
    }
    DWORD v = 0;
    sz = sizeof v;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", L"AccentColor", RRF_RT_REG_DWORD, nullptr,
                     &v, &sz) == ERROR_SUCCESS) {
        out = D2D1::ColorF((v & 0xFF) / 255.f, ((v >> 8) & 0xFF) / 255.f, ((v >> 16) & 0xFF) / 255.f);
        return true;
    }
    return false;
}

} // namespace

struct Flyout::Impl {
    using Body = std::function<float(float, float, float)>;

    HINSTANCE inst;
    AppHost& host;
    HWND hwnd = nullptr;
    ComPtr<ID2D1Factory> d2d;
    ComPtr<IDWriteFactory> dw;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1SolidColorBrush> br;
    ComPtr<ID2D1StrokeStyle> round;
    ID2D1RenderTarget* tgt = nullptr; // what the primitives draw on (rt, or a DC target while making icons)
    std::map<int, ComPtr<IDWriteTextFormat>> fmts;
    std::wstring famText = L"Segoe UI", famDisp = L"Segoe UI";
    Palette pal{};
    UINT dpi = 96;
    float W = F(kDefW), H = F(kDefH);
    HICON bigIcon = nullptr, smallIcon = nullptr;

    Snapshot snap;
    std::vector<BtDevice> devs;
    std::vector<Endpoint> eps;
    DolbyState dolby;

    bool shown = false, placed = false, positioning = false, animOn = false;
    ULONGLONG deactivatedAt = 0;
    std::array<int, 10> pendingBands{}; // curve as last sent; base for the next edit until the device echoes
    ULONGLONG pendingBandsUntil = 0;
    int page = 0;
    float scroll[6]{}, maxScroll = 0.f, clipTop = 0.f, clipBot = 0.f;
    bool inContent = false, dry = false, inert = false; // inert: draw controls but register no hit items
    std::vector<Item> items;
    Item scratch; // target of Add() during dry passes
    size_t modalStart = 0;

    int hoverId = 0, pressId = 0, focusId = 0;
    size_t pressKey = 0;
    bool focusVisible = false, tracking = false;
    int dragId = 0, dragVal = 0, lastSent = 0;
    Item drag;
    ULONGLONG lastSend = 0;
    int heldId = 0, heldVal = 0;
    ULONGLONG heldUntil = 0;
    bool ddOpen = false, confirmPower = false;
    int renameIdx = -1, deleteIdx = -1;
    std::wstring presetName, renameBuf, renameErr;
    ULONGLONG confirmAt = 0; // when a destructive confirm row was opened (double-click guard)
    wchar_t pendingHigh = 0; // high surrogate waiting for its low half
    bool dolbyActing = false;
    D2D1_RECT_F ddAnchor{};
    // open list picker (Choice with many options): refreshed by the field on every paint, closed if it disappears
    int pickId = 0, pickVal = 0;
    size_t pickRowKey = 0;
    bool pickSeen = false;
    D2D1_RECT_F pickAnchor{};
    std::vector<NamedInt> pickOpts;
    std::function<void(int)> pickSend;

    Impl(HINSTANCE i, AppHost& h) : inst(i), host(h) {
        D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d.GetAddressOf());
        if (d2d) {
            D2D1_STROKE_STYLE_PROPERTIES sp = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND);
            d2d->CreateStrokeStyle(sp, nullptr, 0, &round);
        }
        DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                            reinterpret_cast<IUnknown**>(dw.GetAddressOf()));
        if (dw) {
            ComPtr<IDWriteFontCollection> sys;
            UINT32 idx = 0;
            BOOL exists = FALSE;
            if (SUCCEEDED(dw->GetSystemFontCollection(&sys))) {
                if (SUCCEEDED(sys->FindFamilyName(L"Segoe UI Variable Text", &idx, &exists)) && exists)
                    famText = L"Segoe UI Variable Text";
                if (SUCCEEDED(sys->FindFamilyName(L"Segoe UI Variable Display", &idx, &exists)) && exists)
                    famDisp = L"Segoe UI Variable Display";
            }
        }
        WNDCLASSEXW wc{sizeof wc};
        wc.lpfnWndProc = &Impl::Proc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        wc.hIcon = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_SHARED));
        wc.hIconSm = static_cast<HICON>(LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                   GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_SHARED));
        RegisterClassExW(&wc); // fails harmlessly if already registered
        CreateWindowExW(0, kClass, L"XMTray", kStyle, CW_USEDEFAULT, CW_USEDEFAULT, kDefW, kDefH, nullptr, nullptr,
                        inst, this);
        LoadTheme();
    }
    ~Impl() {
        if (hwnd) {
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            DestroyWindow(hwnd);
        }
        if (bigIcon) DestroyIcon(bigIcon);
        if (smallIcon) DestroyIcon(smallIcon);
    }

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l) {
        if (m == WM_NCCREATE) {
            auto self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);
            self->hwnd = h;
            SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        auto self = reinterpret_cast<Impl*>(GetWindowLongPtrW(h, GWLP_USERDATA));
        return self ? self->Handle(m, w, l) : DefWindowProcW(h, m, w, l);
    }

    void Invalidate() {
        if (hwnd) InvalidateRect(hwnd, nullptr, FALSE);
    }
    void Redraw() { // repaint now so the hit-test list matches the new layout before the next input message
        if (!hwnd) return;
        InvalidateRect(hwnd, nullptr, FALSE);
        UpdateWindow(hwnd);
    }
    void Cmd(Command::Kind k, int a) {
        Command c;
        c.kind = k;
        c.a = a;
        host.Send(c);
    }
    void Feat(Feature f, int b, int idx = 0) {
        Command c;
        c.kind = Command::SetFeature;
        c.a = static_cast<int>(f);
        c.b = b;
        c.c = idx;
        host.Send(c);
    }
    void Reload() {
        snap = host.State();
        devs = host.Devices();
        eps = host.Endpoints();
        if (page == 3) dolby = host.DolbyStatus();
    }
    void SetPage(int p) {
        page = p;
        ddOpen = confirmPower = false;
        pickId = 0;
        renameIdx = deleteIdx = -1;
        if (p == 3) dolby = host.DolbyStatus();
        Redraw();
    }

    // ---- theme -------------------------------------------------------------------------
    void LoadTheme() {
        DWORD v = 1, sz = sizeof v;
        bool dark = !(RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                                   L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &sz) == ERROR_SUCCESS &&
                      v != 0);
        Palette& p = pal;
        p.dark = dark;
        if (dark) {
            p.bg = Rgb(0x1A1B1F); p.card = Rgb(0x24252B); p.popup = Rgb(0x2B2C33); p.inset = Rgb(0x1C1D21);
            p.btn = Rgb(0x2D2E35); p.hover = Rgb(0x2F3038); p.press = Rgb(0x28292F); p.border = Rgb(0x33353D);
            p.divider = Rgb(0x2E3037); p.text = Rgb(0xF3F4F6); p.text2 = Rgb(0xB8BBC3); p.text3 = Rgb(0x868A94);
            p.track = Rgb(0x42454E); p.good = Rgb(0x5FCF80); p.warn = Rgb(0xF2B544); p.bad = Rgb(0xFF8A8A);
            p.overlay = Rgb(0x000000, 0.55f);
        } else {
            p.bg = Rgb(0xF3F4F6); p.card = Rgb(0xFFFFFF); p.popup = Rgb(0xFFFFFF); p.inset = Rgb(0xF1F2F5);
            p.btn = Rgb(0xFBFBFC); p.hover = Rgb(0xEEEFF2); p.press = Rgb(0xE6E8EC); p.border = Rgb(0xDEE0E5);
            p.divider = Rgb(0xEBEDF0); p.text = Rgb(0x17181C); p.text2 = Rgb(0x4D5159); p.text3 = Rgb(0x767B85);
            p.track = Rgb(0xD2D5DB); p.good = Rgb(0x1A8346); p.warn = Rgb(0x9A5B00); p.bad = Rgb(0xC4314B);
            p.overlay = Rgb(0x000000, 0.32f);
        }
        p.danger = Rgb(0xC4314B);
        C a = dark ? Rgb(0x6CB8FF) : Rgb(0x0F62C9);
        ReadAccent(dark, a);
        C toward = dark ? Rgb(0xFFFFFF) : Rgb(0x000000);
        p.accent = Ensure(a, p.card, 3.0f, toward);       // UI elements: 3:1 against cards
        p.accentText = Ensure(a, p.card, 4.5f, toward);   // accent-coloured text: 4.5:1
        C white = Rgb(0xFFFFFF), black = Rgb(0x000000);
        p.onAccent = Contrast(white, p.accent) >= Contrast(black, p.accent) ? white : black;
        p.accentSoft = Mix(p.card, p.accent, dark ? 0.16f : 0.11f);
        p.navSel = Mix(p.bg, p.accent, dark ? 0.15f : 0.11f);
        if (hwnd) {
            BOOL d = dark;
            DwmSetWindowAttribute(hwnd, 20 /*DWMWA_USE_IMMERSIVE_DARK_MODE*/, &d, sizeof d);
            COLORREF cap = Ref(p.bg), txt = Ref(p.text);
            DwmSetWindowAttribute(hwnd, 35 /*DWMWA_CAPTION_COLOR*/, &cap, sizeof cap);
            DwmSetWindowAttribute(hwnd, 36 /*DWMWA_TEXT_COLOR*/, &txt, sizeof txt);
            MakeIcons();
        }
    }

    // ---- drawing primitives (DIP) -----------------------------------------------------------
    IDWriteTextFormat* Fmt(float size, Wt wt) {
        auto& f = fmts[static_cast<int>(size * 4.f) * 2 + static_cast<int>(wt)];
        if (!f && dw &&
            SUCCEEDED(dw->CreateTextFormat(size >= 20.f ? famDisp.c_str() : famText.c_str(), nullptr,
                                           wt == Semi ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f))) {
            ComPtr<IDWriteInlineObject> ell;
            dw->CreateEllipsisTrimmingSign(f.Get(), &ell);
            DWRITE_TRIMMING t{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            f->SetTrimming(&t, ell.Get());
        }
        return f.Get();
    }
    void Text(const std::wstring& s, D2D1_RECT_F r, float size, C c, Wt wt = Reg, Align al = AlLeft, bool wrap = false) {
        if (dry || s.empty()) return;
        IDWriteTextFormat* f = Fmt(size, wt);
        if (!f) return;
        f->SetTextAlignment(al == AlLeft     ? DWRITE_TEXT_ALIGNMENT_LEADING
                            : al == AlCenter ? DWRITE_TEXT_ALIGNMENT_CENTER
                                             : DWRITE_TEXT_ALIGNMENT_TRAILING);
        f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        br->SetColor(c);
        tgt->DrawText(s.c_str(), static_cast<UINT32>(s.size()), f, r, br.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }
    D2D1_SIZE_F Measure(const std::wstring& s, float size, Wt wt = Reg, float maxW = 10000.f, bool wrap = false) {
        IDWriteTextFormat* f = Fmt(size, wt);
        ComPtr<IDWriteTextLayout> l;
        if (!f || s.empty()) return {0.f, 0.f};
        f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
        if (FAILED(dw->CreateTextLayout(s.c_str(), static_cast<UINT32>(s.size()), f, std::max(1.f, maxW), 10000.f, &l)))
            return {0.f, 0.f};
        DWRITE_TEXT_METRICS m{};
        l->GetMetrics(&m);
        return {m.widthIncludingTrailingWhitespace, m.height};
    }
    float TextH(const std::wstring& s, float size, float w) { return s.empty() ? 0.f : Measure(s, size, Reg, w, true).height; }
    void Fill(D2D1_RECT_F r, C c, float rad = 0.f) {
        if (dry) return;
        br->SetColor(c);
        if (rad > 0.f) tgt->FillRoundedRectangle({r, rad, rad}, br.Get());
        else tgt->FillRectangle(r, br.Get());
    }
    void Stroke(D2D1_RECT_F r, C c, float rad, float w = 1.f) {
        if (dry) return;
        br->SetColor(c);
        r = Inflate(r, -w / 2.f);
        tgt->DrawRoundedRectangle({r, rad, rad}, br.Get(), w);
    }
    void Circle(float x, float y, float r, C c) {
        if (dry) return;
        br->SetColor(c);
        tgt->FillEllipse({{x, y}, r, r}, br.Get());
    }
    void Ring(float x, float y, float r, C c, float w) {
        if (dry) return;
        br->SetColor(c);
        tgt->DrawEllipse({{x, y}, r, r}, br.Get(), w);
    }
    void Line(float x0, float y0, float x1, float y1, C c, float w = 1.f) {
        if (dry) return;
        br->SetColor(c);
        tgt->DrawLine({x0, y0}, {x1, y1}, br.Get(), w, round.Get());
    }
    void Divider(float x, float y, float w) { Fill({x, y, x + w, y + 1.f}, pal.divider); }
    ComPtr<ID2D1PathGeometry> Path(const std::function<void(ID2D1GeometrySink*)>& fn) {
        ComPtr<ID2D1PathGeometry> g;
        ComPtr<ID2D1GeometrySink> s;
        if (!d2d || FAILED(d2d->CreatePathGeometry(&g)) || FAILED(g->Open(&s))) return {};
        fn(s.Get());
        if (FAILED(s->Close())) return {};
        return g;
    }
    void Poly(const std::vector<D2D1_POINT_2F>& pts, C c, float w, bool fill, bool close) {
        if (dry || pts.size() < 2) return;
        auto g = Path([&](ID2D1GeometrySink* s) {
            s->BeginFigure(pts[0], fill ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
            for (size_t i = 1; i < pts.size(); ++i) s->AddLine(pts[i]);
            s->EndFigure(close || fill ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
        });
        if (!g) return;
        br->SetColor(c);
        if (fill) tgt->FillGeometry(g.Get(), br.Get());
        else tgt->DrawGeometry(g.Get(), br.Get(), w, round.Get());
    }
    void Arc(float cx, float cy, float r, float a0, float a1, C c, float w) {
        if (dry) return;
        auto g = Path([&](ID2D1GeometrySink* s) {
            s->BeginFigure(Polar(cx, cy, r, a0), D2D1_FIGURE_BEGIN_HOLLOW);
            s->AddArc({Polar(cx, cy, r, a1), {r, r}, 0.f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                       a1 - a0 > 180.f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL});
            s->EndFigure(D2D1_FIGURE_END_OPEN);
        });
        if (!g) return;
        br->SetColor(c);
        tgt->DrawGeometry(g.Get(), br.Get(), w, round.Get());
    }
    void Shadow(D2D1_RECT_F r, float rad, bool popup) {
        if (pal.dark) {
            Fill(Inflate(Offset(r, 0.f, popup ? 6.f : 2.f), popup ? 3.f : 0.5f), Rgb(0x000000, popup ? 0.35f : 0.28f), rad + 2.f);
        } else {
            Fill(Inflate(Offset(r, 0.f, popup ? 8.f : 3.f), popup ? 4.f : 1.f), Rgb(0x000000, popup ? 0.06f : 0.035f), rad + 3.f);
            Fill(Inflate(Offset(r, 0.f, 1.f), 0.5f), Rgb(0x000000, popup ? 0.08f : 0.04f), rad + 1.f);
        }
    }

    // Simple line glyphs designed on a 24-unit grid, centred at (cx, cy), `s` DIP across.
    void Icon(Glyph g, float cx, float cy, float s, C c) {
        if (dry || g == GNone) return;
        float u = s / 24.f, sw = std::max(1.4f, 1.8f * u);
        auto P = [&](float x, float y) { return D2D1::Point2F(cx + (x - 12.f) * u, cy + (y - 12.f) * u); };
        auto L = [&](float x0, float y0, float x1, float y1, float w) {
            auto a = P(x0, y0), b = P(x1, y1);
            Line(a.x, a.y, b.x, b.y, c, w);
        };
        switch (g) {
        case GHome:
            Poly({P(3.5f, 11.f), P(12.f, 4.f), P(20.5f, 11.f)}, c, sw, false, false);
            Poly({P(6.f, 9.5f), P(6.f, 20.f), P(18.f, 20.f), P(18.f, 9.5f)}, c, sw, false, false);
            Poly({P(10.f, 20.f), P(10.f, 14.5f), P(14.f, 14.5f), P(14.f, 20.f)}, c, sw, false, false);
            break;
        case GEq:
            for (int i = 0; i < 3; ++i) {
                float x = 6.f + 6.f * F(i), k = i == 0 ? 15.f : i == 1 ? 8.f : 12.5f;
                L(x, 4.f, x, 20.f, sw);
                auto p = P(x, k);
                Circle(p.x, p.y, 2.6f * u, c);
            }
            break;
        case GSpatial: {
            auto p = P(12.f, 12.f);
            Circle(p.x, p.y, 3.f * u, c);
            Arc(p.x, p.y, 7.f * u, -40.f, 40.f, c, sw);
            Arc(p.x, p.y, 7.f * u, 140.f, 220.f, c, sw);
            Arc(p.x, p.y, 10.5f * u, -42.f, 42.f, c, sw);
            Arc(p.x, p.y, 10.5f * u, 138.f, 222.f, c, sw);
            break;
        }
        case GGear: {
            auto p = P(12.f, 12.f);
            Ring(p.x, p.y, 3.f * u, c, sw);
            Ring(p.x, p.y, 6.8f * u, c, sw);
            for (int k = 0; k < 8; ++k) {
                auto a = Polar(p.x, p.y, 7.2f * u, 45.f * F(k) + 22.5f), b = Polar(p.x, p.y, 9.6f * u, 45.f * F(k) + 22.5f);
                Line(a.x, a.y, b.x, b.y, c, 2.8f * u);
            }
            break;
        }
        case GInfo: {
            auto p = P(12.f, 12.f), d = P(12.f, 7.6f);
            Ring(p.x, p.y, 9.f * u, c, sw);
            L(12.f, 11.f, 12.f, 16.8f, sw);
            Circle(d.x, d.y, 1.3f * u, c);
            break;
        }
        case GHeadphones: {
            auto p = P(12.f, 13.5f);
            Arc(p.x, p.y, 8.5f * u, 180.f, 360.f, c, std::max(1.5f, 2.f * u));
            Fill({P(2.6f, 13.f).x, P(2.6f, 13.f).y, P(7.6f, 21.f).x, P(7.6f, 21.f).y}, c, 2.f * u);
            Fill({P(16.4f, 13.f).x, P(16.4f, 13.f).y, P(21.4f, 21.f).x, P(21.4f, 21.f).y}, c, 2.f * u);
            break;
        }
        case GSpeaker: {
            Poly({P(3.5f, 9.5f), P(7.5f, 9.5f), P(12.5f, 5.f), P(12.5f, 19.f), P(7.5f, 14.5f), P(3.5f, 14.5f)}, c, sw, true, true);
            auto p = P(12.5f, 12.f);
            Arc(p.x, p.y, 4.f * u, -45.f, 45.f, c, sw);
            Arc(p.x, p.y, 7.8f * u, -50.f, 50.f, c, sw);
            break;
        }
        case GBolt:
            Poly({P(13.5f, 2.f), P(5.f, 13.5f), P(11.f, 13.5f), P(10.f, 22.f), P(19.f, 10.f), P(13.f, 10.f)}, c, sw, true, true);
            break;
        case GCheck: Poly({P(5.f, 12.5f), P(10.f, 17.f), P(19.f, 7.f)}, c, std::max(1.6f, 2.2f * u), false, false); break;
        case GChevron: Poly({P(7.f, 9.5f), P(12.f, 14.5f), P(17.f, 9.5f)}, c, sw, false, false); break;
        case GLaptop:
            Stroke({P(5.f, 5.5f).x, P(5.f, 5.5f).y, P(19.f, 15.5f).x, P(19.f, 15.5f).y}, c, 1.5f * u, sw);
            L(2.5f, 19.f, 21.5f, 19.f, sw);
            break;
        case GPhone: {
            Stroke({P(7.5f, 2.5f).x, P(7.5f, 2.5f).y, P(16.5f, 21.5f).x, P(16.5f, 21.5f).y}, c, 2.5f * u, sw);
            auto d = P(12.f, 18.2f);
            Circle(d.x, d.y, 1.1f * u, c);
            break;
        }
        case GWarn: {
            Poly({P(12.f, 3.5f), P(21.5f, 20.f), P(2.5f, 20.f)}, c, sw, false, true);
            L(12.f, 9.5f, 12.f, 14.f, sw);
            auto d = P(12.f, 17.f);
            Circle(d.x, d.y, 1.2f * u, c);
            break;
        }
        case GPower: {
            auto p = P(12.f, 13.f);
            Arc(p.x, p.y, 8.f * u, 300.f, 600.f, c, sw);
            L(12.f, 3.f, 12.f, 11.f, sw);
            break;
        }
        case GBluetooth:
            Poly({P(7.f, 7.5f), P(17.f, 16.5f), P(12.f, 21.f), P(12.f, 3.f), P(17.f, 7.5f), P(7.f, 16.5f)}, c, sw, false, false);
            break;
        case GControls: { // two switches
            Stroke({P(2.5f, 4.f).x, P(2.5f, 4.f).y, P(21.5f, 11.f).x, P(21.5f, 11.f).y}, c, 3.5f * u, sw);
            Stroke({P(2.5f, 13.f).x, P(2.5f, 13.f).y, P(21.5f, 20.f).x, P(21.5f, 20.f).y}, c, 3.5f * u, sw);
            auto a = P(17.5f, 7.5f), b = P(6.5f, 16.5f);
            Circle(a.x, a.y, 2.f * u, c);
            Circle(b.x, b.y, 2.f * u, c);
            break;
        }
        case GMusic: {
            Poly({P(9.f, 17.5f), P(9.f, 5.5f), P(19.f, 3.5f), P(19.f, 15.5f)}, c, sw, false, false);
            auto a = P(6.5f, 17.5f), b = P(16.5f, 15.5f);
            Circle(a.x, a.y, 2.9f * u, c);
            Circle(b.x, b.y, 2.9f * u, c);
            break;
        }
        case GPlay: Poly({P(8.f, 4.5f), P(19.5f, 12.f), P(8.f, 19.5f)}, c, sw, true, true); break;
        case GPause:
            Fill({P(6.f, 5.f).x, P(6.f, 5.f).y, P(10.f, 19.f).x, P(10.f, 19.f).y}, c, 1.f * u);
            Fill({P(14.f, 5.f).x, P(14.f, 5.f).y, P(18.f, 19.f).x, P(18.f, 19.f).y}, c, 1.f * u);
            break;
        case GNext:
            Poly({P(5.f, 6.f), P(15.f, 12.f), P(5.f, 18.f)}, c, sw, true, true);
            Fill({P(16.f, 6.f).x, P(16.f, 6.f).y, P(19.f, 18.f).x, P(19.f, 18.f).y}, c, 1.f * u);
            break;
        case GPrev:
            Poly({P(19.f, 6.f), P(9.f, 12.f), P(19.f, 18.f)}, c, sw, true, true);
            Fill({P(5.f, 6.f).x, P(5.f, 6.f).y, P(8.f, 18.f).x, P(8.f, 18.f).y}, c, 1.f * u);
            break;
        default: break;
        }
    }

    // ---- app icon (drawn, no image assets) ------------------------------------------------------
    HICON MakeIcon(int px) {
        if (!d2d || px <= 0) return nullptr;
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), px, -px, 1, 32, BI_RGB, 0, 0, 0, 0, 0};
        void* bits = nullptr;
        HDC screen = GetDC(nullptr);
        HBITMAP color = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HDC mem = CreateCompatibleDC(screen);
        ReleaseDC(nullptr, screen);
        if (!color || !mem) {
            if (color) DeleteObject(color);
            if (mem) DeleteDC(mem);
            return nullptr;
        }
        HGDIOBJ old = SelectObject(mem, color);
        ComPtr<ID2D1DCRenderTarget> dc;
        auto props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                  D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                                  96.f, 96.f);
        ComPtr<ID2D1SolidColorBrush> b;
        RECT r{0, 0, px, px};
        if (SUCCEEDED(d2d->CreateDCRenderTarget(&props, &dc)) && SUCCEEDED(dc->BindDC(mem, &r)) &&
            SUCCEEDED(dc->CreateSolidColorBrush(pal.accent, &b))) {
            auto savedT = tgt;
            auto savedB = br;
            bool savedDry = dry;
            tgt = dc.Get();
            br = b;
            dry = false;
            dc->BeginDraw();
            dc->Clear(D2D1::ColorF(0, 0.f));
            float s = F(px);
            Fill({0.f, 0.f, s, s}, Mix(pal.accent, Rgb(0x000000), 0.15f), s * 0.24f);
            Icon(GHeadphones, s / 2.f, s / 2.f, s * 0.72f, Rgb(0xFFFFFF));
            dc->EndDraw();
            tgt = savedT;
            br = savedB;
            dry = savedDry;
        }
        SelectObject(mem, old);
        DeleteDC(mem);
        std::vector<BYTE> zeros(static_cast<size_t>((px + 15) / 16 * 2 * px), 0);
        HBITMAP mask = CreateBitmap(px, px, 1, 1, zeros.data());
        ICONINFO ii{TRUE, 0, 0, mask, color};
        HICON ic = CreateIconIndirect(&ii);
        DeleteObject(mask);
        DeleteObject(color);
        return ic;
    }
    void MakeIcons() {
        UINT d = hwnd ? GetDpiForWindow(hwnd) : 96;
        HICON b = MakeIcon(GetSystemMetricsForDpi(SM_CXICON, d)), s = MakeIcon(GetSystemMetricsForDpi(SM_CXSMICON, d));
        if (b) SendMessageW(hwnd, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(b));
        if (s) SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(s));
        if (bigIcon && b) DestroyIcon(bigIcon);
        if (smallIcon && s) DestroyIcon(smallIcon);
        if (b) bigIcon = b;
        if (s) smallIcon = s;
    }

    // ---- items + interaction state --------------------------------------------------------
    Item& Add(int id, D2D1_RECT_F rc) {
        if (dry || inert) {
            scratch = Item{};
            return scratch;
        }
        Item it;
        it.id = id;
        it.rc = it.hit = rc;
        it.content = inContent;
        if (inContent) {
            it.hit.top = std::max(rc.top, clipTop);
            it.hit.bottom = std::min(rc.bottom, clipBot);
        }
        items.push_back(std::move(it));
        return items.back();
    }
    bool Hot(int id) const { return hoverId == id && (pressId == 0 || pressId == id); }
    bool Down(int id) const { return hoverId == id && pressId == id; }
    C StateBg(int id, C base) const { return Down(id) ? pal.press : Hot(id) ? pal.hover : base; }
    C Shade(C c, float t) const { return Mix(c, pal.dark ? Rgb(0xFFFFFF) : Rgb(0x000000), t); }
    void FocusRing(int id, D2D1_RECT_F r, float rad) {
        if (focusVisible && focusId == id) Stroke(Inflate(r, 3.f), pal.text, rad + 3.f, 2.f);
    }
    int Shown(int id, int snapVal) const {
        if (id == dragId) return dragVal;
        if (id == heldId && GetTickCount64() < heldUntil) return heldVal;
        return snapVal;
    }

    // ---- widgets -------------------------------------------------------------------------
    float BtnW(const std::wstring& label) { return std::ceil(Measure(label, 13.f, Semi).width) + 32.f; }

    void Button(int id, D2D1_RECT_F r, const std::wstring& label, std::function<void()> fn, Style st = BtnNormal,
                bool enabled = true, Glyph g = GNone) {
        float t = !enabled ? 0.f : Down(id) ? 0.2f : Hot(id) ? 0.1f : 0.f;
        C bg = pal.btn, fg = pal.text, bd = pal.border;
        bool border = true;
        if (!enabled) {
            bg = pal.inset, fg = pal.text3, bd = pal.divider;
        } else if (st == BtnPrimary) {
            bg = Shade(pal.accent, t), fg = pal.onAccent, border = false;
        } else if (st == BtnDanger) {
            bg = Mix(pal.danger, Rgb(0x000000), t), fg = Rgb(0xFFFFFF), border = false;
        } else if (st == BtnDangerOutline) {
            bg = t > 0.f ? Mix(pal.card, pal.bad, t * 0.9f) : pal.btn, fg = pal.bad, bd = Mix(pal.border, pal.bad, 0.45f);
        } else if (st == BtnSubtle) {
            bg = StateBg(id, pal.card), border = false;
        } else {
            bg = StateBg(id, pal.btn);
        }
        Fill(r, bg, 8.f);
        if (border) Stroke(r, bd, 8.f);
        if (g != GNone) {
            float tw = Measure(label, 13.f, Semi).width, x0 = (r.left + r.right - tw - 24.f) / 2.f;
            Icon(g, x0 + 8.f, (r.top + r.bottom) / 2.f, 16.f, fg);
            Text(label, {x0 + 24.f, r.top, r.right, r.bottom}, 13.f, fg, Semi);
        } else {
            Text(label, r, label.size() == 1 ? 17.f : 13.f, fg, Semi, AlCenter);
        }
        if (enabled) Add(id, r).click = std::move(fn);
        FocusRing(id, r, 8.f);
    }

    float CardHeader(float x, float y, float w, const std::wstring& title, const std::wstring& sub = {}, float rightW = 0.f) {
        Text(title, {x, y, x + w - rightW, y + 24.f}, 16.f, pal.text, Semi);
        y += 24.f;
        if (!sub.empty()) {
            float h = TextH(sub, 13.f, w - rightW);
            Text(sub, {x, y + 2.f, x + w - rightW, y + 4.f + h}, 13.f, pal.text3, Reg, AlLeft, true);
            y += h + 4.f;
        }
        return y + 16.f;
    }

    float Card(float x, float y, float w, const Body& body) {
        bool was = dry;
        dry = true;
        float end = body(x + CP, y + CP, w - 2.f * CP);
        dry = was;
        D2D1_RECT_F r{x, y, x + w, end + CP};
        if (!dry) {
            Shadow(r, RAD, false);
            Fill(r, pal.card, RAD);
            Stroke(r, pal.border, RAD);
            body(x + CP, y + CP, w - 2.f * CP);
        }
        return r.bottom;
    }

    void Switch(D2D1_RECT_F sw, bool on, bool enabled, bool hot) {
        float cy = (sw.top + sw.bottom) / 2.f;
        if (on) {
            Fill(sw, enabled ? (hot ? Shade(pal.accent, 0.1f) : pal.accent) : pal.track, 10.f);
            Circle(sw.right - 10.f, cy, hot ? 6.5f : 6.f, enabled ? pal.onAccent : pal.card);
        } else {
            if (hot) Fill(sw, pal.hover, 10.f);
            Stroke(sw, enabled ? pal.text2 : pal.text3, 10.f, 1.2f);
            Circle(sw.left + 10.f, cy, hot ? 5.5f : 5.f, enabled ? pal.text2 : pal.text3);
        }
    }

    float ToggleRow(int id, float x, float y, float w, const std::wstring& label, const std::wstring& hint, bool on,
                    std::function<void(bool)> set, bool enabled = true) {
        float tw = w - 64.f, hh = TextH(hint, 12.5f, tw), lh = std::max(20.f, TextH(label, 14.f, tw));
        float h = hint.empty() ? std::max(48.f, lh + 28.f) : 15.f + lh + 3.f + hh + 13.f;
        D2D1_RECT_F row{x - 10.f, y, x + w + 10.f, y + h};
        if (enabled) {
            if (Hot(id)) Fill(row, StateBg(id, pal.card), 8.f);
            Add(id, row).click = [set, on] { set(!on); };
        }
        if (hint.empty()) {
            Text(label, {x, y + (h - lh) / 2.f, x + tw, y + (h + lh) / 2.f + 2.f}, 14.f, enabled ? pal.text : pal.text3, Reg,
                 AlLeft, true);
        } else {
            Text(label, {x, y + 13.f, x + tw, y + 15.f + lh}, 14.f, enabled ? pal.text : pal.text3, Reg, AlLeft, true);
            Text(hint, {x, y + 16.f + lh, x + tw, y + 18.f + lh + hh}, 12.5f, pal.text3, Reg, AlLeft, true);
        }
        float cy = y + h / 2.f;
        Switch({x + w - 40.f, cy - 10.f, x + w, cy + 10.f}, on, enabled, enabled && Hot(id));
        FocusRing(id, row, 8.f);
        return y + h;
    }

    float CheckRow(int id, float x, float y, float w, const std::wstring& label, bool on, std::function<void(bool)> set) {
        float th = TextH(label, 14.f, w - 34.f), h = std::max(44.f, th + 22.f);
        D2D1_RECT_F row{x - 10.f, y, x + w + 10.f, y + h};
        if (Hot(id)) Fill(row, StateBg(id, pal.card), 8.f);
        float cy = y + h / 2.f;
        D2D1_RECT_F box{x, cy - 10.f, x + 20.f, cy + 10.f};
        if (on) {
            Fill(box, pal.accent, 5.f);
            Icon(GCheck, x + 10.f, cy, 16.f, pal.onAccent);
        } else {
            Stroke(box, pal.text2, 5.f, 1.2f);
        }
        Text(label, {x + 34.f, cy - th / 2.f - 1.f, x + w, cy + th / 2.f + 1.f}, 14.f, pal.text, Reg, AlLeft, true);
        Add(id, row).click = [set, on] { set(!on); };
        FocusRing(id, row, 8.f);
        return y + h;
    }

    // labels may carry a short alternative after '|', used when the long one does not fit
    float Segmented(int idBase, float x, float y, float w, float h, const std::vector<std::wstring>& labels, int sel,
                    std::function<void(int)> pick) {
        if (labels.empty()) return y;
        D2D1_RECT_F r{x, y, x + w, y + h};
        Fill(r, pal.inset, 9.f);
        Stroke(r, pal.divider, 9.f);
        float sw = (w - 6.f) / F(labels.size());
        for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
            int id = idBase + i;
            bool on = i == sel;
            D2D1_RECT_F s{x + 3.f + sw * F(i), y + 3.f, x + 3.f + sw * F(i + 1), y + h - 3.f};
            const std::wstring& lab = labels[static_cast<size_t>(i)];
            size_t bar = lab.find(L'|');
            std::wstring text = lab.substr(0, bar);
            if (bar != std::wstring::npos && Measure(text, 13.f, on ? Semi : Reg).width + 14.f > sw) text = lab.substr(bar + 1);
            if (on) Fill(s, pal.accent, 7.f);
            else if (Hot(id)) Fill(s, StateBg(id, pal.inset), 7.f);
            Text(text, {s.left + 4.f, s.top, s.right - 4.f, s.bottom}, 13.f, on ? pal.onAccent : pal.text, on ? Semi : Reg, AlCenter);
            Add(id, s).click = [pick, i] { pick(i); };
            FocusRing(id, s, 7.f);
        }
        return y + h;
    }

    float Chips(int idBase, float x, float y, float w, const std::vector<std::wstring>& labels, int sel,
                std::function<void(int)> pick) {
        if (labels.empty()) return y;
        float cx = x, h = 34.f;
        for (int i = 0; i < static_cast<int>(labels.size()); ++i) {
            int id = idBase + i;
            bool on = i == sel;
            const std::wstring& lab = labels[static_cast<size_t>(i)];
            float cw = std::min(std::ceil(Measure(lab, 13.f, Semi).width) + 30.f, w);
            if (cx + cw > x + w && cx > x) cx = x, y += h + 8.f;
            D2D1_RECT_F r{cx, y, cx + cw, y + h};
            if (on) {
                Fill(r, pal.accent, h / 2.f);
            } else {
                Fill(r, StateBg(id, pal.btn), h / 2.f);
                Stroke(r, pal.border, h / 2.f);
            }
            Text(lab, r, 13.f, on ? pal.onAccent : pal.text, on ? Semi : Reg, AlCenter);
            Add(id, r).click = [pick, i] { pick(i); };
            FocusRing(id, r, h / 2.f);
            cx += cw + 8.f;
        }
        return y + h;
    }

    void Thumb(int id, float x, float y) {
        bool hot = Hot(id) || dragId == id;
        if (hot) Circle(x, y, 15.f, Alpha(pal.accent, 0.18f));
        Circle(x, y + 1.f, 10.f, Rgb(0x000000, pal.dark ? 0.35f : 0.12f));
        Circle(x, y, 10.f, pal.dark ? Rgb(0xF3F4F6) : Rgb(0xFFFFFF));
        Circle(x, y, hot ? 6.5f : 5.5f, pal.accent);
        if (focusVisible && focusId == id) Ring(x, y, 14.f, pal.text, 2.f);
    }

    Item& SliderItem(int id, D2D1_RECT_F r, const Range& rg, int v, bool vertical, std::function<void(int)> send) {
        Item& it = Add(id, r);
        it.slider = true;
        it.vertical = vertical;
        it.mn = rg.min;
        it.mx = rg.max;
        it.step = std::max(1, rg.step);
        it.val = v;
        it.change = [send](int x, bool) { send(x); };
        return it;
    }

    void Track(int id, float x, float y, float w, const Range& rg, int v, std::function<void(int)> send, bool fromZero = false) {
        D2D1_RECT_F tr{x, y, x + w, y + 30.f};
        float cy = y + 15.f, l = x + 10.f, r = x + w - 10.f;
        auto pos = [&](int val) { return l + (r - l) * F(val - rg.min) / F(rg.max - rg.min); };
        float kx = pos(v), zx = fromZero ? pos(std::clamp(0, rg.min, rg.max)) : l;
        Fill({l, cy - 2.f, r, cy + 2.f}, pal.track, 2.f);
        Fill({std::min(kx, zx), cy - 2.f, std::max(kx, zx), cy + 2.f}, pal.accent, 2.f);
        if (fromZero) Fill({zx - 1.f, cy - 6.f, zx + 1.f, cy + 6.f}, pal.text3, 1.f);
        Thumb(id, kx, cy);
        SliderItem(id, tr, rg, v, false, std::move(send));
    }

    float SliderRow(int id, float x, float y, float w, const std::wstring& label, const Range& rg, int value,
                    std::function<void(int)> send, bool sign = false, Glyph g = GNone, bool enabled = true) {
        if (rg.max <= rg.min) return y;
        int v = std::clamp(Shown(id, value), rg.min, rg.max);
        float lx = x;
        C fg = enabled ? pal.text : pal.text3;
        if (g != GNone) {
            Icon(g, x + 10.f, y + 11.f, 18.f, enabled ? pal.text2 : pal.text3);
            lx = x + 30.f;
        }
        Text(label, {lx, y, x + w - 64.f, y + 22.f}, 14.f, fg);
        Text(sign ? Signed(v) : std::to_wstring(v), {x + w - 64.f, y, x + w, y + 22.f}, 14.f, fg, Semi, AlRight);
        if (enabled) {
            Track(id, x - 2.f, y + 26.f, w + 4.f, rg, v, std::move(send), sign);
        } else { // same geometry as Track, no thumb affordance and no hit item
            float cy = y + 41.f, l = x + 8.f, r = x + w - 8.f, kx = l + (r - l) * F(v - rg.min) / F(rg.max - rg.min);
            Fill({l, cy - 2.f, r, cy + 2.f}, pal.track, 2.f);
            Circle(kx, cy, 7.f, pal.track);
        }
        return y + 56.f;
    }

    // One Choice picker: <= 4 short options -> segmented, <= 8 -> wrapping chips, more -> a list field with a popup.
    // rowKey identifies what the row addresses (assignable key, general item); each option item's key adds its value
    float Picker(int idBase, float x, float y, float w, const std::vector<NamedInt>& opts, int value,
                 std::function<void(int)> send, size_t rowKey = 0) {
        if (opts.empty()) return y;
        std::vector<std::wstring> labels;
        std::vector<int> values;
        int sel = -1;
        for (auto& o : opts) {
            if (o.value == value) sel = static_cast<int>(labels.size());
            labels.push_back(o.label);
            values.push_back(o.value);
        }
        auto pick = [send, values](int i) { send(values[size_t(i)]); };
        size_t n0 = items.size();
        auto tag = [&](float end) { // Segmented / Chips add one item per option, in order
            for (size_t i = n0, k = 0; i < items.size() && k < values.size(); ++i, ++k) items[i].key = KeyOf(rowKey, values[k]);
            return end;
        };
        if (opts.size() <= 4) {
            float sw = (w - 6.f) / F(opts.size());
            bool fit = true;
            for (auto& l : labels) fit = fit && Measure(l, 13.f, Semi).width + 18.f <= sw;
            if (fit) return tag(Segmented(idBase, x, y, w, 36.f, labels, sel, pick));
        }
        if (opts.size() <= 8) return tag(Chips(idBase, x, y, w, labels, sel, pick));
        bool open = pickId == idBase;
        D2D1_RECT_F r{x, y, x + std::min(w, 320.f), y + 38.f};
        Fill(r, open ? pal.hover : StateBg(idBase, pal.btn), 8.f);
        Stroke(r, open ? pal.accent : pal.border, 8.f);
        Text(sel >= 0 ? labels[size_t(sel)] : L"Choose…", {r.left + 14.f, y, r.right - 36.f, r.bottom}, 13.5f,
             sel >= 0 ? pal.text : pal.text3, sel >= 0 ? Semi : Reg);
        Icon(GChevron, r.right - 19.f, y + 19.f, 16.f, pal.text2);
        Item& field = Add(idBase, r);
        field.click = [this, idBase] { pickId = pickId == idBase ? 0 : idBase; };
        field.key = rowKey;
        FocusRing(idBase, r, 8.f);
        if (open && !dry) {
            pickSeen = true;
            pickAnchor = r;
            pickOpts = opts;
            pickVal = value;
            pickSend = send;
            pickRowKey = rowKey;
        }
        return r.bottom;
    }

    static size_t KeyOf(size_t row, int v) { return row * 1000003u ^ (std::hash<int>{}(v) + 0x9E3779B9u); }
    static size_t KeyOf(const std::wstring& label, int pos) { return KeyOf(std::hash<std::wstring>{}(label), pos); }

    // label (+ hint) over a picker, spaced like the auto power-off row
    float ChoiceRow(int idBase, float x, float y, float w, const std::wstring& label, const std::wstring& hint,
                    const std::vector<NamedInt>& opts, int value, std::function<void(int)> send, size_t rowKey = 0) {
        y += 14.f;
        Text(label, {x, y, x + w, y + 22.f}, 14.f, pal.text);
        y += 22.f;
        if (!hint.empty()) {
            float hh = TextH(hint, 12.5f, w);
            Text(hint, {x, y + 1.f, x + w, y + 3.f + hh}, 12.5f, pal.text3, Reg, AlLeft, true);
            y += hh + 1.f;
        }
        y = Picker(idBase, x, y + 10.f, w, opts, value, std::move(send), rowKey);
        return y + 16.f;
    }

    float ValueRow(float x, float y, float w, const std::wstring& label, const std::wstring& value) { // read-only setting
        float vw = std::min(w * 0.5f, std::ceil(Measure(value, 14.f).width) + 2.f);
        Text(label, {x, y, x + w - vw - 12.f, y + 48.f}, 14.f, pal.text3);
        Text(value, {x + w - vw, y, x + w, y + 48.f}, 14.f, pal.text3, Reg, AlRight);
        return y + 48.f;
    }

    void RoundBtn(int id, float cx, float cy, float r, Glyph g, bool primary, std::function<void()> fn) {
        if (primary) {
            Circle(cx, cy, r, Shade(pal.accent, Down(id) ? 0.2f : Hot(id) ? 0.1f : 0.f));
        } else {
            Circle(cx, cy, r, StateBg(id, pal.btn));
            Ring(cx, cy, r - 0.5f, pal.border, 1.f);
        }
        Icon(g, cx, cy, primary ? 20.f : 16.f, primary ? pal.onAccent : pal.text);
        Add(id, {cx - r, cy - r, cx + r, cy + r}).click = std::move(fn);
        if (focusVisible && focusId == id) Ring(cx, cy, r + 3.f, pal.text, 2.f);
    }

    static int ValueAt(const Item& it, float x, float y) {
        if (it.mx <= it.mn) return it.mn;
        float t = it.vertical ? ((it.rc.bottom - 10.f) - y) / ((it.rc.bottom - it.rc.top) - 20.f)
                              : (x - (it.rc.left + 10.f)) / ((it.rc.right - it.rc.left) - 20.f);
        t = std::clamp(t, 0.f, 1.f);
        float steps = std::round(t * F(it.mx - it.mn) / F(it.step));
        return std::clamp(it.mn + static_cast<int>(steps) * it.step, it.mn, it.mx);
    }

    void TextField(int id, D2D1_RECT_F r, std::wstring* buf, const std::wstring& placeholder, std::function<void()> enter) {
        bool f = focusId == id;
        Fill(r, pal.inset, 8.f);
        Stroke(r, f ? pal.accent : Hot(id) ? pal.text3 : pal.border, 8.f, f ? 2.f : 1.f);
        D2D1_RECT_F tr{r.left + 12.f, r.top, r.right - 12.f, r.bottom};
        float tw = Measure(*buf, 14.f).width;
        bool over = tw > tr.right - tr.left;
        Text(buf->empty() ? placeholder : *buf, tr, 14.f, buf->empty() ? pal.text3 : pal.text, Reg, over ? AlRight : AlLeft);
        if (f) {
            float cx = over ? tr.right : std::min(tr.left + tw + 1.f, tr.right);
            Line(cx, r.top + 9.f, cx, r.bottom - 9.f, pal.text, 1.2f);
        }
        Item& it = Add(id, r);
        it.text = buf;
        it.click = std::move(enter);
    }

    float KV(float x, float y, float w, const std::wstring& k, const std::wstring& v, C vc) {
        Text(k, {x, y, x + w * 0.45f, y + 38.f}, 13.5f, pal.text2);
        Text(v, {x + w * 0.38f, y, x + w, y + 38.f}, 13.5f, vc, Reg, AlRight);
        return y + 38.f;
    }

    float Chip(float x, float y, const std::wstring& label, C dot, bool measureOnly = false) {
        float w = std::ceil(Measure(label, 12.5f, Semi).width) + 30.f;
        if (!measureOnly) {
            D2D1_RECT_F r{x, y, x + w, y + 26.f};
            Fill(r, Mix(pal.card, dot, pal.dark ? 0.16f : 0.12f), 13.f);
            Circle(x + 13.f, y + 13.f, 4.f, dot);
            Text(label, {x + 22.f, y, x + w - 6.f, y + 26.f}, 12.5f, pal.text, Semi);
        }
        return w;
    }

    // ---- frame: sidebar, top bar, dropdown, alert ----------------------------------------------
    static constexpr int kPageCount = 6;
    static constexpr const wchar_t* kPages[kPageCount] = {L"Home", L"Equalizer", L"Controls", L"Dolby Atmos", L"Settings", L"About"};

    void Sidebar() {
        D2D1_RECT_F logo{20.f, 22.f, 54.f, 56.f};
        Fill(logo, Mix(pal.accent, Rgb(0x000000), pal.dark ? 0.1f : 0.0f), 10.f);
        Icon(GHeadphones, 37.f, 39.f, 22.f, pal.onAccent);
        Text(L"XMTray", {66.f, 22.f, SB - 12.f, 56.f}, 18.f, pal.text, Semi);
        static const Glyph glyphs[kPageCount] = {GHome, GEq, GControls, GSpatial, GGear, GInfo};
        for (int i = 0; i < kPageCount; ++i) {
            int id = ID_NAV + i;
            float y = 84.f + 44.f * F(i);
            D2D1_RECT_F r{12.f, y, SB - 12.f, y + 40.f};
            bool sel = page == i;
            if (sel) Fill(r, Hot(id) ? Shade(pal.navSel, 0.04f) : pal.navSel, 8.f);
            else if (Hot(id)) Fill(r, StateBg(id, pal.bg), 8.f);
            if (sel) Fill({12.f, y + 11.f, 15.f, y + 29.f}, pal.accent, 1.5f);
            Icon(glyphs[i], 40.f, y + 20.f, 20.f, sel ? pal.accentText : pal.text2);
            Text(kPages[i], {62.f, y, SB - 16.f, y + 40.f}, 14.f, sel ? pal.text : pal.text2, sel ? Semi : Reg);
            Add(id, r).click = [this, i] { SetPage(i); };
            FocusRing(id, r, 8.f);
        }
        Text(std::wstring(L"Version ") + kVersion, {24.f, H - 46.f, SB - 12.f, H - 22.f}, 12.f, pal.text3);
        Fill({SB - 1.f, 0.f, SB, H}, pal.divider);
    }

    std::wstring DeviceTitle() const {
        if (!snap.model.empty()) return snap.model;
        for (auto& d : devs)
            if (d.mac == snap.mac && !d.name.empty()) return d.name;
        if (!snap.mac.empty()) return Widen(snap.mac);
        return devs.empty() ? L"No headphones" : L"Select headphones";
    }

    void LinkLabel(std::wstring& label, C& c) const {
        switch (snap.link) {
        case Link::Ready: label = L"Connected", c = pal.good; break;
        case Link::Connecting:
        case Link::Initializing: label = L"Connecting…", c = pal.warn; break;
        case Link::Failed: label = L"Failed", c = pal.bad; break;
        default: label = L"Not connected", c = pal.text3; break;
        }
    }

    void TopBar(const Col& c) {
        float right = c.x + c.w;
        std::wstring name = DeviceTitle();
        float selW = std::clamp(std::ceil(Measure(name, 13.5f, Semi).width) + 74.f, 200.f, 300.f);
        D2D1_RECT_F sel{right - selW, 20.f, right, 56.f};
        Fill(sel, ddOpen ? pal.hover : StateBg(ID_DEVSEL, pal.card), 9.f);
        Stroke(sel, pal.border, 9.f);
        Icon(GHeadphones, sel.left + 21.f, 38.f, 18.f, pal.text2);
        Text(name, {sel.left + 38.f, 20.f, sel.right - 34.f, 56.f}, 13.5f, pal.text, Semi);
        Icon(GChevron, sel.right - 19.f, 38.f, 16.f, pal.text2);
        Add(ID_DEVSEL, sel).click = [this] { ddOpen = !ddOpen; };
        FocusRing(ID_DEVSEL, sel, 9.f);
        ddAnchor = sel;

        std::wstring label;
        C dot;
        LinkLabel(label, dot);
        float pw = std::ceil(Measure(label, 12.5f, Semi).width) + 34.f;
        D2D1_RECT_F pill{sel.left - 12.f - pw, 25.f, sel.left - 12.f, 51.f};
        Fill(pill, Mix(pal.bg, dot, pal.dark ? 0.16f : 0.12f), 13.f);
        Circle(pill.left + 14.f, 38.f, 4.f, dot);
        Text(label, {pill.left + 24.f, 25.f, pill.right - 4.f, 51.f}, 12.5f, pal.text, Semi);

        Text(kPages[page], {c.x, 16.f, pill.left - 16.f, 60.f}, 26.f, pal.text, Semi);
        if (scroll[page] > 0.5f) Fill({SB, TOPH - 1.f, W, TOPH}, pal.divider);
    }

    void Blocker(std::function<void()> fn) {
        modalStart = items.size();
        Item& b = Add(ID_BLOCK, {0.f, 0.f, W, H});
        b.focus = false;
        b.click = std::move(fn);
    }

    static void OpenBluetoothSettings() {
        ShellExecuteW(nullptr, L"open", L"ms-settings:bluetooth", nullptr, nullptr, SW_SHOWNORMAL);
    }

    void Dropdown() {
        Blocker([this] { ddOpen = false; });
        float w = std::max(ddAnchor.right - ddAnchor.left, 300.f);
        float itemsH = devs.empty() ? 56.f : 56.f * F(devs.size());
        float top = ddAnchor.bottom + 6.f, h = 8.f + 30.f + itemsH + 9.f + 44.f + 6.f;
        D2D1_RECT_F card{ddAnchor.right - w, top, ddAnchor.right, top + h};
        Shadow(card, 12.f, true);
        Fill(card, pal.popup, 12.f);
        Stroke(card, pal.border, 12.f);
        Text(L"Headphones connected to Windows", {card.left + 16.f, top + 8.f, card.right - 16.f, top + 38.f}, 12.f, pal.text3, Semi);
        float y = top + 38.f;
        if (devs.empty())
            Text(L"No Sony headphones are connected.", {card.left + 16.f, y, card.right - 16.f, y + 56.f}, 13.5f, pal.text2);
        for (size_t i = 0; i < devs.size(); ++i, y += 56.f) {
            int id = ID_DD + static_cast<int>(i);
            D2D1_RECT_F r{card.left + 6.f, y + 2.f, card.right - 6.f, y + 54.f};
            bool cur = devs[i].mac == snap.mac;
            if (Hot(id)) Fill(r, StateBg(id, pal.popup), 8.f);
            Circle(r.left + 28.f, y + 28.f, 17.f, cur ? pal.accentSoft : pal.inset);
            Icon(GHeadphones, r.left + 28.f, y + 28.f, 18.f, cur ? pal.accentText : pal.text2);
            Text(devs[i].name.empty() ? Widen(devs[i].mac) : devs[i].name, {r.left + 56.f, y + 8.f, r.right - 40.f, y + 30.f},
                 14.f, pal.text, cur ? Semi : Reg);
            Text(Widen(devs[i].mac), {r.left + 56.f, y + 29.f, r.right - 40.f, y + 47.f}, 12.f, pal.text3);
            if (cur) Icon(GCheck, r.right - 20.f, y + 28.f, 18.f, pal.accentText);
            std::string mac = devs[i].mac;
            Add(id, r).click = [this, mac] {
                ddOpen = false;
                host.SelectDevice(mac);
            };
            FocusRing(id, r, 8.f);
        }
        y += 4.f;
        Divider(card.left + 1.f, y, w - 2.f);
        y += 5.f;
        D2D1_RECT_F bt{card.left + 6.f, y, card.right - 6.f, y + 42.f};
        if (Hot(ID_DD_BT)) Fill(bt, StateBg(ID_DD_BT, pal.popup), 8.f);
        Icon(GBluetooth, bt.left + 28.f, y + 21.f, 18.f, pal.text2);
        Text(L"Bluetooth settings", {bt.left + 56.f, y, bt.right - 12.f, y + 42.f}, 13.5f, pal.text);
        Add(ID_DD_BT, bt).click = [this] {
            ddOpen = false;
            OpenBluetoothSettings();
        };
        FocusRing(ID_DD_BT, bt, 8.f);
    }

    void PickerPopup() { // list for a Picker field with many options; extra columns when it would not fit the height
        Blocker([this] { pickId = 0; });
        const float ih = 36.f, pad = 6.f;
        float colW = std::max(pickAnchor.right - pickAnchor.left, 200.f);
        for (auto& o : pickOpts) colW = std::max(colW, std::ceil(Measure(o.label, 13.5f).width) + 52.f);
        colW = std::min(colW, 340.f);
        size_t n = pickOpts.size(), fit = std::max<size_t>(1, static_cast<size_t>((H - 16.f - 2.f * pad) / ih));
        size_t cols = (n + fit - 1) / fit, rows = (n + cols - 1) / cols;
        float w = std::min(colW * F(cols) + 2.f * pad, W - SB - 16.f), h = ih * F(rows) + 2.f * pad;
        colW = (w - 2.f * pad) / F(cols);
        float left = std::clamp(pickAnchor.left, SB + 8.f, W - 8.f - w), top = pickAnchor.bottom + 6.f;
        if (top + h > H - 8.f) top = pickAnchor.top - 6.f - h;
        if (top < 8.f) top = std::max(8.f, H - 8.f - h);
        D2D1_RECT_F card{left, top, left + w, top + h};
        Shadow(card, 12.f, true);
        Fill(card, pal.popup, 12.f);
        Stroke(card, pal.border, 12.f);
        for (size_t i = 0; i < n; ++i) {
            int id = ID_PICK + static_cast<int>(i);
            float rx = left + pad + colW * F(i / rows), ry = top + pad + ih * F(i % rows);
            D2D1_RECT_F r{rx, ry, rx + colW, ry + ih};
            bool cur = pickOpts[i].value == pickVal;
            if (Hot(id)) Fill(r, StateBg(id, pal.popup), 8.f);
            Text(pickOpts[i].label, {r.left + 12.f, r.top, r.right - 34.f, r.bottom}, 13.5f, pal.text, cur ? Semi : Reg);
            if (cur) Icon(GCheck, r.right - 18.f, ry + ih / 2.f, 16.f, pal.accentText);
            int v = pickOpts[i].value, field = pickId;
            Item& it = Add(id, r);
            it.click = [this, v, field] {
                auto fn = pickSend;
                pickId = 0;
                focusId = field;
                if (fn) fn(v);
            };
            it.key = KeyOf(pickRowKey, v);
            FocusRing(id, r, 8.f);
        }
    }

    void Alert() {
        Blocker([] {});
        Fill({0.f, 0.f, W, H}, pal.overlay);
        float cw = std::min(460.f, W - 64.f), x = (W - cw) / 2.f, inner = cw - 48.f;
        std::wstring msg = snap.alertText.empty() ? L"The headphones ask you to confirm this change." : snap.alertText;
        float th = TextH(msg, 14.f, inner);
        float ch = 24.f + 28.f + 10.f + th + 28.f + 38.f + 24.f, top = (H - ch) / 2.f;
        D2D1_RECT_F card{x, top, x + cw, top + ch};
        Shadow(card, 14.f, true);
        Fill(card, pal.popup, 14.f);
        Stroke(card, pal.border, 14.f);
        Icon(GInfo, x + 24.f + 11.f, top + 24.f + 14.f, 22.f, pal.accentText);
        Text(L"Confirm on headphones", {x + 58.f, top + 24.f, x + cw - 24.f, top + 52.f}, 17.f, pal.text, Semi);
        Text(msg, {x + 24.f, top + 62.f, x + cw - 24.f, top + 64.f + th}, 14.f, pal.text2, Reg, AlLeft, true);
        float by = top + 62.f + th + 28.f, bw = 110.f;
        Button(ID_ALERT_OK, {x + cw - 24.f - bw, by, x + cw - 24.f, by + 38.f}, L"Confirm",
               [this] { Cmd(Command::AnswerAlert, 1); }, BtnPrimary);
        Button(ID_ALERT_NO, {x + cw - 24.f - 2.f * bw - 10.f, by, x + cw - 34.f - bw, by + 38.f}, L"Cancel",
               [this] { Cmd(Command::AnswerAlert, 0); });
    }

    // ---- empty / connecting state ------------------------------------------------------------
    void StateView(const Col& c) {
        std::wstring title, sub;
        std::string mac;
        int kind = 0; // 0 none, 1 connect, 2 retry, 3 bluetooth settings
        bool busy = false;
        switch (snap.link) {
        case Link::Connecting: title = L"Connecting…", sub = L"Talking to " + DeviceTitle() + L".", busy = true; break;
        case Link::Initializing: title = L"Reading headphone settings…", sub = L"This takes a moment.", busy = true; break;
        case Link::Failed:
            title = L"Couldn't connect";
            sub = snap.error.empty() ? L"The headphones did not respond." : snap.error;
            kind = 2;
            mac = snap.mac;
            break;
        default:
            if (devs.empty()) {
                title = L"No headphones connected";
                sub = L"Turn on your headphones and connect them in Windows Bluetooth settings.";
                kind = 3;
            } else {
                title = L"Not connected";
                sub = L"Choose the headphones you want to control.";
                kind = 1;
                mac = devs.front().mac;
                for (auto& d : devs)
                    if (d.mac == host.Config().lastMac) mac = d.mac;
            }
            break;
        }
        float tw = std::min(440.f, c.w), sh = TextH(sub, 14.f, tw);
        float total = 120.f + 24.f + 30.f + 8.f + sh + (kind ? 28.f + 40.f : 0.f);
        float cx = c.x + c.w / 2.f, y = TOPH + std::max(16.f, (H - TOPH - total) / 2.f - 24.f);
        float gy = y + 60.f;
        Circle(cx, gy, 60.f, pal.card);
        Ring(cx, gy, 60.f, pal.border, 1.f);
        Icon(GHeadphones, cx, gy, 56.f, busy ? pal.accentText : pal.text3);
        if (busy) {
            float a0 = F(static_cast<int>(GetTickCount64() % 1100)) / 1100.f * 360.f;
            Arc(cx, gy, 60.f, a0, a0 + 100.f, pal.accent, 3.f);
        }
        if (snap.link == Link::Failed) {
            Circle(cx + 44.f, gy + 44.f, 15.f, pal.bg);
            Circle(cx + 44.f, gy + 44.f, 12.f, pal.danger);
            Line(cx + 44.f, gy + 38.f, cx + 44.f, gy + 45.f, Rgb(0xFFFFFF), 2.2f);
            Circle(cx + 44.f, gy + 49.5f, 1.4f, Rgb(0xFFFFFF));
        }
        y += 120.f + 24.f;
        Text(title, {c.x, y, c.x + c.w, y + 30.f}, 21.f, pal.text, Semi, AlCenter);
        y += 38.f;
        Text(sub, {cx - tw / 2.f, y, cx + tw / 2.f, y + sh + 2.f}, 14.f, pal.text2, Reg, AlCenter, true);
        y += sh + 28.f;
        if (kind == 1) {
            Button(ID_RETRY, {cx - 70.f, y, cx + 70.f, y + 40.f}, L"Connect", [this, mac] { host.SelectDevice(mac); }, BtnPrimary,
                   !mac.empty());
        } else if (kind == 2) {
            Button(ID_RETRY, {cx - 150.f, y, cx - 6.f, y + 40.f}, L"Retry", [this, mac] { host.SelectDevice(mac); }, BtnPrimary,
                   !mac.empty());
            Button(ID_BTSET, {cx + 6.f, y, cx + 186.f, y + 40.f}, L"Bluetooth settings", [] { OpenBluetoothSettings(); });
        } else if (kind == 3) {
            Button(ID_BTSET, {cx - 110.f, y, cx + 110.f, y + 40.f}, L"Open Bluetooth settings", [] { OpenBluetoothSettings(); },
                   BtnPrimary);
        }
    }

    // ---- Home --------------------------------------------------------------------------------
    float BatteryRow(float x, float y, float w, const wchar_t* tag, const Battery& b) {
        float l = x;
        if (tag) {
            Text(tag, {x, y, x + 44.f, y + 26.f}, 13.f, pal.text2);
            l = x + 46.f;
        }
        int pct = std::clamp(b.percent, 0, 100);
        bool chg = b.charging != Charging::No;
        C c = chg ? pal.good : pct <= 20 ? pal.bad : pal.accent;
        float r = x + w - 70.f, cy = y + 13.f;
        Fill({l, cy - 4.f, r, cy + 4.f}, pal.track, 4.f);
        if (pct > 0) Fill({l, cy - 4.f, l + std::max(8.f, (r - l) * F(pct) / 100.f), cy + 4.f}, c, 4.f);
        std::wstring t = std::to_wstring(pct) + L"%";
        float tw = Measure(t, 14.f, Semi).width;
        Text(t, {r, y, x + w, y + 26.f}, 14.f, pal.text, Semi, AlRight);
        if (chg) Icon(GBolt, x + w - tw - 12.f, cy, 15.f, pal.good);
        return y + 30.f;
    }

    float DeviceBody(float x, float y, float w) {
        const Snapshot& s = snap;
        D2D1_RECT_F g{x, y, x + 64.f, y + 64.f};
        Fill(g, pal.accentSoft, 16.f);
        Icon(GHeadphones, x + 32.f, y + 32.f, 38.f, pal.accentText);
        float tx = x + 80.f;
        Text(DeviceTitle(), {tx, y + 2.f, x + w, y + 32.f}, 20.f, pal.text, Semi);
        float cw = Chip(tx, y + 37.f, L"Connected", pal.good);
        if (!s.firmware.empty())
            Text(L"Firmware " + s.firmware, {tx + cw + 10.f, y + 37.f, x + w, y + 63.f}, 12.5f, pal.text3);
        y += 64.f + 22.f;
        bool split = s.battLeft.present || s.battRight.present || s.battCase.present;
        if (split || s.battSingle.present) {
            Text(L"Battery", {x, y, x + w, y + 20.f}, 13.f, pal.text2, Semi);
            y += 26.f;
            if (split) {
                if (s.battLeft.present) y = BatteryRow(x, y, w, L"Left", s.battLeft);
                if (s.battRight.present) y = BatteryRow(x, y, w, L"Right", s.battRight);
                if (s.battCase.present) y = BatteryRow(x, y, w, L"Case", s.battCase);
            } else {
                y = BatteryRow(x, y, w, nullptr, s.battSingle);
            }
            if (s.battSingle.charging == Charging::Full)
                Text(L"Fully charged", {x, y - 4.f, x + w, y + 14.f}, 12.f, pal.good), y += 16.f;
        }
        if (s.hasVolume && s.volumeRange.max > s.volumeRange.min) {
            y += 8.f;
            Divider(x, y, w);
            y += 18.f;
            y = SliderRow(ID_VOL, x, y, w, L"Volume", s.volumeRange, s.volume, [this](int v) { Cmd(Command::SetVolume, v); },
                          false, GSpeaker);
            y -= 6.f;
        }
        return y;
    }

    float NoiseBody(float x, float y, float w) {
        const Snapshot& s = snap;
        const wchar_t* sub = s.nc == NcMode::NoiseCancelling ? L"Blocks out the world around you."
                             : s.nc == NcMode::Ambient       ? L"Lets surrounding sound in so you stay aware."
                             : s.nc == NcMode::Wind          ? L"Reduces wind noise outdoors."
                                                             : L"No noise processing.";
        y = CardHeader(x, y, w, L"Noise control", sub);
        std::vector<NcMode> modes{NcMode::NoiseCancelling};
        if (s.hasAmbient) modes.push_back(NcMode::Ambient);
        modes.push_back(NcMode::Off);
        if (s.hasWind) modes.push_back(NcMode::Wind);
        std::vector<std::wstring> labels;
        int sel = -1;
        for (size_t i = 0; i < modes.size(); ++i) {
            switch (modes[i]) {
            case NcMode::NoiseCancelling: labels.push_back(L"Noise cancelling|Cancelling"); break;
            case NcMode::Ambient: labels.push_back(L"Ambient sound|Ambient"); break;
            case NcMode::Off: labels.push_back(L"Off"); break;
            case NcMode::Wind: labels.push_back(L"Wind"); break;
            }
            if (modes[i] == s.nc) sel = static_cast<int>(i);
        }
        y = Segmented(ID_NC, x, y, w, 40.f, labels, sel, [this, modes](int i) { Cmd(Command::SetNc, static_cast<int>(modes[size_t(i)])); });
        if (s.nc == NcMode::Ambient && s.hasAmbient) {
            bool adapt = s.adaptiveAmbient.has && s.adaptiveAmbient.on;
            if (s.adaptiveAmbient.has) {
                y += 10.f;
                y = ToggleRow(ID_ADAPT, x, y, w, L"Adaptive ambient", L"Adjusts the level to your surroundings", adapt,
                              [this](bool on) { Feat(Feature::AdaptiveAmbient, on ? 1 : 0); });
                if (adapt && s.adaptiveSensitivity.has && !s.adaptiveSensitivity.options.empty()) {
                    Text(L"Sensitivity", {x, y, x + w, y + 20.f}, 12.5f, pal.text2, Semi);
                    y += 26.f;
                    y = Picker(ID_ADAPT_SENS, x, y, w, s.adaptiveSensitivity.options, s.adaptiveSensitivity.value,
                               [this](int v) { Feat(Feature::AdaptiveSensitivity, v); });
                    y += 8.f;
                }
                y += 12.f;
            } else {
                y += 22.f;
            }
            y = SliderRow(ID_AMB, x, y, w, L"Ambient sound level", s.ambientRange, s.ambientLevel,
                          [this](int v) { Cmd(Command::SetAmbientLevel, v); }, false, GNone, !adapt);
            if (adapt) {
                Text(L"Set automatically while Adaptive ambient is on", {x, y - 8.f, x + w, y + 10.f}, 12.5f, pal.text3);
                y += 12.f;
            }
            if (s.hasVoiceFocus) {
                y += 4.f;
                y = ToggleRow(ID_VF, x, y, w, L"Focus on voice", L"Let voices through, reduce other sounds", s.voiceFocus,
                              [this](bool on) { Cmd(Command::SetVoiceFocus, on ? 1 : 0); });
                y -= 8.f;
            }
        }
        return y;
    }

    bool HasDeviceSettings() const {
        const Snapshot& s = snap;
        return s.hasSpeakToChat || s.hasTouchPanel || s.hasDsee || s.hasAutoPause ||
               (s.hasAutoPowerOff && !s.autoPowerOffOptions.empty()) || s.hasConnectionPriority;
    }

    float SettingsBody(float x, float y, float w) {
        const Snapshot& s = snap;
        y = CardHeader(x, y, w, L"Headphone settings");
        y -= 8.f;
        bool first = true;
        auto sep = [&] {
            if (!first) Divider(x, y, w);
            first = false;
        };
        auto stc = [this](int on, int sens, int to) {
            Command c;
            c.kind = Command::SetSpeakToChat;
            c.a = on;
            c.b = sens;
            c.c = to;
            host.Send(c);
        };
        if (s.hasSpeakToChat) {
            sep();
            y = ToggleRow(ID_STC, x, y, w, L"Speak-to-Chat", L"Pauses playback when you start talking", s.speakToChat,
                          [this, stc](bool on) { stc(on ? 1 : 0, snap.stcSensitivity, snap.stcTimeout); });
            if (s.speakToChat) {
                Text(L"Voice detection sensitivity", {x, y, x + w, y + 20.f}, 12.5f, pal.text2, Semi);
                y += 26.f;
                y = Segmented(ID_STC_SENS, x, y, w, 34.f, {L"Auto", L"High", L"Low"}, std::clamp(s.stcSensitivity, 0, 2),
                              [this, stc](int i) { stc(1, i, snap.stcTimeout); });
                y += 14.f;
                Text(L"Resume playback", {x, y, x + w, y + 20.f}, 12.5f, pal.text2, Semi);
                y += 26.f;
                y = Segmented(ID_STC_TO, x, y, w, 34.f, {L"Short", L"Medium", L"Long", L"Manual"}, std::clamp(s.stcTimeout, 0, 3),
                              [this, stc](int i) { stc(1, snap.stcSensitivity, i); });
                y += 16.f;
            }
        }
        if (s.hasTouchPanel) {
            sep();
            y = ToggleRow(ID_TOUCH, x, y, w, L"Touch sensor", L"Tap and swipe controls on the ear cup", s.touchPanel,
                          [this](bool on) { Cmd(Command::SetTouchPanel, on ? 1 : 0); });
        }
        if (s.hasDsee) {
            sep();
            y = ToggleRow(ID_DSEE, x, y, w, s.dseeName.empty() ? L"DSEE" : s.dseeName,
                          s.dseeAvailable ? L"Restores detail lost to compression" : L"Not available in the current audio mode",
                          s.dsee, [this](bool on) { Cmd(Command::SetDsee, on ? 1 : 0); }, s.dseeAvailable);
        }
        if (s.hasAutoPause) {
            sep();
            y = ToggleRow(ID_PAUSE, x, y, w, L"Pause when headphones are removed", L"Resumes when you put them back on",
                          s.autoPause, [this](bool on) { Cmd(Command::SetAutoPause, on ? 1 : 0); });
        }
        if (s.hasAutoPowerOff && !s.autoPowerOffOptions.empty()) {
            sep();
            y += 14.f;
            Text(L"Auto power-off", {x, y, x + w, y + 22.f}, 14.f, pal.text);
            Text(L"Turn off after this long without a connection", {x, y + 23.f, x + w, y + 41.f}, 12.5f, pal.text3);
            y += 50.f;
            std::vector<std::wstring> labels;
            std::vector<int> values;
            int sel = -1;
            for (auto& o : s.autoPowerOffOptions) {
                if (o.value == s.autoPowerOffMinutes) sel = static_cast<int>(labels.size());
                labels.push_back(o.label);
                values.push_back(o.value);
            }
            y = Chips(ID_APO, x, y, w, labels, sel, [this, values](int i) { Cmd(Command::SetAutoPowerOff, values[size_t(i)]); });
            y += 16.f;
        }
        if (s.hasConnectionPriority) {
            sep();
            y += 14.f;
            Text(L"Connection priority", {x, y, x + w, y + 22.f}, 14.f, pal.text);
            Text(L"Favour audio quality or a steadier Bluetooth link", {x, y + 23.f, x + w, y + 41.f}, 12.5f, pal.text3);
            y += 50.f;
            y = Segmented(ID_PRIO, x, y, w, 36.f, {L"Sound quality", L"Stable connection|Stable"},
                          std::clamp(s.connectionPriority, 0, 1), [this](int i) { Cmd(Command::SetConnectionPriority, i); });
            y += 4.f;
        }
        return y;
    }

    const Endpoint* FindEndpoint() const {
        for (auto& e : eps)
            if (!e.btMac.empty() && !snap.mac.empty() && _stricmp(e.btMac.c_str(), snap.mac.c_str()) == 0) return &e;
        return nullptr;
    }

    float CodecBody(float x, float y, float w) {
        const Snapshot& s = snap;
        y = CardHeader(x, y, w, L"Audio codec");
        Codec codec = s.hasCodec ? s.codec : Codec::Unknown;
        std::wstring name = s.hasCodec ? CodecName(codec) : L"Not reported";
        float bw = std::ceil(Measure(name, 22.f, Semi).width) + 32.f;
        Fill({x, y, x + bw, y + 46.f}, pal.accentSoft, 10.f);
        Text(name, {x, y, x + bw, y + 46.f}, 22.f, pal.accentText, Semi, AlCenter);
        bool hiRes = s.hasCodec && (codec == Codec::LDAC || codec == Codec::AptXHD);
        float by = y + 46.f;
        if (hiRes) {
            const std::wstring hr = L"Hi-Res capable codec";
            float hw = std::ceil(Measure(hr, 12.5f, Semi).width) + 34.f, hx = x + bw + 12.f, hy = y + 10.f;
            if (hx + hw > x + w) hx = x, hy = y + 56.f, by = y + 82.f;
            Fill({hx, hy, hx + hw, hy + 26.f}, Mix(pal.card, pal.good, pal.dark ? 0.16f : 0.12f), 13.f);
            Icon(GCheck, hx + 14.f, hy + 13.f, 14.f, pal.good);
            Text(hr, {hx + 26.f, hy, hx + hw - 4.f, hy + 26.f}, 12.5f, pal.text, Semi);
        }
        y = by + 14.f;
        CodecInfo info = InfoFor(codec);
        float dh = TextH(info.desc, 13.5f, w);
        Text(info.desc, {x, y, x + w, y + dh + 2.f}, 13.5f, pal.text2, Reg, AlLeft, true);
        y += dh + 6.f;
        if (s.hasCodec) {
            const std::wstring note = L"Codec as reported by the headphones; with multipoint it may describe another connected device.";
            float nh = TextH(note, 12.5f, w);
            Text(note, {x, y, x + w, y + nh + 2.f}, 12.5f, pal.text3, Reg, AlLeft, true);
            y += nh + 6.f;
        }
        y += 6.f;
        Divider(x, y, w);
        y = KV(x, y, w, L"Max bitrate", info.bitrate, pal.text);
        Divider(x, y, w);
        y = KV(x, y, w, L"Link format", info.format, pal.text);
        Divider(x, y, w);
        const Endpoint* ep = FindEndpoint();
        y = KV(x, y, w, L"Windows output", ep ? ep->name : L"Not in use", ep ? pal.text : pal.warn);
        if (!ep) return y - 6.f;
        y -= 6.f;
        std::wstring line;
        C lc = pal.text3;
        Glyph g = GNone;
        if (!ep->isDefault) {
            line = L"Not the current Windows output", lc = pal.warn, g = GWarn;
        } else if (ep->sampleRate > 0) {
            wchar_t b[96];
            int n = ep->sampleRate % 1000 == 0 ? swprintf_s(b, L"%d kHz", ep->sampleRate / 1000)
                                               : swprintf_s(b, L"%.1f kHz", ep->sampleRate / 1000.0);
            if (ep->bits > 0 && n > 0) n += swprintf_s(b + n, 96 - n, L" · %d-bit", ep->bits);
            if (ep->channels > 0 && n > 0) swprintf_s(b + n, 96 - n, ep->channels == 2 ? L" · stereo" : L" · %d ch", ep->channels);
            line = std::wstring(L"Current output · ") + b;
        } else {
            line = L"Current Windows output";
        }
        float lw = Measure(line, 12.5f).width;
        if (g != GNone) Icon(g, x + w - std::min(lw, w - 20.f) - 12.f, y + 9.f, 14.f, lc);
        Text(line, {x + 20.f, y, x + w, y + 18.f}, 12.5f, lc, Reg, AlRight);
        y += 22.f;
        return y;
    }

    float HostsBody(float x, float y, float w) {
        y = CardHeader(x, y, w, L"Connected devices", L"Multipoint keeps the headphones connected to two sources.");
        bool stack = false; // one layout for every row
        for (auto& h : snap.hosts) {
            float bw = BtnW(h.connected ? L"Disconnect" : L"Connect") +
                       (h.connected && !h.playback ? BtnW(L"Play here") + 8.f : 0.f);
            stack = stack || w - 52.f - bw - 12.f < 110.f;
        }
        for (size_t i = 0; i < snap.hosts.size(); ++i) {
            const PairedHost& h = snap.hosts[i];
            int id = ID_HOST + static_cast<int>(i) * 3;
            if (i) Divider(x, y, w), y += 1.f;
            std::wstring name = h.name.empty() ? Widen(h.mac) : h.name;
            std::wstring lower = name;
            for (auto& ch : lower) ch = static_cast<wchar_t>(towlower(ch));
            bool pc = lower.find(L"pc") != std::wstring::npos || lower.find(L"laptop") != std::wstring::npos ||
                      lower.find(L"desktop") != std::wstring::npos || lower.find(L"windows") != std::wstring::npos;
            std::string mac = h.mac;
            bool conn = h.connected;
            std::wstring mainL = conn ? L"Disconnect" : L"Connect";
            float mw = BtnW(mainL), pw = (conn && !h.playback) ? BtnW(L"Play here") : 0.f;
            float btnsW = mw + (pw > 0.f ? pw + 8.f : 0.f);
            float rowH = 60.f, cy = y + 30.f;
            Circle(x + 18.f, cy, 18.f, h.playback ? pal.accentSoft : pal.inset);
            Icon(pc ? GLaptop : GPhone, x + 18.f, cy, 18.f, h.playback ? pal.accentText : pal.text2);
            float tr = stack ? x + w : x + w - btnsW - 12.f;
            Text(name, {x + 50.f, y + 10.f, tr, y + 31.f}, 14.f, pal.text, Semi);
            Text(h.playback ? L"Playing audio" : conn ? L"Connected" : L"Not connected", {x + 50.f, y + 31.f, tr, y + 49.f}, 12.5f,
                 h.playback ? pal.accentText : pal.text3);
            float bx = stack ? x + 50.f : x + w - btnsW, byy = stack ? y + 58.f : y + 12.f;
            if (stack) rowH = 58.f + 36.f + 12.f;
            if (pw > 0.f) {
                Button(id + 1, {bx, byy, bx + pw, byy + 36.f}, L"Play here", [this, mac] {
                    Command c;
                    c.kind = Command::HostSelectPlayback;
                    c.text = mac;
                    host.Send(c);
                });
                bx += pw + 8.f;
            }
            Button(id, {bx, byy, bx + mw, byy + 36.f}, mainL, [this, mac, conn] {
                Command c;
                c.kind = conn ? Command::HostDisconnect : Command::HostConnect;
                c.text = mac;
                host.Send(c);
            });
            y += rowH;
        }
        return y - 8.f;
    }

    float PowerBody(float x, float y, float w) {
        // Cancel is at least as wide as the trigger and takes its place, so a double-click can never land on the confirm.
        float pw = BtnW(L"Power off"), cw = std::max(BtnW(L"Cancel"), pw), bwC = cw + 8.f + pw;
        float bw = confirmPower ? bwC : pw;
        bool stack = w - 52.f - bwC - 12.f < 150.f;
        float tr = stack ? x + w : x + w - bw - 12.f;
        Circle(x + 18.f, y + 20.f, 18.f, Mix(pal.card, pal.bad, pal.dark ? 0.16f : 0.1f));
        Icon(GPower, x + 18.f, y + 20.f, 18.f, pal.bad);
        Text(confirmPower ? L"Turn off the headphones now?" : L"Power off headphones", {x + 50.f, y, tr, y + 22.f}, 14.f,
             pal.text, Semi);
        Text(confirmPower ? L"They will disconnect from every device." : L"Turn them back on with the power button.",
             {x + 50.f, y + 22.f, tr, y + 40.f}, 12.5f, pal.text3);
        float byy = stack ? y + 52.f : y + 2.f;
        if (!confirmPower) {
            float bx = stack ? x + 50.f : x + w - pw;
            Button(ID_POWER, {bx, byy, bx + pw, byy + 36.f}, L"Power off", [this] {
                confirmPower = true;
                confirmAt = GetTickCount64();
                focusId = ID_POWER_NO;
            }, BtnDangerOutline);
        } else {
            float cl = stack ? x + 50.f : x + w - cw, ol = stack ? cl + cw + 8.f : cl - 8.f - pw;
            Button(ID_POWER_NO, {cl, byy, cl + cw, byy + 36.f}, L"Cancel", [this] { confirmPower = false; });
            Button(ID_POWER_OK, {ol, byy, ol + pw, byy + 36.f}, L"Power off", [this] {
                if (GetTickCount64() - confirmAt < GetDoubleClickTime()) return;
                confirmPower = false;
                Cmd(Command::PowerOff, 0);
            }, BtnDanger);
        }
        return stack ? y + 52.f + 36.f : y + 40.f;
    }

    float NowPlayingBody(float x, float y, float w) {
        const Snapshot& s = snap;
        bool ctl = s.hasPlaybackControl, playing = s.playState == 1;
        std::wstring title = !s.hasTrackInfo ? std::wstring(playing ? L"Playing" : s.playState == 2 ? L"Paused" : L"Playback")
                             : s.trackTitle.empty() ? std::wstring(L"Nothing playing") : s.trackTitle;
        std::wstring sub = !s.hasTrackInfo ? std::wstring(L"Control playback on the connected device")
                           : !s.trackArtist.empty() ? s.trackArtist : s.trackAlbum;
        const float bw = 180.f; // prev / play-pause / next, centres 70 apart
        bool side = ctl && w - 72.f - bw - 16.f >= 150.f; // buttons beside the text, else on their own row
        float tr = side ? x + w - bw - 16.f : x + w;
        Fill({x, y, x + 56.f, y + 56.f}, pal.accentSoft, 14.f);
        Icon(GMusic, x + 28.f, y + 28.f, 28.f, pal.accentText);
        Text(L"Now playing", {x + 72.f, y, tr, y + 18.f}, 12.5f, pal.text3, Semi);
        Text(title, {x + 72.f, y + 18.f, tr, y + 40.f}, 15.5f, pal.text, Semi);
        Text(sub, {x + 72.f, y + 39.f, tr, y + 57.f}, 13.f, pal.text2);
        y += 56.f;
        if (!ctl) return y;
        float cx = side ? x + w - bw / 2.f : x + w / 2.f, cy = side ? y - 28.f : y + 20.f + 24.f;
        auto pb = [this](int a) { Cmd(Command::Playback, a); };
        RoundBtn(ID_PREV, cx - 70.f, cy, 20.f, GPrev, false, [pb] { pb(3); });
        RoundBtn(ID_PLAYPAUSE, cx, cy, 24.f, playing ? GPause : GPlay, true, [pb, playing] { pb(playing ? 1 : 0); });
        RoundBtn(ID_NEXT, cx + 70.f, cy, 20.f, GNext, false, [pb] { pb(2); });
        return side ? y : y + 20.f + 48.f;
    }

    Body B(float (Impl::*fn)(float, float, float)) {
        return [this, fn](float x, float y, float w) { return (this->*fn)(x, y, w); };
    }

    float PageHome(const Col& c, float y) {
        const Snapshot& s = snap;
        bool two = c.w >= 640.f;
        float cw = two ? (c.w - GAP) / 2.f : c.w;
        float col[2] = {y, y}, xs[2] = {c.x, two ? c.x + cw + GAP : c.x};
        auto place = [&](int side, const Body& b) {
            int k = two ? side : 0;
            col[k] = Card(xs[k], col[k], cw, b) + GAP;
        };
        place(0, B(&Impl::DeviceBody));
        if (s.hasPlaybackControl || s.hasTrackInfo) place(1, B(&Impl::NowPlayingBody));
        if (s.hasNc) place(0, B(&Impl::NoiseBody));
        if (HasDeviceSettings()) place(1, B(&Impl::SettingsBody));
        place(0, B(&Impl::CodecBody));
        if (s.hasMultipoint && !s.hosts.empty()) place(1, B(&Impl::HostsBody));
        if (s.hasPowerOff) place(1, B(&Impl::PowerBody));
        return std::max(col[0], col[1]) - GAP;
    }

    // ---- Equalizer ------------------------------------------------------------------------------
    void SendBands(const std::array<int, 10>& bands) {
        Command c;
        c.kind = Command::SetEqBands;
        c.a = snap.eqBandCount;
        c.bands = bands;
        for (int k = 0; k < c.a && k < 10; ++k)
            c.bands[size_t(k)] = std::clamp(c.bands[size_t(k)], snap.eqBandRange.min, snap.eqBandRange.max);
        pendingBandsUntil = 0;
        host.Send(c);
    }

    void ResetFlat() {
        SendBands({});
        if (snap.hasClearBass) Cmd(Command::SetClearBass, std::clamp(0, snap.clearBassRange.min, snap.clearBassRange.max));
    }

    float Graph(float x, float y, float w) {
        const Snapshot& s = snap;
        int n = std::min(s.eqBandCount, 10);
        const Range& rg = s.eqBandRange;
        if (n <= 0 || rg.max <= rg.min) return y;
        float top = y + 30.f, bot = top + 210.f;
        D2D1_RECT_F panel{x, y, x + w, bot + 34.f};
        Fill(panel, pal.inset, 10.f);
        Stroke(panel, pal.divider, 10.f);
        float bx0 = x + 46.f, bx1 = x + w - 14.f, colW = (bx1 - bx0) / F(n);
        auto pos = [&](int v) { return (bot - 10.f) - (bot - top - 20.f) * F(v - rg.min) / F(rg.max - rg.min); };
        // grid + dB labels
        int mid = std::clamp(0, rg.min, rg.max);
        int marks[5] = {rg.max, (rg.max + mid) / 2, mid, (rg.min + mid) / 2, rg.min};
        for (int k = 0; k < 5; ++k) {
            if (k > 0 && marks[k] == marks[k - 1]) continue;
            float gy = pos(marks[k]);
            Fill({bx0 - 4.f, gy - 0.5f, bx1 + 4.f, gy + 0.5f}, marks[k] == mid ? pal.track : pal.divider);
            Text(marks[k] == 0 ? L"0 dB" : Signed(marks[k]), {x + 4.f, gy - 9.f, bx0 - 10.f, gy + 9.f}, 11.5f, pal.text3, Reg, AlRight);
        }
        std::vector<D2D1_POINT_2F> pts;
        std::vector<int> vals;
        for (int i = 0; i < n; ++i) {
            int v = std::clamp(Shown(ID_BAND + i, s.eqBands[size_t(i)]), rg.min, rg.max);
            vals.push_back(v);
            pts.push_back({bx0 + colW * (F(i) + 0.5f), pos(v)});
        }
        for (int i = 0; i < n; ++i) {
            float cx = pts[size_t(i)].x;
            Fill({cx - 0.5f, top + 10.f, cx + 0.5f, bot - 10.f}, pal.divider);
        }
        // smooth curve (Catmull-Rom through the thumbs, flat at the edges)
        if (!dry) {
            std::vector<D2D1_POINT_2F> p;
            p.push_back({bx0 - 4.f, pts.front().y});
            p.insert(p.end(), pts.begin(), pts.end());
            p.push_back({bx1 + 4.f, pts.back().y});
            auto build = [&](ID2D1GeometrySink* sk, bool closed) {
                sk->BeginFigure(p[0], closed ? D2D1_FIGURE_BEGIN_FILLED : D2D1_FIGURE_BEGIN_HOLLOW);
                for (size_t i = 0; i + 1 < p.size(); ++i) {
                    D2D1_POINT_2F p0 = p[i ? i - 1 : 0], p1 = p[i], p2 = p[i + 1], p3 = p[std::min(i + 2, p.size() - 1)];
                    float lo = std::min(p1.y, p2.y), hi = std::max(p1.y, p2.y); // no overshoot past the thumbs
                    sk->AddBezier({{p1.x + (p2.x - p0.x) / 6.f, std::clamp(p1.y + (p2.y - p0.y) / 6.f, lo, hi)},
                                   {p2.x - (p3.x - p1.x) / 6.f, std::clamp(p2.y - (p3.y - p1.y) / 6.f, lo, hi)},
                                   p2});
                }
                if (closed) {
                    sk->AddLine({p.back().x, bot - 10.f});
                    sk->AddLine({p.front().x, bot - 10.f});
                }
                sk->EndFigure(closed ? D2D1_FIGURE_END_CLOSED : D2D1_FIGURE_END_OPEN);
            };
            if (auto fill = Path([&](ID2D1GeometrySink* sk) { build(sk, true); })) {
                br->SetColor(Alpha(pal.accent, pal.dark ? 0.14f : 0.10f));
                tgt->FillGeometry(fill.Get(), br.Get());
            }
            if (auto line = Path([&](ID2D1GeometrySink* sk) { build(sk, false); })) {
                br->SetColor(pal.accent);
                tgt->DrawGeometry(line.Get(), br.Get(), 2.5f, round.Get());
            }
        }
        for (int i = 0; i < n; ++i) {
            int id = ID_BAND + i, v = vals[size_t(i)];
            float cx = pts[size_t(i)].x;
            bool act = dragId == id || Hot(id);
            Text(Signed(v), {cx - colW / 2.f, y + 6.f, cx + colW / 2.f, y + 26.f}, 12.5f, act ? pal.accentText : pal.text2,
                 act ? Semi : Reg, AlCenter);
            Thumb(id, cx, pts[size_t(i)].y);
            Text(HzLabel(s.eqBandHz[size_t(i)], i), {cx - colW / 2.f, bot + 4.f, cx + colW / 2.f, bot + 26.f}, 12.f, pal.text3,
                 Reg, AlCenter);
            SliderItem(id, {cx - colW / 2.f, top, cx + colW / 2.f, bot}, rg, v, true, [this, i, n](int val) {
                Command c;
                c.kind = Command::SetEqBands;
                c.a = n;
                const ULONGLONG now = GetTickCount64();
                c.bands = now < pendingBandsUntil ? pendingBands : snap.eqBands;
                c.bands[size_t(i)] = val;
                pendingBands = c.bands;
                pendingBandsUntil = now + 1500;
                host.Send(c);
            });
        }
        return panel.bottom;
    }

    float EqBody(float x, float y, float w) {
        const Snapshot& s = snap;
        float rw = BtnW(L"Reset to flat");
        bool canReset = s.eqBandCount > 0;
        float top = y;
        y = CardHeader(x, y, w, L"Sound profile", L"Drag a band, or click anywhere in its column.", canReset ? rw + 12.f : 0.f);
        if (canReset)
            Button(ID_FLAT, {x + w - rw, top + 2.f, x + w, top + 38.f}, L"Reset to flat", [this] { ResetFlat(); }, BtnNormal,
                   s.eqAvailable);
        if (!s.eqAvailable) {
            const std::wstring msg = L"The headphones are not accepting equalizer changes right now";
            float h = TextH(msg, 13.f, w - 28.f);
            Icon(GInfo, x + 9.f, y + 9.f, 17.f, pal.warn);
            Text(msg, {x + 28.f, y, x + w, y + h + 2.f}, 13.f, pal.text2, Reg, AlLeft, true);
            y += std::max(h, 18.f) + 16.f;
        }
        if (!s.eqPresets.empty()) {
            Text(L"Sound preset", {x, y, x + w, y + 20.f}, 12.5f, pal.text2, Semi);
            y += 28.f;
            std::vector<std::wstring> labels;
            std::vector<int> values;
            int sel = -1;
            for (auto& p : s.eqPresets) {
                if (p.value == s.eqPreset) sel = static_cast<int>(labels.size());
                labels.push_back(p.label);
                values.push_back(p.value);
            }
            y = Chips(ID_EQP, x, y, w, labels, sel, [this, values](int i) { Cmd(Command::SetEqPreset, values[size_t(i)]); });
            y += 20.f;
        }
        if (s.eqBandCount > 0) y = Graph(x, y, w);
        if (s.hasClearBass && s.clearBassRange.max > s.clearBassRange.min) {
            y += 20.f;
            y = SliderRow(ID_CB, x, y, w, L"Clear Bass", s.clearBassRange, s.clearBass,
                          [this](int v) { Cmd(Command::SetClearBass, v); }, true, GNone, s.eqAvailable);
            y -= 8.f;
        }
        return y;
    }

    void ApplyPreset(size_t i) {
        auto& list = host.Config().eqPresets;
        if (i >= list.size() || list[i].bandCount != snap.eqBandCount) return;
        SendBands(list[i].bands);
        if (snap.hasClearBass)
            Cmd(Command::SetClearBass, std::clamp(list[i].clearBass, snap.clearBassRange.min, snap.clearBassRange.max));
    }

    size_t FindPreset(const std::wstring& name) const {
        auto& list = host.Config().eqPresets;
        for (size_t i = 0; i < list.size(); ++i)
            if (_wcsicmp(list[i].name.c_str(), name.c_str()) == 0) return i;
        return std::wstring::npos;
    }

    bool CanSave() const {
        std::wstring n = Trim(presetName);
        return !n.empty() && snap.eqBandCount > 0 &&
               (FindPreset(n) != std::wstring::npos || host.Config().eqPresets.size() < kMaxPresets);
    }

    void SavePreset() {
        if (!CanSave()) return;
        EqPreset p;
        p.name = Trim(presetName);
        p.bandCount = snap.eqBandCount;
        p.bands = snap.eqBands;
        p.clearBass = snap.clearBass;
        auto& list = host.Config().eqPresets;
        size_t at = FindPreset(p.name);
        if (at != std::wstring::npos) list[at] = p;
        else list.push_back(p);
        host.ConfigChanged();
        presetName.clear();
    }

    void CommitRename() {
        auto& list = host.Config().eqPresets;
        std::wstring n = Trim(renameBuf);
        if (renameIdx < 0 || size_t(renameIdx) >= list.size() || n.empty()) return;
        size_t me = size_t(renameIdx), other = FindPreset(n);
        if (other != std::wstring::npos && other != me) {
            renameErr = L"A preset with this name already exists";
            return;
        }
        list[me].name = n;
        host.ConfigChanged();
        renameIdx = -1;
        renameErr.clear();
        focusId = 0;
    }

    float PresetsBody(float x, float y, float w) {
        auto& list = host.Config().eqPresets;
        y = CardHeader(x, y, w, L"My presets", L"Curves saved on this PC. Applying one sets the bands and Clear Bass.");
        if (list.empty()) {
            Text(L"No saved presets yet. Name the current curve below and save it.", {x, y, x + w, y + 24.f}, 13.5f, pal.text3);
            y += 36.f;
        }
        for (size_t i = 0; i < list.size(); ++i) {
            const EqPreset& p = list[i];
            int id = ID_UP + static_cast<int>(i) * 4;
            bool ok = p.bandCount == snap.eqBandCount;
            Divider(x, y, w);
            float rowY = y, cy = y + 30.f, extra = 0.f;
            if (static_cast<int>(i) == renameIdx) {
                float bw1 = BtnW(L"Save"), bw2 = BtnW(L"Cancel");
                if (!renameErr.empty()) {
                    Text(renameErr, {x, rowY + 50.f, x + w, rowY + 68.f}, 12.5f, pal.bad);
                    extra = 20.f;
                }
                TextField(ID_RENAME, {x, cy - 18.f, x + w - bw1 - bw2 - 20.f, cy + 18.f}, &renameBuf, L"Preset name",
                          [this] { CommitRename(); });
                Button(ID_REN_NO, {x + w - bw1 - bw2 - 8.f, cy - 18.f, x + w - bw1 - 8.f, cy + 18.f}, L"Cancel", [this] {
                    renameIdx = -1;
                    renameErr.clear();
                    focusId = 0;
                });
                Button(ID_REN_OK, {x + w - bw1, cy - 18.f, x + w, cy + 18.f}, L"Save", [this] { CommitRename(); }, BtnPrimary,
                       !Trim(renameBuf).empty());
            } else if (static_cast<int>(i) == deleteIdx) {
                // Cancel (>= trigger width) sits where the Delete trigger was; the destructive button is to its left.
                float bw1 = BtnW(L"Delete"), cw2 = std::max(BtnW(L"Cancel"), bw1);
                Text(L"Delete “" + p.name + L"”?", {x, rowY, x + w - bw1 - cw2 - 20.f, rowY + 60.f}, 14.f, pal.text, Semi);
                Button(ID_DEL_NO, {x + w - cw2, cy - 18.f, x + w, cy + 18.f}, L"Cancel", [this] { deleteIdx = -1; });
                Button(ID_DEL_OK, {x + w - cw2 - 8.f - bw1, cy - 18.f, x + w - cw2 - 8.f, cy + 18.f}, L"Delete", [this, i] {
                    if (GetTickCount64() - confirmAt < GetDoubleClickTime()) return;
                    auto& l = host.Config().eqPresets;
                    if (i < l.size()) {
                        l.erase(l.begin() + static_cast<std::ptrdiff_t>(i));
                        host.ConfigChanged();
                    }
                    deleteIdx = -1;
                }, BtnDanger);
            } else {
                float aw = BtnW(L"Apply"), rw = BtnW(L"Rename"), dw2 = BtnW(L"Delete"), bx = x + w - aw - rw - dw2 - 8.f;
                wchar_t meta[96];
                if (ok) {
                    if (snap.hasClearBass && p.clearBass != 0)
                        swprintf_s(meta, L"%d bands · Clear Bass %ls", p.bandCount, Signed(p.clearBass).c_str());
                    else
                        swprintf_s(meta, L"%d bands", p.bandCount);
                } else {
                    swprintf_s(meta, L"Made for %d-band headphones; these have %d", p.bandCount, snap.eqBandCount);
                }
                Text(p.name, {x, rowY + 10.f, bx - 12.f, rowY + 32.f}, 14.f, ok ? pal.text : pal.text3, Semi);
                Text(meta, {x, rowY + 32.f, bx - 12.f, rowY + 50.f}, 12.5f, ok ? pal.text3 : pal.warn);
                Button(id, {bx, cy - 17.f, bx + aw, cy + 17.f}, L"Apply", [this, i] { ApplyPreset(i); }, BtnNormal,
                       ok && snap.eqAvailable);
                Button(id + 1, {bx + aw + 4.f, cy - 17.f, bx + aw + 4.f + rw, cy + 17.f}, L"Rename", [this, i] {
                    auto& l = host.Config().eqPresets;
                    if (i >= l.size()) return;
                    renameIdx = static_cast<int>(i);
                    deleteIdx = -1;
                    renameBuf = l[i].name;
                    renameErr.clear();
                    focusId = ID_RENAME;
                }, BtnSubtle);
                Button(id + 2, {x + w - dw2, cy - 17.f, x + w, cy + 17.f}, L"Delete", [this, i] {
                    if (i >= host.Config().eqPresets.size()) return;
                    deleteIdx = static_cast<int>(i);
                    renameIdx = -1;
                    confirmAt = GetTickCount64();
                }, BtnSubtle);
            }
            y += 60.f + extra;
        }
        if (!list.empty()) Divider(x, y, w), y += 16.f;
        // name + save
        bool exists = FindPreset(Trim(presetName)) != std::wstring::npos;
        std::wstring sl = exists ? L"Replace" : L"Save";
        float sw = std::max(BtnW(L"Replace"), 96.f);
        TextField(ID_NAME, {x, y, x + w - sw - 10.f, y + 38.f}, &presetName, L"Name the current curve", [this] { SavePreset(); });
        Button(ID_SAVE, {x + w - sw, y, x + w, y + 38.f}, sl, [this] { SavePreset(); }, BtnPrimary, CanSave());
        y += 46.f;
        const wchar_t* hint = exists ? L"A preset with this name exists and will be replaced."
                              : list.size() >= kMaxPresets ? L"Preset limit reached (32). Delete one to save another."
                                                           : L"Saving with an existing name replaces that preset.";
        Text(hint, {x, y, x + w, y + 18.f}, 12.5f, exists || list.size() >= kMaxPresets ? pal.warn : pal.text3);
        return y + 18.f;
    }

    float PageEq(const Col& c, float y) {
        if (!snap.hasEq) {
            return Card(c.x, y, c.w, [this](float x, float yy, float w) {
                yy = CardHeader(x, yy, w, L"Equalizer");
                Text(L"This model has no adjustable equalizer.", {x, yy - 8.f, x + w, yy + 16.f}, 13.5f, pal.text2);
                return yy + 16.f;
            });
        }
        inert = !snap.eqAvailable; // shown but not clickable or focusable while the device refuses EQ changes
        y = Card(c.x, y, c.w, B(&Impl::EqBody)) + GAP;
        inert = false;
        if (snap.eqBandCount > 0) y = Card(c.x, y, c.w, B(&Impl::PresetsBody)) + GAP;
        return y - GAP;
    }

    // ---- Controls ---------------------------------------------------------------------------------
    static bool Usable(const Choice& c) { return c.has && !c.options.empty(); }

    bool RoomRelevant() const { // room size only matters in background-music mode (when the mode list names one)
        const Snapshot& s = snap;
        if (!Usable(s.bgmRoom)) return false;
        if (!Usable(s.listeningMode)) return true;
        bool any = false, cur = false;
        for (auto& o : s.listeningMode.options) {
            std::wstring l = o.label;
            for (auto& ch : l) ch = static_cast<wchar_t>(towlower(ch));
            bool bgm = l.find(L"background") != std::wstring::npos || l.find(L"bgm") != std::wstring::npos;
            any = any || bgm;
            cur = cur || (bgm && o.value == s.listeningMode.value);
        }
        return !any || cur;
    }

    bool CtlButtons() const { return !snap.assignable.empty() || Usable(snap.ncButton); }
    bool CtlVoice() const {
        const Snapshot& s = snap;
        return s.voiceGuidance.has || (s.hasVoiceGuidanceVolume && s.voiceGuidanceRange.max > s.voiceGuidanceRange.min) ||
               Usable(s.voiceAssistant) || s.headGesture.has;
    }
    bool CtlListening() const { return Usable(snap.listeningMode) || RoomRelevant(); }
    bool CtlConnection() const { return snap.sourceSwitch.has || snap.pairingMode.has || !snap.general.empty(); }
    bool CtlWearing() const { return snap.wearingPowerOff.has || snap.wearing != 0; }
    bool CtlSafe() const { return snap.hasSafeListening && snap.soundPressure > 0; }
    bool HasControls() const {
        return CtlButtons() || CtlVoice() || CtlListening() || CtlConnection() || CtlWearing() || CtlSafe();
    }

    float ButtonsBody(float x, float y, float w) {
        const Snapshot& s = snap;
        y = CardHeader(x, y, w, L"Buttons & touch", L"Choose what the controls on the headphones do.");
        y -= 8.f;
        for (size_t k = 0; k < s.assignable.size() && k < kMaxAssign; ++k) {
            const AssignKey& key = s.assignable[k];
            if (k) Divider(x, y, w);
            int pos = static_cast<int>(k);
            y = ChoiceRow(ID_ASSIGN + pos * 50, x, y, w, key.label.empty() ? L"Button " + std::to_wstring(k + 1) : key.label, {},
                          key.options, key.action, [this, pos](int v) { Feat(Feature::Assignable, v, pos); },
                          KeyOf(key.label, pos)); // label + position: release must address the key that was pressed
        }
        if (Usable(s.ncButton)) {
            if (!s.assignable.empty()) Divider(x, y, w);
            y = ChoiceRow(ID_NCBTN, x, y, w, L"Noise control cycle", L"Modes the NC/AMB button steps through", s.ncButton.options,
                          s.ncButton.value, [this](int v) { Feat(Feature::NcButton, v); });
        }
        return y - 8.f;
    }

    float VoiceBody(float x, float y, float w) {
        const Snapshot& s = snap;
        y = CardHeader(x, y, w, L"Voice");
        y -= 8.f;
        bool first = true;
        auto sep = [&] {
            if (!first) Divider(x, y, w);
            first = false;
        };
        if (s.voiceGuidance.has) {
            sep();
            y = ToggleRow(ID_VG, x, y, w, L"Voice guidance", L"Spoken announcements for power, battery and modes",
                          s.voiceGuidance.on, [this](bool on) { Feat(Feature::VoiceGuidance, on ? 1 : 0); });
        }
        const Range& vr = s.voiceGuidanceRange;
        if (s.hasVoiceGuidanceVolume && vr.max > vr.min && (s.voiceGuidance.on || !s.voiceGuidance.has)) {
            if (!s.voiceGuidance.has) sep();
            y += s.voiceGuidance.has ? 2.f : 14.f;
            y = SliderRow(ID_VGVOL, x, y, w, L"Guidance volume", vr, s.voiceGuidanceVolume,
                          [this](int v) { Feat(Feature::VoiceGuidanceVolume, v); }, vr.min < 0, GSpeaker);
            y += 4.f;
        }
        if (Usable(s.voiceAssistant)) {
            sep();
            y = ChoiceRow(ID_VA, x, y, w, L"Voice assistant", L"Started from the headphones", s.voiceAssistant.options,
                          s.voiceAssistant.value, [this](int v) { Feat(Feature::VoiceAssistant, v); });
        }
        if (s.headGesture.has) {
            sep();
            y = ToggleRow(ID_HG, x, y, w, L"Head gestures", L"Nod or shake your head to answer or decline calls",
                          s.headGesture.on, [this](bool on) { Feat(Feature::HeadGesture, on ? 1 : 0); });
        }
        return y - 8.f;
    }

    float ListeningBody(float x, float y, float w) {
        const Snapshot& s = snap;
        y = CardHeader(x, y, w, L"Listening");
        y -= 8.f;
        if (Usable(s.listeningMode))
            y = ChoiceRow(ID_LMODE, x, y, w, L"Listening mode", L"How the headphones present the sound",
                          s.listeningMode.options, s.listeningMode.value, [this](int v) { Feat(Feature::ListeningMode, v); });
        if (RoomRelevant()) {
            if (Usable(s.listeningMode)) Divider(x, y, w);
            y = ChoiceRow(ID_ROOM, x, y, w, L"Room size", L"The space background music sounds like it is playing in",
                          s.bgmRoom.options, s.bgmRoom.value, [this](int v) { Feat(Feature::BgmRoom, v); });
        }
        return y - 8.f;
    }

    static int GeneralValue(const GeneralItem& g) { // current option value; tolerates `value` being an index
        for (auto& o : g.options)
            if (o.value == g.value) return g.value;
        if (g.value >= 0 && size_t(g.value) < g.options.size()) return g.options[size_t(g.value)].value;
        return g.value;
    }

    float ConnectionBody(float x, float y, float w) {
        const Snapshot& s = snap;
        y = CardHeader(x, y, w, L"Connection");
        y -= 8.f;
        bool first = true;
        auto sep = [&] {
            if (!first) Divider(x, y, w);
            first = false;
        };
        if (s.sourceSwitch.has) {
            sep();
            y = ToggleRow(ID_SRC, x, y, w, L"Automatic source switching", L"Switch to the device that starts playing",
                          s.sourceSwitch.on, [this](bool on) { Feat(Feature::SourceSwitch, on ? 1 : 0); });
        }
        if (s.pairingMode.has) {
            sep();
            y = ToggleRow(ID_PAIR, x, y, w, L"Bluetooth pairing mode", L"Makes the headphones discoverable so a new device can pair",
                          s.pairingMode.on, [this](bool on) { Feat(Feature::PairingMode, on ? 1 : 0); });
        }
        for (size_t k = 0; k < s.general.size() && k < kMaxGeneral; ++k) {
            const GeneralItem& g = s.general[k];
            int id = ID_GEN + static_cast<int>(k) * 50, idx = g.index;
            sep();
            size_t rowKey = KeyOf(L"general", idx); // release must hit the same item.index that was pressed
            if (g.isBool) {
                size_t n0 = items.size();
                y = ToggleRow(id, x, y, w, g.label, {}, g.on, [this, idx](bool on) { Feat(Feature::General, on ? 1 : 0, idx); },
                              g.writable);
                for (size_t i = n0; i < items.size(); ++i) items[i].key = rowKey;
            } else if (g.writable) {
                y = ChoiceRow(id, x, y, w, g.label, {}, g.options, GeneralValue(g),
                              [this, idx](int v) { Feat(Feature::General, v, idx); }, rowKey);
            } else {
                std::wstring cur;
                int v = GeneralValue(g);
                for (auto& o : g.options)
                    if (o.value == v) cur = o.label;
                y = ValueRow(x, y, w, g.label, cur);
            }
        }
        return y - 8.f;
    }

    static const wchar_t* WearingLabel(int v) {
        switch (v) {
        case 1: return L"Wearing";
        case 2: return L"Left side removed";
        case 3: return L"Right side removed";
        case 4: return L"Not being worn";
        default: return L"";
        }
    }

    float WearingBody(float x, float y, float w) {
        const Snapshot& s = snap;
        y = CardHeader(x, y, w, L"Wearing & power");
        if (s.wearing != 0) {
            Text(L"Right now", {x, y, x + 80.f, y + 26.f}, 13.f, pal.text2);
            Chip(x + 80.f, y, WearingLabel(s.wearing), s.wearing == 1 ? pal.good : pal.text3);
            y += 26.f + 10.f;
        } else {
            y -= 8.f;
        }
        bool first = s.wearing == 0;
        auto sep = [&] {
            if (!first) Divider(x, y, w);
            first = false;
        };
        if (s.wearingPowerOff.has) {
            sep();
            y = ToggleRow(ID_WPO, x, y, w, L"Power off when removed", L"Turns the headphones off after you take them off",
                          s.wearingPowerOff.on, [this](bool on) { Feat(Feature::WearingPowerOff, on ? 1 : 0); });
        }
        if (s.hasAutoPause) {
            sep();
            y = ToggleRow(ID_PAUSE2, x, y, w, L"Pause when headphones are removed", L"Resumes when you put them back on",
                          s.autoPause, [this](bool on) { Cmd(Command::SetAutoPause, on ? 1 : 0); });
        }
        return y - 8.f;
    }

    float SafeBody(float x, float y, float w) {
        y = CardHeader(x, y, w, L"Safe listening");
        std::wstring v = std::to_wstring(snap.soundPressure);
        float vw = std::ceil(Measure(v, 32.f, Semi).width);
        Text(v, {x, y - 6.f, x + vw + 4.f, y + 38.f}, 32.f, pal.text, Semi);
        Text(L"dB", {x + vw + 6.f, y + 8.f, x + w, y + 34.f}, 15.f, pal.text2, Semi);
        y += 44.f;
        const std::wstring note = L"Current sound level at your ears, as estimated by the headphones.";
        float nh = TextH(note, 13.f, w);
        Text(note, {x, y, x + w, y + nh + 2.f}, 13.f, pal.text3, Reg, AlLeft, true);
        return y + nh;
    }

    float PageControls(const Col& c, float y) {
        bool two = c.w >= 640.f;
        float cw = two ? (c.w - GAP) / 2.f : c.w;
        float col[2] = {y, y}, xs[2] = {c.x, c.x + cw + GAP};
        auto place = [&](const Body& b) { // next card goes into the shorter column
            int k = two && col[1] < col[0] ? 1 : 0;
            col[k] = Card(xs[k], col[k], cw, b) + GAP;
        };
        if (CtlButtons()) place(B(&Impl::ButtonsBody));
        if (CtlVoice()) place(B(&Impl::VoiceBody));
        if (CtlListening()) place(B(&Impl::ListeningBody));
        if (CtlConnection()) place(B(&Impl::ConnectionBody));
        if (CtlWearing()) place(B(&Impl::WearingBody));
        if (CtlSafe()) place(B(&Impl::SafeBody));
        return std::max(col[0], col[1]) - GAP;
    }

    void NoControlsView(const Col& c) {
        const std::wstring title = L"This model has no extra controls";
        const std::wstring sub = L"Everything it supports is on the Home and Equalizer pages.";
        float tw = std::min(440.f, c.w), sh = TextH(sub, 14.f, tw), total = 120.f + 24.f + 38.f + sh;
        float cx = c.x + c.w / 2.f, y = TOPH + std::max(16.f, (H - TOPH - total) / 2.f - 24.f), gy = y + 60.f;
        Circle(cx, gy, 60.f, pal.card);
        Ring(cx, gy, 60.f, pal.border, 1.f);
        Icon(GControls, cx, gy, 52.f, pal.text3);
        y += 120.f + 24.f;
        Text(title, {c.x, y, c.x + c.w, y + 30.f}, 21.f, pal.text, Semi, AlCenter);
        y += 38.f;
        Text(sub, {cx - tw / 2.f, y, cx + tw / 2.f, y + sh + 2.f}, 14.f, pal.text2, Reg, AlCenter, true);
    }

    // ---- Dolby Atmos ------------------------------------------------------------------------------
    float DolbyStatusBody(float x, float y, float w) {
        const DolbyState& d = dolby;
        D2D1_RECT_F g{x, y, x + 56.f, y + 56.f};
        Fill(g, pal.accentSoft, 14.f);
        Icon(GSpatial, x + 28.f, y + 28.f, 32.f, pal.accentText);
        bool tog = d.canToggle && d.statusKnown;
        float tr = tog ? x + w - 56.f : x + w;
        std::wstring state = !d.statusKnown ? L"Unknown" : d.atmosOn ? L"On" : L"Off";
        Text(L"Dolby Atmos for Headphones", {x + 72.f, y + 2.f, tr, y + 28.f}, 17.f, pal.text, Semi);
        float cw = Chip(x + 72.f, y + 32.f, state, !d.statusKnown ? pal.text3 : d.atmosOn ? pal.good : pal.text3);
        if (d.statusKnown && d.spatialOn && !d.atmosOn && !d.formatName.empty())
            Text(L"Active format: " + d.formatName, {x + 72.f + cw + 10.f, y + 32.f, tr, y + 58.f}, 12.5f, pal.text2);
        else if (!d.statusKnown)
            Text(L"Windows did not report the spatial sound state.", {x + 72.f + cw + 10.f, y + 32.f, tr, y + 58.f}, 12.5f, pal.text3);
        if (tog) {
            bool on = d.atmosOn;
            D2D1_RECT_F sw{x + w - 44.f, y + 18.f, x + w, y + 40.f};
            D2D1_RECT_F hit = Inflate(sw, 8.f);
            if (Hot(ID_DOLBY_TOG)) Fill(hit, StateBg(ID_DOLBY_TOG, pal.card), 8.f);
            Switch(sw, on, true, Hot(ID_DOLBY_TOG));
            Add(ID_DOLBY_TOG, hit).click = [this, on] {
                if (dolbyActing) return;
                dolbyActing = true;
                host.SetDolbyEnabled(!on);
                dolby = host.DolbyStatus();
                dolbyActing = false;
            };
            FocusRing(ID_DOLBY_TOG, hit, 8.f);
        }
        y += 56.f;
        if (!d.endpointName.empty()) {
            Text(L"Output: " + d.endpointName, {x, y + 8.f, x + w, y + 28.f}, 12.5f, pal.text3);
            y += 28.f;
        }
        y += 20.f;
        Divider(x, y, w);
        y += 18.f;
        if (!d.canToggle && !d.canSetMode) {
            const wchar_t* p = L"Spatial sound and sound modes are managed by Windows and the Dolby Access app. "
                               L"Open the app to switch Dolby Atmos for Headphones on and choose a sound mode.";
            float h = TextH(p, 13.5f, w);
            Text(p, {x, y, x + w, y + h + 2.f}, 13.5f, pal.text2, Reg, AlLeft, true);
            y += h + 16.f;
        }
        if (!d.note.empty()) {
            float h = TextH(d.note, 13.f, w - 28.f);
            Icon(GInfo, x + 9.f, y + 9.f, 17.f, pal.text3);
            Text(d.note, {x + 28.f, y, x + w, y + h + 2.f}, 13.f, pal.text3, Reg, AlLeft, true);
            y += std::max(h, 18.f) + 16.f;
        }
        std::wstring ol = d.appInstalled ? L"Open Dolby Access" : L"Get Dolby Access";
        float ow = BtnW(ol);
        Button(ID_DOLBY_OPEN, {x, y, x + ow, y + 38.f}, ol, [this] { host.OpenDolbyAccess(); },
               (!d.canToggle && !d.canSetMode) || !d.appInstalled ? BtnPrimary : BtnNormal);
        y += 38.f;
        if (d.appInstalled) {
            y += 20.f;
            Divider(x, y, w);
            y += 16.f;
            Text(L"Experimental", {x, y, x + w, y + 20.f}, 12.5f, pal.text2, Semi);
            y += 24.f;
            y = CheckRow(ID_DOLBY_AUTO, x, y, w, L"Switch sound modes by driving the Dolby Access window",
                         host.Config().dolbyAutomation, [this](bool on) {
                             host.Config().dolbyAutomation = on;
                             host.ConfigChanged();
                         });
            const std::wstring hint =
                L"Experimental. Briefly opens Dolby Access (or uses its open window), moves it out of view, selects the mode "
                L"there and puts it back. Works only while these headphones are the Windows output with Dolby Atmos on. "
                L"May break when Dolby updates its app.";
            float hh = TextH(hint, 12.5f, w - 34.f);
            Text(hint, {x + 34.f, y - 4.f, x + w, y - 2.f + hh}, 12.5f, pal.text3, Reg, AlLeft, true);
            y += hh;
        }
        return y;
    }

    static const wchar_t* ModeHint(const std::wstring& n) {
        if (n == L"Dynamic") return L"Adapts to what is playing";
        if (n == L"Movie") return L"Wide, cinematic staging";
        if (n == L"Music") return L"Balanced and detailed";
        if (n == L"Game") return L"Precise positional cues";
        if (n == L"Voice") return L"Clear speech and dialogue";
        if (n == L"Personalize") return L"Your own tuning";
        return L"";
    }

    float DolbyModeBody(float x, float y, float w) {
        const DolbyState& d = dolby;
        float top = y;
        y = CardHeader(x, y, w, L"Sound mode", L"Pick the tuning that suits what you're listening to.", d.modeBusy ? 120.f : 0.f);
        if (d.modeBusy) {
            float sx = x + w - 96.f, a0 = F(static_cast<int>(GetTickCount64() % 900)) / 900.f * 360.f;
            Arc(sx - 14.f, top + 12.f, 7.f, a0, a0 + 100.f, pal.accent, 2.5f);
            Text(L"Switching…", {sx, top, x + w, top + 24.f}, 13.f, pal.text2, Reg, AlRight);
        }
        int cols = w >= 560.f ? 3 : 2;
        float tw = (w - 12.f * F(cols - 1)) / F(cols), th = 74.f;
        for (size_t i = 0; i < d.modes.size(); ++i) {
            int id = ID_DMODE + static_cast<int>(i), cidx = static_cast<int>(i) % cols, ridx = static_cast<int>(i) / cols;
            D2D1_RECT_F r{x + (tw + 12.f) * F(cidx), y + (th + 12.f) * F(ridx), x + (tw + 12.f) * F(cidx) + tw,
                          y + (th + 12.f) * F(ridx) + th};
            bool sel = d.modes[i].value == d.mode;
            Fill(r, sel ? pal.accentSoft : StateBg(id, pal.btn), 10.f);
            Stroke(r, sel ? pal.accent : pal.border, 10.f, sel ? 2.f : 1.f);
            Text(d.modes[i].label, {r.left + 16.f, r.top + 14.f, r.right - 40.f, r.top + 38.f}, 15.f, pal.text, Semi);
            Text(ModeHint(d.modes[i].label), {r.left + 16.f, r.top + 38.f, r.right - 12.f, r.top + 58.f}, 12.5f, pal.text3);
            if (sel) {
                Circle(r.right - 22.f, r.top + 22.f, 10.f, pal.accent);
                Icon(GCheck, r.right - 22.f, r.top + 22.f, 13.f, pal.onAccent);
            }
            int v = d.modes[i].value;
            if (d.modeBusy) continue; // tiles are inert while a switch is running
            Add(id, r).click = [this, v] {
                if (dolby.modeBusy) return;
                host.SetDolbyMode(v);
                dolby = host.DolbyStatus();
            };
            FocusRing(id, r, 10.f);
        }
        size_t rows = (d.modes.size() + size_t(cols) - 1) / size_t(cols);
        y += (th + 12.f) * F(rows) + 8.f;
        if (!d.modeError.empty()) {
            float eh = TextH(d.modeError, 13.f, w - 28.f);
            Icon(GWarn, x + 9.f, y + 9.f, 17.f, pal.bad);
            Text(d.modeError, {x + 28.f, y, x + w, y + eh + 2.f}, 13.f, pal.bad, Reg, AlLeft, true);
            y += std::max(eh, 18.f) + 12.f;
        }
        y = CheckRow(ID_DOLBY_RESET, x, y, w, L"Reset the headphone equalizer when a mode is picked",
                     host.Config().resetEqOnDolbyMode, [this](bool on) {
                         host.Config().resetEqOnDolbyMode = on;
                         host.ConfigChanged();
                     });
        return y - 6.f;
    }

    float PageDolby(const Col& c, float y) {
        y = Card(c.x, y, c.w, B(&Impl::DolbyStatusBody));
        // keep the card while a switch runs or failed, so the spinner / inert tiles / error stay visible
        if ((dolby.canSetMode && !dolby.modes.empty()) || dolby.modeBusy || !dolby.modeError.empty()) y = Card(c.x, y + GAP, c.w, B(&Impl::DolbyModeBody));
        return y;
    }

    // ---- Settings ---------------------------------------------------------------------------------
    std::function<void(bool)> Bind(bool Settings::*field) {
        return [this, field](bool v) {
            host.Config().*field = v;
            host.ConfigChanged();
        };
    }

    float GeneralBody(float x, float y, float w) {
        Settings& c = host.Config();
        y = CardHeader(x, y, w, L"General");
        y -= 8.f;
        y = ToggleRow(ID_SET + 0, x, y, w, L"Start with Windows", L"Launch XMTray in the tray when you sign in", c.autostart,
                      Bind(&Settings::autostart));
        Divider(x, y, w);
        y = ToggleRow(ID_SET + 1, x, y, w, L"Connect and disconnect notifications",
                      L"Show a notification when headphones connect or disconnect", c.notifyConnect, Bind(&Settings::notifyConnect));
        Divider(x, y, w);
        y = ToggleRow(ID_SET + 2, x, y, w, L"Low battery notification", L"Warn when the battery drops to the threshold",
                      c.notifyLowBattery, Bind(&Settings::notifyLowBattery));
        if (c.notifyLowBattery) {
            Text(L"Warn at", {x, y, x + w - 140.f, y + 44.f}, 14.f, pal.text2);
            auto step = [this](int d) {
                int& p = host.Config().lowBatteryPercent;
                p = std::clamp(p + d, 5, 50);
                host.ConfigChanged();
            };
            float r = x + w;
            Button(ID_STEP_UP, {r - 36.f, y + 4.f, r, y + 40.f}, L"+", [step] { step(5); }, BtnNormal, c.lowBatteryPercent < 50);
            Text(std::to_wstring(c.lowBatteryPercent) + L"%", {r - 96.f, y, r - 40.f, y + 44.f}, 14.f, pal.text, Semi, AlCenter);
            Button(ID_STEP_DN, {r - 136.f, y + 4.f, r - 100.f, y + 40.f}, L"−", [step] { step(-5); }, BtnNormal,
                   c.lowBatteryPercent > 5);
            y += 52.f;
        }
        return y - 8.f;
    }

    float ConnBody(float x, float y, float w) {
        Settings& c = host.Config();
        y = CardHeader(x, y, w, L"Audio and connection");
        y -= 8.f;
        y = ToggleRow(ID_SET + 3, x, y, w, L"Route Windows audio when selecting headphones",
                      L"Make the selected headphones the Windows output device", c.routeAudioOnSelect,
                      Bind(&Settings::routeAudioOnSelect));
        Divider(x, y, w);
        y = ToggleRow(ID_SET + 4, x, y, w, L"Follow the Windows output device",
                      L"Control whichever headphones Windows is playing to", c.followWindowsOutput,
                      Bind(&Settings::followWindowsOutput));
        Divider(x, y, w);
        y = ToggleRow(ID_SET + 5, x, y, w, L"Disconnect the control link when the window is hidden",
                      L"Saves headphone battery; settings refresh when you open XMTray", c.disconnectWhenHidden,
                      Bind(&Settings::disconnectWhenHidden));
        return y - 8.f;
    }

    float PageSettings(const Col& c, float y) {
        y = Card(c.x, y, c.w, B(&Impl::GeneralBody)) + GAP;
        return Card(c.x, y, c.w, B(&Impl::ConnBody));
    }

    // ---- About ------------------------------------------------------------------------------------
    float AboutBody(float x, float y, float w) {
        D2D1_RECT_F logo{x, y, x + 64.f, y + 64.f};
        Fill(logo, pal.accent, 16.f);
        Icon(GHeadphones, x + 32.f, y + 32.f, 40.f, pal.onAccent);
        Text(L"XMTray", {x + 80.f, y + 4.f, x + w, y + 38.f}, 24.f, pal.text, Semi);
        Text(std::wstring(L"Version ") + kVersion, {x + 80.f, y + 38.f, x + w, y + 60.f}, 13.5f, pal.text2);
        y += 64.f + 20.f;
        const std::wstring lines[] = {L"A tray controller for Sony headphones on Windows.",
                                      L"Unofficial. Not affiliated with or endorsed by Sony or Dolby.",
                                      L"Protocol library: libmdr © mos9527 and contributors (MIT)"};
        for (const auto& l : lines) {
            float h = TextH(l, 13.5f, w);
            Text(l, {x, y, x + w, y + h + 2.f}, 13.5f, &l == &lines[0] ? pal.text : pal.text2, Reg, AlLeft, true);
            y += h + 6.f;
        }
        y += 14.f;
        float lw = BtnW(L"Open log folder"), qw = BtnW(L"Quit XMTray");
        Button(ID_LOG, {x, y, x + lw, y + 38.f}, L"Open log folder", [this] { host.OpenLogFolder(); });
        Button(ID_QUIT, {x + lw + 10.f, y, x + lw + 10.f + qw, y + 38.f}, L"Quit XMTray",
               [this] { PostMessageW(hwnd, WM_XM_QUIT, 0, 0); }, BtnDangerOutline);
        return y + 38.f;
    }

    float AboutDeviceBody(float x, float y, float w) {
        y = CardHeader(x, y, w, L"Connected headphones");
        y -= 6.f;
        if (snap.link != Link::Ready) {
            Text(L"No headphones connected.", {x, y, x + w, y + 30.f}, 13.5f, pal.text3);
            return y + 30.f;
        }
        y = KV(x, y, w, L"Model", DeviceTitle(), pal.text);
        Divider(x, y, w);
        y = KV(x, y, w, L"Firmware", snap.firmware.empty() ? L"Unknown" : snap.firmware, pal.text);
        Divider(x, y, w);
        y = KV(x, y, w, L"Protocol version", snap.protocol > 0 ? L"v" + std::to_wstring(snap.protocol) : L"Unknown", pal.text);
        Divider(x, y, w);
        y = KV(x, y, w, L"Bluetooth address", Widen(snap.mac), pal.text);
        return y - 6.f;
    }

    float PageAbout(const Col& c, float y) {
        y = Card(c.x, y, c.w, B(&Impl::AboutBody)) + GAP;
        return Card(c.x, y, c.w, B(&Impl::AboutDeviceBody));
    }

    // ---- paint -----------------------------------------------------------------------------
    Col Column() const {
        float avail = std::max(200.f, W - SB - 2.f * GUT), w = std::min(avail, MAXW);
        return {SB + GUT + (avail - w) / 2.f, w};
    }

    bool EnsureTarget() {
        if (rt) return true;
        if (!d2d) return false;
        RECT rc;
        GetClientRect(hwnd, &rc);
        if (FAILED(d2d->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(),
                D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(static_cast<UINT32>(rc.right), static_cast<UINT32>(rc.bottom))),
                &rt)))
            return false;
        if (FAILED(rt->CreateSolidColorBrush(pal.text, &br))) {
            rt.Reset();
            return false;
        }
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        return true;
    }

    void Paint() {
        if (!EnsureTarget()) return;
        tgt = rt.Get();
        UINT d = GetDpiForWindow(hwnd);
        if (d) dpi = d;
        rt->SetDpi(F(static_cast<int>(dpi)), F(static_cast<int>(dpi)));
        RECT rc;
        GetClientRect(hwnd, &rc);
        W = F(static_cast<int>(rc.right)) * 96.f / F(static_cast<int>(dpi));
        H = F(static_cast<int>(rc.bottom)) * 96.f / F(static_cast<int>(dpi));
        items.clear();
        modalStart = 0;
        if (snap.pendingAlert) ddOpen = false, pickId = 0;
        pickSeen = false;

        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(pal.bg);
        Col c = Column();
        Sidebar();
        TopBar(c);

        float& sc = scroll[page];
        clipTop = TOPH;
        clipBot = H;
        inContent = true;
        rt->PushAxisAlignedClip({SB, TOPH, W, H}, D2D1_ANTIALIAS_MODE_ALIASED);
        bool stateView = page <= 2 && snap.link != Link::Ready;
        bool noCtl = !stateView && page == 2 && !HasControls();
        float y0 = TOPH + 6.f - sc, yEnd = 0.f;
        if (stateView) StateView(c);
        else if (noCtl) NoControlsView(c);
        else if (page == 0) yEnd = PageHome(c, y0);
        else if (page == 1) yEnd = PageEq(c, y0);
        else if (page == 2) yEnd = PageControls(c, y0);
        else if (page == 3) yEnd = PageDolby(c, y0);
        else if (page == 4) yEnd = PageSettings(c, y0);
        else yEnd = PageAbout(c, y0);
        rt->PopAxisAlignedClip();
        inContent = false;
        if (pickId && !pickSeen) pickId = 0; // its field is gone (page changed, option list shrank, ...)

        float viewH = H - TOPH, contentH = stateView || noCtl ? 0.f : yEnd + sc - TOPH + 28.f;
        maxScroll = std::max(0.f, contentH - viewH);
        bool again = false;
        if (sc > maxScroll) sc = maxScroll, again = true;
        if (maxScroll > 0.f) {
            float bh = std::max(32.f, viewH * viewH / contentH), by = TOPH + (viewH - bh) * sc / maxScroll;
            Fill({W - 7.f, by + 3.f, W - 3.f, by + bh - 3.f}, Alpha(pal.text3, 0.55f), 2.f);
        }
        if (ddOpen) Dropdown();
        else if (pickId) PickerPopup();
        if (snap.pendingAlert) Alert();

        bool anim = shown && !IsIconic(hwnd) &&
                    ((stateView && (snap.link == Link::Connecting || snap.link == Link::Initializing)) ||
                     (!stateView && page == 3 && dolby.modeBusy));
        if (anim != animOn) {
            animOn = anim;
            if (anim) SetTimer(hwnd, TIMER_ANIM, 33, nullptr);
            else KillTimer(hwnd, TIMER_ANIM);
        }

        if (rt->EndDraw() == D2DERR_RECREATE_TARGET) {
            br.Reset();
            rt.Reset();
            tgt = nullptr;
            again = true;
        }
        if (again) Invalidate();
    }

    // ---- input ----------------------------------------------------------------------------
    D2D1_POINT_2F Dip(LPARAM l) const {
        float s = 96.f / F(static_cast<int>(dpi));
        return {F(GET_X_LPARAM(l)) * s, F(GET_Y_LPARAM(l)) * s};
    }
    Item* Hit(D2D1_POINT_2F p) {
        for (size_t i = items.size(); i-- > modalStart;) {
            const D2D1_RECT_F& r = items[i].hit;
            if (r.top < r.bottom && p.x >= r.left && p.x < r.right && p.y >= r.top && p.y < r.bottom) return &items[i];
        }
        return nullptr;
    }
    Item* Find(int id) {
        if (!id) return nullptr;
        for (size_t i = modalStart; i < items.size(); ++i)
            if (items[i].id == id) return &items[i];
        return nullptr;
    }
    void Activate(const Item& it) {
        auto fn = it.click; // copy: the item list is rebuilt on the next paint
        if (fn) fn();
        Redraw();
    }
    void MaybeSend() {
        ULONGLONG now = GetTickCount64();
        if (dragVal != lastSent && now - lastSend >= kSendMs) {
            drag.change(dragVal, false);
            lastSent = dragVal;
            lastSend = now;
        }
    }
    void EndDrag() {
        if (!dragId) return;
        KillTimer(hwnd, TIMER_DRAG);
        int id = dragId, v = dragVal;
        dragId = 0;
        heldId = id;
        heldVal = v;
        heldUntil = GetTickCount64() + 600; // bridge until the worker's snapshot reflects the value
        auto fn = drag.change;
        fn(v, true);
        Invalidate();
    }
    void ScrollBy(float d) {
        float& sc = scroll[page];
        float n = std::clamp(sc + d, 0.f, maxScroll);
        if (n != sc) sc = n, Redraw();
    }
    void ScrollIntoView(const Item& it) {
        if (!it.content) return;
        float& sc = scroll[page];
        if (it.rc.top < clipTop + 8.f) sc -= clipTop + 8.f - it.rc.top + 8.f;
        else if (it.rc.bottom > clipBot - 8.f) sc += it.rc.bottom - clipBot + 16.f;
        sc = std::clamp(sc, 0.f, maxScroll);
    }
    void MoveFocus(bool back) {
        std::vector<size_t> idx;
        for (size_t i = modalStart; i < items.size(); ++i)
            if (items[i].focus) idx.push_back(i);
        if (idx.empty()) return;
        size_t n = idx.size(), cur = n;
        for (size_t k = 0; k < n; ++k)
            if (items[idx[k]].id == focusId) cur = k;
        size_t next = cur == n ? (back ? n - 1 : 0) : (back ? (cur + n - 1) % n : (cur + 1) % n);
        focusId = items[idx[next]].id;
        focusVisible = true;
        ScrollIntoView(items[idx[next]]);
        Redraw();
    }
    void Key(WPARAM k, LPARAM l) {
        bool ctrl = GetKeyState(VK_CONTROL) < 0, repeat = (l & 0x40000000) != 0;
        if (ctrl && k >= '1' && k < '1' + kPageCount) return SetPage(static_cast<int>(k - '1'));
        Item* f = Find(focusId);
        if (k == VK_ESCAPE) {
            if (pickId) focusId = pickId, pickId = 0;
            else if (ddOpen) ddOpen = false;
            else if (f && f->text && renameIdx >= 0 && f->id == ID_RENAME) renameIdx = -1, renameErr.clear(), focusId = 0;
            else if (f && f->text) focusId = 0;
            else if (deleteIdx >= 0) deleteIdx = -1;
            else if (confirmPower) confirmPower = false;
            Redraw();
            return;
        }
        if (repeat && (k == VK_SPACE || k == VK_RETURN)) return; // no auto-repeat for activation keys
        if (k == VK_TAB) return MoveFocus(GetKeyState(VK_SHIFT) < 0);
        if (f && f->text) {
            if (k == VK_RETURN) Activate(*f);
            return; // characters arrive via WM_CHAR
        }
        if (k == VK_NEXT || k == VK_PRIOR) return ScrollBy((k == VK_NEXT ? 1.f : -1.f) * (H - TOPH - 60.f));
        if (!f) {
            if (k == VK_DOWN || k == VK_UP) ScrollBy(k == VK_DOWN ? 48.f : -48.f);
            return;
        }
        if ((k == VK_SPACE || k == VK_RETURN) && f->click) {
            focusVisible = true;
            return Activate(*f);
        }
        if (f->slider && (k == VK_LEFT || k == VK_RIGHT || k == VK_UP || k == VK_DOWN)) {
            int d = (k == VK_RIGHT || k == VK_UP) ? f->step : -f->step;
            int v = std::clamp(f->val + d, f->mn, f->mx);
            if (v == f->val) return;
            heldId = f->id;
            heldVal = v;
            heldUntil = GetTickCount64() + 800;
            focusVisible = true;
            auto fn = f->change;
            fn(v, true);
            Redraw();
        }
    }
    void Char(wchar_t ch) {
        Item* f = Find(focusId);
        if (!f || !f->text) {
            pendingHigh = 0;
            return;
        }
        std::wstring& b = *f->text;
        if (ch == L'\b') {
            pendingHigh = 0;
            size_t n = b.size();
            if (n >= 2 && IS_LOW_SURROGATE(b[n - 1]) && IS_HIGH_SURROGATE(b[n - 2])) b.resize(n - 2);
            else if (n) b.pop_back();
        } else if (IS_HIGH_SURROGATE(ch)) {
            pendingHigh = ch; // wait for the low half
            return;
        } else if (IS_LOW_SURROGATE(ch)) {
            wchar_t hi = pendingHigh;
            pendingHigh = 0;
            if (!hi || b.size() + 2 > kMaxName) return; // unpaired, or the pair would not fit
            b.push_back(hi);
            b.push_back(ch);
        } else {
            pendingHigh = 0; // a high surrogate not followed by a low one is dropped
            if (!(ch >= 32 && ch != 0x7F && ch != L'"' && b.size() < kMaxName)) return;
            b.push_back(ch);
        }
        renameErr.clear();
        Invalidate();
    }

    void HideInternal() {
        EndDrag();
        if (GetCapture() == hwnd) ReleaseCapture();
        shown = false;
        if (animOn) KillTimer(hwnd, TIMER_ANIM);
        animOn = false;
        ShowWindow(hwnd, SW_HIDE);
        ddOpen = confirmPower = false;
        pickId = 0;
        renameIdx = deleteIdx = -1;
        renameErr.clear();
        hoverId = pressId = 0;
        focusVisible = false;
    }

    void Show(const RECT& a) {
        if (!hwnd) return;
        Reload();
        if (!placed) {
            POINT c{};
            if (a.right > a.left && a.bottom > a.top) c = {(a.left + a.right) / 2, (a.top + a.bottom) / 2};
            else GetCursorPos(&c);
            HMONITOR mon = MonitorFromPoint(c, MONITOR_DEFAULTTONEAREST);
            MONITORINFO mi{sizeof mi};
            GetMonitorInfoW(mon, &mi);
            UINT dx = 96, dy = 96;
            if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
            RECT r{0, 0, MulDiv(kDefW, static_cast<int>(dx), 96), MulDiv(kDefH, static_cast<int>(dx), 96)};
            AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, dx);
            const RECT& wk = mi.rcWork;
            int w = std::min<int>(r.right - r.left, wk.right - wk.left), h = std::min<int>(r.bottom - r.top, wk.bottom - wk.top);
            int x = wk.left + (wk.right - wk.left - w) / 2, y = wk.top + (wk.bottom - wk.top - h) / 2;
            dpi = dx;
            positioning = true; // the size is already right for the target monitor; ignore WM_DPICHANGED's rect
            SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
            positioning = false;
            placed = true;
        }
        shown = true;
        ShowWindow(hwnd, IsIconic(hwnd) ? SW_RESTORE : SW_SHOW);
        SetForegroundWindow(hwnd);
        Invalidate();
        UpdateWindow(hwnd);
    }

    void Toggle(const RECT& a) {
        if (!shown) return Show(a);
        if (IsIconic(hwnd)) {
            ShowWindow(hwnd, SW_RESTORE);
            SetForegroundWindow(hwnd);
            return;
        }
        // A tray click deactivates us just before it arrives: treat "foreground a moment ago" as foreground.
        if (GetForegroundWindow() == hwnd || GetTickCount64() - deactivatedAt < 400) return HideInternal();
        SetForegroundWindow(hwnd);
    }

    LRESULT Handle(UINT m, WPARAM w, LPARAM l) {
        switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            Paint();
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_ERASEBKGND: return 1;
        case WM_CLOSE: // close button / Alt+F4: hide, never destroy
            HideInternal();
            return 0;
        case WM_SIZE:
            if (w == SIZE_MINIMIZED) {
                if (animOn) KillTimer(hwnd, TIMER_ANIM);
                animOn = false;
                return 0;
            }
            if (rt) rt->Resize(D2D1::SizeU(LOWORD(l), HIWORD(l)));
            if (w == SIZE_RESTORED && shown) Reload();
            Invalidate();
            return 0;
        case WM_GETMINMAXINFO: {
            UINT d = GetDpiForWindow(hwnd);
            if (!d) d = 96;
            RECT r{0, 0, MulDiv(kMinW, static_cast<int>(d), 96), MulDiv(kMinH, static_cast<int>(d), 96)};
            AdjustWindowRectExForDpi(&r, kStyle, FALSE, 0, d);
            LONG mw = r.right - r.left, mh = r.bottom - r.top;
            MONITORINFO mi{sizeof mi};
            if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)) { // never larger than the work area
                mw = std::min<LONG>(mw, mi.rcWork.right - mi.rcWork.left);
                mh = std::min<LONG>(mh, mi.rcWork.bottom - mi.rcWork.top);
            }
            reinterpret_cast<MINMAXINFO*>(l)->ptMinTrackSize = {mw, mh};
            return 0;
        }
        case WM_DPICHANGED:
            dpi = HIWORD(w);
            if (!positioning) {
                auto r = reinterpret_cast<const RECT*>(l);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            MakeIcons();
            Invalidate();
            return 0;
        case WM_ACTIVATE:
            if (LOWORD(w) == WA_INACTIVE) deactivatedAt = GetTickCount64();
            break; // DefWindowProc sets keyboard focus on activation
        case WM_SETCURSOR:
            if (LOWORD(l) == HTCLIENT) {
                Item* it = Find(hoverId);
                SetCursor(LoadCursorW(nullptr, it && it->text ? IDC_IBEAM : IDC_ARROW));
                return TRUE;
            }
            break;
        case WM_KEYDOWN: Key(w, l); return 0;
        case WM_CHAR: Char(static_cast<wchar_t>(w)); return 0;
        case WM_MOUSEMOVE: {
            auto p = Dip(l);
            if (!tracking) {
                TRACKMOUSEEVENT t{sizeof t, TME_LEAVE, hwnd, 0};
                tracking = TrackMouseEvent(&t) != FALSE;
            }
            if (dragId) {
                int v = ValueAt(drag, p.x, p.y);
                if (v != dragVal) dragVal = v, Invalidate();
                MaybeSend();
                return 0;
            }
            Item* it = Hit(p);
            int h = it ? it->id : 0;
            if (h == ID_BLOCK) h = 0;
            if (h != hoverId) hoverId = h, Invalidate();
            return 0;
        }
        case WM_MOUSELEAVE:
            tracking = false;
            if (!dragId && hoverId) hoverId = 0, Invalidate();
            return 0;
        case WM_LBUTTONDOWN: {
            auto p = Dip(l);
            SetCapture(hwnd);
            focusVisible = false;
            Item* it = Hit(p);
            pressId = 0;
            if (!it) {
                focusId = 0;
            } else {
                if (it->focus) focusId = it->id;
                if (it->slider) {
                    dragId = it->id;
                    drag = *it;
                    lastSent = it->val;
                    lastSend = 0;
                    dragVal = ValueAt(drag, p.x, p.y);
                    SetTimer(hwnd, TIMER_DRAG, 30, nullptr);
                    MaybeSend();
                } else {
                    pressId = it->id;
                    pressKey = it->key;
                }
            }
            Invalidate();
            return 0;
        }
        case WM_LBUTTONUP: {
            if (dragId) {
                EndDrag();
            } else {
                Item* it = Hit(Dip(l));
                int pressed = pressId;
                pressId = 0;
                if (it && it->id == pressed && it->key == pressKey && !it->text) Activate(*it);
            }
            ReleaseCapture();
            Invalidate();
            return 0;
        }
        case WM_CAPTURECHANGED:
            EndDrag();
            pressId = 0;
            return 0;
        case WM_TIMER:
            if (w == TIMER_DRAG && dragId) MaybeSend();
            if (w == TIMER_ANIM) Invalidate();
            return 0;
        case WM_MOUSEWHEEL:
            if (!dragId && !ddOpen && !pickId && !snap.pendingAlert) ScrollBy(-F(GET_WHEEL_DELTA_WPARAM(w)) / 120.f * 64.f);
            return 0;
        case WM_SETTINGCHANGE:
            if (l && wcscmp(reinterpret_cast<const wchar_t*>(l), L"ImmersiveColorSet") == 0) {
                LoadTheme();
                Invalidate();
            }
            break;
        case WM_DWMCOLORIZATIONCOLORCHANGED:
            LoadTheme();
            Invalidate();
            return 0;
        case WM_XM_QUIT: {
            HideInternal();
            AppHost& h = host; // Quit() destroys the main window; this object is freed later by its owner. Touch no members afterwards anyway.
            h.Quit();
            return 0;
        }
        case WM_NCDESTROY: {
            HWND h = hwnd;
            hwnd = nullptr;
            shown = false;
            SetWindowLongPtrW(h, GWLP_USERDATA, 0);
            return DefWindowProcW(h, m, w, l);
        }
        }
        return DefWindowProcW(hwnd, m, w, l);
    }
};

Flyout::Flyout(HINSTANCE inst, AppHost& host) : impl_(new Impl(inst, host)) {}
Flyout::~Flyout() { delete impl_; }

void Flyout::Toggle(const RECT& anchor) { impl_->Toggle(anchor); }
void Flyout::Hide() {
    if (impl_->shown) impl_->HideInternal();
}
bool Flyout::Visible() const { return impl_->shown && impl_->hwnd && !IsIconic(impl_->hwnd); }
void Flyout::Refresh() {
    if (!impl_->shown) return;
    impl_->Reload();
    impl_->Invalidate();
}

} // namespace xm
