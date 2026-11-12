// forge_mobile Windows backend (Win32; tablets, 2-in-1s and desktops).
//   touch     WM_POINTER messages from the app's WndProc via windows_feed_pointer (GetPointerInfo /
//             GetPointerPenInfo pressure, GetPointerFrameInfo for the contact count, QPC timestamps on the
//             monotonic_ns clock); pinch/rotate come from the shared gesture recognizer.
//   display   GetSystemMetrics + GetDpiForSystem + EnumDisplaySettings; orientation from DEVMODE.
//   power     GetSystemPowerStatus (forge::os::power).  keep_awake: a power request object
//             (PowerCreateRequest / PowerSetRequest, display + system required), process-wide.
//   open_url  ShellExecuteW, only for allow-listed URL schemes.  info: SMBIOS manufacturer/product from
//             HKLM\HARDWARE\DESCRIPTION\System\BIOS; form factor: SM_CONVERTIBLESLATEMODE / battery presence.
//   sensors   Windows Sensor API (ISensorManager, sensorsapi; Windows 7+): accelerometer, gyroscope, compass
//             (magnetometer), ambient light, aggregated device orientation (rotation_vector quaternion), barometer,
//             humidity, temperature, human proximity; polled with ISensor::GetData at the requested rate.
//   cameras   Media Foundation (MFEnumDeviceSources, video capture); capture_photo reads one frame through an
//             IMFSourceReader with video processing (RGB32) and encodes it with the WIC JPEG encoder.
//   brightness WMI WmiMonitorBrightness(Methods) (internal laptop panels), else DDC/CI through dxva2
//             (Get/SetMonitorBrightness) on the primary monitor's physical monitors.
//   bluetooth BluetoothFindFirstRadio / BluetoothFindFirstDevice (BluetoothAPIs.dll): classic inquiry, no RSSI.
//   audio     WASAPI shared mode on the default endpoints (IMMDeviceEnumerator, IAudioClient), converted to and
//             from the mix format in software.
//   notify    Shell_NotifyIconW balloon from a tray icon owned by a hidden message-only window (Windows 10+ shows
//             it as a toast); the icon stays until the device is destroyed. No AUMID or shortcut needed.
//   Every COM and DLL entry point is resolved at run time from System32 (no extra link libraries). Geolocation and
//   BLE (WinRT only) still come through provide_sensor + feed_sensor / set_service_hook.
#include "internal.hpp"
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <objbase.h>
#include <propidl.h>
#include <oleauto.h>
#include <portabledevicetypes.h>
#include <sensorsapi.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <wincodec.h>
#include <wbemcli.h>
#include <bluetoothapis.h>
#include <physicalmonitorenumerationapi.h>
#include <highlevelmonitorconfigurationapi.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace forge::os::mobile {
namespace {
// POINTER_INFO.PerformanceCount is a QPC tick count (0 when the source gave none); monotonic_ns uses the same counter.
std::uint64_t qpc_to_ns(std::uint64_t t) {
    static const std::uint64_t f = [] { LARGE_INTEGER q; QueryPerformanceFrequency(&q); return std::uint64_t(q.QuadPart); }();
    return t / f * 1000000000ull + t % f * 1000000000ull / f;
}
}  // namespace
bool windows_feed_pointer(void* hwnd, unsigned msg, std::uintptr_t wp, std::intptr_t) {
#ifndef WM_POINTERDOWN
    (void)hwnd; (void)msg; (void)wp; return false;
#else
    if (msg != WM_POINTERDOWN && msg != WM_POINTERUPDATE && msg != WM_POINTERUP && msg != WM_POINTERCAPTURECHANGED) return false;
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    using get_info_t = BOOL(WINAPI*)(UINT32, POINTER_INFO*);
    static auto get_info = reinterpret_cast<get_info_t>(reinterpret_cast<void*>(GetProcAddress(u32, "GetPointerInfo")));
    if (!get_info) return false;
    UINT32 id = UINT32(wp & 0xFFFF); POINTER_INFO pi{};
    if (!get_info(id, &pi)) return false;
    POINT pt = pi.ptPixelLocation; if (hwnd) ScreenToClient(static_cast<HWND>(hwnd), &pt);
    touch_phase ph = msg == WM_POINTERDOWN ? touch_phase::down : msg == WM_POINTERUP ? touch_phase::up : msg == WM_POINTERCAPTURECHANGED ? touch_phase::cancel : touch_phase::move;
    double pressure = 1;
    if (pi.pointerType == PT_PEN) {
        using pen_t = BOOL(WINAPI*)(UINT32, POINTER_PEN_INFO*);
        static auto pen = reinterpret_cast<pen_t>(reinterpret_cast<void*>(GetProcAddress(u32, "GetPointerPenInfo")));
        POINTER_PEN_INFO pp{}; if (pen && pen(id, &pp)) pressure = pp.pressure / 1024.0;
    }
    // Contacts in this pointer's frame (all fingers down together), not always 1.
    int pointers = 1;
    using frame_t = BOOL(WINAPI*)(UINT32, UINT32*, POINTER_INFO*);
    static auto frame = reinterpret_cast<frame_t>(reinterpret_cast<void*>(GetProcAddress(u32, "GetPointerFrameInfo")));
    if (frame) { POINTER_INFO buf[16]; UINT32 n = 16; if (frame(id, &n, buf) && n > 0) pointers = int(n); }
    std::uint64_t t = pi.PerformanceCount ? qpc_to_ns(std::uint64_t(pi.PerformanceCount)) : detail::now_ns();
    feed_touch({t, ph, int(id), double(pt.x), double(pt.y), pressure, pointers});
    return true;
#endif
}
} // namespace forge::os::mobile

namespace forge::os::mobile::detail {
namespace {
std::string reg_string(const wchar_t* key, const wchar_t* value) {
    wchar_t buf[256]; DWORD size = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, key, value, RRF_RT_REG_SZ, nullptr, buf, &size) != ERROR_SUCCESS) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr); if (n <= 1) return {};
    std::string s(std::size_t(n - 1), '\0'); WideCharToMultiByte(CP_UTF8, 0, buf, -1, s.data(), n, nullptr, nullptr);
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}
// kernel32 power requests (Windows 7+), resolved at run time. REASON_CONTEXT with a simple reason string.
struct reason_context { ULONG version; DWORD flags; union { LPWSTR simple; struct { HMODULE m; ULONG id; ULONG count; LPWSTR* strings; } detailed; } reason; };
struct power_api {
    using create_t = HANDLE(WINAPI*)(reason_context*); using set_t = BOOL(WINAPI*)(HANDLE, int);
    create_t create = nullptr; set_t set = nullptr, clear = nullptr;
    power_api() {
        HMODULE k = GetModuleHandleW(L"kernel32.dll");
        create = reinterpret_cast<create_t>(reinterpret_cast<void*>(GetProcAddress(k, "PowerCreateRequest")));
        set = reinterpret_cast<set_t>(reinterpret_cast<void*>(GetProcAddress(k, "PowerSetRequest")));
        clear = reinterpret_cast<set_t>(reinterpret_cast<void*>(GetProcAddress(k, "PowerClearRequest")));
    }
};
constexpr int k_display_required = 0, k_system_required = 1;   // POWER_REQUEST_TYPE

// ---- run-time binding (System32 only) and COM plumbing ------------------------------------------------------
HMODULE sys_module(const wchar_t* n) { return LoadLibraryExW(n, nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/); }
template <class F> void bind_sym(F& f, HMODULE m, const char* n) { f = m ? reinterpret_cast<F>(reinterpret_cast<void*>(GetProcAddress(m, n))) : nullptr; }

struct ole_api {
    decltype(&CoInitializeEx) init = nullptr; decltype(&CoUninitialize) uninit = nullptr; decltype(&CoCreateInstance) create = nullptr;
    decltype(&CoTaskMemFree) task_free = nullptr; decltype(&PropVariantClear) pv_clear = nullptr; decltype(&CoSetProxyBlanket) blanket = nullptr;
    decltype(&CreateStreamOnHGlobal) hglobal_stream = nullptr; decltype(&GetHGlobalFromStream) stream_hglobal = nullptr;
    decltype(&SysAllocString) bstr_alloc = nullptr; decltype(&SysFreeString) bstr_free = nullptr; decltype(&VariantClear) var_clear = nullptr;
    bool ok() const { return init && uninit && create && task_free && pv_clear && blanket && hglobal_stream && stream_hglobal && bstr_alloc && bstr_free && var_clear; }
    static const ole_api& get() {
        static const ole_api a = [] {
            ole_api x; HMODULE o = sys_module(L"ole32.dll"), au = sys_module(L"oleaut32.dll");
            bind_sym(x.init, o, "CoInitializeEx"); bind_sym(x.uninit, o, "CoUninitialize"); bind_sym(x.create, o, "CoCreateInstance"); bind_sym(x.task_free, o, "CoTaskMemFree");
            bind_sym(x.pv_clear, o, "PropVariantClear"); bind_sym(x.blanket, o, "CoSetProxyBlanket"); bind_sym(x.hglobal_stream, o, "CreateStreamOnHGlobal");
            bind_sym(x.stream_hglobal, o, "GetHGlobalFromStream"); bind_sym(x.bstr_alloc, au, "SysAllocString"); bind_sym(x.bstr_free, au, "SysFreeString"); bind_sym(x.var_clear, au, "VariantClear");
            return x; }();
        return a;
    }
};
// COM on the calling thread for the scope: MTA, or whatever apartment the thread already has.
struct com_scope {
    bool ok = false, owns = false;
    com_scope() {
        auto& o = ole_api::get(); if (!o.ok()) return;
        HRESULT h = o.init(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(h)) ok = owns = true; else if (h == RPC_E_CHANGED_MODE) ok = true;
    }
    ~com_scope() { if (owns) ole_api::get().uninit(); }
    com_scope(const com_scope&) = delete; com_scope& operator=(const com_scope&) = delete;
};
template <class T> struct com_ptr {
    T* p = nullptr;
    com_ptr() = default; com_ptr(const com_ptr&) = delete; com_ptr& operator=(const com_ptr&) = delete;
    ~com_ptr() { reset(); }
    void reset() { if (p) { p->Release(); p = nullptr; } }
    T** put() { reset(); return &p; }
    void** pv() { return reinterpret_cast<void**>(put()); }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};
template <class T> HRESULT co_create(const GUID& clsid, const GUID& iid, com_ptr<T>& out) {
    auto& o = ole_api::get(); if (!o.ok()) return E_NOTIMPL;
    return o.create(clsid, nullptr, CLSCTX_INPROC_SERVER, iid, out.pv());
}
error hr_error(HRESULT h, const std::string& what) {
    char b[16]; std::snprintf(b, sizeof b, "0x%08lx", static_cast<unsigned long>(h));
    return {int(h), what + " failed (HRESULT " + b + ")"};
}
std::string narrow(const wchar_t* w) {
    if (!w) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr); if (n <= 1) return {};
    std::string s(std::size_t(n - 1), '\0'); WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr); return s;
}
std::wstring widen_s(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(std::size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n); return w;
}
const HRESULT k_not_found = HRESULT(0x80070490);   // HRESULT_FROM_WIN32(ERROR_NOT_FOUND)

// ---- Sensor API --------------------------------------------------------------------------------------------
const GUID k_clsid_sensor_manager = {0x77a1c827, 0xfcd2, 0x4689, {0x89, 0x15, 0x9d, 0x61, 0x3c, 0xc5, 0xfa, 0x3e}};
const GUID k_iid_sensor_manager = {0xbd77db67, 0x45a8, 0x42dc, {0x8d, 0x00, 0x6d, 0xcf, 0x15, 0xf8, 0x37, 0x7a}};
struct win_sensor { sensor_kind kind; GUID type; };
const win_sensor k_win_sensors[] = {
    {sensor_kind::accelerometer, {0xc2fb0f5f, 0xe2d2, 0x4c78, {0xbc, 0xd0, 0x35, 0x2a, 0x95, 0x82, 0x81, 0x9d}}},
    {sensor_kind::gyroscope, {0x09485f5a, 0x759e, 0x42c2, {0xbd, 0x4b, 0xa3, 0x49, 0xb7, 0x5c, 0x86, 0x43}}},
    {sensor_kind::magnetometer, {0x76b5ce0d, 0x17dd, 0x414d, {0x93, 0xa1, 0xe1, 0x27, 0xf4, 0x0b, 0xdf, 0x6e}}},          // COMPASS_3D
    {sensor_kind::light, {0x97f115c8, 0x599a, 0x4153, {0x88, 0x94, 0xd2, 0xd1, 0x28, 0x99, 0x91, 0x8a}}},
    {sensor_kind::rotation_vector, {0xcdb5d8f7, 0x3cfd, 0x41c8, {0x85, 0x42, 0xcc, 0xe6, 0x22, 0xcf, 0x5d, 0x6e}}},     // AGGREGATED_DEVICE_ORIENTATION
    {sensor_kind::pressure, {0x0e903829, 0xff8a, 0x4a93, {0x97, 0xdf, 0x3d, 0xcb, 0xde, 0x40, 0x22, 0x88}}},
    {sensor_kind::humidity, {0x5c72bf67, 0xbd7e, 0x4257, {0x99, 0x0b, 0x98, 0xa3, 0xba, 0x3b, 0x40, 0x0a}}},
    {sensor_kind::temperature, {0x04fd0ec4, 0xd5da, 0x45fa, {0x95, 0xa9, 0x5d, 0xb3, 0x8e, 0xe1, 0x93, 0x06}}},
    {sensor_kind::proximity, {0x5220dae9, 0x3179, 0x4430, {0x9f, 0x90, 0x06, 0x26, 0x6d, 0x2a, 0x34, 0xde}}}};       // HUMAN_PROXIMITY
const GUID k_dt_motion = {0x3f8a69a2, 0x07c5, 0x4e48, {0xa9, 0x65, 0xcd, 0x79, 0x7a, 0xab, 0x56, 0xd5}};
const GUID k_dt_orient = {0x1637d8a2, 0x4248, 0x4275, {0x86, 0x5d, 0x55, 0x8d, 0xe8, 0x4a, 0xed, 0xfd}};
const GUID k_dt_light = {0xe4c77ce2, 0xdcb7, 0x46e9, {0x84, 0x39, 0x4f, 0xec, 0x54, 0x88, 0x33, 0xa6}};
const GUID k_dt_env = {0x8b0aa2f1, 0x2d57, 0x42ee, {0x8c, 0xc0, 0x4d, 0x27, 0x62, 0x2b, 0x46, 0xc4}};
const GUID k_dt_bio = {0x2299288a, 0x6d9e, 0x4b0b, {0xb7, 0xec, 0x35, 0x28, 0xf8, 0x9e, 0x40, 0xaf}};

const win_sensor* win_sensor_of(sensor_kind k) { for (auto& s : k_win_sensors) if (s.kind == k) return &s; return nullptr; }
bool pv_num(const PROPVARIANT& v, double& out) {
    switch (v.vt) {
        case VT_R8: out = v.dblVal; return true; case VT_R4: out = v.fltVal; return true;
        case VT_I4: out = v.lVal; return true; case VT_UI4: out = v.ulVal; return true;
        case VT_I2: out = v.iVal; return true; case VT_UI2: out = v.uiVal; return true;
        case VT_I1: out = v.cVal; return true; case VT_UI1: out = v.bVal; return true;
        default: return false;
    }
}
// Units as mobile.hpp defines them. The Sensor API accelerometer reports the gravity vector (-1 g on Z lying face
// up); Android, iOS and IIO report the reaction (+9.81 m/s^2), so the sign is flipped.
bool read_report(ISensorDataReport* r, sensor_kind k, sensor_event& e) {
    auto& o = ole_api::get();
    auto val = [&](const GUID& g, DWORD pid, double& out) {
        PROPERTYKEY key{g, pid}; PROPVARIANT v; std::memset(&v, 0, sizeof v);
        bool ok = SUCCEEDED(r->GetSensorValue(key, &v)) && pv_num(v, out); o.pv_clear(&v); return ok;
    };
    auto xyz = [&](const GUID& g, DWORD pid0, double mul) {
        for (int i = 0; i < 3; ++i) if (!val(g, pid0 + DWORD(i), e.v[std::size_t(i)])) return false; else e.v[std::size_t(i)] *= mul;
        e.n = 3; return true;
    };
    auto one = [&](const GUID& g, DWORD pid, double mul) { if (!val(g, pid, e.v[0])) return false; e.v[0] *= mul; e.n = 1; return true; };
    switch (k) {
        case sensor_kind::accelerometer: return xyz(k_dt_motion, 2, -9.80665);
        case sensor_kind::gyroscope: return xyz(k_dt_motion, 10, 3.14159265358979323846 / 180.0);
        case sensor_kind::magnetometer: return xyz(k_dt_orient, 19, 0.1);   // milligauss -> uT
        case sensor_kind::light: return one(k_dt_light, 2, 1.0);
        case sensor_kind::pressure: return one(k_dt_env, 4, 1000.0);        // bar -> hPa
        case sensor_kind::humidity: return one(k_dt_env, 3, 1.0);
        case sensor_kind::temperature: return one(k_dt_env, 2, 1.0);
        case sensor_kind::proximity: return one(k_dt_bio, 3, 100.0);        // m -> cm
        case sensor_kind::rotation_vector: {                                 // VT_VECTOR|VT_UI1: 4 floats x, y, z, w
            PROPERTYKEY key{k_dt_orient, 17}; PROPVARIANT v; std::memset(&v, 0, sizeof v); bool ok = false;
            if (SUCCEEDED(r->GetSensorValue(key, &v)) && v.vt == (VT_VECTOR | VT_UI1) && v.caub.cElems >= 16) {
                float q[4]; std::memcpy(q, v.caub.pElems, sizeof q); for (int i = 0; i < 4; ++i) e.v[std::size_t(i)] = q[i]; e.n = 4; ok = true;
            }
            o.pv_clear(&v); return ok;
        }
        default: return false;
    }
}
// First sensor of a type (null when there is none).
HRESULT find_sensor(ISensorManager* m, const GUID& type, com_ptr<ISensor>& out) {
    com_ptr<ISensorCollection> c; HRESULT h = m->GetSensorsByType(type, c.put());
    if (FAILED(h)) return h == k_not_found ? S_OK : h;
    ULONG n = 0; if (FAILED(c->GetCount(&n)) || n == 0) return S_OK;
    return c->GetAt(0, out.put());
}

// ---- Media Foundation + WIC ------------------------------------------------------------------------------
const GUID k_mf_src_type = {0xc60ac5fe, 0x252a, 0x478f, {0xa0, 0xef, 0xbc, 0x8f, 0xa5, 0xf7, 0xca, 0xd3}};
const GUID k_mf_vidcap = {0x8ac3587a, 0x4ae7, 0x42d8, {0x99, 0xe0, 0x0a, 0x60, 0x13, 0xee, 0xf9, 0x0f}};
const GUID k_mf_friendly = {0x60d0e559, 0x52f8, 0x4fa2, {0xbb, 0xce, 0xac, 0xdb, 0x34, 0xa8, 0xec, 0x01}};
const GUID k_mf_symlink = {0x58f0aad8, 0x22bf, 0x4f8a, {0xbb, 0x3d, 0xd2, 0xc4, 0x97, 0x8c, 0x6e, 0x2f}};
const GUID k_mf_video_processing = {0xfb394f3d, 0xccf1, 0x42ee, {0xbb, 0xb3, 0xf9, 0xb8, 0x45, 0xd5, 0x68, 0x1d}};
const GUID k_mt_major = {0x48eba18e, 0xf8c9, 0x4687, {0xbf, 0x11, 0x0a, 0x74, 0xc9, 0xf9, 0x6a, 0x8f}};
const GUID k_mt_subtype = {0xf7e34c9a, 0x42e8, 0x4714, {0xb7, 0x4b, 0xcb, 0x29, 0xd7, 0x2c, 0x35, 0xe5}};
const GUID k_mt_frame_size = {0x1652c33d, 0xd6b2, 0x4012, {0xb8, 0x34, 0x72, 0x03, 0x08, 0x49, 0xa3, 0x7d}};
const GUID k_mt_stride = {0x644b4e48, 0x1e02, 0x4516, {0xb0, 0xeb, 0xc0, 0x1c, 0xa9, 0xd4, 0x9a, 0xc6}};
const GUID k_mf_video = {0x73646976, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID k_mf_rgb32 = {0x00000016, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};   // D3DFMT_X8R8G8B8
const GUID k_iid_mf_source = {0x279a808d, 0xaec7, 0x40c8, {0x9c, 0x6b, 0xa6, 0xb4, 0x92, 0xc7, 0x8a, 0x66}};
const GUID k_clsid_wic = {0xcacaf262, 0x9370, 0x4615, {0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a}};
const GUID k_iid_wic = {0xec5ec8a9, 0xc395, 0x4314, {0x9c, 0x77, 0x54, 0xd7, 0xa9, 0x35, 0xff, 0x70}};
const GUID k_wic_jpeg = {0x19e4a5aa, 0x5662, 0x4fc5, {0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57}};
const GUID k_wic_bgr24 = {0x6fddc324, 0x4e03, 0x4bfe, {0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c}};

struct mf_api {
    decltype(&MFStartup) startup = nullptr; decltype(&MFShutdown) shutdown = nullptr; decltype(&MFCreateAttributes) attrs = nullptr;
    decltype(&MFCreateMediaType) media_type = nullptr; decltype(&MFGetStrideForBitmapInfoHeader) stride = nullptr;
    decltype(&MFEnumDeviceSources) enum_sources = nullptr; decltype(&MFCreateSourceReaderFromMediaSource) reader = nullptr;
    bool ok() const { return startup && shutdown && attrs && media_type && stride && enum_sources && reader; }
    static const mf_api& get() {
        static const mf_api a = [] {
            mf_api x; HMODULE p = sys_module(L"mfplat.dll"), m = sys_module(L"mf.dll"), r = sys_module(L"mfreadwrite.dll");
            bind_sym(x.startup, p, "MFStartup"); bind_sym(x.shutdown, p, "MFShutdown"); bind_sym(x.attrs, p, "MFCreateAttributes"); bind_sym(x.media_type, p, "MFCreateMediaType");
            bind_sym(x.stride, p, "MFGetStrideForBitmapInfoHeader"); bind_sym(x.enum_sources, m, "MFEnumDeviceSources"); bind_sym(x.reader, r, "MFCreateSourceReaderFromMediaSource");
            return x; }();
        return a;
    }
};
// MFStartup for the scope + the video capture devices (IMFActivate, released on destruction).
struct mf_devices {
    bool started = false; IMFActivate** d = nullptr; UINT32 n = 0; HRESULT hr = S_OK;
    mf_devices() {
        auto& mf = mf_api::get(); if (!mf.ok()) { hr = E_NOTIMPL; return; }
        if (FAILED(hr = mf.startup(MF_VERSION, 1 /*MFSTARTUP_LITE*/))) return;
        started = true;
        com_ptr<IMFAttributes> a; if (FAILED(hr = mf.attrs(a.put(), 1))) return;
        if (FAILED(hr = a->SetGUID(k_mf_src_type, k_mf_vidcap))) return;
        hr = mf.enum_sources(a.p, &d, &n);
    }
    ~mf_devices() {
        for (UINT32 i = 0; d && i < n; ++i) if (d[i]) d[i]->Release();
        if (d) ole_api::get().task_free(d);
        if (started) mf_api::get().shutdown();
    }
    std::string str(UINT32 i, const GUID& key) const {
        LPWSTR w = nullptr; UINT32 len = 0; std::string s;
        if (SUCCEEDED(d[i]->GetAllocatedString(key, &w, &len)) && w) { s = narrow(w); ole_api::get().task_free(w); }
        return s;
    }
};
// 24bpp BGR rows (top-down) -> JPEG through WIC.
result<std::vector<std::uint8_t>> wic_jpeg(const std::vector<std::uint8_t>& bgr, UINT w, UINT h) {
    auto& o = ole_api::get();
    com_ptr<IWICImagingFactory> f; HRESULT hr = co_create(k_clsid_wic, k_iid_wic, f);
    if (FAILED(hr)) return {{}, hr_error(hr, "WIC imaging factory")};
    com_ptr<IStream> st; if (FAILED(hr = o.hglobal_stream(nullptr, TRUE, st.put()))) return {{}, hr_error(hr, "CreateStreamOnHGlobal")};
    com_ptr<IWICBitmapEncoder> enc; if (FAILED(hr = f->CreateEncoder(k_wic_jpeg, nullptr, enc.put()))) return {{}, hr_error(hr, "WIC JPEG encoder")};
    if (FAILED(hr = enc->Initialize(st.p, WICBitmapEncoderNoCache))) return {{}, hr_error(hr, "IWICBitmapEncoder::Initialize")};
    com_ptr<IWICBitmapFrameEncode> fr; com_ptr<IPropertyBag2> props;
    if (FAILED(hr = enc->CreateNewFrame(fr.put(), props.put())) || FAILED(hr = fr->Initialize(props.p))) return {{}, hr_error(hr, "WIC frame")};
    WICPixelFormatGUID pf = k_wic_bgr24;
    if (FAILED(hr = fr->SetSize(w, h)) || FAILED(hr = fr->SetPixelFormat(&pf))) return {{}, hr_error(hr, "WIC frame format")};
    if (!IsEqualGUID(pf, k_wic_bgr24)) return {{}, {-1, "WIC JPEG encoder refused 24bpp BGR"}};
    if (FAILED(hr = fr->WritePixels(h, w * 3, UINT(bgr.size()), const_cast<BYTE*>(bgr.data()))) || FAILED(hr = fr->Commit()) || FAILED(hr = enc->Commit()))
        return {{}, hr_error(hr, "WIC JPEG encode")};
    LARGE_INTEGER zero{}; ULARGE_INTEGER size{}; HGLOBAL hg = nullptr;
    if (FAILED(hr = st->Seek(zero, STREAM_SEEK_END, &size)) || FAILED(hr = o.stream_hglobal(st.p, &hg))) return {{}, hr_error(hr, "JPEG stream")};
    std::vector<std::uint8_t> out(std::size_t(size.QuadPart));
    if (void* p = GlobalLock(hg)) { std::memcpy(out.data(), p, out.size()); GlobalUnlock(hg); } else return {{}, last_error("GlobalLock")};
    return {out, {}};
}
// One frame from a capture device: source reader with the video processor converting to RGB32. The first frames
// are skipped while auto exposure settles.
result<std::vector<std::uint8_t>> mf_capture(IMFActivate* act) {
    auto& mf = mf_api::get();
    com_ptr<IMFMediaSource> src; HRESULT hr = act->ActivateObject(k_iid_mf_source, src.pv());
    if (FAILED(hr)) return {{}, hr == E_ACCESSDENIED ? error{int(hr), "camera access denied (Settings > Privacy > Camera)"} : hr_error(hr, "IMFActivate::ActivateObject (camera)")};
    struct shut { IMFMediaSource* s; IMFActivate* a; ~shut() { s->Shutdown(); a->ShutdownObject(); } } sh{src.p, act};
    com_ptr<IMFAttributes> ra; if (FAILED(hr = mf.attrs(ra.put(), 1)) || FAILED(hr = ra->SetUINT32(k_mf_video_processing, TRUE))) return {{}, hr_error(hr, "reader attributes")};
    com_ptr<IMFSourceReader> rd; if (FAILED(hr = mf.reader(src.p, ra.p, rd.put()))) return {{}, hr_error(hr, "MFCreateSourceReaderFromMediaSource")};
    const DWORD stream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
    com_ptr<IMFMediaType> mt;
    if (FAILED(hr = mf.media_type(mt.put())) || FAILED(hr = mt->SetGUID(k_mt_major, k_mf_video)) || FAILED(hr = mt->SetGUID(k_mt_subtype, k_mf_rgb32))
        || FAILED(hr = rd->SetCurrentMediaType(stream, nullptr, mt.p))) return {{}, hr_error(hr, "camera RGB32 output")};
    com_ptr<IMFMediaType> cur; UINT64 fs = 0;
    if (FAILED(hr = rd->GetCurrentMediaType(stream, cur.put())) || FAILED(hr = cur->GetUINT64(k_mt_frame_size, &fs))) return {{}, hr_error(hr, "camera frame size")};
    UINT w = UINT(fs >> 32), h = UINT(fs & 0xffffffffu); if (!w || !h) return {{}, {-1, "camera reported a 0x0 frame"}};
    LONG stride = 0; UINT32 s32 = 0;
    if (SUCCEEDED(cur->GetUINT32(k_mt_stride, &s32))) stride = LONG(INT32(s32)); else if (FAILED(mf.stride(k_mf_rgb32.Data1, w, &stride))) stride = LONG(w * 4);
    com_ptr<IMFSample> last; int frames = 0;
    for (int i = 0; i < 60 && frames < 5; ++i) {   // ReadSample blocks until a frame (or an error) arrives
        DWORD idx = 0, flags = 0; LONGLONG ts = 0; com_ptr<IMFSample> s;
        if (FAILED(hr = rd->ReadSample(stream, 0, &idx, &flags, &ts, s.put()))) return {{}, hr_error(hr, "IMFSourceReader::ReadSample")};
        if (flags & (MF_SOURCE_READERF_ERROR | MF_SOURCE_READERF_ENDOFSTREAM)) break;
        if (s) { last.reset(); last.p = s.p; s.p = nullptr; ++frames; }
    }
    if (!last) return {{}, {-1, "camera delivered no frame"}};
    com_ptr<IMFMediaBuffer> buf; if (FAILED(hr = last->ConvertToContiguousBuffer(buf.put()))) return {{}, hr_error(hr, "ConvertToContiguousBuffer")};
    BYTE* data = nullptr; DWORD max = 0, len = 0; if (FAILED(hr = buf->Lock(&data, &max, &len))) return {{}, hr_error(hr, "IMFMediaBuffer::Lock")};
    std::size_t row = std::size_t(stride < 0 ? -stride : stride);
    if (row < std::size_t(w) * 4 || std::size_t(len) < row * h) { buf->Unlock(); return {{}, {-1, "camera frame smaller than its format"}}; }
    std::vector<std::uint8_t> bgr(std::size_t(w) * h * 3);
    for (UINT y = 0; y < h; ++y) {   // negative stride: bottom-up rows
        const BYTE* s = data + (stride < 0 ? std::size_t(h - 1 - y) : std::size_t(y)) * row; std::uint8_t* d = bgr.data() + std::size_t(y) * w * 3;
        for (UINT x = 0; x < w; ++x) { d[0] = s[0]; d[1] = s[1]; d[2] = s[2]; d += 3; s += 4; }
    }
    buf->Unlock();
    return wic_jpeg(bgr, w, h);
}

// ---- brightness: WMI (internal panels), DDC/CI (external monitors) --------------------------------------------
const GUID k_clsid_wbem_locator = {0x4590f811, 0x1d3a, 0x11d0, {0x89, 0x1f, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
const GUID k_iid_wbem_locator = {0xdc12a687, 0x737f, 0x11cf, {0x88, 0x4d, 0x00, 0xaa, 0x00, 0x4b, 0x2e, 0x24}};
struct bstr {
    BSTR s; explicit bstr(const wchar_t* w) : s(ole_api::get().bstr_alloc(w)) {}
    ~bstr() { if (s) ole_api::get().bstr_free(s); }
    bstr(const bstr&) = delete; bstr& operator=(const bstr&) = delete;
};
struct variant { VARIANT v; variant() { std::memset(&v, 0, sizeof v); } ~variant() { ole_api::get().var_clear(&v); } };
HRESULT wmi_connect(com_ptr<IWbemServices>& svc) {
    com_ptr<IWbemLocator> loc; HRESULT hr = co_create(k_clsid_wbem_locator, k_iid_wbem_locator, loc); if (FAILED(hr)) return hr;
    bstr ns(L"ROOT\\WMI"); if (FAILED(hr = loc->ConnectServer(ns.s, nullptr, nullptr, nullptr, 0, nullptr, nullptr, svc.put()))) return hr;
    return ole_api::get().blanket(svc.p, RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
}
HRESULT wmi_first(IWbemServices* svc, const wchar_t* query, com_ptr<IWbemClassObject>& obj) {
    bstr lang(L"WQL"), q(query); com_ptr<IEnumWbemClassObject> en;
    HRESULT hr = svc->ExecQuery(lang.s, q.s, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, en.put()); if (FAILED(hr)) return hr;
    ULONG got = 0; hr = en->Next(5000, 1, obj.put(), &got);
    return SUCCEEDED(hr) && got == 1 ? S_OK : k_not_found;
}
bool wmi_brightness(double& out) {
    com_scope com; if (!com.ok) return false;
    com_ptr<IWbemServices> svc; com_ptr<IWbemClassObject> o;
    if (FAILED(wmi_connect(svc)) || FAILED(wmi_first(svc.p, L"SELECT CurrentBrightness FROM WmiMonitorBrightness WHERE Active=TRUE", o))) return false;
    variant v; if (FAILED(o->Get(L"CurrentBrightness", 0, &v.v, nullptr, nullptr))) return false;
    double d;
    if (v.v.vt == VT_UI1) d = v.v.bVal; else if (v.v.vt == VT_I4) d = v.v.lVal; else return false;
    out = d / 100.0; return true;
}
// 1: set, 0: no WMI-controllable panel, -1: failed (hr set).
int wmi_set_brightness(double level, HRESULT& hr) {
    com_scope com; if (!com.ok) return 0;
    com_ptr<IWbemServices> svc; com_ptr<IWbemClassObject> inst;
    if (FAILED(wmi_connect(svc)) || FAILED(wmi_first(svc.p, L"SELECT * FROM WmiMonitorBrightnessMethods WHERE Active=TRUE", inst))) return 0;
    variant path; if (FAILED(hr = inst->Get(L"__PATH", 0, &path.v, nullptr, nullptr)) || path.v.vt != VT_BSTR) return -1;
    bstr cls_name(L"WmiMonitorBrightnessMethods"), method(L"WmiSetBrightness");
    com_ptr<IWbemClassObject> cls, in_def, in;
    if (FAILED(hr = svc->GetObject(cls_name.s, 0, nullptr, cls.put(), nullptr)) || FAILED(hr = cls->GetMethod(L"WmiSetBrightness", 0, in_def.put(), nullptr))
        || FAILED(hr = in_def->SpawnInstance(0, in.put()))) return -1;
    VARIANT t; std::memset(&t, 0, sizeof t); t.vt = VT_I4; t.lVal = 0;
    VARIANT b; std::memset(&b, 0, sizeof b); b.vt = VT_UI1; b.bVal = BYTE(std::lround(level * 100));
    if (FAILED(hr = in->Put(L"Timeout", 0, &t, 0)) || FAILED(hr = in->Put(L"Brightness", 0, &b, 0))) return -1;
    return SUCCEEDED(hr = svc->ExecMethod(path.v.bstrVal, method.s, 0, nullptr, in.p, nullptr, nullptr)) ? 1 : -1;
}
struct dxva2_api {
    decltype(&GetNumberOfPhysicalMonitorsFromHMONITOR) count = nullptr; decltype(&GetPhysicalMonitorsFromHMONITOR) list = nullptr;
    decltype(&DestroyPhysicalMonitors) destroy = nullptr; decltype(&GetMonitorBrightness) get_b = nullptr; decltype(&SetMonitorBrightness) set_b = nullptr;
    bool ok() const { return count && list && destroy && get_b && set_b; }
    static const dxva2_api& get() {
        static const dxva2_api a = [] {
            dxva2_api x; HMODULE d = sys_module(L"dxva2.dll");
            bind_sym(x.count, d, "GetNumberOfPhysicalMonitorsFromHMONITOR"); bind_sym(x.list, d, "GetPhysicalMonitorsFromHMONITOR"); bind_sym(x.destroy, d, "DestroyPhysicalMonitors");
            bind_sym(x.get_b, d, "GetMonitorBrightness"); bind_sym(x.set_b, d, "SetMonitorBrightness");
            return x; }();
        return a;
    }
};
// The primary monitor's physical monitors (DDC/CI); destroyed with the object.
struct physical_monitors {
    std::vector<PHYSICAL_MONITOR> m;
    physical_monitors() {
        auto& a = dxva2_api::get(); if (!a.ok()) return;
        HMONITOR mon = MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY); DWORD n = 0;
        if (!mon || !a.count(mon, &n) || n == 0 || n > 16) return;
        m.resize(n); if (!a.list(mon, n, m.data())) m.clear();
    }
    ~physical_monitors() { if (!m.empty()) dxva2_api::get().destroy(DWORD(m.size()), m.data()); }
};

// ---- Bluetooth ----------------------------------------------------------------------------------------------
struct bt_api {
    decltype(&BluetoothFindFirstRadio) first_radio = nullptr; decltype(&BluetoothFindRadioClose) radio_close = nullptr;
    decltype(&BluetoothFindFirstDevice) first_dev = nullptr; decltype(&BluetoothFindNextDevice) next_dev = nullptr; decltype(&BluetoothFindDeviceClose) dev_close = nullptr;
    bool ok() const { return first_radio && radio_close && first_dev && next_dev && dev_close; }
    static const bt_api& get() {
        static const bt_api a = [] {
            bt_api x; HMODULE b = sys_module(L"BluetoothApis.dll");
            bind_sym(x.first_radio, b, "BluetoothFindFirstRadio"); bind_sym(x.radio_close, b, "BluetoothFindRadioClose");
            bind_sym(x.first_dev, b, "BluetoothFindFirstDevice"); bind_sym(x.next_dev, b, "BluetoothFindNextDevice"); bind_sym(x.dev_close, b, "BluetoothFindDeviceClose");
            return x; }();
        return a;
    }
};

// ---- WASAPI ---------------------------------------------------------------------------------------------------
const GUID k_clsid_mmde = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const GUID k_iid_mmde = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
const GUID k_iid_audio_client = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
const GUID k_iid_render_client = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
const GUID k_iid_capture_client = {0xc8adbd64, 0xe71e, 0x48a0, {0xa4, 0xde, 0x18, 0x5c, 0x39, 0x5c, 0xd3, 0x17}};
struct mix_format { int rate = 0, ch = 0, bytes = 0; bool flt = false; };
// Shared-mode mix format: float32, or PCM in 16/24/32-bit containers.
bool describe(const WAVEFORMATEX* w, mix_format& f) {
    if (!w || w->nChannels == 0) return false;
    f.rate = int(w->nSamplesPerSec); f.ch = w->nChannels; f.bytes = w->nBlockAlign / w->nChannels;
    unsigned tag = w->wFormatTag;
    if (tag == 0xFFFE /*WAVE_FORMAT_EXTENSIBLE*/ && w->cbSize >= 22) { GUID sub; std::memcpy(&sub, reinterpret_cast<const BYTE*>(w) + 24, sizeof sub); tag = sub.Data1; }
    f.flt = tag == 3 /*IEEE_FLOAT*/;
    return (f.flt && f.bytes == 4) || (tag == 1 /*PCM*/ && (f.bytes == 2 || f.bytes == 3 || f.bytes == 4));
}
float sample_in(const BYTE* p, const mix_format& f) {
    if (f.flt) { float v; std::memcpy(&v, p, 4); return v; }
    if (f.bytes == 2) { std::int16_t v; std::memcpy(&v, p, 2); return float(v) / 32768.0f; }
    if (f.bytes == 3) { std::int32_t v = std::int32_t(std::uint32_t(p[0]) << 8 | std::uint32_t(p[1]) << 16 | std::uint32_t(p[2]) << 24) >> 8; return float(v) / 8388608.0f; }
    std::int32_t v; std::memcpy(&v, p, 4); return float(double(v) / 2147483648.0);
}
void sample_out(BYTE* p, float x, const mix_format& f) {
    x = std::clamp(x, -1.0f, 1.0f);
    if (f.flt) { std::memcpy(p, &x, 4); return; }
    if (f.bytes == 2) { auto v = std::int16_t(std::lround(x * 32767.0f)); std::memcpy(p, &v, 2); return; }
    if (f.bytes == 3) { auto v = std::int32_t(std::lround(x * 8388607.0f)); p[0] = BYTE(v); p[1] = BYTE(v >> 8); p[2] = BYTE(v >> 16); return; }
    auto v = std::int32_t(std::llround(double(x) * 2147483647.0)); std::memcpy(p, &v, 4);
}
// The default endpoint's IAudioClient, initialised in shared mode with a 200 ms buffer in its mix format.
struct wasapi_stream {
    com_ptr<IAudioClient> ac; WAVEFORMATEX* mix = nullptr; mix_format f; UINT32 frames = 0;
    ~wasapi_stream() { if (mix) ole_api::get().task_free(mix); }
    error open(bool capture) {
        com_ptr<IMMDeviceEnumerator> en; HRESULT hr = co_create(k_clsid_mmde, k_iid_mmde, en);
        if (FAILED(hr)) return hr_error(hr, "MMDeviceEnumerator");
        com_ptr<IMMDevice> dev; hr = en->GetDefaultAudioEndpoint(capture ? eCapture : eRender, eConsole, dev.put());
        if (hr == k_not_found || (SUCCEEDED(hr) && !dev)) return unsupported(capture ? "audio recording (no default capture device)" : "audio playback (no default output device)");
        if (FAILED(hr)) return hr_error(hr, "GetDefaultAudioEndpoint");
        if (FAILED(hr = dev->Activate(k_iid_audio_client, CLSCTX_ALL, nullptr, ac.pv()))) return hr_error(hr, "IMMDevice::Activate(IAudioClient)");
        if (FAILED(hr = ac->GetMixFormat(&mix))) return hr_error(hr, "IAudioClient::GetMixFormat");
        if (!describe(mix, f)) return unsupported("audio (mix format not float32 or 16/24/32-bit PCM)");
        if (FAILED(hr = ac->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 2000000 /*200 ms*/, 0, mix, nullptr))) {
            if (hr == E_ACCESSDENIED) return {int(hr), "audio access denied (Settings > Privacy > Microphone)"};
            return hr_error(hr, "IAudioClient::Initialize");
        }
        if (FAILED(hr = ac->GetBufferSize(&frames))) return hr_error(hr, "IAudioClient::GetBufferSize");
        return {};
    }
};

// ---- tray-icon notifications ----------------------------------------------------------------------------------
// A message-only window on its own thread owns the tray icon (Shell_NotifyIconW needs a window that outlives the
// call); the icon and the thread go away with the backend.
struct tray {
    std::mutex m; std::condition_variable cv; std::thread t; HWND hwnd = nullptr; DWORD tid = 0; bool ready = false, added = false;
    bool start() {
        std::unique_lock g(m);
        if (ready) return hwnd != nullptr;
        t = std::thread([this] {
            HINSTANCE inst = GetModuleHandleW(nullptr);
            WNDCLASSW wc{}; wc.lpfnWndProc = DefWindowProcW; wc.hInstance = inst; wc.lpszClassName = L"forge_mobile_tray";
            RegisterClassW(&wc);   // ERROR_CLASS_ALREADY_EXISTS from an earlier device is fine
            HWND w = CreateWindowExW(0, L"forge_mobile_tray", L"forge", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
            MSG msg; PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);   // make sure the thread has a queue for WM_QUIT
            { std::lock_guard g2(m); hwnd = w; tid = GetCurrentThreadId(); ready = true; } cv.notify_all();
            if (!w) return;
            while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
            { std::lock_guard g2(m); if (added) { NOTIFYICONDATAW nid{}; nid.cbSize = sizeof nid; nid.hWnd = w; nid.uID = 1; Shell_NotifyIconW(NIM_DELETE, &nid); added = false; } }
            DestroyWindow(w);
        });
        cv.wait(g, [&] { return ready; });
        return hwnd != nullptr;
    }
    status show(std::string_view title, std::string_view body) {
        if (!start()) return {{}, last_error("CreateWindowExW (tray window)")};
        std::lock_guard g(m);
        NOTIFYICONDATAW nid{}; nid.cbSize = sizeof nid; nid.hWnd = hwnd; nid.uID = 1;
        nid.uFlags = NIF_ICON | NIF_TIP | NIF_INFO; nid.hIcon = LoadIconW(nullptr, MAKEINTRESOURCEW(32516) /*IDI_INFORMATION*/);
        auto copy = [](wchar_t* dst, std::size_t cap, std::string_view s) { auto w = widen_s(s); std::size_t n = std::min(w.size(), cap - 1); std::memcpy(dst, w.data(), n * sizeof(wchar_t)); dst[n] = 0; };
        copy(nid.szTip, sizeof nid.szTip / sizeof(wchar_t), "forge");
        copy(nid.szInfoTitle, sizeof nid.szInfoTitle / sizeof(wchar_t), title); copy(nid.szInfo, sizeof nid.szInfo / sizeof(wchar_t), body.empty() ? std::string_view(" ") : body);
        nid.dwInfoFlags = NIIF_INFO;
        if (!added) {
            if (!Shell_NotifyIconW(NIM_ADD, &nid)) return {{}, {-1, "Shell_NotifyIconW(NIM_ADD) failed (no notification area?)"}};
            added = true; NOTIFYICONDATAW v = nid; v.uVersion = 4 /*NOTIFYICON_VERSION_4*/; Shell_NotifyIconW(NIM_SETVERSION, &v);
            return {};
        }
        return Shell_NotifyIconW(NIM_MODIFY, &nid) ? status{} : status{{}, {-1, "Shell_NotifyIconW(NIM_MODIFY) failed"}};
    }
    ~tray() {
        if (!t.joinable()) return;
        DWORD id; { std::lock_guard g(m); id = tid; }
        if (id) PostThreadMessageW(id, WM_QUIT, 0, 0);
        t.join();
    }
};

struct windows_backend final : backend {
    std::mutex awake_m; HANDLE awake = nullptr; bool awake_on = false;
    ~windows_backend() override { if (awake) { keep_awake(false); CloseHandle(awake); } }
    std::string name() const override { return "windows"; }
    tray notifier;
    result<std::vector<sensor_info>> sensors() override {
        std::vector<sensor_info> v{{sensor_kind::battery, "GetSystemPowerStatus", "Microsoft", 1, 100, 1}};
        com_scope com; com_ptr<ISensorManager> m;
        if (com.ok && SUCCEEDED(co_create(k_clsid_sensor_manager, k_iid_sensor_manager, m)))
            for (auto& s : k_win_sensors) {
                com_ptr<ISensor> se; if (FAILED(find_sensor(m.p, s.type, se)) || !se) continue;
                std::string name = "Sensor API"; BSTR nm = nullptr;
                if (SUCCEEDED(se->GetFriendlyName(&nm)) && nm) { name = narrow(nm); ole_api::get().bstr_free(nm); }
                v.push_back({s.kind, name, "Windows Sensor API", 0, 0, 0});
            }
        return {v, {}};
    }
    // Sensor API sensors are polled (ISensor::GetData returns the latest report) on a worker thread with its own
    // COM apartment; the calling thread only checks that the sensor exists and is accessible.
    result<sensor_stream> open_sensor_api(sensor_kind k, const win_sensor& def, double hz) {
        {
            com_scope com; if (!com.ok) return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " (COM unavailable)")};
            com_ptr<ISensorManager> m; HRESULT hr = co_create(k_clsid_sensor_manager, k_iid_sensor_manager, m);
            if (FAILED(hr)) return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " (no Windows Sensor API)")};
            com_ptr<ISensor> se;   // a failing query lists nothing in sensors() either, so it is unsupported here too
            if (FAILED(hr = find_sensor(m.p, def.type, se))) return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " (" + hr_error(hr, "GetSensorsByType").what + ")")};
            if (!se) return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " (no Sensor API device)")};
            SensorState st = SENSOR_STATE_READY;
            if (SUCCEEDED(se->GetState(&st)) && st == SENSOR_STATE_ACCESS_DENIED) return {{}, {int(E_ACCESSDENIED), std::string("sensor ") + sensor_name(k) + ": access denied (Settings > Privacy)"}};
        }
        auto q = std::make_shared<queue>(); bridge::get().add_sensor(k, q);
        auto w = std::make_shared<worker>(); auto period = std::uint64_t(1e9 / hz);
        w->start([q, k, type = def.type, wp = w.get(), period] {
            com_scope com; com_ptr<ISensorManager> m; com_ptr<ISensor> se;
            if (!com.ok || FAILED(co_create(k_clsid_sensor_manager, k_iid_sensor_manager, m)) || FAILED(find_sensor(m.p, type, se)) || !se) { q->finish(); return; }
            do {
                com_ptr<ISensorDataReport> r; sensor_event e; e.kind = k;
                if (SUCCEEDED(se->GetData(r.put())) && r && read_report(r.p, k, e)) { e.t_ns = now_ns(); q->push({false, e, {}}); }
            } while (wp->sleep_ns(period));
        });
        return {make_sensor(k, q, w), {}};
    }
    result<sensor_stream> open_sensor(sensor_kind k, double hz) override {
        if (auto* def = win_sensor_of(k)) return open_sensor_api(k, *def, hz);
        // Location (WinRT Geolocator) arrives through feed_sensor from app code (provide_sensor).
        if (k != sensor_kind::battery) return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " (Win32; supply via provide_sensor + feed_sensor from WinRT)")};
        auto q = std::make_shared<queue>(); bridge::get().add_sensor(k, q);
        auto w = std::make_shared<worker>(); auto period = std::uint64_t(1e9 / hz);
        w->start([q, wp = w.get(), period] {
            do { auto p = power_status(); if (p.battery_percent >= 0) q->push({false, {sensor_kind::battery, now_ns(), {double(p.battery_percent), p.charging ? 1.0 : 0.0}, 2}, {}}); } while (wp->sleep_ns(period));
        });
        return {make_sensor(k, q, w), {}};
    }
    result<touch_stream> open_touch() override { auto q = std::make_shared<queue>(); bridge::get().add_touch(q); return {make_touch(q, nullptr), {}}; }
    result<display_info> display() override {
        if (auto b = backend::display()) return b;
        display_info d; DEVMODEW dm{}; dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm)) {
            d.width_px = int(dm.dmPelsWidth); d.height_px = int(dm.dmPelsHeight); d.refresh_hz = dm.dmDisplayFrequency;
            switch (dm.dmDisplayOrientation) { case 0: d.orient = d.width_px >= d.height_px ? orientation::landscape_left : orientation::portrait; break;
                case 1: d.orient = orientation::portrait; break; case 2: d.orient = orientation::landscape_right; break; case 3: d.orient = orientation::portrait_upside_down; break; }
        } else { d.width_px = GetSystemMetrics(SM_CXSCREEN); d.height_px = GetSystemMetrics(SM_CYSCREEN); }
        using dpi_t = UINT(WINAPI*)(); auto f = reinterpret_cast<dpi_t>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForSystem")));
        d.dpi = f ? f() : 96; d.scale = d.dpi / 96.0;
        return {d, {}};
    }
    // A power request belongs to the process (any thread may toggle it), unlike SetThreadExecutionState, which is
    // per thread and is dropped when that thread exits.
    status keep_awake(bool on) override {
        static const power_api api;
        std::lock_guard g(awake_m);
        if (!api.create || !api.set || !api.clear) {   // pre-Windows 7: the thread-scoped fallback
            return SetThreadExecutionState(on ? (ES_CONTINUOUS | ES_DISPLAY_REQUIRED | ES_SYSTEM_REQUIRED) : ES_CONTINUOUS) ? status{} : status{{}, last_error("SetThreadExecutionState")};
        }
        if (on == awake_on) return {};
        if (!awake) {
            static wchar_t why[] = L"forge keep_awake";
            reason_context rc{}; rc.version = 0 /*POWER_REQUEST_CONTEXT_VERSION*/; rc.flags = 0x1 /*POWER_REQUEST_CONTEXT_SIMPLE_STRING*/; rc.reason.simple = why;
            awake = api.create(&rc);
            if (awake == INVALID_HANDLE_VALUE || !awake) { awake = nullptr; return {{}, last_error("PowerCreateRequest")}; }
        }
        if (on) {
            if (!api.set(awake, k_display_required)) return {{}, last_error("PowerSetRequest")};
            if (!api.set(awake, k_system_required)) { auto e = last_error("PowerSetRequest"); api.clear(awake, k_display_required); return {{}, e}; }
        } else { api.clear(awake, k_display_required); api.clear(awake, k_system_required); }
        awake_on = on; return {};
    }
    status open_url(std::string_view u) override {
        if (auto c = check_url(u); !c) return c;
        std::string s(u);
        int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0); std::wstring w(std::size_t(n), L'\0'); MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
        auto r = reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        return r > 32 ? status{} : status{{}, {int(r), "ShellExecuteW failed"}};
    }
    result<device_info> info() override {
        auto d = backend::info(); if (!d) return d;
        bool slate = GetSystemMetrics(0x2003 /*SM_CONVERTIBLESLATEMODE*/) == 0, tablet = GetSystemMetrics(SM_TABLETPC) != 0;
        d.value.form = slate && tablet ? form_factor::tablet : power_status().has_battery ? form_factor::laptop : form_factor::desktop;
        const wchar_t* bios = L"HARDWARE\\DESCRIPTION\\System\\BIOS";   // SMBIOS system information, as msinfo32 shows it
        d.value.manufacturer = reg_string(bios, L"SystemManufacturer"); d.value.model = reg_string(bios, L"SystemProductName");
        return d;
    }
    // ---- cameras (Media Foundation). id: the device's symbolic link; capture_photo also takes "0", "1", ...
    result<std::vector<camera_info>> cameras() override {
        com_scope com; if (!com.ok) return {{}, unsupported("camera enumeration (COM unavailable)")};
        mf_devices devs; if (devs.hr == E_NOTIMPL) return {{}, unsupported("camera enumeration (no Media Foundation)")};
        if (FAILED(devs.hr)) return {{}, hr_error(devs.hr, "MFEnumDeviceSources")};
        std::vector<camera_info> v;
        for (UINT32 i = 0; i < devs.n; ++i) {
            auto nm = devs.str(i, k_mf_friendly); auto lc = nm; for (auto& c : lc) c = char(std::tolower(static_cast<unsigned char>(c)));
            v.push_back({devs.str(i, k_mf_symlink), nm, lc.find("front") != std::string::npos});
        }
        return {v, {}};
    }
    result<std::vector<std::uint8_t>> capture_photo(std::string_view id) override {
        com_scope com; if (!com.ok) return {{}, unsupported("camera capture (COM unavailable)")};
        mf_devices devs; if (devs.hr == E_NOTIMPL) return {{}, unsupported("camera capture (no Media Foundation)")};
        if (FAILED(devs.hr)) return {{}, hr_error(devs.hr, "MFEnumDeviceSources")};
        if (devs.n == 0) return {{}, unsupported("camera capture (no video capture device)")};
        UINT32 pick = devs.n;
        if (id.empty()) pick = 0;
        else for (UINT32 i = 0; i < devs.n && pick == devs.n; ++i) if (devs.str(i, k_mf_symlink) == id || std::to_string(i) == id) pick = i;
        if (pick == devs.n) return {{}, {ERROR_NOT_FOUND, "capture_photo: no camera " + std::string(id)}};
        return mf_capture(devs.d[pick]);
    }
    // ---- brightness: WMI for internal panels, else DDC/CI on the primary monitor
    result<double> brightness() override {
        double b; if (wmi_brightness(b)) return {b, {}};
        physical_monitors pm; auto& a = dxva2_api::get();
        for (auto& m : pm.m) {
            DWORD lo = 0, cur = 0, hi = 0;
            if (a.get_b(m.hPhysicalMonitor, &lo, &cur, &hi) && hi > lo) return {double(cur - lo) / double(hi - lo), {}};
        }
        return {{}, unsupported("screen brightness (no WMI backlight, no DDC/CI monitor)")};
    }
    status set_brightness(double l) override {
        HRESULT hr = S_OK; int w = wmi_set_brightness(l, hr);
        if (w == 1) return {};
        if (w < 0) return {{}, hr_error(hr, "WmiSetBrightness")};
        physical_monitors pm; auto& a = dxva2_api::get(); bool any = false; error last{};
        for (auto& m : pm.m) {
            DWORD lo = 0, cur = 0, hi = 0;
            if (!a.get_b(m.hPhysicalMonitor, &lo, &cur, &hi) || hi <= lo) continue;
            if (a.set_b(m.hPhysicalMonitor, lo + DWORD(std::lround(l * double(hi - lo))))) any = true; else last = last_error("SetMonitorBrightness");
        }
        if (any) return {};
        return {{}, last ? last : unsupported("screen brightness (no WMI backlight, no DDC/CI monitor)")};
    }
    // ---- Bluetooth classic inquiry (BluetoothApis.dll). Returns the devices the inquiry found plus paired and
    // connected ones the stack reports; RSSI is not available through this API (0).
    result<std::vector<ble_device>> bluetooth_scan(int ms) override {
        auto& a = bt_api::get(); if (!a.ok()) return {{}, unsupported("bluetooth scan (no BluetoothApis.dll)")};
        BLUETOOTH_FIND_RADIO_PARAMS rp{}; rp.dwSize = sizeof rp; HANDLE radio = nullptr;
        HBLUETOOTH_RADIO_FIND rf = a.first_radio(&rp, &radio);
        if (!rf) return {{}, unsupported("bluetooth scan (no Bluetooth radio)")};
        CloseHandle(radio); a.radio_close(rf);
        BLUETOOTH_DEVICE_SEARCH_PARAMS sp{}; sp.dwSize = sizeof sp;
        sp.fReturnAuthenticated = TRUE; sp.fReturnRemembered = TRUE; sp.fReturnUnknown = TRUE; sp.fReturnConnected = TRUE; sp.fIssueInquiry = TRUE;
        sp.cTimeoutMultiplier = UCHAR(std::clamp((ms + 1279) / 1280, 1, 48)); sp.hRadio = nullptr;   // units of 1.28 s, all radios
        BLUETOOTH_DEVICE_INFO di{}; di.dwSize = sizeof di;
        std::vector<ble_device> v;
        HBLUETOOTH_DEVICE_FIND df = a.first_dev(&sp, &di);
        if (!df) { DWORD e = GetLastError(); if (e == ERROR_NO_MORE_ITEMS) return {v, {}}; return {{}, last_error("BluetoothFindFirstDevice")}; }
        do {
            char addr[18]; auto& b = di.Address.rgBytes;
            std::snprintf(addr, sizeof addr, "%02X:%02X:%02X:%02X:%02X:%02X", b[5], b[4], b[3], b[2], b[1], b[0]);
            v.push_back({addr, narrow(di.szName), 0});
            di = {}; di.dwSize = sizeof di;
        } while (a.next_dev(df, &di));
        a.dev_close(df);
        return {v, {}};
    }
    // ---- WASAPI shared mode on the default endpoints
    result<audio_clip> record_audio(int ms, int rate) override {
        com_scope com; if (!com.ok) return {{}, unsupported("audio recording (COM unavailable)")};
        wasapi_stream s; if (auto e = s.open(true)) return {{}, e};
        com_ptr<IAudioCaptureClient> cc; HRESULT hr = s.ac->GetService(k_iid_capture_client, cc.pv());
        if (FAILED(hr)) return {{}, hr_error(hr, "IAudioClient::GetService(IAudioCaptureClient)")};
        std::size_t need = std::size_t(s.f.rate) * std::size_t(ms) / 1000; std::vector<float> in; in.reserve(need * std::size_t(s.f.ch));
        if (FAILED(hr = s.ac->Start())) return {{}, hr_error(hr, "IAudioClient::Start")};
        auto deadline = now_ns() + std::uint64_t(ms + 3000) * 1000000ull;
        while (in.size() < need * std::size_t(s.f.ch) && now_ns() < deadline) {
            Sleep(10);
            UINT32 packet = 0;
            while (SUCCEEDED(cc->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* p = nullptr; UINT32 n = 0; DWORD flags = 0;
                if (FAILED(hr = cc->GetBuffer(&p, &n, &flags, nullptr, nullptr))) break;
                for (UINT32 i = 0; i < n * UINT32(s.f.ch); ++i) in.push_back((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? 0.0f : sample_in(p + std::size_t(i) * std::size_t(s.f.bytes), s.f));
                cc->ReleaseBuffer(n);
            }
        }
        s.ac->Stop();
        std::size_t frames = std::min(in.size() / std::size_t(s.f.ch), need);
        if (frames == 0) return {{}, {-1, "audio recording: the capture device delivered no data"}};
        auto mono = remix(in.data(), frames, s.f.ch, s.f.rate, 1, rate);
        audio_clip c; c.sample_rate = rate; c.channels = 1; c.samples.resize(std::min(mono.size(), std::size_t(rate) * std::size_t(ms) / 1000));
        for (std::size_t i = 0; i < c.samples.size(); ++i) c.samples[i] = std::int16_t(std::lround(std::clamp(mono[i], -1.0f, 1.0f) * 32767.0f));
        return {c, {}};
    }
    status play_audio(const audio_clip& clip) override {
        com_scope com; if (!com.ok) return {{}, unsupported("audio playback (COM unavailable)")};
        if (clip.samples.empty()) return {};
        wasapi_stream s; if (auto e = s.open(false)) return {{}, e};
        com_ptr<IAudioRenderClient> rc; HRESULT hr = s.ac->GetService(k_iid_render_client, rc.pv());
        if (FAILED(hr)) return {{}, hr_error(hr, "IAudioClient::GetService(IAudioRenderClient)")};
        std::vector<float> src(clip.samples.size()); for (std::size_t i = 0; i < src.size(); ++i) src[i] = float(clip.samples[i]) / 32768.0f;
        auto out = remix(src.data(), src.size() / std::size_t(clip.channels), clip.channels, clip.sample_rate, s.f.ch, s.f.rate);
        std::size_t total = out.size() / std::size_t(s.f.ch), done = 0; bool started = false;
        auto deadline = now_ns() + (std::uint64_t(total) * 1000 / std::uint64_t(s.f.rate) + 3000) * 1000000ull;
        while (now_ns() < deadline) {
            UINT32 pad = 0; if (FAILED(hr = s.ac->GetCurrentPadding(&pad))) break;
            if (done >= total) { if (pad == 0) break; Sleep(5); continue; }
            UINT32 n = UINT32(std::min<std::size_t>(s.frames - pad, total - done));
            if (n > 0) {
                BYTE* p = nullptr; if (FAILED(hr = rc->GetBuffer(n, &p))) break;
                for (std::size_t i = 0; i < std::size_t(n) * std::size_t(s.f.ch); ++i) sample_out(p + i * std::size_t(s.f.bytes), out[done * std::size_t(s.f.ch) + i], s.f);
                rc->ReleaseBuffer(n, 0); done += n;
            }
            if (!started) { if (FAILED(hr = s.ac->Start())) break; started = true; }
            Sleep(10);
        }
        if (started) s.ac->Stop();
        if (FAILED(hr)) return {{}, hr_error(hr, "WASAPI playback")};
        return done >= total ? status{} : status{{}, {-1, "audio playback timed out"}};
    }
    status notify(std::string_view t, std::string_view b) override {
        if (auto h = backend::notify(t, b); !is_unsupported(h.err)) return h;
        return notifier.show(t, b);
    }
    result<permission_state> permission_status(permission p) override { if (auto b = backend::permission_status(p)) return b; return {permission_state::not_required, {}}; }
    status request_permission(permission) override { return {}; }
};
} // namespace
std::shared_ptr<backend> make_windows() { return std::make_shared<windows_backend>(); }
} // namespace forge::os::mobile::detail
#endif
