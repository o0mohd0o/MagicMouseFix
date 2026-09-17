// magicmousefix - drives the cursor from raw Magic Mouse HID reports.
//
// Works around a macOS 27 bug where AppleMultitouchMouseHIDEventDriver starts
// but never enumerates the multitouch device, so the mouse produces no events.
//
// Report 0x12 (8 bytes, incl. report ID) per the device's own descriptor:
//   [0] report id 0x12
//   [1] buttons (bit0 = left, bit1 = right)
//   [2..3] X, int16 little endian, relative
//   [4..5] Y, int16 little endian, relative
//   [6..7] padding
//
// Needs Input Monitoring (to read the mouse) and Accessibility (to post events).
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <ApplicationServices/ApplicationServices.h>
#include <CoreFoundation/CoreFoundation.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define MOUSE_REPORT_ID 0x12
#define MAX_DEVS 8

// Pointer curve. The mouse reports ~1600 counts per inch at 125 Hz, so raw
// counts are far denser than screen points and must be scaled down. Like
// macOS, gain rises with hand speed: precise when moving slowly, fast on a
// flick. Gain is in points per count, interpolated between gGainMin at rest
// and gGainMax at gFullSpeedIPS inches/second.
#define MOUSE_CPI 1600.0
#define REPORT_HZ 125.0
static double gTracking = 1.0;       // overall multiplier (like the OS slider)
static double gGainMin = 0.15;       // gain when moving slowly
static double gGainMax = 0.90;       // gain at full speed
static double gFullSpeedIPS = 12.0;  // hand speed where gain tops out

static double gainForSpeed(double countsPerReport) {
    double ips = countsPerReport * REPORT_HZ / MOUSE_CPI;
    double t = ips / gFullSpeedIPS;
    if (t > 1.0) t = 1.0;
    if (t < 0.0) t = 0.0;
    // Slight curve so the low end stays controllable.
    t = t * t * (3.0 - 2.0 * t);
    return (gGainMin + (gGainMax - gGainMin) * t) * gTracking;
}
static int    gVerbose = 0;

static uint8_t gBufs[MAX_DEVS][1024];
static int gBufIdx = 0;

static uint8_t gPrevButtons = 0;
static CGPoint gPos;
static int gPosValid = 0;

// CGEventPost needs Accessibility permission. CGWarpMouseCursorPosition does
// not, so without permission we can still move the cursor (but not click).
static int gTrusted = 0;
static unsigned long gReportsSinceTrustCheck = 0;

// double-click tracking
static CFAbsoluteTime gLastClickTime = 0;
static CGPoint gLastClickPos;
static int gClickCount = 0;

// ~/.config/magicmousefix.conf, re-read when it changes. Format: key=value
//   tracking=1.0     overall speed multiplier
//   gain_min=0.15    gain when moving slowly
//   gain_max=0.90    gain at full speed
//   full_speed=12.0  hand speed (inches/sec) where gain tops out
static char gConfPath[512];
static time_t gConfMtime = 0;

static void loadConfig(int announce) {
    struct stat st;
    if (stat(gConfPath, &st) != 0) return;
    if (st.st_mtime == gConfMtime) return;
    gConfMtime = st.st_mtime;

    FILE *f = fopen(gConfPath, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char key[64];
        double val;
        if (line[0] == '#' || line[0] == '\n') continue;
        if (sscanf(line, " %63[a-z_] = %lf", key, &val) != 2) continue;
        if (!strcmp(key, "tracking")) gTracking = val;
        else if (!strcmp(key, "gain_min")) gGainMin = val;
        else if (!strcmp(key, "gain_max")) gGainMax = val;
        else if (!strcmp(key, "full_speed")) gFullSpeedIPS = val;
    }
    fclose(f);
    if (announce)
        fprintf(stderr, "config: tracking=%.2f gain_min=%.2f gain_max=%.2f "
                        "full_speed=%.1f\n",
                gTracking, gGainMin, gGainMax, gFullSpeedIPS);
}

static void clampToDisplays(CGPoint *p) {
    // Keep the cursor inside the union of all display bounds.
    uint32_t count = 0;
    CGGetActiveDisplayList(0, NULL, &count);
    if (count == 0) return;
    CGDirectDisplayID ids[16];
    if (count > 16) count = 16;
    CGGetActiveDisplayList(count, ids, &count);

    // If inside any display, nothing to do.
    for (uint32_t i = 0; i < count; i++) {
        CGRect r = CGDisplayBounds(ids[i]);
        if (CGRectContainsPoint(r, *p)) return;
    }
    // Otherwise clamp to the nearest display's bounds.
    double best = 1e18;
    CGPoint bestPt = *p;
    for (uint32_t i = 0; i < count; i++) {
        CGRect r = CGDisplayBounds(ids[i]);
        double x = fmin(fmax(p->x, CGRectGetMinX(r)), CGRectGetMaxX(r) - 1);
        double y = fmin(fmax(p->y, CGRectGetMinY(r)), CGRectGetMaxY(r) - 1);
        double d = (x - p->x) * (x - p->x) + (y - p->y) * (y - p->y);
        if (d < best) { best = d; bestPt = CGPointMake(x, y); }
    }
    *p = bestPt;
}

static void syncPosFromSystem(void) {
    CGEventRef e = CGEventCreate(NULL);
    if (e) {
        gPos = CGEventGetLocation(e);
        CFRelease(e);
        gPosValid = 1;
    }
}

static void postMove(double dx, double dy, uint8_t buttons) {
    if (!gPosValid) syncPosFromSystem();

    double factor = gainForSpeed(sqrt(dx * dx + dy * dy));

    gPos.x += dx * factor;
    gPos.y += dy * factor;
    clampToDisplays(&gPos);

    if (!gTrusted) {
        // No Accessibility permission: move the cursor directly.
        CGWarpMouseCursorPosition(gPos);
        return;
    }

    CGEventType type = kCGEventMouseMoved;
    CGMouseButton btn = kCGMouseButtonLeft;
    if (buttons & 0x01) { type = kCGEventLeftMouseDragged; btn = kCGMouseButtonLeft; }
    else if (buttons & 0x02) { type = kCGEventRightMouseDragged; btn = kCGMouseButtonRight; }

    CGEventRef ev = CGEventCreateMouseEvent(NULL, type, gPos, btn);
    if (!ev) return;
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaX, (int64_t)llround(dx * factor));
    CGEventSetIntegerValueField(ev, kCGMouseEventDeltaY, (int64_t)llround(dy * factor));
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

static void postButton(int isLeft, int down) {
    if (!gPosValid) syncPosFromSystem();
    if (!gTrusted) {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "Click ignored: Accessibility permission needed "
                            "(movement works without it).\n");
            warned = 1;
        }
        return;
    }
    CGEventType type;
    CGMouseButton btn;
    if (isLeft) {
        type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp;
        btn = kCGMouseButtonLeft;
    } else {
        type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp;
        btn = kCGMouseButtonRight;
    }

    if (down) {
        CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        double dist = hypot(gPos.x - gLastClickPos.x, gPos.y - gLastClickPos.y);
        if (now - gLastClickTime < 0.5 && dist < 6.0 && gClickCount > 0)
            gClickCount++;
        else
            gClickCount = 1;
        if (gClickCount > 3) gClickCount = 3;
        gLastClickTime = now;
        gLastClickPos = gPos;
    }

    CGEventRef ev = CGEventCreateMouseEvent(NULL, type, gPos, btn);
    if (!ev) return;
    CGEventSetIntegerValueField(ev, kCGMouseEventClickState, gClickCount);
    CGEventPost(kCGHIDEventTap, ev);
    CFRelease(ev);
}

static void reportCB(void *ctx, IOReturn result, void *sender,
                     IOHIDReportType type, uint32_t reportID,
                     uint8_t *report, CFIndex len) {
    if (len < 6) return;
    // The callback buffer includes the report ID in byte 0.
    int off = (report[0] == MOUSE_REPORT_ID) ? 1 : 0;
    if (off == 0 && reportID != MOUSE_REPORT_ID) return;
    if ((CFIndex)(off + 5) > len) return;

    uint8_t buttons = report[off] & 0x03;
    int16_t dx = (int16_t)((uint16_t)report[off + 1] | ((uint16_t)report[off + 2] << 8));
    int16_t dy = (int16_t)((uint16_t)report[off + 3] | ((uint16_t)report[off + 4] << 8));

    if (gVerbose && (dx || dy || buttons != gPrevButtons))
        fprintf(stderr, "dx=%d dy=%d buttons=0x%02x\n", dx, dy, buttons);

    // Pick up permission grants and config edits without a restart.
    if (++gReportsSinceTrustCheck >= 120) {
        gReportsSinceTrustCheck = 0;
        loadConfig(1);
        if (!gTrusted && AXIsProcessTrusted()) {
            gTrusted = 1;
            fprintf(stderr, "Accessibility granted - clicks enabled.\n");
        }
    }

    if (dx || dy) postMove((double)dx, (double)dy, buttons);

    uint8_t changed = buttons ^ gPrevButtons;
    if (changed & 0x01) postButton(1, (buttons & 0x01) != 0);
    if (changed & 0x02) postButton(0, (buttons & 0x02) != 0);
    gPrevButtons = buttons;
}

static void attach(IOHIDDeviceRef dev) {
    int32_t up = 0, u = 0;
    CFNumberRef upn = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDPrimaryUsagePageKey));
    CFNumberRef un = IOHIDDeviceGetProperty(dev, CFSTR(kIOHIDPrimaryUsageKey));
    if (upn) CFNumberGetValue(upn, kCFNumberSInt32Type, &up);
    if (un) CFNumberGetValue(un, kCFNumberSInt32Type, &u);
    // Only the pointer interface carries report 0x12.
    if (up != 1 || u != 2) return;
    if (gBufIdx >= MAX_DEVS) return;

    IOReturn r = IOHIDDeviceOpen(dev, kIOHIDOptionsTypeNone);
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "could not open mouse (0x%08x)\n", r);
        return;
    }
    IOHIDDeviceRegisterInputReportCallback(dev, gBufs[gBufIdx], sizeof(gBufs[0]),
                                           reportCB, NULL);
    gBufIdx++;
    IOHIDDeviceScheduleWithRunLoop(dev, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
    syncPosFromSystem();
    fprintf(stderr, "Magic Mouse attached - cursor control active.\n");
}

static void matchedCB(void *ctx, IOReturn r, void *sender, IOHIDDeviceRef dev) {
    attach(dev);
}

static void removedCB(void *ctx, IOReturn r, void *sender, IOHIDDeviceRef dev) {
    fprintf(stderr, "Magic Mouse disconnected (waiting for it to come back).\n");
    gPrevButtons = 0;
    if (gBufIdx > 0) gBufIdx--;
}

static void usage(const char *p) {
    printf("usage: %s [--tracking N] [--seconds N] [-v]\n"
           "  --tracking N  pointer speed multiplier (default 1.0)\n"
           "  --seconds N   run for N seconds then exit (default: run forever)\n"
           "  -v            print decoded reports to stderr\n"
           "Tuning without a rebuild: ~/.config/magicmousefix.conf\n", p);
}

int main(int argc, char **argv) {
    double seconds = 0;
    const char *home = getenv("HOME");
    snprintf(gConfPath, sizeof(gConfPath), "%s/.config/magicmousefix.conf",
             home ? home : "");
    loadConfig(0);

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--tracking") && i + 1 < argc) gTracking = atof(argv[++i]);
        else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "-v")) gVerbose = 1;
        else { usage(argv[0]); return 2; }
    }

    int access = IOHIDCheckAccess(kIOHIDRequestTypeListenEvent);
    if (access != kIOHIDAccessTypeGranted) {
        fprintf(stderr, "Input Monitoring permission missing - requesting it.\n");
        IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
    }
    CFStringRef keys[] = {kAXTrustedCheckOptionPrompt};
    CFBooleanRef vals[] = {kCFBooleanTrue};
    CFDictionaryRef opts = CFDictionaryCreate(kCFAllocatorDefault,
        (const void **)keys, (const void **)vals, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    gTrusted = AXIsProcessTrustedWithOptions(opts) ? 1 : 0;
    CFRelease(opts);
    if (!gTrusted)
        fprintf(stderr, "No Accessibility permission yet: the cursor will move, "
                        "but clicks will not work.\nGrant it in System Settings > "
                        "Privacy & Security > Accessibility.\n");

    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, 0);
    if (!mgr) { fprintf(stderr, "IOHIDManagerCreate failed\n"); return 1; }

    int vids[2] = {0x05ac, 0x004c};
    int pid = 0x0269, up = 1, u = 2;
    CFMutableArrayRef matches =
        CFArrayCreateMutable(kCFAllocatorDefault, 2, &kCFTypeArrayCallBacks);
    for (int i = 0; i < 2; i++) {
        CFMutableDictionaryRef m = CFDictionaryCreateMutable(
            kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
            &kCFTypeDictionaryValueCallBacks);
        CFNumberRef v = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &vids[i]);
        CFNumberRef p = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &pid);
        CFNumberRef pu = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &up);
        CFNumberRef puu = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &u);
        CFDictionarySetValue(m, CFSTR(kIOHIDVendorIDKey), v);
        CFDictionarySetValue(m, CFSTR(kIOHIDProductIDKey), p);
        CFDictionarySetValue(m, CFSTR(kIOHIDPrimaryUsagePageKey), pu);
        CFDictionarySetValue(m, CFSTR(kIOHIDPrimaryUsageKey), puu);
        CFArrayAppendValue(matches, m);
        CFRelease(v); CFRelease(p); CFRelease(pu); CFRelease(puu); CFRelease(m);
    }
    IOHIDManagerSetDeviceMatchingMultiple(mgr, matches);
    CFRelease(matches);

    IOHIDManagerRegisterDeviceMatchingCallback(mgr, matchedCB, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(mgr, removedCB, NULL);
    IOHIDManagerScheduleWithRunLoop(mgr, CFRunLoopGetMain(), kCFRunLoopDefaultMode);

    // Opening fails until Input Monitoring is granted. Keep retrying rather
    // than exiting, so granting permission starts it working on its own.
    IOReturn r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
    for (int attempt = 0; r != kIOReturnSuccess; attempt++) {
        if (attempt == 0)
            fprintf(stderr, "Waiting for Input Monitoring permission "
                            "(open failed: 0x%08x). Retrying every 3s.\n", r);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 3.0, false);
        if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted)
            continue;
        r = IOHIDManagerOpen(mgr, kIOHIDOptionsTypeNone);
        if (r == kIOReturnSuccess)
            fprintf(stderr, "Input Monitoring granted - reading the mouse now.\n");
    }

    fprintf(stderr, "magicmousefix running (tracking %.2f, gain %.2f-%.2f). "
                    "Ctrl-C to stop.\n", gTracking, gGainMin, gGainMax);
    if (seconds > 0) {
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false);
        fprintf(stderr, "time limit reached, exiting.\n");
    } else {
        CFRunLoopRun();
    }
    return 0;
}
