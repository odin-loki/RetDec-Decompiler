// forge::os phase 4: devices, clipboard.
#include <forge/os/devices.hpp>
#include <forge/os/clipboard.hpp>
#include "check.hpp"
using namespace forge;

TEST(devices_enumerate) {
    auto b = os::devices::block(); auto p = os::devices::pci(); auto u = os::devices::usb();
    std::printf("  block %s, pci %s, usb %s\n", b ? std::to_string(b->size()).c_str() : b.err.what.c_str(), p ? std::to_string(p->size()).c_str() : p.err.what.c_str(),
                u ? std::to_string(u->size()).c_str() : u.err.what.c_str());
    for (auto& d : b.value) {
        std::printf("  disk %s %llu B '%s' '%s'%s%s%s parts:", d.name.c_str(), (unsigned long long)d.size_bytes, d.vendor.c_str(), d.model.c_str(), d.removable ? " removable" : "", d.rotational ? " rotational" : "", d.read_only ? " ro" : "");
        for (auto& x : d.partitions) std::printf(" %s", x.c_str());
        std::printf("\n"); CHECK(!d.name.empty());
    }
    for (auto& d : p.value) {
        CHECK(d.vendor_id.size() == 6 && d.vendor_id.rfind("0x", 0) == 0); CHECK(d.device_id.rfind("0x", 0) == 0);
        CHECK(d.class_id.empty() || d.class_id.size() == 8); CHECK(!d.address.empty());
    }
    if (!p.value.empty()) { auto& d = p.value[0]; std::printf("  pci %s %s:%s class %s driver '%s'\n", d.address.c_str(), d.vendor_id.c_str(), d.device_id.c_str(), d.class_id.c_str(), d.driver.c_str()); }
    for (auto& d : u.value) {
        CHECK(d.vendor_id.size() == 4 && d.product_id.size() == 4); CHECK(!d.path.empty());
        std::printf("  usb %s %s:%s '%s' '%s' bus %d dev %d\n", d.path.c_str(), d.vendor_id.c_str(), d.product_id.c_str(), d.manufacturer.c_str(), d.product.c_str(), d.bus, d.devnum);
    }
#if defined(__linux__) || defined(_WIN32) || defined(__FreeBSD__) || (defined(__APPLE__) && !TARGET_OS_IPHONE)
    CHECK(b);   // may be empty in containers, but readable
    CHECK(p || os::is_unsupported(p.err)); CHECK(u || os::is_unsupported(u.err));   // Linux containers may lack /sys/bus/*
#if !defined(__linux__)
    CHECK(p); CHECK(u);
#endif
#if defined(_WIN32) || defined(__FreeBSD__) || defined(__APPLE__)
    CHECK(!b.value.empty());   // a real or virtual machine always has a boot disk
#endif
#else
    CHECK(!b ? os::is_unsupported(b.err) : true);
#endif
}
TEST(freebsd_geom_partitions) {   // fixture: ada1 must not claim ada10's partitions, MBR slices and BSD labels count
    const char* conftxt =
        "0 DISK ada1 21474836480 512 hd 16 sc 63\n"
        "1 PART ada1p1 524288 512 i 1 o 20480 ty freebsd-boot xs GPT xt 83bd6b9d\n"
        "1 PART ada1p2 21474000000 512 i 2 o 544768 ty freebsd-ufs xs GPT xt 516e7cb6\n"
        "0 DISK ada10 1000204886016 512 hd 16 sc 63\n"
        "1 PART ada10p1 1000204000000 512 i 1 o 40960 ty freebsd-zfs xs GPT xt 516e7cba\n"
        "0 DISK da0 8053063680 512 hd 255 sc 63\n"
        "1 PART da0s1 8053000000 512 i 1 o 32256 ty freebsd xs MBR xt 165\n"
        "2 PART da0s1a 8000000000 512 i 1 o 0 ty freebsd-ufs xs BSD xt 7\n"
        "0 DISK nvd0 512110190592 512 hd 0 sc 0\n"
        "1 PART nvd0p1 209715200 512 i 1 o 20480 ty efi xs GPT xt c12a7328\n"
        "0 DISK cd0 0 2048 hd 0 sc 0\n";
    auto v = os::devices::geom_conftxt_disks(conftxt);
    CHECK(v.size() == 5); if (v.size() != 5) return;
    CHECK(v[0].name == "ada1" && v[0].size_bytes == 21474836480ull); CHECK((v[0].partitions == std::vector<std::string>{"ada1p1", "ada1p2"}));
    CHECK(v[1].name == "ada10"); CHECK((v[1].partitions == std::vector<std::string>{"ada10p1"}));
    CHECK((v[2].partitions == std::vector<std::string>{"da0s1", "da0s1a"}));
    CHECK((v[3].partitions == std::vector<std::string>{"nvd0p1"})); CHECK(v[4].partitions.empty());
    CHECK(!os::devices::geom_partition_of("ada1", "ada10p1")); CHECK(!os::devices::geom_partition_of("da0", "da0p")); CHECK(os::devices::geom_partition_of("da0", "da0s2"));
}
TEST(clipboard_roundtrip_or_unsupported) {
    if (!os::clipboard::available()) { CHECK(os::is_unsupported(os::clipboard::get().err)); return; }
    auto old = os::clipboard::get();
    CHECK(os::clipboard::set("forge clipboard ✓"));
    auto g = os::clipboard::get(); CHECK(g); CHECK(g.value == "forge clipboard ✓");
    if (old) os::clipboard::set(old.value);
}
