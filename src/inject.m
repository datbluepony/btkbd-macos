// inject.m - receives key presses from the btkbd service and types them into macOS.
// Runs as a LaunchAgent in the login-window session (as root) and in the user's session.
// It only types while its own session owns the screen, so keys never arrive twice.
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <SystemConfiguration/SystemConfiguration.h>
#import <IOKit/hidsystem/IOHIDLib.h>
#import <IOKit/hidsystem/ev_keymap.h>
#import <IOKit/hidsystem/IOHIDParameter.h>
#ifndef kIOHIDCapsLockState
#define kIOHIDCapsLockState 0x00000001
#endif
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <stdio.h>

#define SOCK "/var/run/btkbd.sock"

static int kc[256];          // HID keyboard usage -> mac virtual key code
static void init_map(void) {
    for (int i = 0; i < 256; i++) kc[i] = -1;
    int letters[26] = {0x00,0x0B,0x08,0x02,0x0E,0x03,0x05,0x04,0x22,0x26,0x28,0x25,0x2E,0x2D,0x1F,0x23,0x0C,0x0F,0x01,0x11,0x20,0x09,0x0D,0x07,0x10,0x06};
    for (int i = 0; i < 26; i++) kc[0x04 + i] = letters[i];
    int digits[10] = {0x12,0x13,0x14,0x15,0x17,0x16,0x1A,0x1C,0x19,0x1D};
    for (int i = 0; i < 10; i++) kc[0x1E + i] = digits[i];
    int a[][2] = {{0x28,0x24},{0x29,0x35},{0x2A,0x33},{0x2B,0x30},{0x2C,0x31},{0x2D,0x1B},{0x2E,0x18},{0x2F,0x21},
        {0x30,0x1E},{0x31,0x2A},{0x32,0x2A},{0x33,0x29},{0x34,0x27},{0x35,0x32},{0x36,0x2B},{0x37,0x2F},{0x38,0x2C},
        {0x39,0x39},{0x3A,0x7A},{0x3B,0x78},{0x3C,0x63},{0x3D,0x76},{0x3E,0x60},{0x3F,0x61},{0x40,0x62},{0x41,0x64},
        {0x42,0x65},{0x43,0x6D},{0x44,0x67},{0x45,0x6F},{0x46,0x69},{0x47,0x6B},{0x48,0x71},{0x49,0x72},{0x4A,0x73},
        {0x4B,0x74},{0x4C,0x75},{0x4D,0x77},{0x4E,0x79},{0x4F,0x7C},{0x50,0x7B},{0x51,0x7D},{0x52,0x7E},{0x53,0x47},
        {0x54,0x4B},{0x55,0x43},{0x56,0x4E},{0x57,0x45},{0x58,0x4C},{0x59,0x53},{0x5A,0x54},{0x5B,0x55},{0x5C,0x56},
        {0x5D,0x57},{0x5E,0x58},{0x5F,0x59},{0x60,0x5B},{0x61,0x5C},{0x62,0x52},{0x63,0x41},{0x64,0x0A},{0x65,0x6E},
        {0x67,0x51},{0x68,0x69},{0x69,0x6B},{0x6A,0x71},{0x6B,0x6A},{0x6C,0x40},{0x6D,0x4F},{0x6E,0x50},{0x6F,0x5A},
        {0x7F,0x4A},{0x80,0x48},{0x81,0x49},
        {0xE0,0x3B},{0xE1,0x38},{0xE2,0x3A},{0xE3,0x37},{0xE4,0x3E},{0xE5,0x3C},{0xE6,0x3D},{0xE7,0x36}};
    for (unsigned i = 0; i < sizeof a / sizeof a[0]; i++) kc[a[i][0]] = a[i][1];
}

// modifier bit -> (generic flag, device-dependent flag)
static const uint64_t MODF[8] = {kCGEventFlagMaskControl, kCGEventFlagMaskShift, kCGEventFlagMaskAlternate, kCGEventFlagMaskCommand,
                                 kCGEventFlagMaskControl, kCGEventFlagMaskShift, kCGEventFlagMaskAlternate, kCGEventFlagMaskCommand};
static const uint64_t MODD[8] = {0x01, 0x02, 0x20, 0x08, 0x2000, 0x04, 0x40, 0x10};
static int mods;              // bits 0..7 = E0..E7 held
static CGEventSourceRef src;
static io_connect_t hidsys;

static uint64_t flags(void) {
    uint64_t f = 0x100;       // non-coalesced, like real key events
    for (int i = 0; i < 8; i++) if (mods & (1 << i)) f |= MODF[i] | MODD[i];
    bool caps = false;
    if (hidsys && IOHIDGetModifierLockState(hidsys, kIOHIDCapsLockState, &caps) == KERN_SUCCESS && caps) f |= kCGEventFlagMaskAlphaShift;
    return f;
}

static int my_turn(void) {    // does our session own the screen right now?
    uid_t uid = 99;
    CFStringRef u = SCDynamicStoreCopyConsoleUser(NULL, &uid, NULL);
    int atlogin = !u || CFStringCompare(u, CFSTR("loginwindow"), 0) == kCFCompareEqualTo;
    if (u) CFRelease(u);
    if (getuid() == 0) return atlogin;
    return !atlogin && uid == getuid();
}

static void post_key(int code, bool down, bool repeat) {
    CGEventRef e = CGEventCreateKeyboardEvent(src, (CGKeyCode)code, down);
    CGEventSetFlags(e, flags());
    if (repeat) CGEventSetIntegerValueField(e, kCGKeyboardEventAutorepeat, 1);
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

static void post_mod(int bit, bool down) {
    if (down) mods |= 1 << bit; else mods &= ~(1 << bit);
    CGEventRef e = CGEventCreateKeyboardEvent(src, (CGKeyCode)kc[0xE0 + bit], down);
    CGEventSetType(e, kCGEventFlagsChanged);
    CGEventSetFlags(e, flags());
    CGEventPost(kCGHIDEventTap, e);
    CFRelease(e);
}

static void post_media(int key, bool down) {
    NSEvent *ev = [NSEvent otherEventWithType:NSEventTypeSystemDefined location:NSZeroPoint
                                 modifierFlags:(down ? 0xa00 : 0xb00) timestamp:0 windowNumber:0 context:nil
                                       subtype:8 data1:((key << 16) | ((down ? 0xa : 0xb) << 8)) data2:-1];
    CGEventPost(kCGHIDEventTap, [ev CGEvent]);
}

static int media_key(int usage) {
    switch (usage) {
    case 0xE9: return NX_KEYTYPE_SOUND_UP;   case 0xEA: return NX_KEYTYPE_SOUND_DOWN;
    case 0xE2: return NX_KEYTYPE_MUTE;       case 0xCD: return NX_KEYTYPE_PLAY;
    case 0xB5: return NX_KEYTYPE_NEXT;       case 0xB6: return NX_KEYTYPE_PREVIOUS;
    case 0xB3: return NX_KEYTYPE_FAST;       case 0xB4: return NX_KEYTYPE_REWIND;
    case 0x6F: return NX_KEYTYPE_BRIGHTNESS_UP; case 0x70: return NX_KEYTYPE_BRIGHTNESS_DOWN;
    case 0xB8: return NX_KEYTYPE_EJECT;
    }
    return -1;
}

// key repeat (posted events don't auto-repeat like a real keyboard)
static int rep_code = -1; static CFRunLoopTimerRef rep_timer; static double rep_first = 0.4, rep_every = 0.05;
static void rep_fire(CFRunLoopTimerRef t, void *i) { if (rep_code >= 0 && my_turn()) post_key(rep_code, true, true); }
static void rep_start(int code) {
    rep_code = code;
    if (rep_timer) { CFRunLoopTimerInvalidate(rep_timer); CFRelease(rep_timer); }
    rep_timer = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + rep_first, rep_every, 0, 0, rep_fire, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), rep_timer, kCFRunLoopDefaultMode);
}
static void rep_stop(int code) {
    if (code != rep_code && code != -2) return;
    rep_code = -1;
    if (rep_timer) { CFRunLoopTimerInvalidate(rep_timer); CFRelease(rep_timer); rep_timer = NULL; }
}
static void read_repeat_prefs(void) {
    CFPreferencesAppSynchronize(kCFPreferencesAnyApplication);
    Boolean ok; CFIndex v;
    v = CFPreferencesGetAppIntegerValue(CFSTR("InitialKeyRepeat"), kCFPreferencesAnyApplication, &ok); if (ok && v > 0) rep_first = v * 0.015;
    v = CFPreferencesGetAppIntegerValue(CFSTR("KeyRepeat"), kCFPreferencesAnyApplication, &ok); if (ok && v > 0) rep_every = v * 0.015;
}

// app-launch keys (consumer page): only in a logged-in session, never at the login window
static void run_script(NSString *src) {
    NSAppleScript *s = [[NSAppleScript alloc] initWithSource:src];
    NSDictionary *err = nil; [s executeAndReturnError:&err];
    if (err) NSLog(@"btkbd-inject: script error %@", err);
}
static int app_key(int usage) {
    if (getuid() == 0) return 0;
    NSWorkspace *ws = [NSWorkspace sharedWorkspace];
    switch (usage) {
    case 0x183: [ws launchApplication:@"iTunes"]; return 1;                                    // media player
    case 0xB7: run_script(@"if application \"iTunes\" is running then tell application \"iTunes\" to stop"); return 1;  // stop
    case 0x223: [ws openURL:[NSURL URLWithString:@"https://www.google.com"]]; return 1;        // home
    case 0x18A: [ws openURL:[NSURL URLWithString:@"https://mail.google.com"]]; return 1;       // mail
    case 0x194: run_script(@"tell application \"Finder\"\nactivate\nmake new Finder window to computer container\nend tell"); return 1;  // my computer
    case 0x22A: case 0x1B4: case 0x1A7:                                                         // files / bookmarks / documents
        [ws openURL:[NSURL fileURLWithPath:NSHomeDirectory()]]; return 1;
    }
    return 0;
}

// Fn swap: the keyboard sends F1..F12 normally and these consumer codes with Fn held. We swap them, so the
// media/app action is the default and Fn+F-key gives a real F-key (like a Mac keyboard).
// Turn off with: sudo touch /usr/local/btkbd/standard-fkeys
static const int FN_CODES[12] = {0x183, 0xEA, 0xE9, 0xE2, 0xB6, 0xB5, 0xCD, 0xB7, 0x223, 0x18A, 0x194, 0x22A};
static int fn_swap = -1;
static void swap_fn(int *page, int *usage) {
    if (fn_swap < 0) fn_swap = access("/usr/local/btkbd/standard-fkeys", F_OK) != 0;
    if (!fn_swap) return;
    if (*page == 7 && *usage >= 0x3A && *usage <= 0x45) { *usage = FN_CODES[*usage - 0x3A]; *page = 0x0C; return; }
    if (*page == 0x0C) for (int i = 0; i < 12; i++) if (FN_CODES[i] == *usage) { *page = 7; *usage = 0x3A + i; return; }
}

static void handle(char d, int page, int usage) {
    bool down = d == 'D';
    swap_fn(&page, &usage);
    if (!my_turn()) { if (!down) rep_stop(-2); return; }
    if (page == 7 && usage >= 0xE0 && usage <= 0xE7) { post_mod(usage - 0xE0, down); return; }
    if (page == 7 && usage == 0x39) {                          // Caps Lock toggles the lock state
        if (down && hidsys) { bool c = false; IOHIDGetModifierLockState(hidsys, kIOHIDCapsLockState, &c); IOHIDSetModifierLockState(hidsys, kIOHIDCapsLockState, !c); }
        return;
    }
    if (page == 7 && usage < 256 && kc[usage] >= 0) {
        if (down) { read_repeat_prefs(); post_key(kc[usage], true, false); rep_start(kc[usage]); }
        else { rep_stop(kc[usage]); post_key(kc[usage], false, false); }
        return;
    }
    if (page == 0x0C) {
        int m = media_key(usage);
        if (m >= 0) post_media(m, down);
        else if (down) app_key(usage);
    }
}

int main(void) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        init_map();
        src = CGEventSourceCreate(kCGEventSourceStateHIDSystemState);
        io_service_t s = IOServiceGetMatchingService(kIOMasterPortDefault, IOServiceMatching(kIOHIDSystemClass));
        if (s) { IOServiceOpen(s, mach_task_self(), kIOHIDParamConnectType, &hidsys); IOObjectRelease(s); }
        printf("btkbd-inject started (uid %d)\n", getuid());
        for (;;) {
            int fd = socket(AF_UNIX, SOCK_STREAM, 0);
            struct sockaddr_un a = {0}; a.sun_family = AF_UNIX; strlcpy(a.sun_path, SOCK, sizeof a.sun_path);
            if (connect(fd, (struct sockaddr *)&a, sizeof a)) { close(fd); sleep(1); continue; }
            printf("connected to btkbd\n");
            char buf[512]; int have = 0;
            for (;;) {
                // wait for data while letting the repeat timer run
                fd_set rs; FD_ZERO(&rs); FD_SET(fd, &rs);
                struct timeval tv = {0, 10000};
                int r = select(fd + 1, &rs, NULL, NULL, &tv);
                CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
                if (r <= 0) continue;
                ssize_t n = read(fd, buf + have, sizeof buf - 1 - have);
                if (n <= 0) break;
                have += n; buf[have] = 0;
                char *line = buf, *nl;
                while ((nl = strchr(line, '\n'))) {
                    *nl = 0; char d; unsigned p, u;
                    if (sscanf(line, "%c %x %x", &d, &p, &u) == 3) handle(d, p, u);
                    line = nl + 1;
                }
                have = (int)strlen(line); memmove(buf, line, have);
            }
            close(fd);
            rep_stop(-2);
            for (int i = 0; i < 8; i++) if (mods & (1 << i)) post_mod(i, false);
            printf("btkbd went away, reconnecting\n");
            sleep(1);
        }
    }
}
