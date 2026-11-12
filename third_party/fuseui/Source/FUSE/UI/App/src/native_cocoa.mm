// fuse_app macOS backend (RetDec addition, not upstream FUSE yet). See cocoa_bridge.hpp.
//
// Built without ARC (-fno-objc-arc): every object made here is retained/released by hand.
#include "cocoa_bridge.hpp"

#include "fuse/ui/ui.hpp"

#import <Cocoa/Cocoa.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

namespace {

using fuse::app::cocoa::Callbacks;
using FKey = fuse::ui::Key;

bool gQuit = false;
int gIconSize = 0;

std::uint8_t modsOf(NSEventModifierFlags f) {
    std::uint8_t m = 0;
    if (f & NSEventModifierFlagShift) m |= fuse::ui::kModShift;
    // Command is the shortcut modifier on macOS: FuseUI's widgets bind Ctrl+C / Ctrl+V / Ctrl+Z, so Cmd maps to Ctrl
    // and Cmd+C copies as a Mac user expects. Control maps to Ctrl as well.
    if (f & (NSEventModifierFlagCommand | NSEventModifierFlagControl)) m |= fuse::ui::kModCtrl;
    if (f & NSEventModifierFlagOption) m |= fuse::ui::kModAlt;
    return m;
}

// kVK_* virtual key codes (Carbon HIToolbox Events.h).
FKey mapKeyCode(unsigned short code) {
    switch (code) {
    case 48: return FKey::Tab;
    case 36: case 76: return FKey::Enter;
    case 53: return FKey::Escape;
    case 49: return FKey::Space;
    case 51: return FKey::Backspace;
    case 117: return FKey::Delete;
    case 123: return FKey::Left;
    case 124: return FKey::Right;
    case 125: return FKey::Down;
    case 126: return FKey::Up;
    case 115: return FKey::Home;
    case 119: return FKey::End;
    case 116: return FKey::PageUp;
    case 121: return FKey::PageDown;
    case 122: return FKey::F1;
    case 120: return FKey::F2;
    case 99: return FKey::F3;
    case 118: return FKey::F4;
    case 96: return FKey::F5;
    case 97: return FKey::F6;
    case 98: return FKey::F7;
    case 100: return FKey::F8;
    case 101: return FKey::F9;
    case 109: return FKey::F10;
    case 103: return FKey::F11;
    case 111: return FKey::F12;
    case 33: return FKey::BracketLeft;
    case 30: return FKey::BracketRight;
    default: return FKey::Other;
    }
}

FKey mapAscii(unichar c) {
    if (c >= 'A' && c <= 'Z') c = unichar(c - 'A' + 'a');
    switch (c) {
    case 'a': return FKey::A; case 'b': return FKey::B; case 'c': return FKey::C; case 'd': return FKey::D;
    case 'e': return FKey::E; case 'f': return FKey::F; case 'g': return FKey::G; case 'h': return FKey::H;
    case 'i': return FKey::I; case 'j': return FKey::J; case 'k': return FKey::K; case 'l': return FKey::L;
    case 'm': return FKey::M; case 'n': return FKey::N; case 'o': return FKey::O; case 'p': return FKey::P;
    case 'q': return FKey::Q; case 'r': return FKey::R; case 's': return FKey::S; case 't': return FKey::T;
    case 'u': return FKey::U; case 'v': return FKey::V; case 'w': return FKey::W; case 'x': return FKey::X;
    case 'y': return FKey::Y; case 'z': return FKey::Z;
    case '0': return FKey::Num0; case '1': return FKey::Num1; case '2': return FKey::Num2; case '3': return FKey::Num3;
    case '4': return FKey::Num4; case '5': return FKey::Num5; case '6': return FKey::Num6; case '7': return FKey::Num7;
    case '8': return FKey::Num8; case '9': return FKey::Num9;
    default: return FKey::Other;
    }
}

} // namespace

// ---- the content view ------------------------------------------------------------------------------------------
@interface FuseContentView : NSView {
@public
    Callbacks cb;
    std::vector<std::uint32_t> pixels; // 0xAABBGGRR, as fuse::ui::Image stores them
    int pw, ph;
    NSTrackingArea* tracking;
}
@end

@implementation FuseContentView
- (BOOL)isFlipped { return YES; } // top-left origin, as FuseUI lays out
- (BOOL)acceptsFirstResponder { return YES; }
- (BOOL)acceptsFirstMouse:(NSEvent*)e { (void)e; return YES; }
- (BOOL)isOpaque { return YES; }

- (void)updateTrackingAreas {
    if (tracking) { [self removeTrackingArea:tracking]; [tracking release]; tracking = nil; }
    tracking = [[NSTrackingArea alloc] initWithRect:[self bounds]
                                            options:(NSTrackingMouseMoved | NSTrackingActiveInKeyWindow |
                                                     NSTrackingInVisibleRect | NSTrackingMouseEnteredAndExited)
                                              owner:self
                                           userInfo:nil];
    [self addTrackingArea:tracking];
    [super updateTrackingAreas];
}

- (void)drawRect:(NSRect)dirty {
    (void)dirty;
    CGContextRef ctx = [[NSGraphicsContext currentContext] CGContext];
    if (pixels.empty() || pw <= 0 || ph <= 0) {
        CGContextSetRGBFillColor(ctx, 0.12, 0.12, 0.18, 1.0);
        CGContextFillRect(ctx, NSRectToCGRect([self bounds]));
        return;
    }
    // Bytes in memory are R, G, B, A: RGBA with the alpha last, opaque (the raster is composited over its clear colour).
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGDataProviderRef dp = CGDataProviderCreateWithData(nullptr, pixels.data(), pixels.size() * 4, nullptr);
    CGImageRef img = CGImageCreate(size_t(pw), size_t(ph), 8, 32, size_t(pw) * 4, cs,
                                   kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast, dp, nullptr, false,
                                   kCGRenderingIntentDefault);
    // The view is flipped; CGContextDrawImage draws images bottom-up, so flip back for the image.
    CGContextSaveGState(ctx);
    CGContextTranslateCTM(ctx, 0, CGFloat(ph));
    CGContextScaleCTM(ctx, 1, -1);
    CGContextSetInterpolationQuality(ctx, kCGInterpolationNone);
    CGContextDrawImage(ctx, CGRectMake(0, 0, CGFloat(pw), CGFloat(ph)), img);
    CGContextRestoreGState(ctx);
    CGImageRelease(img);
    CGDataProviderRelease(dp);
    CGColorSpaceRelease(cs);
}

- (NSPoint)local:(NSEvent*)e { return [self convertPoint:[e locationInWindow] fromView:nil]; }
- (void)mods:(NSEvent*)e { if (cb.modifiers) cb.modifiers(cb.user, modsOf([e modifierFlags])); }

- (void)mouseMoved:(NSEvent*)e { [self mods:e]; NSPoint p = [self local:e]; if (cb.pointerMove) cb.pointerMove(cb.user, float(p.x), float(p.y)); }
- (void)mouseDragged:(NSEvent*)e { [self mouseMoved:e]; }
- (void)rightMouseDragged:(NSEvent*)e { [self mouseMoved:e]; }
- (void)otherMouseDragged:(NSEvent*)e { [self mouseMoved:e]; }
- (void)down:(NSEvent*)e button:(int)b {
    [self mods:e];
    NSPoint p = [self local:e];
    if (cb.pointerMove) cb.pointerMove(cb.user, float(p.x), float(p.y));
    if (cb.pointerDown) cb.pointerDown(cb.user, float(p.x), float(p.y), b);
}
- (void)up:(NSEvent*)e {
    [self mods:e];
    NSPoint p = [self local:e];
    if (cb.pointerUp) cb.pointerUp(cb.user, float(p.x), float(p.y));
}
- (void)mouseDown:(NSEvent*)e {
    // Control-click is the one-button right click.
    [self down:e button:(([e modifierFlags] & NSEventModifierFlagControl) ? 1 : 0)];
}
- (void)rightMouseDown:(NSEvent*)e { [self down:e button:1]; }
- (void)otherMouseDown:(NSEvent*)e { [self down:e button:2]; }
- (void)mouseUp:(NSEvent*)e { [self up:e]; }
- (void)rightMouseUp:(NSEvent*)e { [self up:e]; }
- (void)otherMouseUp:(NSEvent*)e { [self up:e]; }
- (void)scrollWheel:(NSEvent*)e {
    NSPoint p = [self local:e];
    CGFloat dx = [e scrollingDeltaX], dy = [e scrollingDeltaY];
    if ([e hasPreciseScrollingDeltas]) { dx /= 10.0; dy /= 10.0; } // trackpad points -> wheel notches
    if (cb.wheel && (dx != 0 || dy != 0)) cb.wheel(cb.user, float(p.x), float(p.y), float(-dx), float(dy));
}
- (void)flagsChanged:(NSEvent*)e { [self mods:e]; }

- (void)keyEvent:(NSEvent*)e down:(bool)down {
    const std::uint8_t m = modsOf([e modifierFlags]);
    FKey k = mapKeyCode([e keyCode]);
    if (k == FKey::Other) {
        NSString* bare = [e charactersIgnoringModifiers];
        if ([bare length] == 1) k = mapAscii([bare characterAtIndex:0]);
    }
    if (k != FKey::Other && cb.key) cb.key(cb.user, int(k), m, down);
    if (!down || (m & (fuse::ui::kModCtrl | fuse::ui::kModAlt)) || !cb.text) return;
    NSString* chars = [e characters];
    if ([chars length] == 0) return;
    const unichar c0 = [chars characterAtIndex:0];
    if (c0 < 0x20 || c0 == 0x7F || (c0 >= 0xF700 && c0 <= 0xF8FF)) return; // controls and function-key private use
    cb.text(cb.user, [chars UTF8String]);
}
- (void)keyDown:(NSEvent*)e { [self keyEvent:e down:true]; }
- (void)keyUp:(NSEvent*)e { [self keyEvent:e down:false]; }
// Keep Cmd+key from being swallowed as a menu key equivalent unless the app menu actually handles it (Cmd+Q).
- (BOOL)performKeyEquivalent:(NSEvent*)e {
    if ([[e charactersIgnoringModifiers] isEqualToString:@"q"]) return NO;
    if ([[self window] firstResponder] == self && ([e modifierFlags] & NSEventModifierFlagCommand)) {
        [self keyDown:e];
        return YES;
    }
    return [super performKeyEquivalent:e];
}
- (void)dealloc {
    if (tracking) [tracking release];
    [super dealloc];
}
@end

// ---- window delegate -----------------------------------------------------------------------------------------
@interface FuseWindowDelegate : NSObject <NSWindowDelegate> {
@public
    FuseContentView* view;
}
@end

@implementation FuseWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)w {
    (void)w;
    if (view->cb.close) view->cb.close(view->cb.user); // the app decides; AppWindow::requestClose
    return NO;
}
- (void)windowDidResize:(NSNotification*)n {
    (void)n;
    const NSSize s = [view bounds].size;
    if (view->cb.resize && s.width > 0 && s.height > 0) view->cb.resize(view->cb.user, unsigned(s.width), unsigned(s.height));
}
- (void)windowDidExpose:(NSNotification*)n { (void)n; if (view->cb.expose) view->cb.expose(view->cb.user); }
- (void)windowDidBecomeKey:(NSNotification*)n { (void)n; if (view->cb.focus) view->cb.focus(view->cb.user, true); }
- (void)windowDidResignKey:(NSNotification*)n { (void)n; if (view->cb.focus) view->cb.focus(view->cb.user, false); }
@end

// ---- application menu target ---------------------------------------------------------------------------------
@interface FuseAppTarget : NSObject
- (void)quit:(id)sender;
@end
@implementation FuseAppTarget
- (void)quit:(id)sender { (void)sender; gQuit = true; }
@end

namespace fuse::app::cocoa {

bool init(const std::string& appName, std::string& error) {
    @autoreleasepool {
        NSApplication* app = [NSApplication sharedApplication];
        if (!app) { error = "NSApplication unavailable"; return false; }
        [app setActivationPolicy:NSApplicationActivationPolicyRegular];
        static FuseAppTarget* target = [[FuseAppTarget alloc] init];
        NSMenu* bar = [[NSMenu alloc] init];
        NSMenuItem* appItem = [[NSMenuItem alloc] init];
        [bar addItem:appItem];
        NSMenu* appMenu = [[NSMenu alloc] init];
        NSString* name = [NSString stringWithUTF8String:appName.c_str()];
        NSMenuItem* quit = [[NSMenuItem alloc] initWithTitle:[@"Quit " stringByAppendingString:name]
                                                      action:@selector(quit:)
                                               keyEquivalent:@"q"];
        [quit setTarget:target];
        [appMenu addItem:quit];
        [appItem setSubmenu:appMenu];
        [app setMainMenu:bar];
        [quit release];
        [appMenu release];
        [appItem release];
        [bar release];
        [app finishLaunching];
        [app activateIgnoringOtherApps:YES];
        return true;
    }
}

void* createWindow(const std::string& title, unsigned width, unsigned height, const Callbacks& cb) {
    @autoreleasepool {
        const NSRect frame = NSMakeRect(0, 0, CGFloat(width), CGFloat(height));
        NSWindow* win = [[NSWindow alloc]
            initWithContentRect:frame
                      styleMask:(NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable |
                                 NSWindowStyleMaskResizable)
                        backing:NSBackingStoreBuffered
                          defer:NO];
        if (!win) return nullptr;
        [win setReleasedWhenClosed:NO];
        [win setTitle:[NSString stringWithUTF8String:title.c_str()]];
        FuseContentView* view = [[FuseContentView alloc] initWithFrame:frame];
        view->cb = cb;
        view->pw = 0;
        view->ph = 0;
        view->tracking = nil;
        [win setContentView:view];
        [win makeFirstResponder:view];
        [win setAcceptsMouseMovedEvents:YES];
        FuseWindowDelegate* del = [[FuseWindowDelegate alloc] init];
        del->view = view;
        [win setDelegate:del];
        [win center];
        [win makeKeyAndOrderFront:nil];
        [view release]; // the window's content view keeps it
        return win;     // +1, released in destroyWindow
    }
}

void destroyWindow(void* window) {
    if (!window) return;
    @autoreleasepool {
        NSWindow* win = static_cast<NSWindow*>(window);
        id del = [win delegate];
        [win setDelegate:nil];
        [win orderOut:nil];
        [win close];
        [del release];
        [win release];
    }
}

void blit(void* window, const std::uint32_t* rgba, int width, int height) {
    if (!window || !rgba || width <= 0 || height <= 0) return;
    NSWindow* win = static_cast<NSWindow*>(window);
    FuseContentView* view = static_cast<FuseContentView*>([win contentView]);
    view->pixels.assign(rgba, rgba + std::size_t(width) * std::size_t(height));
    view->pw = width;
    view->ph = height;
    [view setNeedsDisplay:YES];
    [view displayIfNeeded];
}

bool readback(void* window, std::uint32_t* rgba, int width, int height) {
    if (!window) return false;
    NSWindow* win = static_cast<NSWindow*>(window);
    FuseContentView* view = static_cast<FuseContentView*>([win contentView]);
    if (view->pixels.empty() || view->pw != width || view->ph != height) return false;
    std::memcpy(rgba, view->pixels.data(), view->pixels.size() * 4);
    return true;
}

bool pump(float waitSeconds) {
    @autoreleasepool {
        NSDate* until = std::isinf(waitSeconds) ? [NSDate distantFuture]
                        : waitSeconds > 0.f      ? [NSDate dateWithTimeIntervalSinceNow:double(waitSeconds)]
                                                 : [NSDate distantPast];
        NSEvent* e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:until inMode:NSDefaultRunLoopMode dequeue:YES];
        while (e) {
            [NSApp sendEvent:e];
            e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES];
        }
        [NSApp updateWindows];
    }
    return !gQuit;
}

bool resize(void* window, unsigned width, unsigned height) {
    if (!window) return false;
    [static_cast<NSWindow*>(window) setContentSize:NSMakeSize(CGFloat(width), CGFloat(height))];
    return true;
}

bool screenInfo(void* window, int& x, int& y, float& workX, float& workY, float& workW, float& workH) {
    if (!window) return false;
    NSWindow* win = static_cast<NSWindow*>(window);
    NSScreen* scr = [win screen] ? [win screen] : [NSScreen mainScreen];
    if (!scr) return false;
    const NSRect full = [scr frame];
    const NSRect vis = [scr visibleFrame];
    const NSRect content = [win contentRectForFrameRect:[win frame]];
    // Cocoa screen coordinates are bottom-left; report top-left like X11 / Win32.
    const CGFloat top = NSMaxY(full);
    x = int(content.origin.x);
    y = int(top - NSMaxY(content));
    workX = float(vis.origin.x);
    workY = float(top - NSMaxY(vis));
    workW = float(vis.size.width);
    workH = float(vis.size.height);
    return true;
}

bool setIcon(const std::uint32_t* rgba, int width, int height) {
    if (!rgba || width <= 0 || height <= 0) return false;
    @autoreleasepool {
        NSBitmapImageRep* rep = [[NSBitmapImageRep alloc] initWithBitmapDataPlanes:nullptr
                                                                         pixelsWide:width
                                                                         pixelsHigh:height
                                                                      bitsPerSample:8
                                                                    samplesPerPixel:4
                                                                           hasAlpha:YES
                                                                           isPlanar:NO
                                                                     colorSpaceName:NSDeviceRGBColorSpace
                                                                        bytesPerRow:width * 4
                                                                       bitsPerPixel:32];
        if (!rep) return false;
        std::memcpy([rep bitmapData], rgba, std::size_t(width) * std::size_t(height) * 4); // RGBA bytes
        NSImage* img = [[NSImage alloc] initWithSize:NSMakeSize(width, height)];
        [img addRepresentation:rep];
        [NSApp setApplicationIconImage:img];
        [img release];
        [rep release];
        gIconSize = width;
        return true;
    }
}

int iconSize() { return gIconSize; }

} // namespace fuse::app::cocoa
