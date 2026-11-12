#pragma once
// GPU discovery without initialising any compute runtime (no CUDA/Vulkan/OpenCL/Metal device creation).
//   Linux: /sys/bus/pci/devices (class 0x03xxxx) + driver link + amdgpu VRAM sysfs; platform GPUs from
//   /sys/class/drm (Arm Mali, Adreno via kgsl).  Android: /sys/class/kgsl, /dev/mali0, ro.hardware.egl.
//   Windows: DXGI adapter enumeration (dxgi.dll, CreateDXGIFactory1 — a display-only API, it does not create a
//   device).  macOS: IOKit IOAccelerator / IOPCIDevice registry properties.  iOS: the integrated Apple GPU from
//   hw.machine.  FreeBSD: /dev/pci PCIOCGETCONF.
#include <forge/os/detail.hpp>
#if defined(FORGE_OS_WINDOWS)
#include <dxgi.h>
#endif
#if defined(FORGE_OS_FREEBSD)
#include <sys/pciio.h>
#include <sys/ioctl.h>
#endif

namespace forge::os {
struct gpu_device {
    std::uint32_t vendor_id = 0, device_id = 0;
    std::string name, vendor, driver, bus;      // bus: PCI address or platform node
    std::uint64_t dedicated_memory = 0;         // bytes, 0 if unknown
    bool integrated = false;
};
inline const char* gpu_vendor_name(std::uint32_t v) {
    switch (v) { case 0x10de: return "NVIDIA"; case 0x1002: return "AMD"; case 0x8086: return "Intel"; case 0x106b: return "Apple"; case 0x13b5: return "Arm";
                 case 0x5143: return "Qualcomm"; case 0x1414: return "Microsoft"; case 0x1af4: return "Virtio"; case 0x15ad: return "VMware"; case 0x1234: return "QEMU"; default: return "unknown"; }
}
// Intel discrete GPUs by PCI device id: DG1 / Iris Xe MAX, Arc Alchemist (DG2, incl. Pro), Arc Battlemage,
// Ponte Vecchio / Data Center Max, Flex (ATS-M).
inline bool intel_discrete_id(std::uint32_t d) {
    return (d >= 0x4905 && d <= 0x4909) || (d >= 0x5690 && d <= 0x56bf) || (d >= 0x56c0 && d <= 0x56cf) || (d >= 0xe202 && d <= 0xe22f) || (d >= 0x0bd0 && d <= 0x0bdf);
}
// Integrated vs discrete from PCI ids and location: Intel iGPUs sit at bus 0 device 2 (00:02.0), discrete Arc
// behind a root port; an AMD APU reports a small VRAM carve-out (<= 1 GiB). Apple GPUs are integrated.
inline bool gpu_integrated_guess(std::uint32_t vendor, std::uint32_t device, int bus, int dev, std::uint64_t vram) {
    if (vendor == 0x106b) return true;
    if (vendor == 0x8086) return !intel_discrete_id(device) && (bus < 0 || (bus == 0 && dev == 2));
    if (vendor == 0x1002) return vram > 0 && vram <= (std::uint64_t(1) << 30);
    return false;
}
#if defined(FORGE_OS_WINDOWS)
namespace detail {
// DXCore (dxcore.dll, display-only like DXGI: no device is created) answers IsIntegrated per adapter LUID.
// Hand-declared vtables (no dxcore.h in older SDKs / MinGW). 1 integrated, 0 discrete, -1 unknown.
inline int dxcore_is_integrated(LUID luid) {
    struct unk_vt { HRESULT(STDMETHODCALLTYPE* qi)(void*, REFIID, void**); ULONG(STDMETHODCALLTYPE* add)(void*); ULONG(STDMETHODCALLTYPE* rel)(void*); };
    struct factory_vt { unk_vt u; void* create_list; HRESULT(STDMETHODCALLTYPE* by_luid)(void*, const LUID&, REFIID, void**); };
    struct adapter_vt { unk_vt u; void* is_valid; void* attr_supported; bool(STDMETHODCALLTYPE* prop_supported)(void*, std::uint32_t); HRESULT(STDMETHODCALLTYPE* get_prop)(void*, std::uint32_t, std::size_t, void*); };
    struct obj { const void* vt; };
    using create_fn = HRESULT(WINAPI*)(REFIID, void**);
    static const create_fn create = [] {
        HMODULE h = LoadLibraryExW(L"dxcore.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/);
        return h ? reinterpret_cast<create_fn>(reinterpret_cast<void*>(GetProcAddress(h, "DXCoreCreateAdapterFactory"))) : create_fn(nullptr);
    }();
    if (!create) return -1;
    static const GUID iid_factory = {0x78ee5945, 0xc36e, 0x4b13, {0xa6, 0x69, 0x00, 0x5d, 0xd1, 0x1c, 0x0f, 0x06}};   // IDXCoreAdapterFactory
    static const GUID iid_adapter = {0xf0db4c7f, 0xfe5a, 0x42a2, {0xbd, 0x62, 0xf2, 0xa6, 0xcf, 0x6f, 0xc8, 0x3e}};   // IDXCoreAdapter
    void* f = nullptr; if (FAILED(create(iid_factory, &f)) || !f) return -1;
    auto* fv = static_cast<const factory_vt*>(static_cast<obj*>(f)->vt);
    int r = -1; void* a = nullptr;
    if (SUCCEEDED(fv->by_luid(f, luid, iid_adapter, &a)) && a) {
        auto* av = static_cast<const adapter_vt*>(static_cast<obj*>(a)->vt);
        constexpr std::uint32_t is_integrated = 12;   // DXCoreAdapterProperty::IsIntegrated
        bool v = false; if (av->prop_supported(a, is_integrated) && SUCCEEDED(av->get_prop(a, is_integrated, sizeof v, &v))) r = v ? 1 : 0;
        av->u.rel(a);
    }
    fv->u.rel(f);
    return r;
}
}
#endif
inline std::vector<gpu_device> gpu_devices() {
    std::vector<gpu_device> out;
#if defined(__linux__)
    if (auto l = list_dir("/sys/bus/pci/devices")) {
        for (auto& e : l.value) {
            std::string d = "/sys/bus/pci/devices/" + e.name;
            auto cls = detail::slurp_ll(d + "/class", 0); if ((cls >> 16) != 0x03) continue;
            gpu_device g; g.bus = e.name; g.vendor_id = std::uint32_t(detail::slurp_ll(d + "/vendor", 0)); g.device_id = std::uint32_t(detail::slurp_ll(d + "/device", 0));
            g.vendor = gpu_vendor_name(g.vendor_id);
            char b[4096]; auto n = ::readlink((d + "/driver").c_str(), b, sizeof b - 1); if (n > 0) g.driver = basename(std::string(b, std::size_t(n)));
            auto vram = detail::slurp_ll(d + "/mem_info_vram_total", 0); if (vram > 0) g.dedicated_memory = std::uint64_t(vram);
            unsigned dom = 0, bus = 0, dv = 0, fn = 0; bool addr = std::sscanf(e.name.c_str(), "%x:%x:%x.%x", &dom, &bus, &dv, &fn) == 4;
            g.integrated = gpu_integrated_guess(g.vendor_id, g.device_id, addr && dom == 0 ? int(bus) : 1, int(dv), std::uint64_t(vram > 0 ? vram : 0));
            auto label = detail::trim(detail::slurp(d + "/label"));
            if (label.empty() && g.vendor_id == 0x10de) {   // the NVIDIA driver names the board: "Model: \t\t NVIDIA GeForce ..."
                auto info = detail::slurp("/proc/driver/nvidia/gpus/" + e.name + "/information");
                if (auto p = info.find("Model:"); p != std::string::npos) label = detail::trim(info.substr(p + 6, info.find('\n', p) - p - 6));
            }
            g.name = label.empty() ? std::string(g.vendor) + " GPU " + [&] { char x[16]; std::snprintf(x, sizeof x, "%04x", g.device_id); return std::string(x); }() : label;
            out.push_back(std::move(g));
        }
    }
    auto add_platform = [&](const char* vendor, std::uint32_t vid, const std::string& node, const std::string& name, const std::string& drv) {
        for (auto& g : out) if (g.bus == node) return;
        gpu_device g; g.vendor = vendor; g.vendor_id = vid; g.bus = node; g.name = name; g.driver = drv; g.integrated = true; out.push_back(std::move(g));
    };
    if (exists("/sys/class/kgsl/kgsl-3d0")) { auto m = detail::trim(detail::slurp("/sys/class/kgsl/kgsl-3d0/gpu_model")); add_platform("Qualcomm", 0x5143, "kgsl-3d0", m.empty() ? "Adreno" : m, "kgsl"); }
    if (exists("/dev/mali0") || exists("/sys/class/misc/mali0")) add_platform("Arm", 0x13b5, "mali0", "Mali", "mali");
    if (auto l = list_dir("/sys/class/drm")) for (auto& e : l.value) {
        if (e.name.rfind("card", 0) || e.name.find('-') != std::string::npos) continue;
        std::string d = "/sys/class/drm/" + e.name + "/device";
        if (exists(d + "/vendor")) continue;   // PCI: already listed
        char b[4096]; auto n = ::readlink((d + "/driver").c_str(), b, sizeof b - 1); if (n <= 0) continue;
        std::string drv = basename(std::string(b, std::size_t(n)));
        add_platform(drv == "panfrost" || drv == "lima" ? "Arm" : drv == "msm" ? "Qualcomm" : "platform", drv == "msm" ? 0x5143 : drv == "panfrost" || drv == "lima" ? 0x13b5 : 0, e.name, drv, drv);
    }
#if defined(__ANDROID__)
    if (out.empty()) { auto egl = detail::android_prop("ro.hardware.egl"); if (!egl.empty()) add_platform(egl == "adreno" ? "Qualcomm" : egl == "mali" ? "Arm" : "android", 0, "egl", egl, egl); }
#endif
#elif defined(FORGE_OS_WINDOWS)
    if (HMODULE h = LoadLibraryExW(L"dxgi.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/)) {
        using fn = HRESULT(WINAPI*)(REFIID, void**);
        if (auto f = reinterpret_cast<fn>(reinterpret_cast<void*>(GetProcAddress(h, "CreateDXGIFactory1")))) {
            IDXGIFactory1* fac = nullptr;
            static const GUID iid = {0x770aae78, 0xf26f, 0x4dba, {0xa8, 0x29, 0x25, 0x3c, 0x83, 0xd1, 0xb3, 0x87}};   // IID_IDXGIFactory1
            if (SUCCEEDED(f(iid, reinterpret_cast<void**>(&fac))) && fac) {
                IDXGIAdapter1* a = nullptr;
                for (UINT i = 0; fac->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i) {
                    DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); a->Release();
                    if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;
                    gpu_device g; g.vendor_id = d.VendorId; g.device_id = d.DeviceId; g.vendor = gpu_vendor_name(d.VendorId); g.name = narrow(d.Description);
                    g.dedicated_memory = d.DedicatedVideoMemory;
                    int igpu = detail::dxcore_is_integrated(d.AdapterLuid);   // what the driver says (Windows 10 2004+)
                    g.integrated = igpu >= 0 ? igpu == 1 : d.VendorId == 0x8086 ? !intel_discrete_id(d.DeviceId) : d.DedicatedVideoMemory < (std::uint64_t(512) << 20);
                    char luid[40]; std::snprintf(luid, sizeof luid, "luid:%08lx%08lx", (unsigned long)d.AdapterLuid.HighPart, (unsigned long)d.AdapterLuid.LowPart); g.bus = luid; g.driver = "dxgi";
                    out.push_back(std::move(g));
                }
                fac->Release();
            }
        }
        FreeLibrary(h);
    }
#elif defined(FORGE_OS_MACOS)
    if (void* io = ::dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY)) {
        auto& cf = detail::cf_api::get();
        auto matching = reinterpret_cast<const void* (*)(const char*)>(::dlsym(io, "IOServiceMatching"));
        auto get = reinterpret_cast<int (*)(unsigned, const void*, unsigned*)>(::dlsym(io, "IOServiceGetMatchingServices"));
        auto next = reinterpret_cast<unsigned (*)(unsigned)>(::dlsym(io, "IOIteratorNext"));
        auto rel = reinterpret_cast<int (*)(unsigned)>(::dlsym(io, "IOObjectRelease"));
        auto prop = reinterpret_cast<const void* (*)(unsigned, const void*, const void*, unsigned)>(::dlsym(io, "IORegistryEntryCreateCFProperty"));
        auto parent = reinterpret_cast<int (*)(unsigned, const char*, unsigned*)>(::dlsym(io, "IORegistryEntryGetParentEntry"));
        auto cls = reinterpret_cast<int (*)(unsigned, char*)>(::dlsym(io, "IOObjectGetClass"));
        if (cf.ok && matching && get && next && rel && prop) {
            unsigned it = 0;
            if (get(0, matching("IOAccelerator"), &it) == 0) {
                while (unsigned s = next(it)) {
                    gpu_device g; char c[128] = {}; if (cls) cls(s, c); g.driver = c;
                    unsigned pci = 0; unsigned src = s; if (parent && parent(s, "IOService", &pci) == 0) src = pci;
                    auto rd = [&](unsigned e, const char* k) -> std::string { auto key = cf.str(k); auto v = prop(e, key, nullptr, 0); cf.CFRelease(key); if (!v) return {};
                        std::string r = cf.to_string(v); if (r.empty() && cf.CFGetTypeID(v) == cf.CFDataGetTypeID()) r.assign(reinterpret_cast<const char*>(cf.CFDataGetBytePtr(v)), std::size_t(cf.CFDataGetLength(v)));
                        cf.CFRelease(v); return r; };
                    auto rd32 = [&](unsigned e, const char* k) -> std::uint32_t { auto key = cf.str(k); auto v = prop(e, key, nullptr, 0); cf.CFRelease(key); if (!v) return 0;
                        std::uint32_t r = 0; if (cf.CFGetTypeID(v) == cf.CFDataGetTypeID() && cf.CFDataGetLength(v) >= 4) std::memcpy(&r, cf.CFDataGetBytePtr(v), 4); cf.CFRelease(v); return r; };
                    g.name = detail::trim(rd(src, "model").c_str()); if (g.name.empty()) g.name = detail::trim(rd(s, "model").c_str());
                    g.vendor_id = rd32(src, "vendor-id"); g.device_id = rd32(src, "device-id");
                    if (!g.vendor_id && g.driver.rfind("AGX", 0) == 0) g.vendor_id = 0x106b;
                    g.vendor = gpu_vendor_name(g.vendor_id); g.integrated = g.vendor_id == 0x106b || g.vendor_id == 0x8086;
                    {   // discrete GPUs on Intel Macs publish their VRAM on the PCI device; Apple silicon memory is unified (0)
                        auto key = cf.str("VRAM,totalMB"); auto v = prop(src, key, nullptr, 0); cf.CFRelease(key);
                        if (v) { long long mb = cf.to_ll(v); cf.CFRelease(v); if (mb > 0) g.dedicated_memory = std::uint64_t(mb) << 20; }
                    }
                    if (g.name.empty()) g.name = g.vendor_id == 0x106b ? "Apple GPU" : g.driver;
                    out.push_back(std::move(g)); if (pci) rel(pci); rel(s);
                }
                rel(it);
            }
        }
    }
#elif defined(FORGE_OS_IOS)
    gpu_device g; g.vendor_id = 0x106b; g.vendor = "Apple"; g.name = "Apple GPU (" + detail::sysctl_str("hw.machine") + ")"; g.integrated = true; g.driver = "agx"; out.push_back(g);
#elif defined(FORGE_OS_FREEBSD)
    int fd = ::open("/dev/pci", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        std::vector<pci_conf> conf(256); pci_conf_io pc{}; pc.num_patterns = 0; pc.matches = conf.data(); pc.match_buf_len = std::uint32_t(conf.size() * sizeof(pci_conf));
        do {   // the kernel hands out one buffer-full per call; pc.offset carries on from where it stopped
            if (::ioctl(fd, PCIOCGETCONF, &pc) || pc.status == PCI_GETCONF_LIST_CHANGED || pc.status == PCI_GETCONF_ERROR) break;
            for (std::uint32_t i = 0; i < pc.num_matches; ++i) {
                auto& c = conf[i]; if (c.pc_class != 0x03) continue;
                gpu_device g; g.vendor_id = c.pc_vendor; g.device_id = c.pc_device; g.vendor = gpu_vendor_name(g.vendor_id); g.driver = c.pd_name;
                char b[64]; std::snprintf(b, sizeof b, "pci%d:%d:%d:%d", c.pc_sel.pc_domain, c.pc_sel.pc_bus, c.pc_sel.pc_dev, c.pc_sel.pc_func); g.bus = b;
                std::snprintf(b, sizeof b, "%s GPU %04x", g.vendor.c_str(), g.device_id); g.name = b;
                g.integrated = gpu_integrated_guess(g.vendor_id, g.device_id, c.pc_sel.pc_domain == 0 ? int(c.pc_sel.pc_bus) : 1, int(c.pc_sel.pc_dev), 0);
                out.push_back(std::move(g));
            }
        } while (pc.status == PCI_GETCONF_MORE_DEVS);
        ::close(fd);
    }
#endif
    return out;
}
} // namespace forge::os
