// MagicMouseFix - un-sticks Apple's Magic Mouse driver on macOS 27.
//
// On macOS 27.0 AppleMultitouchMouseHIDEventDriver starts but never runs its
// "Enumerate MT" step, so the mouse connects (and even reports battery) yet
// produces no input. The macOS 26 driver wrote feature report F1 06 01 37 to
// the mouse at startup; the macOS 27 driver never does and waits forever.
// Writing that report ourselves lets the driver continue, after which the
// mouse works natively again (pointer, clicks, scrolling, gestures).
//
// This tool only sends that one report. It never reads input or moves the
// cursor.
//
//   MagicMouseFix              install the login agent, then exit
//   MagicMouseFix --agent      run as the background agent (used by launchd)
//   MagicMouseFix --uninstall  remove the login agent
#include <IOKit/IOKitLib.h>
#include <IOKit/hid/IOHIDManager.h>
#include <IOKit/hid/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach-o/dyld.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define AGENT_LABEL "io.github.o0mohd0o.magicmousefix"
#define MOUSE_PID 0x0269  // Magic Mouse 2 (Lightning)
#define KEYHOLE_ID 0xF1
static const uint8_t kKick[] = {0x06, 0x01, 0x37};  // goes out as F1 06 01 37

#define SETTINGS_URL \
    "x-apple.systempreferences:com.apple.preference.security?Privacy_ListenEvent"

static IOHIDDeviceRef gDev = NULL;
static int gAttempts = 0;

static void logmsg(const char *msg) {
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(stderr, "%s  %s\n", ts, msg);
}

// ---------------------------------------------------------------- agent ----

// True once Apple's driver has created the native multitouch device.
static int nativeDriverUp(void) {
    io_iterator_t it;
    if (IOServiceGetMatchingServices(kIOMainPortDefault,
            IOServiceMatching("AppleMultitouchDevice"), &it) != KERN_SUCCESS)
        return 0;
    int found = 0;
    io_object_t svc;
    while ((svc = IOIteratorNext(it))) {
        CFTypeRef pid = IORegistryEntrySearchCFProperty(svc, kIOServicePlane,
            CFSTR("ProductID"), kCFAllocatorDefault,
            kIORegistryIterateRecursively | kIORegistryIterateParents);
        if (pid) {
            int v = 0;
            if (CFGetTypeID(pid) == CFNumberGetTypeID() &&
                CFNumberGetValue(pid, kCFNumberIntType, &v) && v == MOUSE_PID)
                found = 1;
            CFRelease(pid);
        }
        IOObjectRelease(svc);
    }
    IOObjectRelease(it);
    return found;
}

static void tryKick(CFRunLoopTimerRef timer, void *info);

static void scheduleKick(double delay) {
    CFRunLoopTimerRef t = CFRunLoopTimerCreate(kCFAllocatorDefault,
        CFAbsoluteTimeGetCurrent() + delay, 0, 0, 0, tryKick, NULL);
    CFRunLoopAddTimer(CFRunLoopGetMain(), t, kCFRunLoopDefaultMode);
    CFRelease(t);
}

static void tryKick(CFRunLoopTimerRef timer, void *info) {
    if (!gDev) return;
    if (nativeDriverUp()) {
        logmsg(gAttempts > 0 ? "native driver is up - mouse working."
                             : "native driver already up - nothing to do.");
        gAttempts = 0;
        return;
    }
    if (gAttempts >= 10) { logmsg("gave up after 10 attempts."); return; }
    gAttempts++;

    IOReturn r = IOHIDDeviceSetReport(gDev, kIOHIDReportTypeFeature, KEYHOLE_ID,
                                      kKick, sizeof(kKick));
    if (r != kIOReturnSuccess &&
        IOHIDDeviceOpen(gDev, kIOHIDOptionsTypeNone) == kIOReturnSuccess)
        r = IOHIDDeviceSetReport(gDev, kIOHIDReportTypeFeature, KEYHOLE_ID,
                                 kKick, sizeof(kKick));
    char buf[96];
    snprintf(buf, sizeof(buf), "driver stalled - sent wake-up, attempt %d -> 0x%08x",
             gAttempts, r);
    logmsg(buf);

    // Check again shortly; retry if the driver still has not come up.
    scheduleKick(2.0);
}

static void matchedCB(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef dev) {
    gDev = dev;
    gAttempts = 0;
    logmsg("Magic Mouse connected.");
    // Give Apple's driver a moment to start (and stall) before waking it.
    scheduleKick(1.5);
}

static void removedCB(void *ctx, IOReturn res, void *sender, IOHIDDeviceRef dev) {
    if (dev == gDev) { gDev = NULL; logmsg("Magic Mouse disconnected."); }
}

static int runAgent(void) {
    // Writing to the mouse needs Input Monitoring. macOS caches the answer at
    // process start, so if it is missing, ask once, then exit and let launchd
    // restart us - each restart re-checks until the user has granted it.
    if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) != kIOHIDAccessTypeGranted) {
        logmsg("Input Monitoring not granted yet - enable MagicMouseFix in "
               "System Settings > Privacy & Security > Input Monitoring.");
        IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
        sleep(5);
        return 1;
    }

    IOHIDManagerRef mgr = IOHIDManagerCreate(kCFAllocatorDefault, 0);
    int vids[2] = {0x05ac, 0x004c}, pid = MOUSE_PID, up = 1, u = 2;
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

    logmsg("permission OK - watching for the Magic Mouse.");
    CFRunLoopRun();
    return 0;
}

// ------------------------------------------------------------ installer ----

// Returns 1 if the user clicked the default button.
static int alert(const char *title, const char *msg, const char *button,
                 const char *other) {
    CFStringRef t = CFStringCreateWithCString(NULL, title, kCFStringEncodingUTF8);
    CFStringRef m = CFStringCreateWithCString(NULL, msg, kCFStringEncodingUTF8);
    CFStringRef b = CFStringCreateWithCString(NULL, button, kCFStringEncodingUTF8);
    CFStringRef o = other ? CFStringCreateWithCString(NULL, other, kCFStringEncodingUTF8) : NULL;
    CFOptionFlags resp = 0;
    CFUserNotificationDisplayAlert(0, kCFUserNotificationNoteAlertLevel, NULL, NULL,
                                   NULL, t, m, b, o, NULL, &resp);
    CFRelease(t); CFRelease(m); CFRelease(b);
    if (o) CFRelease(o);
    return resp == kCFUserNotificationDefaultResponse;
}

static int plistPath(char *out, size_t n) {
    const char *home = getenv("HOME");
    if (!home) return -1;
    snprintf(out, n, "%s/Library/LaunchAgents/%s.plist", home, AGENT_LABEL);
    return 0;
}

static void bootout(void) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "/bin/launchctl bootout gui/%d/%s 2>/dev/null",
             (int)getuid(), AGENT_LABEL);
    system(cmd);
}

static int uninstall(int quiet) {
    char plist[PATH_MAX];
    if (plistPath(plist, sizeof(plist)) != 0) return 1;
    bootout();
    unlink(plist);
    if (!quiet)
        alert("MagicMouseFix removed",
              "The login agent has been removed. You can now delete the app.",
              "OK", NULL);
    return 0;
}

static int install(void) {
    char exe[PATH_MAX], real[PATH_MAX], plist[PATH_MAX], dir[PATH_MAX];
    uint32_t size = sizeof(exe);
    if (_NSGetExecutablePath(exe, &size) != 0 || !realpath(exe, real)) return 1;

    // Gatekeeper runs freshly downloaded apps from a random read-only path; an
    // agent pointing there would break as soon as the app quits.
    if (strstr(real, "/AppTranslocation/")) {
        alert("Move MagicMouseFix first",
              "Drag MagicMouseFix into your Applications folder, then open it "
              "again from there.", "OK", NULL);
        return 1;
    }
    // The path is written into XML; refuse the characters that would need escaping.
    if (strpbrk(real, "<>&")) {
        alert("MagicMouseFix", "Please move the app to a folder whose name does "
              "not contain <, > or &.", "OK", NULL);
        return 1;
    }

    const char *home = getenv("HOME");
    if (!home || plistPath(plist, sizeof(plist)) != 0) return 1;
    snprintf(dir, sizeof(dir), "%s/Library/LaunchAgents", home);
    mkdir(dir, 0755);

    FILE *f = fopen(plist, "w");
    if (!f) {
        alert("MagicMouseFix", "Could not write the login agent file.", "OK", NULL);
        return 1;
    }
    fprintf(f,
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
        "<plist version=\"1.0\">\n<dict>\n"
        "\t<key>Label</key><string>%s</string>\n"
        "\t<key>ProgramArguments</key>\n\t<array>\n"
        "\t\t<string>%s</string>\n\t\t<string>--agent</string>\n\t</array>\n"
        "\t<key>RunAtLoad</key><true/>\n"
        "\t<key>KeepAlive</key><true/>\n"
        "\t<key>ThrottleInterval</key><integer>15</integer>\n"
        "\t<key>ProcessType</key><string>Interactive</string>\n"
        "\t<key>StandardErrorPath</key><string>%s/Library/Logs/MagicMouseFix.log</string>\n"
        "</dict>\n</plist>\n",
        AGENT_LABEL, real, home);
    fclose(f);

    bootout();
    char cmd[PATH_MAX + 128];
    snprintf(cmd, sizeof(cmd), "/bin/launchctl bootstrap gui/%d '%s'",
             (int)getuid(), plist);
    if (system(cmd) != 0) {
        alert("MagicMouseFix", "Could not start the login agent.", "OK", NULL);
        return 1;
    }

    if (IOHIDCheckAccess(kIOHIDRequestTypeListenEvent) == kIOHIDAccessTypeGranted) {
        alert("MagicMouseFix is running",
              "It starts automatically at login and wakes your Magic Mouse "
              "whenever it connects. You can close this.", "OK", NULL);
        return 0;
    }
    IOHIDRequestAccess(kIOHIDRequestTypeListenEvent);
    if (alert("One permission needed",
              "MagicMouseFix is installed. To talk to the mouse it needs Input "
              "Monitoring:\n\nSystem Settings > Privacy & Security > Input "
              "Monitoring > turn on MagicMouseFix.\n\nIt never reads what you "
              "type; macOS simply files all device access under this permission. "
              "It picks up the change within a few seconds.",
              "Open Settings", "Later"))
        system("/usr/bin/open '" SETTINGS_URL "'");
    return 0;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--agent")) return runAgent();
    if (argc > 1 && !strcmp(argv[1], "--uninstall")) return uninstall(0);
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        printf("usage: MagicMouseFix [--agent | --uninstall]\n"
               "  (no arguments)  install the login agent\n");
        return 0;
    }
    return install();
}
