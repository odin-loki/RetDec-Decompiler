#pragma once
// forge::os devices: read-only enumeration of block devices, PCI functions and USB devices.
//   Linux/Android: sysfs.  Windows: \\.\PhysicalDriveN storage IOCTLs (no admin: zero-access opens) with drive
//   letters as partitions; PCI/USB through SetupAPI loaded from setupapi.dll at run time.  macOS: the IOKit registry
//   (IOMedia, IOPCIDevice, IOUSBHostDevice), late-bound like the rest of forge::os.  FreeBSD: GEOM (kern.geom.conftxt
//   and confxml sysctls, no privileges), /dev/pci PCIOCGETCONF, and the newbus device tree (hw.bus.devices) for USB.
//   iOS has no device registry access: unsupported.  Ids use the Linux sysfs spelling everywhere: pci "0x8086"
//   class "0x030000" address "0000:00:02.0"; usb "046d".  Guide: docs/os-wrapper.md.
#include <forge/os/detail.hpp>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <map>
#if defined(FORGE_OS_WINDOWS)
#include <winioctl.h>
#elif defined(FORGE_OS_FREEBSD)
#include <sys/ioctl.h>
#include <sys/pciio.h>
#include <sys/bus.h>
#endif

namespace forge::os::devices {
struct block_device { std::string name, model, vendor, serial; std::uint64_t size_bytes = 0; bool removable = false, rotational = false, read_only = false; std::vector<std::string> partitions; };
struct pci_device { std::string address, vendor_id, device_id, class_id, driver; };
struct usb_device { std::string path, vendor_id, product_id, manufacturer, product, serial; int bus = 0, devnum = 0; };

// FreeBSD GEOM naming: is `part` a partition of `disk`? The name continues with 'p' (GPT: ada0p1) or 's' (MBR
// slice: da0s1, da0s1a), then a digit - so ada1 does not claim ada10p1.
inline bool geom_partition_of(const std::string& disk, const std::string& part) {
    return part.size() > disk.size() + 1 && part.compare(0, disk.size(), disk) == 0 && (part[disk.size()] == 'p' || part[disk.size()] == 's') &&
           std::isdigit(static_cast<unsigned char>(part[disk.size() + 1]));
}
// Disks and their partitions out of kern.geom.conftxt ("<depth> <class> <name> <mediasize> <sectorsize> ...").
inline std::vector<block_device> geom_conftxt_disks(const std::string& txt) {
    std::vector<block_device> out; std::vector<std::string> parts;
    for (std::size_t p = 0; p < txt.size();) {
        auto e = txt.find('\n', p); std::string line = txt.substr(p, e == std::string::npos ? std::string::npos : e - p); p = e == std::string::npos ? txt.size() : e + 1;
        char cls[32] = {}, nm[256] = {}; unsigned long long sz = 0; int depth = 0;
        if (std::sscanf(line.c_str(), "%d %31s %255s %llu", &depth, cls, nm, &sz) < 3) continue;
        if (!std::strcmp(cls, "DISK")) { block_device d; d.name = nm; d.size_bytes = sz; out.push_back(std::move(d)); }
        else if (!std::strcmp(cls, "PART")) parts.push_back(nm);
    }
    for (auto& d : out) {
        for (auto& s : parts) if (geom_partition_of(d.name, s)) d.partitions.push_back(s);
        std::sort(d.partitions.begin(), d.partitions.end());
    }
    return out;
}
inline std::string hex_(unsigned v, int digits, bool prefix) { char b[16]; std::snprintf(b, sizeof b, prefix ? "0x%0*x" : "%0*x", digits, v); return b; }
inline std::string pci_addr_(unsigned dom, unsigned bus, unsigned dev, unsigned fn) { char b[32]; std::snprintf(b, sizeof b, "%04x:%02x:%02x.%x", dom, bus, dev, fn); return b; }

#if defined(__linux__)
inline std::string sysfs_read_(const std::string& p) {
    std::ifstream f(p); std::string s; std::getline(f, s);
    while (!s.empty() && (s.back() == '\n' || s.back() == ' ' || s.back() == '\r')) s.pop_back();
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    return s;
}
inline std::vector<std::string> sysfs_list_(const std::string& dir) {
    std::vector<std::string> v; auto l = list_dir(dir); if (!l) return v;
    for (auto& e : l.value) if (e.name != "." && e.name != "..") v.push_back(e.name);
    std::sort(v.begin(), v.end()); return v;
}
#elif defined(FORGE_OS_WINDOWS)
// SetupAPI, late-bound (own declarations: no setupapi.h, no link library).
struct setupapi_ {
    struct devinfo { DWORD cbSize; GUID ClassGuid; DWORD DevInst; ULONG_PTR Reserved; };
    struct propkey { GUID fmtid; ULONG pid; };
    HANDLE (WINAPI* classdevs)(const GUID*, PCWSTR, HWND, DWORD) = nullptr;
    BOOL (WINAPI* enum_info)(HANDLE, DWORD, devinfo*) = nullptr;
    BOOL (WINAPI* reg_prop)(HANDLE, devinfo*, DWORD, DWORD*, BYTE*, DWORD, DWORD*) = nullptr;
    BOOL (WINAPI* instance_id)(HANDLE, devinfo*, PWSTR, DWORD, DWORD*) = nullptr;
    BOOL (WINAPI* dev_prop)(HANDLE, devinfo*, const propkey*, ULONG*, BYTE*, DWORD, DWORD*, DWORD) = nullptr;
    BOOL (WINAPI* destroy)(HANDLE) = nullptr;
    static const setupapi_& api() {
        static setupapi_ a = [] {
            setupapi_ s; HMODULE h = LoadLibraryExW(L"setupapi.dll", nullptr, 0x800 /*LOAD_LIBRARY_SEARCH_SYSTEM32*/); if (!h) return s;
            auto f = [&](auto& p, const char* n) { p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(reinterpret_cast<void*>(GetProcAddress(h, n))); };
            f(s.classdevs, "SetupDiGetClassDevsW"); f(s.enum_info, "SetupDiEnumDeviceInfo"); f(s.reg_prop, "SetupDiGetDeviceRegistryPropertyW");
            f(s.instance_id, "SetupDiGetDeviceInstanceIdW"); f(s.dev_prop, "SetupDiGetDevicePropertyW"); f(s.destroy, "SetupDiDestroyDeviceInfoList");
            return s;
        }();
        return a;
    }
    bool ok() const { return classdevs && enum_info && reg_prop && instance_id && destroy; }
    // Every present device the given enumerator ("PCI", "USB") reports.
    template <class F> bool each(const wchar_t* enumerator, F f) const {
        HANDLE set = classdevs(nullptr, enumerator, nullptr, 0x2 /*DIGCF_PRESENT*/ | 0x4 /*DIGCF_ALLCLASSES*/);
        if (set == INVALID_HANDLE_VALUE) return false;
        devinfo d{}; d.cbSize = sizeof d;
        for (DWORD i = 0; enum_info(set, i, &d); ++i) f(set, d);
        destroy(set); return true;
    }
    // REG_SZ / REG_MULTI_SZ property as its strings.
    std::vector<std::string> strs(HANDLE set, devinfo& d, DWORD prop) const {
        std::vector<std::string> v; wchar_t b[2048] = {}; DWORD ty = 0;
        if (!reg_prop(set, &d, prop, &ty, reinterpret_cast<BYTE*>(b), sizeof b - 4, nullptr)) return v;
        for (const wchar_t* p = b; *p; p += wcslen(p) + 1) { v.push_back(narrow(p)); if (ty != REG_MULTI_SZ) break; }
        return v;
    }
    std::string str(HANDLE set, devinfo& d, DWORD prop) const { auto v = strs(set, d, prop); return v.empty() ? std::string() : v[0]; }
    long long num(HANDLE set, devinfo& d, DWORD prop) const { DWORD v = 0, ty = 0; return reg_prop(set, &d, prop, &ty, reinterpret_cast<BYTE*>(&v), sizeof v, nullptr) && ty == REG_DWORD ? (long long)v : -1; }
    std::string id(HANDLE set, devinfo& d) const { wchar_t b[512] = {}; return instance_id(set, &d, b, 512, nullptr) ? narrow(b) : std::string(); }
    std::string devprop_str(HANDLE set, devinfo& d, const propkey& k) const {
        wchar_t b[512] = {}; ULONG ty = 0;
        return dev_prop && dev_prop(set, &d, &k, &ty, reinterpret_cast<BYTE*>(b), sizeof b - 2, nullptr, 0) && ty == 0x12 /*DEVPROP_TYPE_STRING*/ ? narrow(b) : std::string();
    }
};
// "PCI\VEN_8086&DEV_A780&..." -> the hex digits after `key`, lowercased ("" if absent).
inline std::string hwid_field_(const std::string& id, const char* key, std::size_t digits) {
    auto p = id.find(key); if (p == std::string::npos) return {};
    std::string s = id.substr(p + std::strlen(key), digits); if (s.size() != digits) return {};
    for (auto& c : s) { if (!std::isxdigit(static_cast<unsigned char>(c))) return {}; c = char(std::tolower(static_cast<unsigned char>(c))); }
    return s;
}
inline std::string trim_ascii_(const char* b, std::size_t n) { std::string s(b, strnlen(b, n)); return detail::trim(s); }
#elif defined(FORGE_OS_MACOS)
// IOKit, late-bound (io_object_t etc. are mach ports: unsigned int).
struct iokit_ {
    const void* (*matching)(const char*) = nullptr;
    int (*services)(unsigned, const void*, unsigned*) = nullptr;
    unsigned (*next)(unsigned) = nullptr;
    int (*release)(unsigned) = nullptr;
    const void* (*prop)(unsigned, const void*, const void*, unsigned) = nullptr;
    const void* (*search)(unsigned, const char*, const void*, const void*, unsigned) = nullptr;
    int (*child)(unsigned, const char*, unsigned*) = nullptr;
    int (*cls)(unsigned, char*) = nullptr;
    static const iokit_& api() {
        static iokit_ a = [] {
            iokit_ k; void* h = ::dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_LAZY); if (!h) return k;
            auto f = [&](auto& p, const char* n) { p = reinterpret_cast<std::remove_reference_t<decltype(p)>>(::dlsym(h, n)); };
            f(k.matching, "IOServiceMatching"); f(k.services, "IOServiceGetMatchingServices"); f(k.next, "IOIteratorNext"); f(k.release, "IOObjectRelease");
            f(k.prop, "IORegistryEntryCreateCFProperty"); f(k.search, "IORegistryEntrySearchCFProperty"); f(k.child, "IORegistryEntryGetChildEntry"); f(k.cls, "IOObjectGetClass");
            return k;
        }();
        return a;
    }
    bool ok() const { return matching && services && next && release && prop && detail::cf_api::get().ok; }
    // Every registered service of an IOKit class; returns how many were visited.
    template <class F> int each(const char* klass, F f) const {
        unsigned it = 0; int n = 0; if (services(0 /*kIOMainPortDefault*/, matching(klass), &it)) return 0;
        while (unsigned s = next(it)) { f(s); release(s); ++n; }
        release(it); return n;
    }
    // Property of the entry; up=true also searches its parents (kIORegistryIterateRecursively | kIORegistryIterateParents).
    const void* get(unsigned e, const char* k, bool up = false) const {
        auto& cf = detail::cf_api::get(); auto key = cf.str(k);
        auto v = up ? (search ? search(e, "IOService", key, nullptr, 3) : nullptr) : prop(e, key, nullptr, 0);
        cf.CFRelease(key); return v;
    }
    std::string str(unsigned e, const char* k, bool up = false) const { auto& cf = detail::cf_api::get(); auto v = get(e, k, up); if (!v) return {}; auto s = cf.to_string(v); cf.CFRelease(v); return detail::trim(s); }
    long long num(unsigned e, const char* k, bool up = false) const { auto& cf = detail::cf_api::get(); auto v = get(e, k, up); if (!v) return -1; auto n = cf.to_ll(v); cf.CFRelease(v); return n; }
    int flag(unsigned e, const char* k) const { auto& cf = detail::cf_api::get(); auto v = get(e, k); if (!v) return -1; int r = v == cf.kCFBooleanTrue; cf.CFRelease(v); return r; }
    std::uint32_t data32(unsigned e, const char* k) const {   // PCI ids: little-endian 4-byte CFData
        auto& cf = detail::cf_api::get(); auto v = get(e, k); if (!v) return 0; std::uint32_t r = 0;
        if (cf.CFGetTypeID(v) == cf.CFDataGetTypeID() && cf.CFDataGetLength(v) >= 4) std::memcpy(&r, cf.CFDataGetBytePtr(v), 4);
        cf.CFRelease(v); return r;
    }
    std::string dict_str(unsigned e, const char* dict, const char* k) const {   // e.g. "Device Characteristics" -> "Product Name"
        auto& cf = detail::cf_api::get(); auto d = get(e, dict, true); if (!d) return {};
        auto key = cf.str(k); auto v = cf.CFDictionaryGetValue(d, key); cf.CFRelease(key);
        auto s = cf.to_string(v); cf.CFRelease(d); return detail::trim(s);
    }
};
#elif defined(FORGE_OS_FREEBSD)
inline std::string xml_unescape_(std::string s) {
    static const std::pair<const char*, char> m[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'}, {"&apos;", '\''}, {"&amp;", '&'}};
    for (auto& [k, c] : m) for (std::size_t p; (p = s.find(k)) != std::string::npos;) s.replace(p, std::strlen(k), 1, c);
    return s;
}
inline std::string xml_tag_(const std::string& s, const char* tag) {   // first <tag>text</tag> in s
    std::string o = std::string("<") + tag + ">", c = std::string("</") + tag + ">";
    auto a = s.find(o); if (a == std::string::npos) return {}; a += o.size(); auto b = s.find(c, a); if (b == std::string::npos) return {};
    return detail::trim(xml_unescape_(s.substr(a, b - a)));
}
// key=value out of a newbus pnpinfo/location string ("vendor=0x80ee product=0x0021 sernum=\"x\"").
inline std::string kv_(const std::string& s, const std::string& k) {
    std::size_t p = 0;
    while ((p = s.find(k + "=", p)) != std::string::npos) {
        if (p && s[p - 1] != ' ') { ++p; continue; }
        p += k.size() + 1;
        if (p < s.size() && s[p] == '"') { auto e = s.find('"', p + 1); return s.substr(p + 1, e == std::string::npos ? std::string::npos : e - p - 1); }
        return s.substr(p, s.find(' ', p) == std::string::npos ? std::string::npos : s.find(' ', p) - p);
    }
    return {};
}
#endif

inline result<std::vector<block_device>> block() {
#if defined(__linux__)
    std::vector<block_device> out;
    for (auto& n : sysfs_list_("/sys/block")) {
        if (n.rfind("loop", 0) == 0 || n.rfind("ram", 0) == 0 || n.rfind("zram", 0) == 0) continue;
        std::string b = "/sys/block/" + n;
        block_device d; d.name = n;
        d.size_bytes = std::strtoull(sysfs_read_(b + "/size").c_str(), nullptr, 10) * 512ull;
        d.removable = sysfs_read_(b + "/removable") == "1"; d.rotational = sysfs_read_(b + "/queue/rotational") == "1"; d.read_only = sysfs_read_(b + "/ro") == "1";
        d.model = sysfs_read_(b + "/device/model"); d.vendor = sysfs_read_(b + "/device/vendor"); d.serial = sysfs_read_(b + "/device/serial");
        for (auto& p : sysfs_list_(b)) if (p.rfind(n, 0) == 0 && stat(b + "/" + p + "/partition").exists) d.partitions.push_back(p);
        out.push_back(std::move(d));
    }
    return {std::move(out), {}};
#elif defined(FORGE_OS_WINDOWS)
    // Disk number -> the drive letters on it (a volume can span disks).
    std::map<DWORD, std::vector<std::string>> letters;
    wchar_t drives[512] = {}; DWORD dn = GetLogicalDriveStringsW(511, drives);
    for (const wchar_t* d = drives; dn && dn < 512 && *d; d += wcslen(d) + 1) {
        std::wstring vol = L"\\\\.\\" + std::wstring(d, 2);
        HANDLE h = CreateFileW(vol.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        alignas(8) BYTE b[sizeof(VOLUME_DISK_EXTENTS) + 15 * sizeof(DISK_EXTENT)] = {}; DWORD got = 0;
        if (DeviceIoControl(h, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, b, sizeof b, &got, nullptr)) {
            auto* e = reinterpret_cast<VOLUME_DISK_EXTENTS*>(b);
            for (DWORD i = 0; i < e->NumberOfDiskExtents && i < 16; ++i) letters[e->Extents[i].DiskNumber].push_back(narrow(std::wstring(d, 2)));
        }
        CloseHandle(h);
    }
    std::vector<block_device> out;
    for (DWORD i = 0; i < 64; ++i) {   // zero-access opens: storage-property IOCTLs are FILE_ANY_ACCESS, no admin needed
        HANDLE h = CreateFileW((L"\\\\.\\PhysicalDrive" + std::to_wstring(i)).c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) continue;
        block_device d; d.name = "PhysicalDrive" + std::to_string(i); DWORD got = 0;
        alignas(8) BYTE g[sizeof(DISK_GEOMETRY_EX) + 256] = {};
        if (DeviceIoControl(h, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, g, sizeof g, &got, nullptr)) d.size_bytes = std::uint64_t(reinterpret_cast<DISK_GEOMETRY_EX*>(g)->DiskSize.QuadPart);
        STORAGE_PROPERTY_QUERY q{}; q.PropertyId = StorageDeviceProperty; q.QueryType = PropertyStandardQuery;
        alignas(8) char sd[1024] = {};
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, sd, sizeof sd, &got, nullptr) && got >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
            auto* s = reinterpret_cast<STORAGE_DEVICE_DESCRIPTOR*>(sd);
            auto at = [&](DWORD off) { return off && off < got ? trim_ascii_(sd + off, got - off) : std::string(); };
            d.vendor = at(s->VendorIdOffset); d.model = at(s->ProductIdOffset); d.serial = at(s->SerialNumberOffset); d.removable = s->RemovableMedia != 0;
        }
        q.PropertyId = StorageDeviceSeekPenaltyProperty; DEVICE_SEEK_PENALTY_DESCRIPTOR sp{};
        if (DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &q, sizeof q, &sp, sizeof sp, &got, nullptr) && got >= sizeof sp) d.rotational = sp.IncursSeekPenalty != 0;
        if (!DeviceIoControl(h, IOCTL_DISK_IS_WRITABLE, nullptr, 0, nullptr, 0, &got, nullptr) && GetLastError() == ERROR_WRITE_PROTECT) d.read_only = true;
        if (auto l = letters.find(i); l != letters.end()) d.partitions = l->second;
        CloseHandle(h); out.push_back(std::move(d));
    }
    return {std::move(out), {}};
#elif defined(FORGE_OS_MACOS)
    auto& io = iokit_::api(); if (!io.ok()) return {{}, unsupported("block devices (IOKit not loadable)")};
    std::vector<block_device> out; std::vector<std::string> slices;
    io.each("IOMedia", [&](unsigned m) {
        std::string bsd = io.str(m, "BSD Name"); if (bsd.empty()) return;
        if (io.flag(m, "Whole") != 1) { slices.push_back(bsd); return; }
        block_device d; d.name = bsd; d.size_bytes = std::uint64_t(std::max(0ll, io.num(m, "Size")));
        d.removable = io.flag(m, "Removable") == 1 || io.flag(m, "Ejectable") == 1; d.read_only = io.flag(m, "Writable") == 0;
        d.model = io.dict_str(m, "Device Characteristics", "Product Name"); d.vendor = io.dict_str(m, "Device Characteristics", "Vendor Name");
        d.serial = io.dict_str(m, "Device Characteristics", "Serial Number");
        d.rotational = io.dict_str(m, "Device Characteristics", "Medium Type") == "Rotational";
        out.push_back(std::move(d));
    });
    for (auto& d : out) {   // disk0 -> disk0s1, disk0s2 (not disk01)
        for (auto& s : slices) if (s.size() > d.name.size() + 1 && s.rfind(d.name, 0) == 0 && s[d.name.size()] == 's') d.partitions.push_back(s);
        std::sort(d.partitions.begin(), d.partitions.end());
    }
    return {std::move(out), {}};
#elif defined(FORGE_OS_FREEBSD)
    // conftxt: "<depth> <class> <name> <mediasize> <sectorsize> ..." per geom; confxml adds the DISK descr/ident/rotationrate.
    auto txt = detail::sysctl_str("kern.geom.conftxt"); if (txt.empty()) return {{}, last_error("sysctl kern.geom.conftxt")};
    auto out = geom_conftxt_disks(txt);
    auto xml = detail::sysctl_str("kern.geom.confxml");
    std::string disk; if (auto a = xml.find("<name>DISK</name>"); a != std::string::npos) disk = xml.substr(a, xml.find("<class id=", a) - a);
    for (auto& d : out) {
        if (auto g = disk.find("<name>" + d.name + "</name>"); g != std::string::npos) {
            auto end = disk.find("<geom id=", g); std::string gx = disk.substr(g, end == std::string::npos ? std::string::npos : end - g);
            d.model = xml_tag_(gx, "descr"); d.serial = xml_tag_(gx, "ident");
            auto rr = xml_tag_(gx, "rotationrate"); d.rotational = !rr.empty() && std::atoi(rr.c_str()) > 1;   // 0 unknown, 1 non-rotating
        }
        d.removable = d.name.rfind("cd", 0) == 0; d.read_only = d.removable;
    }
    return {std::move(out), {}};
#else
    return {{}, unsupported("block device enumeration")};
#endif
}
inline result<std::vector<pci_device>> pci() {
#if defined(__linux__)
    std::vector<pci_device> out; std::string root = "/sys/bus/pci/devices";
    if (!stat(root).is_dir) return {{}, unsupported("pci (no /sys/bus/pci)")};
    std::size_t listed = 0;
    for (auto& a : sysfs_list_(root)) {
        ++listed;
        std::string b = root + "/" + a; pci_device d; d.address = a;
        d.vendor_id = sysfs_read_(b + "/vendor"); d.device_id = sysfs_read_(b + "/device"); d.class_id = sysfs_read_(b + "/class");
        if (d.vendor_id.empty()) continue;   // attributes not readable (Android SELinux denies them to apps and shell)
        char lk[512]; auto n = ::readlink((b + "/driver").c_str(), lk, sizeof lk - 1); if (n > 0) d.driver = basename(std::string(lk, std::size_t(n)));
        out.push_back(std::move(d));
    }
    if (listed && out.empty()) return {{}, unsupported("pci (sysfs device attributes not readable)")};
    return {std::move(out), {}};
#elif defined(FORGE_OS_WINDOWS)
    auto& sa = setupapi_::api(); if (!sa.ok()) return {{}, unsupported("pci (setupapi.dll not loadable)")};
    std::vector<pci_device> out;
    bool ok = sa.each(L"PCI", [&](HANDLE set, setupapi_::devinfo& di) {
        auto hw = sa.strs(set, di, 1 /*SPDRP_HARDWAREID*/); if (hw.empty()) return;
        pci_device d; auto v = hwid_field_(hw[0], "VEN_", 4), dv = hwid_field_(hw[0], "DEV_", 4); if (v.empty()) return;
        d.vendor_id = "0x" + v; d.device_id = "0x" + dv;
        for (auto& c : sa.strs(set, di, 2 /*SPDRP_COMPATIBLEIDS*/)) if (auto cc = hwid_field_(c, "CC_", 6); !cc.empty()) { d.class_id = "0x" + cc; break; }
        long long bus = sa.num(set, di, 0x15 /*SPDRP_BUSNUMBER*/), adr = sa.num(set, di, 0x1C /*SPDRP_ADDRESS: device << 16 | function*/);
        if (bus >= 0 && adr >= 0) d.address = pci_addr_(0, unsigned(bus), unsigned(adr >> 16) & 0x1f, unsigned(adr) & 7); else d.address = sa.id(set, di);
        d.driver = sa.str(set, di, 4 /*SPDRP_SERVICE*/);
        out.push_back(std::move(d));
    });
    if (!ok) return {{}, last_error("SetupDiGetClassDevs PCI")};
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.address < b.address; });
    return {std::move(out), {}};
#elif defined(FORGE_OS_MACOS)
    auto& io = iokit_::api(); if (!io.ok()) return {{}, unsupported("pci (IOKit not loadable)")};
    std::vector<pci_device> out;
    io.each("IOPCIDevice", [&](unsigned e) {
        pci_device d; std::uint32_t v = io.data32(e, "vendor-id"); if (!v) return;
        d.vendor_id = hex_(v & 0xffff, 4, true); d.device_id = hex_(io.data32(e, "device-id") & 0xffff, 4, true); d.class_id = hex_(io.data32(e, "class-code") & 0xffffff, 6, true);
        unsigned b = 0, dv = 0, f = 0;   // "pcidebug" = "bus:device:function"
        if (std::sscanf(io.str(e, "pcidebug").c_str(), "%u:%u:%u", &b, &dv, &f) == 3) d.address = pci_addr_(0, b, dv, f);
        unsigned c = 0; char cn[128] = {};
        if (io.child && io.cls && io.child(e, "IOService", &c) == 0 && c) { io.cls(c, cn); d.driver = cn; io.release(c); }
        out.push_back(std::move(d));
    });
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.address < b.address; });
    return {std::move(out), {}};
#elif defined(FORGE_OS_FREEBSD)
    int fd = ::open("/dev/pci", O_RDONLY | O_CLOEXEC); if (fd < 0) return {{}, last_error("open /dev/pci")};
    std::vector<pci_device> out; std::vector<pci_conf> conf(256); pci_conf_io pc{};
    pc.matches = conf.data(); pc.match_buf_len = std::uint32_t(conf.size() * sizeof(pci_conf));
    do {
        if (::ioctl(fd, PCIOCGETCONF, &pc) || pc.status == PCI_GETCONF_LIST_CHANGED || pc.status == PCI_GETCONF_ERROR) { auto e = last_error("PCIOCGETCONF"); ::close(fd); return {{}, e}; }
        for (std::uint32_t i = 0; i < pc.num_matches; ++i) {
            auto& c = conf[i]; pci_device d; d.address = pci_addr_(c.pc_sel.pc_domain, c.pc_sel.pc_bus, c.pc_sel.pc_dev, c.pc_sel.pc_func);
            d.vendor_id = hex_(c.pc_vendor, 4, true); d.device_id = hex_(c.pc_device, 4, true); d.class_id = hex_(unsigned(c.pc_class) << 16 | unsigned(c.pc_subclass) << 8 | c.pc_progif, 6, true);
            d.driver = c.pd_name; out.push_back(std::move(d));
        }
    } while (pc.status == PCI_GETCONF_MORE_DEVS);
    ::close(fd); return {std::move(out), {}};
#else
    return {{}, unsupported("pci enumeration")};
#endif
}
inline result<std::vector<usb_device>> usb() {
#if defined(__linux__)
    std::vector<usb_device> out; std::string root = "/sys/bus/usb/devices";
    if (!stat(root).is_dir) return {{}, unsupported("usb (no /sys/bus/usb)")};
    for (auto& a : sysfs_list_(root)) {
        if (a.find(':') != std::string::npos) continue;   // interfaces, not devices
        std::string b = root + "/" + a; usb_device d; d.path = a;
        d.vendor_id = sysfs_read_(b + "/idVendor"); d.product_id = sysfs_read_(b + "/idProduct"); if (d.vendor_id.empty()) continue;
        d.manufacturer = sysfs_read_(b + "/manufacturer"); d.product = sysfs_read_(b + "/product"); d.serial = sysfs_read_(b + "/serial");
        d.bus = std::atoi(sysfs_read_(b + "/busnum").c_str()); d.devnum = std::atoi(sysfs_read_(b + "/devnum").c_str());
        out.push_back(std::move(d));
    }
    return {std::move(out), {}};
#elif defined(FORGE_OS_WINDOWS)
    // Devices only: "USB\VID_xxxx&PID_xxxx\<serial or instance>"; interfaces (&MI_xx), child functions and root hubs are skipped.
    // path is the device instance id; bus is the hub number and devnum the hub port (Windows has no USB address).
    auto& sa = setupapi_::api(); if (!sa.ok()) return {{}, unsupported("usb (setupapi.dll not loadable)")};
    std::vector<usb_device> out;
    bool ok = sa.each(L"USB", [&](HANDLE set, setupapi_::devinfo& di) {
        std::string id = sa.id(set, di);
        if (id.size() < 22 || id.find('\\', 4) != 21) return;   // "USB\VID_xxxx&PID_xxxx\": not &MI_ interfaces or &LAMPARRAY-style children
        usb_device d; d.vendor_id = hwid_field_(id, "VID_", 4); d.product_id = hwid_field_(id, "PID_", 4); if (d.vendor_id.empty()) return;
        d.path = id; auto tail = id.substr(id.rfind('\\') + 1); if (tail.find('&') == std::string::npos) d.serial = tail;   // unique serial -> instance id
        static const setupapi_::propkey bus_desc{{0x540b947e, 0x8b40, 0x45bc, {0xa8, 0xa2, 0x6a, 0x0b, 0x89, 0x4c, 0xbd, 0xa2}}, 4};   // DEVPKEY_Device_BusReportedDeviceDesc
        d.product = sa.devprop_str(set, di, bus_desc); if (d.product.empty()) d.product = sa.str(set, di, 0 /*SPDRP_DEVICEDESC*/);
        d.manufacturer = sa.str(set, di, 0xB /*SPDRP_MFG*/); if (!d.manufacturer.empty() && d.manufacturer[0] == '(') d.manufacturer.clear();   // "(Standard USB ...)"
        auto loc = sa.str(set, di, 0xD /*SPDRP_LOCATION_INFORMATION: "Port_#0002.Hub_#0001"*/);
        if (auto h = loc.find("Hub_#"); h != std::string::npos) d.bus = std::atoi(loc.c_str() + h + 5);
        long long port = sa.num(set, di, 0x1C /*SPDRP_ADDRESS*/); d.devnum = port > 0 ? int(port) : 0;
        out.push_back(std::move(d));
    });
    if (!ok) return {{}, last_error("SetupDiGetClassDevs USB")};
    return {std::move(out), {}};
#elif defined(FORGE_OS_MACOS)
    auto& io = iokit_::api(); if (!io.ok()) return {{}, unsupported("usb (IOKit not loadable)")};
    std::vector<usb_device> out;
    auto add = [&](unsigned e) {
        long long v = io.num(e, "idVendor"), p = io.num(e, "idProduct"); if (v < 0) return;
        usb_device d; d.vendor_id = hex_(unsigned(v), 4, false); d.product_id = hex_(unsigned(p < 0 ? 0 : p), 4, false);
        auto s = [&](const char* a, const char* b) { auto r = io.str(e, a); return r.empty() ? io.str(e, b) : r; };
        d.manufacturer = s("USB Vendor Name", "kUSBVendorString"); d.product = s("USB Product Name", "kUSBProductString"); d.serial = s("USB Serial Number", "kUSBSerialNumberString");
        long long loc = io.num(e, "locationID"); if (loc >= 0) { d.path = hex_(unsigned(loc), 8, true); d.bus = int((loc >> 24) & 0xff); }
        long long a = io.num(e, "USB Address"); d.devnum = a > 0 ? int(a) : 0;
        out.push_back(std::move(d));
    };
    if (!io.each("IOUSBHostDevice", add)) io.each("IOUSBDevice", add);   // IOUSBHostDevice: macOS 10.11+
    return {std::move(out), {}};
#elif defined(FORGE_OS_FREEBSD)
    // The newbus tree (what devinfo(8) reads): USB devices carry "vendor=.. product=.." pnpinfo and a "ugen=ugenB.A"
    // location; a device's interfaces share the ugen, so the first one stands for the device.
    int mib[CTL_MAXNAME]; std::size_t ml = CTL_MAXNAME - 2;
    if (::sysctlnametomib("hw.bus.devices", mib, &ml)) return {{}, last_error("sysctl hw.bus.devices")};
    u_businfo bi{}; std::size_t bn = sizeof bi;
    if (::sysctlbyname("hw.bus.info", &bi, &bn, nullptr, 0)) return {{}, last_error("sysctl hw.bus.info")};
    if (bi.ub_version != BUS_USER_VERSION) return {{}, unsupported("usb (newbus ABI version mismatch)")};
    std::vector<usb_device> out; std::vector<std::string> seen; mib[ml] = bi.ub_generation;
    auto u = std::make_unique<u_device>();
    for (int i = 0;; ++i) {
        mib[ml + 1] = i; std::size_t n = sizeof *u;
        if (::sysctl(mib, unsigned(ml + 2), u.get(), &n, nullptr, 0)) break;   // ENOENT: end of the list
        const char* f = u->dv_fields; std::string fl[5];   // name, desc, drivername, pnpinfo, location
        for (auto& x : fl) { if (f >= u->dv_fields + sizeof u->dv_fields) break; x = f; f += x.size() + 1; }
        auto ugen = kv_(fl[4], "ugen"), v = kv_(fl[3], "vendor"); if (ugen.empty() || v.size() < 3) continue;
        if (std::find(seen.begin(), seen.end(), ugen) != seen.end()) continue;
        seen.push_back(ugen);
        usb_device d; d.path = ugen; d.vendor_id = hex_(unsigned(std::strtoul(v.c_str(), nullptr, 16)), 4, false);
        d.product_id = hex_(unsigned(std::strtoul(kv_(fl[3], "product").c_str(), nullptr, 16)), 4, false); d.serial = kv_(fl[3], "sernum");
        d.bus = std::atoi(kv_(fl[4], "bus").c_str()); d.devnum = std::atoi(kv_(fl[4], "devaddr").c_str());
        d.product = fl[1].substr(0, fl[1].find(", class "));   // "<manufacturer> <product>, class 0/0, rev ..."
        out.push_back(std::move(d));
    }
    return {std::move(out), {}};
#else
    return {{}, unsupported("usb enumeration")};
#endif
}
} // namespace forge::os::devices
