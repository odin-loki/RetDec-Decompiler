// forge_mobile Apple backends (Objective-C++, ARC): iOS / iPadOS below, then macOS, and the null backend on watchOS.
//   sensors   one shared CMMotionManager (accelerometer, gyro, magnetometer, and deviceMotion for rotation_vector /
//             gravity / linear_acceleration; fanned out per kind), CMAltimeter (pressure), CMPedometer (step_counter,
//             step_detector), UIDevice proximity monitoring, CLLocationManager (location), UIDevice battery.
//             Light: not exposed by iOS.
//   clipboard UIPasteboard (iOS), NSPasteboard (macOS).
//   touch     FORGETouchView (a UIView subclass the app adds or subclasses) forwards UITouch phases.
//   haptics   UIImpactFeedbackGenerator for short pulses on iPhone (amplitude -> intensity); otherwise the
//             AudioServices system vibration.
//   display   UIScreen nativeBounds/scale/maximumFramesPerSecond + UIDevice orientation.
//   cameras   AVCaptureDeviceDiscoverySession.  permissions: AVCaptureDevice / CLLocationManager / UNUserNotificationCenter.
#include "internal.hpp"
#include <algorithm>
#include <array>
#include <TargetConditionals.h>
#if TARGET_OS_IPHONE && !TARGET_OS_WATCH   // iOS, iPadOS, tvOS-less UIKit targets; watchOS uses the null backend + glue
#import <AVFoundation/AVFoundation.h>
#import <AudioToolbox/AudioToolbox.h>
#import <CoreLocation/CoreLocation.h>
#import <CoreMotion/CoreMotion.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <UIKit/UIKit.h>
#import <UserNotifications/UserNotifications.h>

using namespace forge::os::mobile;
using forge::os::mobile::detail::queue;

@interface FORGELocationDelegate : NSObject <CLLocationManagerDelegate>
@property (nonatomic) std::shared_ptr<queue> q;
@end
@implementation FORGELocationDelegate
- (void)locationManager:(CLLocationManager*)m didUpdateLocations:(NSArray<CLLocation*>*)locs {
    for (CLLocation* l in locs) _q->push({false, {sensor_kind::location, detail::now_ns(), {l.coordinate.latitude, l.coordinate.longitude, l.altitude, l.horizontalAccuracy}, 4}, {}});
}
- (void)locationManager:(CLLocationManager*)m didFailWithError:(NSError*)e {
    if (e.code == kCLErrorDenied) _q->finish();   // the user refused (or revoked) access: end the stream
}
@end

// Apps put this view (or a subclass) under their content; every touch reaches forge::os::mobile.
@interface FORGETouchView : UIView
@end
@implementation FORGETouchView
- (instancetype)initWithFrame:(CGRect)f { if ((self = [super initWithFrame:f])) self.multipleTouchEnabled = YES; return self; }
static void forge_send(UIView* v, NSSet<UITouch*>* ts, UIEvent* ev, touch_phase ph) {
    int n = int([ev allTouches].count);
    for (UITouch* t in ts) {
        CGPoint p = [t locationInView:v]; CGFloat s = v.contentScaleFactor;
        double pr = t.maximumPossibleForce > 0 ? t.force / t.maximumPossibleForce : 1.0;
        feed_touch({std::uint64_t(t.timestamp * 1e9), ph, int(reinterpret_cast<std::uintptr_t>((__bridge void*)t) & 0x7fffffff), p.x * s, p.y * s, pr, n});
    }
}
- (void)touchesBegan:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { forge_send(self, t, e, touch_phase::down); }
- (void)touchesMoved:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { forge_send(self, t, e, touch_phase::move); }
- (void)touchesEnded:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { forge_send(self, t, e, touch_phase::up); }
- (void)touchesCancelled:(NSSet<UITouch*>*)t withEvent:(UIEvent*)e { forge_send(self, t, e, touch_phase::cancel); }
@end

namespace forge::os::mobile::detail {
namespace {
template <class F> void on_main(F&& f) { if ([NSThread isMainThread]) f(); else dispatch_sync(dispatch_get_main_queue(), ^{ f(); }); }

// One CMMotionManager per app (Apple's advice) shared by every stream: each motion kind has one handler that fans
// events out to all of that kind's subscribers, runs at the fastest rate any of them asked for, and stops when the
// last subscriber goes. (Starting a second handler would replace the first; stopping per stream would stop both.)
struct motion_hub : std::enable_shared_from_this<motion_hub> {
    CMMotionManager* mm = [CMMotionManager new]; NSOperationQueue* oq = [NSOperationQueue new];
    struct sub { std::weak_ptr<queue> q; double hz; };
    // accelerometer, gyroscope, magnetometer; then the three kinds one deviceMotion handler feeds: rotation_vector,
    // gravity, linear_acceleration (deviceMotion runs while any of those has a subscriber)
    std::mutex m; std::array<std::vector<sub>, 6> subs;
    static int slot(sensor_kind k) {
        switch (k) {
        case sensor_kind::accelerometer: return 0; case sensor_kind::gyroscope: return 1; case sensor_kind::magnetometer: return 2;
        case sensor_kind::rotation_vector: return 3; case sensor_kind::gravity: return 4; default: return 5;   // linear_acceleration
        }
    }
    static bool device_motion(int s) { return s >= 3; }
    bool device_motion_used() const { return !subs[3].empty() || !subs[4].empty() || !subs[5].empty(); }   // with m held
    void fan_out(int s, const sensor_event& e) {
        std::lock_guard g(m);
        for (auto& x : subs[std::size_t(s)]) if (auto q = x.q.lock()) q->push({false, e, {}});
    }
    void set_rate(int s) {   // called with m held
        double hz = 0;
        if (device_motion(s)) { for (std::size_t i = 3; i < 6; ++i) for (auto& x : subs[i]) hz = std::max(hz, x.hz); }
        else for (auto& x : subs[std::size_t(s)]) hz = std::max(hz, x.hz);
        if (hz <= 0) return;
        if (s == 0) mm.accelerometerUpdateInterval = 1.0 / hz; else if (s == 1) mm.gyroUpdateInterval = 1.0 / hz;
        else if (s == 2) mm.magnetometerUpdateInterval = 1.0 / hz; else mm.deviceMotionUpdateInterval = 1.0 / hz;
    }
    void add(sensor_kind k, const std::shared_ptr<queue>& q, double hz) {
        int s = slot(k); const double g = 9.80665;
        std::lock_guard lk(m);
        bool first = device_motion(s) ? !device_motion_used() : subs[std::size_t(s)].empty();
        subs[std::size_t(s)].push_back({q, hz}); set_rate(s);
        if (!first) return;
        std::weak_ptr<motion_hub> weak_hub = weak_from_this();
        if (s == 0) [mm startAccelerometerUpdatesToQueue:oq withHandler:^(CMAccelerometerData* d, NSError*) {
            if (auto h = weak_hub.lock(); h && d) h->fan_out(0, {k, std::uint64_t(d.timestamp * 1e9), {d.acceleration.x * g, d.acceleration.y * g, d.acceleration.z * g}, 3}); }];
        else if (s == 1) [mm startGyroUpdatesToQueue:oq withHandler:^(CMGyroData* d, NSError*) {
            if (auto h = weak_hub.lock(); h && d) h->fan_out(1, {k, std::uint64_t(d.timestamp * 1e9), {d.rotationRate.x, d.rotationRate.y, d.rotationRate.z}, 3}); }];
        else if (s == 2) [mm startMagnetometerUpdatesToQueue:oq withHandler:^(CMMagnetometerData* d, NSError*) {
            if (auto h = weak_hub.lock(); h && d) h->fan_out(2, {k, std::uint64_t(d.timestamp * 1e9), {d.magneticField.x, d.magneticField.y, d.magneticField.z}, 3}); }];
        else [mm startDeviceMotionUpdatesToQueue:oq withHandler:^(CMDeviceMotion* d, NSError*) {
            auto h = weak_hub.lock(); if (!h || !d) return;
            auto t = std::uint64_t(d.timestamp * 1e9); CMQuaternion a = d.attitude.quaternion;
            h->fan_out(3, {sensor_kind::rotation_vector, t, {a.x, a.y, a.z, a.w}, 4});
            h->fan_out(4, {sensor_kind::gravity, t, {d.gravity.x * g, d.gravity.y * g, d.gravity.z * g}, 3});   // g units -> m/s^2, as the accelerometer
            h->fan_out(5, {sensor_kind::linear_acceleration, t, {d.userAcceleration.x * g, d.userAcceleration.y * g, d.userAcceleration.z * g}, 3}); }];
    }
    void remove(sensor_kind k, const queue* q) {
        int s = slot(k);
        std::lock_guard lk(m); auto& v = subs[std::size_t(s)];
        std::erase_if(v, [&](const sub& x) { auto p = x.q.lock(); return !p || p.get() == q; });
        if (device_motion(s) ? device_motion_used() : !v.empty()) { set_rate(s); return; }
        if (s == 0) [mm stopAccelerometerUpdates]; else if (s == 1) [mm stopGyroUpdates]; else if (s == 2) [mm stopMagnetometerUpdates];
        else [mm stopDeviceMotionUpdates];
    }
};

// RAII owner of whatever CoreMotion/CoreLocation object feeds one stream; stops updates on destruction.
// Proximity monitoring is one UIDevice switch: on while any stream uses it (main thread only).
int proximity_users = 0;
struct apple_sub {
    std::shared_ptr<motion_hub> hub; const queue* hq = nullptr;
    CMAltimeter* alt = nil; CLLocationManager* lm = nil; FORGELocationDelegate* del = nil; CMPedometer* ped = nil;
    id proximity = nil;   // NSNotificationCenter observer token
    NSOperationQueue* oq = nil; sensor_kind k{}; std::shared_ptr<worker> battery;
    ~apple_sub() {
        if (hub) hub->remove(k, hq);
        if (alt) [alt stopRelativeAltitudeUpdates];
        if (ped) [ped stopPedometerUpdates];
        if (lm) { CLLocationManager* l = lm; on_main([l] { [l stopUpdatingLocation]; }); }
        if (proximity) {
            id t = proximity;
            on_main([t] {
                [NSNotificationCenter.defaultCenter removeObserver:t];
                if (--proximity_users == 0) UIDevice.currentDevice.proximityMonitoringEnabled = NO;
            });
        }
    }
};

struct apple_backend final : backend {
    std::shared_ptr<motion_hub> hub = std::make_shared<motion_hub>();
    CMMotionManager* mm = hub->mm;
    std::string name() const override { return "ios"; }
    result<std::vector<sensor_info>> sensors() override {
        std::vector<sensor_info> v;
        if (mm.accelerometerAvailable) v.push_back({sensor_kind::accelerometer, "CoreMotion accelerometer", "Apple", 100, 0, 0});
        if (mm.gyroAvailable) v.push_back({sensor_kind::gyroscope, "CoreMotion gyro", "Apple", 100, 0, 0});
        if (mm.magnetometerAvailable) v.push_back({sensor_kind::magnetometer, "CoreMotion magnetometer", "Apple", 100, 0, 0});
        if (mm.deviceMotionAvailable) {
            v.push_back({sensor_kind::rotation_vector, "CoreMotion deviceMotion attitude", "Apple", 100, 1, 0});
            v.push_back({sensor_kind::gravity, "CoreMotion deviceMotion gravity", "Apple", 100, 0, 0});
            v.push_back({sensor_kind::linear_acceleration, "CoreMotion deviceMotion userAcceleration", "Apple", 100, 0, 0});
        }
        if ([CMPedometer isStepCountingAvailable]) {
            v.push_back({sensor_kind::step_counter, "CMPedometer", "Apple", 1, 0, 1});
            v.push_back({sensor_kind::step_detector, "CMPedometer", "Apple", 1, 1, 1});
        }
        if (UIDevice.currentDevice.userInterfaceIdiom == UIUserInterfaceIdiomPhone)   // iPhones have one; enabling it is the only probe
            v.push_back({sensor_kind::proximity, "UIDevice proximity", "Apple", 0, 5, 5});
        if ([CMAltimeter isRelativeAltitudeAvailable]) v.push_back({sensor_kind::pressure, "CMAltimeter", "Apple", 1, 0, 0});
        if ([CLLocationManager locationServicesEnabled]) v.push_back({sensor_kind::location, "CoreLocation", "Apple", 1, 0, 0});
        v.push_back({sensor_kind::battery, "UIDevice battery", "Apple", 1, 100, 1});
        return {v, {}};
    }
    result<sensor_stream> open_sensor(sensor_kind k, double hz) override {
        auto q = std::make_shared<queue>(); bridge::get().add_sensor(k, q);
        auto sub = std::make_shared<apple_sub>(); sub->k = k; sub->oq = [NSOperationQueue new];
        switch (k) {
        case sensor_kind::accelerometer: case sensor_kind::gyroscope: case sensor_kind::magnetometer: {
            bool avail = k == sensor_kind::accelerometer ? mm.accelerometerAvailable : k == sensor_kind::gyroscope ? mm.gyroAvailable : mm.magnetometerAvailable;
            if (!avail) return {{}, unsupported(sensor_name(k))};
            hub->add(k, q, hz); sub->hub = hub; sub->hq = q.get();
            break;
        }
        case sensor_kind::rotation_vector: case sensor_kind::gravity: case sensor_kind::linear_acceleration:
            if (!mm.deviceMotionAvailable) return {{}, unsupported(std::string(sensor_name(k)) + " (no deviceMotion)")};
            hub->add(k, q, hz); sub->hub = hub; sub->hq = q.get();
            break;
        case sensor_kind::step_counter: case sensor_kind::step_detector: {
            // step_counter: steps since boot as far as CoreMotion keeps history (7 days), cumulative; step_detector: 1 per
            // new step since the stream opened (CMPedometer batches them, typically every few seconds).
            if (![CMPedometer isStepCountingAvailable]) return {{}, unsupported("step counting")};
            CMAuthorizationStatus a = [CMPedometer authorizationStatus];
            if (a == CMAuthorizationStatusDenied || a == CMAuthorizationStatusRestricted) return {{}, unsupported("step counting (Motion & Fitness access denied)")};
            double back = k == sensor_kind::step_counter ? std::min(double(NSProcessInfo.processInfo.systemUptime), 7 * 86400.0) : 0.0;
            NSDate* from = [NSDate dateWithTimeIntervalSinceNow:-back];
            auto last = std::make_shared<std::atomic<long long>>(0);
            sub->ped = [CMPedometer new];
            [sub->ped startPedometerUpdatesFromDate:from withHandler:^(CMPedometerData* d, NSError* e) {
                if (e) { q->finish(); return; }   // not authorized / not available: the stream ends
                if (!d) return;
                long long n = d.numberOfSteps.longLongValue;
                if (k == sensor_kind::step_counter) { q->push({false, {k, now_ns(), {double(n)}, 1}, {}}); return; }
                long long prev = last->exchange(n);
                for (long long i = prev; i < n && i - prev < 10000; ++i) q->push({false, {k, now_ns(), {1.0}, 1}, {}});
            }];
            break;
        }
        case sensor_kind::proximity: {
            // v[0]: 0 cm when something is near the screen, 5 cm otherwise (a binary sensor, as on Android).
            bool ok = false; id token = nil;
            void (^changed)(NSNotification*) = ^(NSNotification*) {   // copies q (block literal at function scope)
                q->push({false, {sensor_kind::proximity, now_ns(), {UIDevice.currentDevice.proximityState ? 0.0 : 5.0}, 1}, {}}); };
            on_main([&] {
                UIDevice* d = UIDevice.currentDevice;
                if (proximity_users++ == 0) d.proximityMonitoringEnabled = YES;
                if (!d.proximityMonitoringEnabled) { --proximity_users; return; }   // stays NO without a sensor
                ok = true;
                token = [NSNotificationCenter.defaultCenter addObserverForName:UIDeviceProximityStateDidChangeNotification object:d queue:nil usingBlock:changed];
                changed(nil);   // the current state
            });
            if (!ok) return {{}, unsupported("proximity sensor")};
            sub->proximity = token;
            break;
        }
        case sensor_kind::pressure: {   // braced: a block literal in an unbraced case makes later labels jump into its scope
            if (![CMAltimeter isRelativeAltitudeAvailable]) return {{}, unsupported("barometer")};
            sub->alt = [CMAltimeter new];
            [sub->alt startRelativeAltitudeUpdatesToQueue:sub->oq withHandler:^(CMAltitudeData* d, NSError*) { if (d) q->push({false, {k, std::uint64_t(d.timestamp * 1e9), {d.pressure.doubleValue * 10.0 /* kPa -> hPa */}, 1}, {}}); }];
            break;
        }
        case sensor_kind::location: {
            if (![CLLocationManager locationServicesEnabled]) return {{}, unsupported("location services disabled")};
            auto* del = [FORGELocationDelegate new]; del.q = q; sub->del = del;
            CLLocationManager* lm = nil;   // captured by reference in a C++ lambda (__block cannot be)
            on_main([&] { lm = [CLLocationManager new]; lm.delegate = del; [lm requestWhenInUseAuthorization]; [lm startUpdatingLocation]; });
            sub->lm = lm; break;
        }
        case sensor_kind::battery: {
            sub->battery = std::make_shared<worker>(); auto period = std::uint64_t(1e9 / hz);
            on_main([] { [UIDevice currentDevice].batteryMonitoringEnabled = YES; });
            sub->battery->start([q, wp = sub->battery.get(), period] {
                do { UIDevice* d = [UIDevice currentDevice]; if (d.batteryLevel >= 0) q->push({false, {sensor_kind::battery, now_ns(), {d.batteryLevel * 100.0, d.batteryState == UIDeviceBatteryStateCharging || d.batteryState == UIDeviceBatteryStateFull ? 1.0 : 0.0}, 2}, {}}); } while (wp->sleep_ns(period));
            });
            break;
        }
        default: return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " on iOS")};
        }
        return {make_sensor(k, q, sub), {}};
    }
    result<touch_stream> open_touch() override { auto q = std::make_shared<queue>(); bridge::get().add_touch(q); return {make_touch(q, nullptr), {}}; }
    status vibrate(int ms, double amp) override {
        if (auto h = backend::vibrate(ms, amp); !is_unsupported(h.err)) return h;
        // Short pulses: a Taptic Engine impact (amplitude -> intensity). Longer ones, or devices without haptic
        // feedback support (iPad, iPod), use the system vibration (a fixed ~400 ms buzz where there is a motor).
        bool taptic = [[UIDevice currentDevice] userInterfaceIdiom] == UIUserInterfaceIdiomPhone;
        if (taptic && ms <= 100) on_main([amp] { auto* g = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleHeavy]; [g prepare]; [g impactOccurredWithIntensity:amp]; });
        else AudioServicesPlaySystemSound(kSystemSoundID_Vibrate);
        return {};
    }
    result<display_info> display() override {
        if (auto b = backend::display()) return b;
        display_info d;
        on_main([&] {
            UIScreen* s = [UIScreen mainScreen]; d.width_px = int(s.nativeBounds.size.width); d.height_px = int(s.nativeBounds.size.height);
            d.scale = s.nativeScale; d.dpi = 163.0 * s.nativeScale; d.refresh_hz = double(s.maximumFramesPerSecond);
            switch ([UIDevice currentDevice].orientation) {
            case UIDeviceOrientationPortrait: d.orient = orientation::portrait; break;
            case UIDeviceOrientationPortraitUpsideDown: d.orient = orientation::portrait_upside_down; break;
            case UIDeviceOrientationLandscapeLeft: d.orient = orientation::landscape_left; break;
            case UIDeviceOrientationLandscapeRight: d.orient = orientation::landscape_right; break;
            default: d.orient = orientation::unknown;
            }
        });
        return {d, {}};
    }
    result<std::vector<camera_info>> cameras() override {
        std::vector<camera_info> v;
        auto* ds = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:@[AVCaptureDeviceTypeBuiltInWideAngleCamera, AVCaptureDeviceTypeBuiltInUltraWideCamera, AVCaptureDeviceTypeBuiltInTelephotoCamera]
                                                                         mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified];
        for (AVCaptureDevice* c in ds.devices) v.push_back({c.uniqueID.UTF8String, c.localizedName.UTF8String, c.position == AVCaptureDevicePositionFront});
        return {v, {}};
    }
    result<permission_state> permission_status(permission p) override {
        if (auto b = backend::permission_status(p)) return b;
        switch (p) {
        case permission::camera: switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
            case AVAuthorizationStatusAuthorized: return {permission_state::granted, {}};
            case AVAuthorizationStatusNotDetermined: return {permission_state::not_determined, {}};
            default: return {permission_state::denied, {}}; }
        case permission::location: {
            CLAuthorizationStatus s = [CLLocationManager new].authorizationStatus;
            if (s == kCLAuthorizationStatusNotDetermined) return {permission_state::not_determined, {}};
            if (s == kCLAuthorizationStatusAuthorizedAlways || s == kCLAuthorizationStatusAuthorizedWhenInUse) return {permission_state::granted, {}};
            return {permission_state::denied, {}}; }
        case permission::sensors: switch ([CMMotionActivityManager authorizationStatus]) {
            case CMAuthorizationStatusAuthorized: return {permission_state::granted, {}};
            case CMAuthorizationStatusNotDetermined: return {permission_state::not_determined, {}};
            default: return {permission_state::denied, {}}; }
        case permission::notifications: {
            dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block UNAuthorizationStatus st = UNAuthorizationStatusNotDetermined;
            [[UNUserNotificationCenter currentNotificationCenter] getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* s) { st = s.authorizationStatus; dispatch_semaphore_signal(sem); }];
            dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC * 2));
            return {st == UNAuthorizationStatusNotDetermined ? permission_state::not_determined : st == UNAuthorizationStatusDenied ? permission_state::denied : permission_state::granted, {}}; }
        case permission::vibrate: return {permission_state::not_required, {}};
        }
        return {permission_state::not_required, {}};
    }
    status request_permission(permission p) override {
        if (auto h = backend::request_permission(p); !is_unsupported(h.err)) return h;
        switch (p) {
        case permission::camera: [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL) {}]; return {};
        case permission::location: on_main([] { static CLLocationManager* lm = [CLLocationManager new]; [lm requestWhenInUseAuthorization]; }); return {};
        case permission::notifications:
            [[UNUserNotificationCenter currentNotificationCenter] requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound) completionHandler:^(BOOL, NSError*) {}];
            return {};
        default: return {};
        }
    }
    status notify(std::string_view t, std::string_view b) override {
        if (auto h = backend::notify(t, b); !is_unsupported(h.err)) return h;
        auto* c = [UNMutableNotificationContent new];
        c.title = [[NSString alloc] initWithBytes:t.data() length:t.size() encoding:NSUTF8StringEncoding];
        c.body = [[NSString alloc] initWithBytes:b.data() length:b.size() encoding:NSUTF8StringEncoding];
        auto* r = [UNNotificationRequest requestWithIdentifier:[NSUUID UUID].UUIDString content:c trigger:nil];
        [[UNUserNotificationCenter currentNotificationCenter] addNotificationRequest:r withCompletionHandler:nil];
        return {};
    }
    static NSString* ns(std::string_view s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]; }
    result<double> brightness() override { double v = 0; on_main([&] { v = UIScreen.mainScreen.brightness; }); return {v, {}}; }
    status set_brightness(double l) override { on_main([l] { UIScreen.mainScreen.brightness = CGFloat(l); }); return {}; }
    result<std::string> clipboard_get() override {
        std::string s;
        on_main([&] { NSString* v = UIPasteboard.generalPasteboard.string; const char* u = v ? v.UTF8String : nullptr; if (u) s = u; });
        return {s, {}};
    }
    status clipboard_set(std::string_view t) override {
        NSString* v = ns(t); if (!v) return {{}, {22, "clipboard_set: text is not UTF-8"}};
        on_main([v] { UIPasteboard.generalPasteboard.string = v; }); return {};
    }
    status keep_awake(bool on) override { on_main([on] { UIApplication.sharedApplication.idleTimerDisabled = on; }); return {}; }
    status torch(bool on) override {
        AVCaptureDevice* d = [AVCaptureDevice defaultDeviceWithMediaType:AVMediaTypeVideo];
        if (!d || !d.hasTorch) return {{}, unsupported("torch (no flash)")};
        NSError* e = nil; if (![d lockForConfiguration:&e]) return {{}, {1, "torch: lockForConfiguration failed"}};
        d.torchMode = on ? AVCaptureTorchModeOn : AVCaptureTorchModeOff; [d unlockForConfiguration]; return {};
    }
    status open_url(std::string_view u) override {
        if (auto c = check_url(u); !c) return c;
        NSURL* url = [NSURL URLWithString:ns(u)]; if (!url) return {{}, {22, "open_url: bad URL"}};
        on_main([url] { [UIApplication.sharedApplication openURL:url options:@{} completionHandler:nil]; }); return {};
    }
    result<biometric_info> biometrics() override {
        LAContext* c = [LAContext new]; NSError* e = nil;
        bool ok = [c canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics error:&e];
        // biometryType is set by canEvaluatePolicy, also when nothing is enrolled (LAErrorBiometryNotEnrolled)
        biometric_info b; b.available = c.biometryType != LABiometryTypeNone || (e && e.code == LAErrorBiometryNotEnrolled); b.enrolled = ok;
        b.kind = c.biometryType == LABiometryTypeFaceID ? "face" : c.biometryType == LABiometryTypeTouchID ? "fingerprint" : "";
        return {b, {}};
    }
    result<device_info> info() override {
        auto d = backend::info(); if (!d) return d;
        UIDevice* u = UIDevice.currentDevice; d.value.manufacturer = "Apple"; d.value.model = u.model.UTF8String;
        d.value.os_name = u.systemName.UTF8String; d.value.os_version = u.systemVersion.UTF8String;
        d.value.form = u.userInterfaceIdiom == UIUserInterfaceIdiomPad ? form_factor::tablet : u.userInterfaceIdiom == UIUserInterfaceIdiomTV ? form_factor::tv
                     : u.userInterfaceIdiom == UIUserInterfaceIdiomCarPlay ? form_factor::car : form_factor::phone;
        return d;
    }
    void lifecycle_started() override {
        backend::lifecycle_started();
        static std::once_flag once; std::call_once(once, [] {
            auto* nc = NSNotificationCenter.defaultCenter;
            [nc addObserverForName:UIApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification*) { dispatch_lifecycle(app_event::foreground); }];
            [nc addObserverForName:UIApplicationDidEnterBackgroundNotification object:nil queue:nil usingBlock:^(NSNotification*) { dispatch_lifecycle(app_event::background); }];
            [nc addObserverForName:UIApplicationDidReceiveMemoryWarningNotification object:nil queue:nil usingBlock:^(NSNotification*) { dispatch_lifecycle(app_event::low_memory); }];
            [nc addObserverForName:UIApplicationWillTerminateNotification object:nil queue:nil usingBlock:^(NSNotification*) { dispatch_lifecycle(app_event::terminate); }];
        });
    }
};
} // namespace
std::shared_ptr<backend> make_apple() { return std::make_shared<apple_backend>(); }
} // namespace forge::os::mobile::detail
#elif defined(__APPLE__) && TARGET_OS_OSX
// macOS: trackpad touches/gestures come from the app's NSView via feed_touch. Here: CoreLocation (location; the
// app's provide_sensor + feed_sensor take precedence), AVCaptureDeviceDiscoverySession (cameras), LocalAuthentication
// (Touch ID), NSPasteboard (clipboard), IOKit power assertions (keep-awake), NSWorkspace open_url, user notifications
// (UNUserNotificationCenter, osascript when unbundled), device info; everything else uses the portable defaults
// (power, connectivity) or is not_supported.
// CoreLocation delivers to the run loop of the thread that made the manager: the main one here, made asynchronously
// when the caller is another thread (a command-line tool whose main thread never runs its loop gets no fixes, but
// nothing blocks waiting for it).
#import <AppKit/AppKit.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreLocation/CoreLocation.h>
#import <IOKit/pwr_mgt/IOPMLib.h>
#import <LocalAuthentication/LocalAuthentication.h>
#import <UserNotifications/UserNotifications.h>
#include <atomic>

using namespace forge::os::mobile;
using forge::os::mobile::detail::queue;

@interface FORGELocationDelegate : NSObject <CLLocationManagerDelegate>
@property (nonatomic) std::shared_ptr<queue> q;
@end
@implementation FORGELocationDelegate
- (void)locationManager:(CLLocationManager*)m didUpdateLocations:(NSArray<CLLocation*>*)locs {
    for (CLLocation* l in locs) _q->push({false, {sensor_kind::location, detail::now_ns(), {l.coordinate.latitude, l.coordinate.longitude, l.altitude, l.horizontalAccuracy}, 4}, {}});
}
- (void)locationManager:(CLLocationManager*)m didFailWithError:(NSError*)e {
    if (e.code == kCLErrorDenied) _q->finish();   // the user refused (or revoked) access: end the stream
}
@end

namespace forge::os::mobile::detail {
namespace {
void on_main_async(dispatch_block_t b) { if ([NSThread isMainThread]) b(); else dispatch_async(dispatch_get_main_queue(), b); }

// One location stream's manager; made and stopped on the main thread, never waited for.
struct mac_location {
    CLLocationManager* lm = nil; FORGELocationDelegate* del = nil; std::atomic<bool> stopped{false};
};
struct mac_location_sub {
    std::shared_ptr<mac_location> h;
    ~mac_location_sub() {
        auto loc = h; loc->stopped = true;
        on_main_async(^{ if (loc->lm) { [loc->lm stopUpdatingLocation]; loc->lm.delegate = nil; loc->lm = nil; } loc->del = nil; });
    }
};
permission_state location_state() {
    CLAuthorizationStatus s;
    if (@available(macOS 11.0, *)) s = [CLLocationManager new].authorizationStatus;
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
        s = [CLLocationManager authorizationStatus];
#pragma clang diagnostic pop
    }
    if (s == kCLAuthorizationStatusNotDetermined) return permission_state::not_determined;
    if (s == kCLAuthorizationStatusDenied || s == kCLAuthorizationStatusRestricted) return permission_state::denied;
    return permission_state::granted;   // authorized (always / when in use)
}

struct mac_backend final : backend {
    IOPMAssertionID awake = 0;
    ~mac_backend() override { if (awake) IOPMAssertionRelease(awake); }
    std::string name() const override { return "macos"; }
    static NSString* ns(std::string_view s) { return [[NSString alloc] initWithBytes:s.data() length:s.size() encoding:NSUTF8StringEncoding]; }
    result<touch_stream> open_touch() override { auto q = std::make_shared<queue>(); bridge::get().add_touch(q); return {make_touch(q, nullptr), {}}; }
    result<std::vector<sensor_info>> sensors() override {
        std::vector<sensor_info> v;
        if ([CLLocationManager locationServicesEnabled]) v.push_back({sensor_kind::location, "CoreLocation", "Apple", 1, 0, 0});
        return {v, {}};
    }
    result<sensor_stream> open_sensor(sensor_kind k, double) override {
        if (k != sensor_kind::location) return {{}, unsupported(std::string("sensor ") + sensor_name(k) + " on macOS")};
        bool glue = false; { auto& br = bridge::get(); std::lock_guard g(br.m); glue = br.provided[std::size_t(k)]; }
        auto q = std::make_shared<queue>(); bridge::get().add_sensor(k, q);
        if (glue) return {make_sensor(k, q, nullptr), {}};   // the app feeds location itself
        if (![CLLocationManager locationServicesEnabled]) return {{}, unsupported("location services disabled")};
        if (location_state() == permission_state::denied) return {{}, unsupported("location (access denied)")};
        auto loc = std::make_shared<mac_location>(); loc->del = [FORGELocationDelegate new]; loc->del.q = q;
        on_main_async(^{
            if (loc->stopped) return;
            loc->lm = [CLLocationManager new]; loc->lm.delegate = loc->del;
            if (@available(macOS 10.15, *)) [loc->lm requestWhenInUseAuthorization];
            [loc->lm startUpdatingLocation];
        });
        auto sub = std::make_shared<mac_location_sub>(); sub->h = loc;
        return {make_sensor(k, q, sub), {}};
    }
    result<std::vector<camera_info>> cameras() override {
        if (@available(macOS 10.15, *)) {
            NSMutableArray<AVCaptureDeviceType>* types = [NSMutableArray arrayWithObject:AVCaptureDeviceTypeBuiltInWideAngleCamera];
            if (@available(macOS 14.0, *)) { [types addObject:AVCaptureDeviceTypeExternal]; [types addObject:AVCaptureDeviceTypeContinuityCamera]; }
            else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
                [types addObject:AVCaptureDeviceTypeExternalUnknown];
#pragma clang diagnostic pop
            }
            auto* ds = [AVCaptureDeviceDiscoverySession discoverySessionWithDeviceTypes:types mediaType:AVMediaTypeVideo position:AVCaptureDevicePositionUnspecified];
            std::vector<camera_info> v;
            for (AVCaptureDevice* c in ds.devices) {
                const char* uid = c.uniqueID.UTF8String; const char* nm = c.localizedName.UTF8String;
                v.push_back({uid ? uid : "", nm ? nm : "", c.position == AVCaptureDevicePositionFront});
            }
            return {v, {}};
        }
        return {{}, unsupported("camera enumeration (macOS 10.15+)")};
    }
    result<biometric_info> biometrics() override {
        LAContext* c = [LAContext new]; NSError* e = nil;
        bool ok = [c canEvaluatePolicy:LAPolicyDeviceOwnerAuthenticationWithBiometrics error:&e];
        biometric_info b; b.enrolled = ok;
        if (@available(macOS 10.13.2, *)) {
            // biometryType is set by canEvaluatePolicy, also when nothing is enrolled
            b.available = c.biometryType != LABiometryTypeNone || (e && e.code == LAErrorBiometryNotEnrolled);
            b.kind = c.biometryType == LABiometryTypeTouchID ? "fingerprint" : "";   // Macs only have Touch ID
        } else { b.available = ok; b.kind = ok ? "fingerprint" : ""; }
        return {b, {}};
    }
    result<std::string> clipboard_get() override {
        NSPasteboard* pb = NSPasteboard.generalPasteboard; if (!pb) return backend::clipboard_get();
        NSString* s = [pb stringForType:NSPasteboardTypeString]; const char* u = s ? s.UTF8String : nullptr;
        return {u ? std::string(u) : std::string(), {}};
    }
    status clipboard_set(std::string_view t) override {
        NSString* s = ns(t); if (!s) return {{}, {22, "clipboard_set: text is not UTF-8"}};
        NSPasteboard* pb = NSPasteboard.generalPasteboard; if (!pb) return backend::clipboard_set(t);
        [pb clearContents];
        return [pb setString:s forType:NSPasteboardTypeString] ? status{} : status{{}, {1, "NSPasteboard setString failed"}};
    }
    status keep_awake(bool on) override {
        if (on && !awake) return IOPMAssertionCreateWithName(kIOPMAssertionTypeNoDisplaySleep, kIOPMAssertionLevelOn, CFSTR("forge keep_awake"), &awake) == kIOReturnSuccess ? status{} : status{{}, {1, "IOPMAssertionCreateWithName failed"}};
        if (!on && awake) { IOPMAssertionRelease(awake); awake = 0; }
        return {};
    }
    status open_url(std::string_view u) override {
        if (auto c = check_url(u); !c) return c;
        NSURL* url = [NSURL URLWithString:[[NSString alloc] initWithBytes:u.data() length:u.size() encoding:NSUTF8StringEncoding]];
        if (!url) return {{}, {22, "open_url: bad URL"}};
        return [NSWorkspace.sharedWorkspace openURL:url] ? status{} : status{{}, {1, "NSWorkspace openURL failed"}};
    }
    // UNUserNotificationCenter (macOS 10.14+) for bundled apps; it raises in a bare executable, so command-line
    // tools post through Notification Center's AppleScript bridge instead.
    status notify(std::string_view t, std::string_view b) override {
        if (auto h = backend::notify(t, b); !is_unsupported(h.err)) return h;
        NSString* title = [[NSString alloc] initWithBytes:t.data() length:t.size() encoding:NSUTF8StringEncoding];
        NSString* body = [[NSString alloc] initWithBytes:b.data() length:b.size() encoding:NSUTF8StringEncoding];
        if (!title || !body) return {{}, {22, "notify: text is not UTF-8"}};
        if (@available(macOS 10.14, *)) {
            if (NSBundle.mainBundle.bundleIdentifier != nil) {
                UNUserNotificationCenter* center = [UNUserNotificationCenter currentNotificationCenter];
                [center requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound) completionHandler:^(BOOL, NSError*) {}];
                UNMutableNotificationContent* c = [UNMutableNotificationContent new]; c.title = title; c.body = body;
                [center addNotificationRequest:[UNNotificationRequest requestWithIdentifier:[NSUUID UUID].UUIDString content:c trigger:nil] withCompletionHandler:nil];
                return {};
            }
        }
        auto quote = [](std::string_view s) {
            std::string r = "\"";
            for (char ch : s) { if (ch == '"' || ch == '\\') r += '\\'; r += (static_cast<unsigned char>(ch) < 0x20) ? ' ' : ch; }
            return r + "\"";
        };
        int rc = run_tool({"osascript", "-e", "display notification " + quote(b) + " with title " + quote(t)});
        if (rc < 0) return {{}, unsupported("notifications (no osascript)")};
        return rc == 0 ? status{} : status{{}, {rc, "osascript display notification failed"}};
    }
    result<device_info> info() override {
        auto d = backend::info(); if (!d) return d; d.value.manufacturer = "Apple";
        d.value.form = power_status().has_battery ? form_factor::laptop : form_factor::desktop; return d;
    }
    result<permission_state> permission_status(permission p) override {
        if (auto b = backend::permission_status(p)) return b;
        switch (p) {
        case permission::camera:
            if (@available(macOS 10.14, *)) {
                switch ([AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo]) {
                case AVAuthorizationStatusAuthorized: return {permission_state::granted, {}};
                case AVAuthorizationStatusNotDetermined: return {permission_state::not_determined, {}};
                default: return {permission_state::denied, {}};
                }
            }
            return {permission_state::not_required, {}};
        case permission::location: return {location_state(), {}};
        default: return {permission_state::not_required, {}};
        }
    }
    // Asynchronous, like iOS: the answer arrives later through permission_status. Prompts need a bundled app with the
    // usage-description keys; elsewhere macOS answers "denied" or ignores the request.
    status request_permission(permission p) override {
        if (auto h = backend::request_permission(p); !is_unsupported(h.err)) return h;
        switch (p) {
        case permission::camera:
            if (@available(macOS 10.14, *)) { [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL) {}]; }
            return {};
        case permission::location:
            on_main_async(^{
                static CLLocationManager* lm = [CLLocationManager new];
                if (@available(macOS 10.15, *)) { [lm requestWhenInUseAuthorization]; }   // older: startUpdatingLocation prompts
            });
            return {};
        case permission::notifications:
            if (@available(macOS 10.14, *)) {
                if (NSBundle.mainBundle.bundleIdentifier != nil)   // raises in an unbundled executable
                    [[UNUserNotificationCenter currentNotificationCenter] requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound) completionHandler:^(BOOL, NSError*) {}];
            }
            return {};
        default: return {};
        }
    }
};
} // namespace
std::shared_ptr<backend> make_apple() { return std::make_shared<mac_backend>(); }
} // namespace forge::os::mobile::detail
#elif defined(__APPLE__)
namespace forge::os::mobile::detail { std::shared_ptr<backend> make_apple() { return make_null(); } }   // watchOS: glue only
#endif
