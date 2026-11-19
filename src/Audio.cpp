// Audio.cpp - render endpoint enumeration, change notifications, default-endpoint switching.
#include <initguid.h> // must precede other headers so DEVPKEY_/PKEY_ constants get defined here
#include "Core.h"
#include <mmsystem.h>
#include <mmreg.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <devpropdef.h>
#include <devpkey.h>
#include <cfgmgr32.h>
#include <cwctype>
#include <utility>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "user32.lib")

namespace xm {
namespace {

// Undocumented IPolicyConfig (Win7+ layout, same IID used on Win10/11).
// Vtable order confirmed against EarTrumpet's IPolicyConfig.cs (12 methods after IUnknown).
struct __declspec(uuid("f8679f50-850a-41cf-9c72-430f290290c8")) IPolicyConfig : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX*, WAVEFORMATEX*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, INT64*, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, INT64*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, INT, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, INT, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
// CLSID_PolicyConfigClient
const CLSID kPolicyConfigClient = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

struct PropVar {
    PROPVARIANT v;
    PropVar() { PropVariantInit(&v); }
    ~PropVar() { PropVariantClear(&v); }
    PropVar(const PropVar&) = delete;
    PropVar& operator=(const PropVar&) = delete;
};

bool IsHex(wchar_t c) { return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'F'); }

bool Hex12(const std::wstring& s, size_t pos) {
    if (pos + 12 > s.size()) return false;
    bool nonZero = false;
    for (size_t i = 0; i < 12; ++i) {
        if (!IsHex(s[pos + i])) return false;
        if (s[pos + i] != L'0') nonZero = true;
    }
    return nonZero;
}

std::string FormatMac(const std::wstring& s, size_t pos) {
    std::string out;
    for (size_t i = 0; i < 12; ++i) {
        if (i && i % 2 == 0) out += ':';
        out += (char)s[pos + i];
    }
    return out;
}

// Handles  BTHENUM\DEV_AABBCCDDEEFF\...  and
//          BTHENUM\{0000110b-...}_VID&...\7&hash&0&AABBCCDDEEFF_C00000000
std::string MacFromInstanceId(std::wstring id) {
    for (auto& c : id) c = (wchar_t)towupper(c);
    size_t d = id.find(L"DEV_");
    if (d != std::wstring::npos && Hex12(id, d + 4)) return FormatMac(id, d + 4);
    size_t slash = id.rfind(L'\\');
    std::wstring tail = slash == std::wstring::npos ? id : id.substr(slash + 1);
    size_t start = 0;
    while (start <= tail.size()) {
        size_t end = tail.find_first_of(L"&_", start);
        if (end == std::wstring::npos) end = tail.size();
        if (end - start == 12 && Hex12(tail, start)) return FormatMac(tail, start);
        start = end + 1;
    }
    return {};
}

struct BtNode {
    GUID container;
    std::string mac;
    std::wstring a2dpProvider; // driver provider of this node if it is the A2DP sink service (UUID 110B)
};

std::wstring DevString(DEVINST inst, const DEVPROPKEY& key) {
    wchar_t buf[256] = {};
    ULONG sz = sizeof(buf) - sizeof(wchar_t);
    DEVPROPTYPE type = 0;
    if (CM_Get_DevNode_PropertyW(inst, &key, &type, reinterpret_cast<PBYTE>(buf), &sz, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING)
        return {};
    return buf;
}

std::vector<BtNode> BtNodes() {
    std::vector<BtNode> out;
    const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, L"BTHENUM", flags) != CR_SUCCESS || len < 2) return out;
    std::vector<wchar_t> buf(len);
    if (CM_Get_Device_ID_ListW(L"BTHENUM", buf.data(), len, flags) != CR_SUCCESS) return out;
    for (const wchar_t* p = buf.data(); *p; p += wcslen(p) + 1) {
        DEVINST inst = 0;
        if (CM_Locate_DevNodeW(&inst, const_cast<DEVINSTID_W>(p), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) continue;
        GUID g{};
        ULONG sz = sizeof(g);
        DEVPROPTYPE type = 0;
        if (CM_Get_DevNode_PropertyW(inst, &DEVPKEY_Device_ContainerId, &type, reinterpret_cast<PBYTE>(&g), &sz, 0) != CR_SUCCESS ||
            type != DEVPROP_TYPE_GUID)
            continue;
        std::string mac = MacFromInstanceId(p);
        if (mac.empty()) continue;
        std::wstring upper(p);
        for (auto& c : upper) c = (wchar_t)towupper(c);
        const bool sink = upper.find(L"{0000110B-0000-1000-8000-00805F9B34FB}") != std::wstring::npos;
        out.push_back({g, mac, sink ? DevString(inst, DEVPKEY_Device_DriverProvider) : std::wstring()});
    }
    return out;
}

class Notifier final : public IMMNotificationClient {
public:
    explicit Notifier(HWND h) : hwnd_(h) {}
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&ref_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = (ULONG)InterlockedDecrement(&ref_);
        if (r == 0) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return Poke(); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Poke(); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return Poke(); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override { return Poke(); }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT Poke() {
        PostMessageW(hwnd_, WM_XM_AUDIO, 0, 0);
        return S_OK;
    }
    HWND hwnd_;
    LONG ref_ = 1;
};

} // namespace

struct Audio::Impl {
    IMMDeviceEnumerator* en = nullptr;
    Notifier* notifier = nullptr;
};

Audio::Audio(HWND notify) : impl_(new Impl) {
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&impl_->en));
    if (FAILED(hr)) {
        Log(L"Audio: MMDeviceEnumerator failed 0x%08X", (unsigned)hr);
        return;
    }
    impl_->notifier = new Notifier(notify);
    hr = impl_->en->RegisterEndpointNotificationCallback(impl_->notifier);
    if (FAILED(hr)) Log(L"Audio: RegisterEndpointNotificationCallback failed 0x%08X", (unsigned)hr);
}

Audio::~Audio() {
    if (impl_->en && impl_->notifier) impl_->en->UnregisterEndpointNotificationCallback(impl_->notifier);
    if (impl_->notifier) impl_->notifier->Release();
    if (impl_->en) impl_->en->Release();
    delete impl_;
}

std::vector<Endpoint> Audio::RenderEndpoints() const {
    std::vector<Endpoint> out;
    IMMDeviceEnumerator* en = impl_->en;
    if (!en) return out;

    IMMDeviceCollection* coll = nullptr;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &coll)) || !coll) return out;

    std::wstring defaultId;
    IMMDevice* def = nullptr;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eMultimedia, &def)) && def) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(def->GetId(&id)) && id) {
            defaultId = id;
            CoTaskMemFree(id);
        }
        def->Release();
    }

    std::vector<BtNode> nodes;
    bool nodesLoaded = false;

    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* dev = nullptr;
        if (FAILED(coll->Item(i, &dev)) || !dev) continue;

        Endpoint ep;
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
            ep.id = id;
            CoTaskMemFree(id);
        }
        ep.isDefault = !defaultId.empty() && ep.id == defaultId;

        IPropertyStore* ps = nullptr;
        bool skip = false;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
            {
                PropVar pv;
                if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &pv.v)) && pv.v.vt == VT_LPWSTR && pv.v.pwszVal)
                    ep.name = pv.v.pwszVal;
            }
            {
                PropVar pv;
                if (SUCCEEDED(ps->GetValue(PKEY_AudioEngine_DeviceFormat, &pv.v)) && pv.v.vt == VT_BLOB &&
                    pv.v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
                    const auto* wf = reinterpret_cast<const WAVEFORMATEX*>(pv.v.blob.pBlobData);
                    ep.sampleRate = (int)wf->nSamplesPerSec;
                    ep.bits = wf->wBitsPerSample;
                    ep.channels = wf->nChannels;
                    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22 &&
                        pv.v.blob.cbSize >= sizeof(WAVEFORMATEXTENSIBLE)) {
                        const auto* wfx = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
                        if (wfx->Samples.wValidBitsPerSample) ep.bits = wfx->Samples.wValidBitsPerSample;
                    }
                }
            }
            std::wstring enumerator;
            {
                PropVar pv;
                if (SUCCEEDED(ps->GetValue(PKEY_Device_EnumeratorName, &pv.v)) && pv.v.vt == VT_LPWSTR && pv.v.pwszVal)
                    enumerator = pv.v.pwszVal;
            }
            if (_wcsicmp(enumerator.c_str(), L"BTHHFENUM") == 0) {
                skip = true; // hands-free endpoint
            } else if (_wcsicmp(enumerator.c_str(), L"BTHENUM") == 0) {
                PropVar pv;
                if (SUCCEEDED(ps->GetValue(PKEY_Device_ContainerId, &pv.v)) && pv.v.vt == VT_CLSID && pv.v.puuid) {
                    if (!nodesLoaded) {
                        nodes = BtNodes();
                        nodesLoaded = true;
                    }
                    for (const auto& n : nodes)
                        if (IsEqualGUID(n.container, *pv.v.puuid)) {
                            if (ep.btMac.empty()) ep.btMac = n.mac;
                            if (!n.a2dpProvider.empty()) ep.a2dpDriver = n.a2dpProvider;
                        }
                }

            }
            ps->Release();
        }
        dev->Release();
        if (!skip) out.push_back(std::move(ep));
    }
    coll->Release();
    return out;
}

bool Audio::SetDefault(const std::wstring& endpointId) {
    IPolicyConfig* pc = nullptr;
    HRESULT hr = CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, __uuidof(IPolicyConfig),
                                  reinterpret_cast<void**>(&pc));
    if (FAILED(hr) || !pc) {
        Log(L"Audio: PolicyConfigClient unavailable 0x%08X", (unsigned)hr);
        return false;
    }
    bool ok = true;
    for (ERole role : {eConsole, eMultimedia, eCommunications}) {
        hr = pc->SetDefaultEndpoint(endpointId.c_str(), role);
        if (FAILED(hr)) {
            Log(L"Audio: SetDefaultEndpoint role %d failed 0x%08X", (int)role, (unsigned)hr);
            ok = false;
        }
    }
    pc->Release();
    return ok;
}

} // namespace xm
