// forge_mobile Android backend (NDK, API 26+).
//   sensors   ASensorManager_getInstanceForPackage + an ASensorEventQueue on a private ALooper thread.
//             Location comes from the app (FusedLocationProvider via JNI -> feed_sensor), battery from
//             BatteryManager-backed sysfs (os::power_status) sampled on the same thread.
//   touch     the app's NativeActivity AInputQueue / GameActivity loop calls android_feed_motion().
//   cameras   ACameraManager (camera2 NDK).
//   display   ANativeWindow/AConfiguration live in the app; it calls report_display(). Vibrate, notify and
//             permission requests go through action_hooks set by the app's JNI glue (they need a Context).
#include "internal.hpp"
#if defined(__ANDROID__)
#include <android/input.h>
#include <android/looper.h>
#include <android/sensor.h>
#include <camera/NdkCameraManager.h>
#include <forge/os/power.hpp>
#include <dlfcn.h>
#include <future>
#include <sys/system_properties.h>

namespace forge::os::mobile {
bool android_feed_motion(const void* p) {
    auto* e = static_cast<const AInputEvent*>(p);
    if (!e || AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return false;
    int32_t a = AMotionEvent_getAction(e), act = a & AMOTION_EVENT_ACTION_MASK;
    std::size_t idx = std::size_t((a & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    std::size_t n = AMotionEvent_getPointerCount(e); auto t = std::uint64_t(AMotionEvent_getEventTime(e));
    auto emit = [&](std::size_t i, touch_phase ph) {
        feed_touch({t, ph, int(AMotionEvent_getPointerId(e, i)), AMotionEvent_getX(e, i), AMotionEvent_getY(e, i), AMotionEvent_getPressure(e, i), int(n)});
    };
    switch (act) {
    case AMOTION_EVENT_ACTION_DOWN: emit(0, touch_phase::down); break;
    case AMOTION_EVENT_ACTION_POINTER_DOWN: emit(idx, touch_phase::down); break;
    case AMOTION_EVENT_ACTION_UP: emit(0, touch_phase::up); break;
    case AMOTION_EVENT_ACTION_POINTER_UP: emit(idx, touch_phase::up); break;
    case AMOTION_EVENT_ACTION_MOVE: for (std::size_t i = 0; i < n; ++i) emit(i, touch_phase::move); break;
    case AMOTION_EVENT_ACTION_CANCEL: emit(0, touch_phase::cancel); break;
    default: return false;
    }
    return true;
}
} // namespace forge::os::mobile

namespace forge::os::mobile::detail {
namespace {
int ndk_type(sensor_kind k) {
    switch (k) {
    case sensor_kind::accelerometer: return ASENSOR_TYPE_ACCELEROMETER;
    case sensor_kind::gyroscope: return ASENSOR_TYPE_GYROSCOPE;
    case sensor_kind::magnetometer: return ASENSOR_TYPE_MAGNETIC_FIELD;
    case sensor_kind::light: return ASENSOR_TYPE_LIGHT;
    case sensor_kind::proximity: return ASENSOR_TYPE_PROXIMITY;
    case sensor_kind::pressure: return ASENSOR_TYPE_PRESSURE;
    case sensor_kind::gravity: return 9;               // ASENSOR_TYPE_GRAVITY
    case sensor_kind::linear_acceleration: return 10;
    case sensor_kind::rotation_vector: return 11;      // v = x,y,z,w of the unit quaternion
    case sensor_kind::humidity: return 12;
    case sensor_kind::temperature: return 13;          // ambient
    case sensor_kind::step_detector: return 18;
    case sensor_kind::step_counter: return 19;
    case sensor_kind::heart_rate: return 21;           // needs BODY_SENSORS
    default: return -1;
    }
}
ASensorManager* manager() {
    static ASensorManager* m = [] {
        using fn = ASensorManager* (*)(const char*);   // API 26; resolved at runtime so older loaders still work
        if (auto f = reinterpret_cast<fn>(dlsym(RTLD_DEFAULT, "ASensorManager_getInstanceForPackage"))) return f("dev.forge");
        return static_cast<ASensorManager*>(nullptr);
    }();
    return m;
}

struct android_backend final : backend {
    std::string name() const override { return "android"; }
    result<device_info> info() override {
        auto d = backend::info(); if (!d) return d;
        auto prop = [](const char* k) { char v[PROP_VALUE_MAX] = {}; __system_property_get(k, v); return std::string(v); };
        d.value.os_name = "Android"; d.value.os_version = prop("ro.build.version.release");
        d.value.manufacturer = prop("ro.product.manufacturer"); d.value.model = prop("ro.product.model");
        auto ch = prop("ro.build.characteristics");
        d.value.form = ch.find("watch") != std::string::npos ? form_factor::watch : ch.find("tv") != std::string::npos ? form_factor::tv
                     : ch.find("automotive") != std::string::npos ? form_factor::car : ch.find("tablet") != std::string::npos ? form_factor::tablet : form_factor::phone;
        return d;
    }
    result<std::vector<sensor_info>> sensors() override {
        std::vector<sensor_info> v; auto* m = manager(); if (!m) return {{}, unsupported("ASensorManager")};
        for (int k = 0; k < sensor_kind_count; ++k) {
            int ty = ndk_type(sensor_kind(k)); if (ty < 0) continue;
            if (const ASensor* s = ASensorManager_getDefaultSensor(m, ty)) {
                int md = ASensor_getMinDelay(s);
                v.push_back({sensor_kind(k), ASensor_getName(s), ASensor_getVendor(s), md > 0 ? 1e6 / md : 0, 0, ASensor_getResolution(s)});
            }
        }
        v.push_back({sensor_kind::battery, "battery", "android", 1, 100, 1});
        return {v, {}};
    }
    result<sensor_stream> open_sensor(sensor_kind k, double hz) override {
        auto q = std::make_shared<queue>(); bridge::get().add_sensor(k, q);
        if (k == sensor_kind::location) return {make_sensor(k, q, nullptr), {}};   // fed by the app via feed_sensor
        auto w = std::make_shared<worker>(); auto period = std::uint64_t(1e9 / hz);
        if (k == sensor_kind::battery) {
            w->start([q, wp = w.get(), period] {
                do { auto p = power_status(); if (p.battery_percent >= 0) q->push({false, {sensor_kind::battery, now_ns(), {double(p.battery_percent), p.charging ? 1.0 : 0.0}, 2}, {}}); } while (wp->sleep_ns(period));
            });
            return {make_sensor(k, q, w), {}};
        }
        auto* m = manager(); if (!m) return {{}, unsupported("ASensorManager")};
        const ASensor* s = ASensorManager_getDefaultSensor(m, ndk_type(k));
        if (!s) return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " (not on this device)")};
        // The queue lives on this worker's ALooper; registration happens there too, and its result comes back here
        // (registerSensor fails, for example, for heart rate without BODY_SENSORS or above the rate limit).
        auto ready = std::make_shared<std::promise<int>>(); auto started = ready->get_future();
        w->start([m, s, q, k, ready, wp = w.get(), us = int32_t(1e6 / hz)] {
            ALooper* lp = ALooper_prepare(0);
            ASensorEventQueue* eq = ASensorManager_createEventQueue(m, lp, 1, nullptr, nullptr);
            if (!eq) { ready->set_value(-1); q->finish(); return; }
            if (int rc = ASensorEventQueue_registerSensor(eq, s, us, 0); rc < 0) { ASensorManager_destroyEventQueue(m, eq); ready->set_value(rc); q->finish(); return; }
            ready->set_value(0);
            while (!wp->stop) {
                int fd, ev; void* data;
                if (ALooper_pollOnce(100, &fd, &ev, &data) != 1) continue;
                ASensorEvent buf[32]; ssize_t n;
                while ((n = ASensorEventQueue_getEvents(eq, buf, 32)) > 0)
                    for (ssize_t i = 0; i < n; ++i) {
                        sensor_event e; e.kind = k; e.t_ns = std::uint64_t(buf[i].timestamp);
                        switch (k) {
                        case sensor_kind::light: e.v[0] = buf[i].light; e.n = 1; break;
                        case sensor_kind::proximity: e.v[0] = buf[i].distance; e.n = 1; break;
                        case sensor_kind::pressure: e.v[0] = buf[i].pressure; e.n = 1; break;
                        case sensor_kind::humidity: case sensor_kind::temperature: case sensor_kind::heart_rate: case sensor_kind::step_detector:
                            e.v[0] = buf[i].data[0]; e.n = 1; break;
                        case sensor_kind::step_counter: e.v[0] = double(buf[i].u64.step_counter); e.n = 1; break;
                        case sensor_kind::rotation_vector:   // x, y, z, w (data[3] = cos(theta/2), API 18+)
                            for (int a = 0; a < 4; ++a) e.v[std::size_t(a)] = buf[i].data[a];
                            e.n = 4; break;
                        default: for (int a = 0; a < 3; ++a) e.v[std::size_t(a)] = buf[i].data[a]; e.n = 3;
                        }
                        q->push({false, e, {}});
                    }
            }
            ASensorEventQueue_disableSensor(eq, s); ASensorManager_destroyEventQueue(m, eq); q->finish();
        });
        if (int rc = started.get(); rc < 0)
            return {{}, {rc == -1 ? 5 : -rc, std::string("sensor ") + sensor_name(k) + (rc == -1 ? ": ASensorManager_createEventQueue failed" : ": ASensorEventQueue_registerSensor failed (permission or rate)")}};
        return {make_sensor(k, q, w), {}};
    }
    result<touch_stream> open_touch() override { auto q = std::make_shared<queue>(); bridge::get().add_touch(q); return {make_touch(q, nullptr), {}}; }
    result<std::vector<camera_info>> cameras() override {
        ACameraManager* cm = ACameraManager_create(); if (!cm) return {{}, unsupported("ACameraManager")};
        std::vector<camera_info> v; ACameraIdList* ids = nullptr;
        if (ACameraManager_getCameraIdList(cm, &ids) == ACAMERA_OK && ids) {
            for (int i = 0; i < ids->numCameras; ++i) {
                camera_info c; c.id = ids->cameraIds[i]; c.name = "camera " + c.id;
                ACameraMetadata* md = nullptr;
                if (ACameraManager_getCameraCharacteristics(cm, ids->cameraIds[i], &md) == ACAMERA_OK && md) {
                    ACameraMetadata_const_entry en{};
                    if (ACameraMetadata_getConstEntry(md, ACAMERA_LENS_FACING, &en) == ACAMERA_OK && en.count) c.front = en.data.u8[0] == ACAMERA_LENS_FACING_FRONT;
                    ACameraMetadata_free(md);
                }
                v.push_back(c);
            }
            ACameraManager_deleteCameraIdList(ids);
        }
        ACameraManager_delete(cm); return {v, {}};
    }
};
} // namespace
std::shared_ptr<backend> make_android() { return std::make_shared<android_backend>(); }
} // namespace forge::os::mobile::detail
#endif
