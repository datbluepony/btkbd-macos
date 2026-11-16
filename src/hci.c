// hci.c - user-space Bluetooth HCI transport for a USB dongle (macOS IOKit, no kext)
#include "hci.h"
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static IOUSBInterfaceInterface300 **intf;
static UInt8 pEvt, pAclIn, pAclOut;
static UInt8 evbuf[512], aclbuf[2048];
static hci_event_cb ev_user;
static hci_acl_cb acl_user;

// last command result for synchronous calls
static uint16_t want_op;
static int got_cc;
static uint8_t cc_buf[300];
static int cc_len;

int hci_verbose = 0;
volatile int hci_dead = 0;

static void hexdump(const char *p, const uint8_t *b, int n) {
    if (!hci_verbose) return;
    fprintf(stderr, "%s", p);
    for (int i = 0; i < n && i < 64; i++) fprintf(stderr, " %02x", b[i]);
    if (n > 64) fprintf(stderr, " ...(%d)", n);
    fprintf(stderr, "\n");
}

static void arm_evt(void);
static void arm_acl(void);

static void evt_cb(void *ref, IOReturn r, void *arg0) {
    UInt32 n = (UInt32)(uintptr_t)arg0;
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "event pipe error 0x%x\n", r);
        if (r == kIOReturnAborted) return;
        if (r == kIOReturnNoDevice || r == kIOReturnNotOpen) { if (intf) hci_dead = 1; return; }
        (*intf)->ClearPipeStallBothEnds(intf, pEvt);
        arm_evt();
        return;
    }
    if (n >= 2) {
        hexdump("EVT <", evbuf, n);
        uint8_t code = evbuf[0];
        if (want_op && code == 0x0e && n >= 6 && (evbuf[3] | (evbuf[4] << 8)) == want_op) {
            cc_len = n - 5; memcpy(cc_buf, evbuf + 5, cc_len); got_cc = 1;
        } else if (want_op && code == 0x0f && n >= 6 && (evbuf[4] | (evbuf[5] << 8)) == want_op) {
            cc_len = 1; cc_buf[0] = evbuf[2]; got_cc = 1;   // command status: status byte
        } else if (ev_user) ev_user(evbuf[0], evbuf + 2, n - 2);
    }
    arm_evt();
}

static void acl_cb(void *ref, IOReturn r, void *arg0) {
    UInt32 n = (UInt32)(uintptr_t)arg0;
    if (r != kIOReturnSuccess) {
        fprintf(stderr, "acl pipe error 0x%x\n", r);
        if (r == kIOReturnAborted || r == kIOReturnNoDevice || r == kIOReturnNotOpen) return;
        (*intf)->ClearPipeStallBothEnds(intf, pAclIn);
        arm_acl();
        return;
    }
    if (n >= 4) { hexdump("ACL <", aclbuf, n); if (acl_user) acl_user(aclbuf, n); }
    arm_acl();
}

static void arm_evt(void) { (*intf)->ReadPipeAsync(intf, pEvt, evbuf, sizeof evbuf, evt_cb, NULL); }
static void arm_acl(void) { (*intf)->ReadPipeAsync(intf, pAclIn, aclbuf, sizeof aclbuf, acl_cb, NULL); }

int hci_open(uint16_t vid, uint16_t pid) {
    // the device is registered; its interfaces are not (no driver claimed them), so walk to the child
    CFMutableDictionaryRef m = IOServiceMatching("IOUSBHostDevice");
    int v = vid, p = pid;
    CFNumberRef nv = CFNumberCreate(NULL, kCFNumberIntType, &v);
    CFNumberRef np = CFNumberCreate(NULL, kCFNumberIntType, &p);
    CFDictionarySetValue(m, CFSTR("idVendor"), nv);
    CFDictionarySetValue(m, CFSTR("idProduct"), np);
    io_service_t dev = IOServiceGetMatchingService(kIOMasterPortDefault, m);
    if (!dev) { if (hci_verbose) fprintf(stderr, "dongle %04x:%04x not found\n", vid, pid); return -2; }
    {   // open the device and (re)set configuration 1 so the interfaces get registered
        IOCFPlugInInterface **dp = NULL; SInt32 sc;
        IOUSBDeviceInterface320 **d = NULL;
        if (!IOCreatePlugInInterfaceForService(dev, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &dp, &sc) && dp) {
            (*dp)->QueryInterface(dp, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320), (LPVOID *)&d);
            (*dp)->Release(dp);
        }
        if (d) {
            IOReturn r = (*d)->USBDeviceOpenSeize(d);
            if (r) fprintf(stderr, "device open 0x%x\n", r);
            else {
                UInt8 cur = 0; (*d)->GetConfiguration(d, &cur);
                (*d)->SetConfiguration(d, 0);
                r = (*d)->SetConfiguration(d, 1);
                if (r) fprintf(stderr, "set configuration 0x%x (was %u)\n", r, cur);
                (*d)->USBDeviceClose(d);
            }
            (*d)->Release(d);
        }
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.5, false);
    }
    io_iterator_t it; io_service_t svc = 0, c;
    IORegistryEntryGetChildIterator(dev, kIOServicePlane, &it);
    while ((c = IOIteratorNext(it))) {
        CFTypeRef n = IORegistryEntryCreateCFProperty(c, CFSTR("bInterfaceNumber"), NULL, 0);
        int ifn = -1;
        if (n) { CFNumberGetValue(n, kCFNumberIntType, &ifn); CFRelease(n); }
        if (ifn == 0 && !svc) svc = c; else IOObjectRelease(c);
    }
    IOObjectRelease(it); IOObjectRelease(dev);
    if (!svc) { fprintf(stderr, "dongle interface 0 not found\n"); return -1; }
    IOCFPlugInInterface **plug = NULL; SInt32 score;
    kern_return_t kr = IOCreatePlugInInterfaceForService(svc, kIOUSBInterfaceUserClientTypeID,
                                                         kIOCFPlugInInterfaceID, &plug, &score);
    IOObjectRelease(svc);
    if (kr || !plug) { fprintf(stderr, "plugin failed 0x%x\n", kr); return -1; }
    (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBInterfaceInterfaceID300), (LPVOID *)&intf);
    (*plug)->Release(plug);
    if (!intf) { fprintf(stderr, "no interface\n"); return -1; }
    kr = (*intf)->USBInterfaceOpen(intf);
    if (kr) { fprintf(stderr, "USBInterfaceOpen failed 0x%x (something else has the dongle?)\n", kr); return -1; }
    UInt8 ne = 0; (*intf)->GetNumEndpoints(intf, &ne);
    for (UInt8 i = 1; i <= ne; i++) {
        UInt8 dir, num, type, ival; UInt16 mps;
        (*intf)->GetPipeProperties(intf, i, &dir, &num, &type, &mps, &ival);
        if (type == kUSBInterrupt && dir == kUSBIn) pEvt = i;
        else if (type == kUSBBulk && dir == kUSBIn) pAclIn = i;
        else if (type == kUSBBulk && dir == kUSBOut) pAclOut = i;
    }
    if (!pEvt || !pAclIn || !pAclOut) { fprintf(stderr, "endpoints missing\n"); return -1; }
    CFRunLoopSourceRef src;
    (*intf)->CreateInterfaceAsyncEventSource(intf, &src);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    arm_evt(); arm_acl();
    return 0;
}

void hci_close(void) {
    if (!intf) return;
    (*intf)->USBInterfaceClose(intf);
    (*intf)->Release(intf);
    intf = NULL;
}

void hci_set_callbacks(hci_event_cb e, hci_acl_cb a) { ev_user = e; acl_user = a; }

int hci_send_cmd(uint16_t op, const void *params, uint8_t plen) {
    uint8_t buf[260];
    buf[0] = op & 0xff; buf[1] = op >> 8; buf[2] = plen;
    if (plen) memcpy(buf + 3, params, plen);
    hexdump("CMD >", buf, plen + 3);
    IOUSBDevRequest req;
    req.bmRequestType = USBmakebmRequestType(kUSBOut, kUSBClass, kUSBDevice);
    req.bRequest = 0; req.wValue = 0; req.wIndex = 0;
    req.wLength = plen + 3; req.pData = buf; req.wLenDone = 0;
    IOReturn r = (*intf)->ControlRequest(intf, 0, &req);
    if (r) { fprintf(stderr, "cmd %04x send failed 0x%x\n", op, r); return -1; }
    return 0;
}

int hci_cmd(uint16_t op, const void *params, uint8_t plen, uint8_t *rsp, int *rlen, double timeout) {
    want_op = op; got_cc = 0;
    if (hci_send_cmd(op, params, plen)) { want_op = 0; return -1; }
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + timeout;
    while (!got_cc && CFAbsoluteTimeGetCurrent() < end)
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.02, true);
    want_op = 0;
    if (!got_cc) { fprintf(stderr, "cmd %04x: no answer\n", op); return -1; }
    if (rsp) memcpy(rsp, cc_buf, cc_len);
    if (rlen) *rlen = cc_len;
    return cc_buf[0];   // HCI status (0 = ok)
}

int hci_send_acl(uint16_t handle, const void *data, uint16_t len) {
    uint8_t buf[1100];
    buf[0] = handle & 0xff; buf[1] = (handle >> 8) & 0x0f;   // PB=00: first non-flushable (LE)
    buf[2] = len & 0xff; buf[3] = len >> 8;
    memcpy(buf + 4, data, len);
    hexdump("ACL >", buf, len + 4);
    IOReturn r = (*intf)->WritePipeTO(intf, pAclOut, buf, len + 4, 1000, 2000);
    if (r) { fprintf(stderr, "acl write failed 0x%x\n", r); return -1; }
    return 0;
}

void hci_run(double seconds) { CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, false); }

// ---------- Realtek firmware loader (same steps as Linux btrtl.c) ----------
static uint8_t *readfile(const char *path, long *n) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n); fread(b, 1, *n, f); fclose(f); return b;
}
static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }

int hci_read_version(uint8_t *hci_ver, uint16_t *hci_rev, uint8_t *lmp_ver, uint16_t *lmp_sub) {
    uint8_t r[20]; int n;
    if (hci_cmd(0x1001, NULL, 0, r, &n, 2) != 0 || n < 9) return -1;
    *hci_ver = r[1]; *hci_rev = le16(r + 2); *lmp_ver = r[4]; *lmp_sub = le16(r + 7);
    return 0;
}

int rtl_load_firmware(const char *fw_path, const char *cfg_path) {
    uint8_t hv, lv; uint16_t hr, ls;
    int tries = 0;
    while (hci_read_version(&hv, &hr, &lv, &ls)) {     // the first reply after (re)configuring is sometimes lost
        if (++tries >= 6) return -1;
        (*intf)->AbortPipe(intf, pEvt);
        (*intf)->ClearPipeStallBothEnds(intf, pEvt);
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.3, false);
        arm_evt();
    }
    printf("chip: hci_ver %u hci_rev 0x%04x lmp_ver %u lmp_subver 0x%04x\n", hv, hr, lv, ls);
    if (ls != 0x8761) { printf("firmware already loaded (lmp_subver 0x%04x)\n", ls); return 0; }
    uint8_t r[8]; int n;
    if (hci_cmd(0xfc6d, NULL, 0, r, &n, 2) != 0 || n < 2) { fprintf(stderr, "read rom version failed\n"); return -1; }
    uint8_t rom = r[1];
    printf("rom version %u\n", rom);

    long fl; uint8_t *fw = readfile(fw_path, &fl);
    if (!fw || fl < 20 || memcmp(fw, "Realtech", 8)) { fprintf(stderr, "bad firmware file\n"); return -1; }
    static const uint8_t ext_sig[4] = {0x51, 0x04, 0xfd, 0x77};
    if (memcmp(fw + fl - 4, ext_sig, 4)) { fprintf(stderr, "no extension signature\n"); return -1; }
    // project id, read backwards from the end
    const uint8_t *p = fw + fl - 4; int project = -1;
    while (p >= fw + 14 + 3) {
        uint8_t op = *--p, len = *--p, data = *--p;
        if (op == 0xff) break;
        if (len == 0) break;
        if (op == 0 && len == 1) { project = data; break; }
        p -= len;
    }
    if (project != 14) { fprintf(stderr, "firmware project %d is not RTL8761 (14)\n", project); return -1; }
    uint32_t fwver = le32(fw + 8); uint16_t np = le16(fw + 12);
    const uint8_t *base = fw + 14;
    long plen = -1, poff = -1;
    for (int i = 0; i < np; i++) {
        if (le16(base + i * 2) == rom + 1) {
            plen = le16(base + np * 2 + i * 2);
            poff = le32(base + np * 4 + i * 4);
        }
    }
    if (plen < 4 || poff < 0 || poff + plen > fl) { fprintf(stderr, "no patch for rom %u\n", rom); return -1; }
    long cl = 0; uint8_t *cfg = cfg_path ? readfile(cfg_path, &cl) : NULL;
    if (!cfg) cl = 0;
    long total = plen + cl;
    uint8_t *img = malloc(total);
    memcpy(img, fw + poff, plen);
    memcpy(img + plen - 4, &fwver, 4);
    if (cl) memcpy(img + plen, cfg, cl);
    printf("patch: fw_version 0x%08x, %ld bytes (+%ld config)\n", fwver, plen, cl);

    int frags = (int)((total + 251) / 252);
    for (int i = 0; i < frags; i++) {
        uint8_t cmd[253];
        int idx = i > 0x7f ? ((i & 0x7f) + 1) : i;
        if (i == frags - 1) idx |= 0x80;
        int len = (i == frags - 1) ? (int)(total - i * 252) : 252;
        cmd[0] = idx; memcpy(cmd + 1, img + i * 252, len);
        int st = hci_cmd(0xfc20, cmd, len + 1, r, &n, 3);
        if (st != 0) { fprintf(stderr, "download fragment %d failed (status %d)\n", i, st); return -1; }
    }
    free(img); free(fw); free(cfg);
    if (hci_read_version(&hv, &hr, &lv, &ls)) return -1;
    printf("firmware loaded: lmp_subver now 0x%04x\n", ls);
    return 0;
}
