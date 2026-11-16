// btkbd.c - user-space Bluetooth LE keyboard driver for macOS (tested on 10.13 High Sierra) + a Realtek RTL8761B USB dongle.
//   btkbd pair [name-filter]   pair a keyboard (put it in pairing mode first)
//   btkbd                      run: reconnect to the paired keyboard and type its keys into macOS
// HCI over IOKit (hci.c), LE legacy pairing (SMP), GATT HID-over-GATT client, IOHIDUserDevice output (vhid.c).
#include "hci.h"
#include "hidparse.h"
#include <CommonCrypto/CommonCryptor.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <errno.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>
#include <sys/stat.h>

// USB IDs of RTL8761B / RTL8761BU dongles (same list as Linux btusb). Override: BTKBD_USB=vvvv:pppp
static const uint16_t DONGLES[][2] = {
    {0x2357, 0x0604},   // TP-Link UB500 (tested)
    {0x0bda, 0x8771},   // Realtek RTL8761BU reference / many no-name sticks
    {0x0b05, 0x190e},   // ASUS USB-BT500
    {0x7392, 0xc611},   // Edimax BT-8500
    {0x2550, 0x8761},   // Realtek RTL8761BU (other vendors)
};
static const char *BOND = "/usr/local/btkbd/bond.txt";
static const char *FW = "/usr/local/btkbd/rtl8761bu_fw.bin";
static const char *CFG = "/usr/local/btkbd/rtl8761bu_config.bin";

static void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logf_(const char *fmt, ...) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    char ts[32]; strftime(ts, sizeof ts, "%m-%d %H:%M:%S", &tm);
    printf("%s ", ts);
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    printf("\n"); fflush(stdout);
}
#define LOG(...) logf_(__VA_ARGS__)

static uint16_t le16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static void put16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static const char *astr(const uint8_t *a) {
    static char b[4][20]; static int i; i = (i + 1) & 3;
    snprintf(b[i], 20, "%02X:%02X:%02X:%02X:%02X:%02X", a[5], a[4], a[3], a[2], a[1], a[0]);
    return b[i];
}
static void hex(char *out, const uint8_t *b, int n) { for (int i = 0; i < n; i++) sprintf(out + 2 * i, "%02x", b[i]); }
static int unhex(uint8_t *out, const char *s, int max) {
    int n = 0; while (s[0] && s[1] && n < max) { unsigned v; if (sscanf(s, "%2x", &v) != 1) break; out[n++] = v; s += 2; }
    return n;
}

// ---------------- crypto (Bluetooth "e" = AES-128 on big-endian values; our arrays are little-endian) ----------------
static void e_(const uint8_t k[16], const uint8_t in[16], uint8_t out[16]) {
    uint8_t K[16], P[16], C[16]; size_t m;
    for (int i = 0; i < 16; i++) { K[i] = k[15 - i]; P[i] = in[15 - i]; }
    CCCrypt(kCCEncrypt, kCCAlgorithmAES128, kCCOptionECBMode, K, 16, NULL, P, 16, C, 16, &m);
    for (int i = 0; i < 16; i++) out[i] = C[15 - i];
}
static void c1(const uint8_t k[16], const uint8_t r[16], const uint8_t preq[7], const uint8_t pres[7],
               uint8_t iat, const uint8_t ia[6], uint8_t rat, const uint8_t ra[6], uint8_t out[16]) {
    uint8_t p1[16], p2[16], t[16];
    p1[0] = iat; p1[1] = rat; memcpy(p1 + 2, preq, 7); memcpy(p1 + 9, pres, 7);
    memset(p2, 0, 16); memcpy(p2, ra, 6); memcpy(p2 + 6, ia, 6);
    for (int i = 0; i < 16; i++) t[i] = r[i] ^ p1[i];
    e_(k, t, t);
    for (int i = 0; i < 16; i++) t[i] ^= p2[i];
    e_(k, t, out);
}
static void s1(const uint8_t k[16], const uint8_t r1[16], const uint8_t r2[16], uint8_t out[16]) {
    uint8_t r[16]; memcpy(r, r2, 8); memcpy(r + 8, r1, 8); e_(k, r, out);
}
static int rpa_match(const uint8_t irk[16], const uint8_t a[6]) {
    if ((a[5] & 0xc0) != 0x40) return 0;
    uint8_t r[16] = {0}; memcpy(r, a + 3, 3); e_(irk, r, r);
    return memcmp(r, a, 3) == 0;
}

// ---------------- bond (saved pairing + GATT layout) ----------------
#define MAXREP 16
typedef struct { uint16_t val, cccd; uint8_t id, type; int last_len; } report_t;
static struct {
    int valid;
    uint8_t addr[6], type;          // identity address
    int has_irk; uint8_t irk[16];
    uint8_t ltk[16], rand[8]; uint16_t ediv; int keysize;
    char name[64];
    uint8_t map[1024]; int maplen; int uses_ids;
    report_t rep[MAXREP]; int nrep;
    uint16_t protomode;
} B;

static int bond_save(void) {
    char tmp[300]; snprintf(tmp, sizeof tmp, "%s.tmp", BOND);
    FILE *f = fopen(tmp, "w"); if (!f) { perror(tmp); return -1; }
    char h[2100];
    hex(h, B.addr, 6); fprintf(f, "addr %s\ntype %u\n", h, B.type);
    if (B.has_irk) { hex(h, B.irk, 16); fprintf(f, "irk %s\n", h); }
    hex(h, B.ltk, 16); fprintf(f, "ltk %s\n", h);
    hex(h, B.rand, 8); fprintf(f, "rand %s\nediv %u\nkeysize %d\n", h, B.ediv, B.keysize);
    fprintf(f, "name %s\n", B.name);
    hex(h, B.map, B.maplen); fprintf(f, "map %s\n", h);
    fprintf(f, "protomode %u\n", B.protomode);
    for (int i = 0; i < B.nrep; i++) fprintf(f, "report %u %u %u %u\n", B.rep[i].val, B.rep[i].cccd, B.rep[i].id, B.rep[i].type);
    fclose(f);
    chmod(tmp, 0600);
    return rename(tmp, BOND);
}

static int bond_load(void) {
    FILE *f = fopen(BOND, "r"); if (!f) return -1;
    memset(&B, 0, sizeof B);
    char line[2200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *v = strchr(line, ' '); if (!v) continue; *v++ = 0;
        if (!strcmp(line, "addr")) unhex(B.addr, v, 6);
        else if (!strcmp(line, "type")) B.type = atoi(v);
        else if (!strcmp(line, "irk")) B.has_irk = unhex(B.irk, v, 16) == 16;
        else if (!strcmp(line, "ltk")) unhex(B.ltk, v, 16);
        else if (!strcmp(line, "rand")) unhex(B.rand, v, 8);
        else if (!strcmp(line, "ediv")) B.ediv = atoi(v);
        else if (!strcmp(line, "keysize")) B.keysize = atoi(v);
        else if (!strcmp(line, "name")) strlcpy(B.name, v, sizeof B.name);
        else if (!strcmp(line, "map")) B.maplen = unhex(B.map, v, sizeof B.map);
        else if (!strcmp(line, "protomode")) B.protomode = atoi(v);
        else if (!strcmp(line, "report") && B.nrep < MAXREP) {
            unsigned a, b, c, d;
            if (sscanf(v, "%u %u %u %u", &a, &b, &c, &d) == 4) {
                report_t *r = &B.rep[B.nrep++]; r->val = a; r->cccd = b; r->id = c; r->type = d;
            }
        }
    }
    fclose(f);
    for (int i = 0; i < B.maplen; i++) if (B.map[i] == 0x85) B.uses_ids = 1;   // crude, refined below
    B.valid = B.maplen > 0 && B.nrep > 0;
    return B.valid ? 0 : -1;
}

// walk HID descriptor items properly to see whether Report IDs are used
static int map_uses_ids(const uint8_t *d, int n) {
    for (int i = 0; i < n;) {
        uint8_t b = d[i];
        if (b == 0xfe) { if (i + 2 >= n) break; i += 3 + d[i + 1]; continue; }   // long item
        int sz = b & 3; if (sz == 3) sz = 4;
        if ((b & 0xfc) == 0x84) return 1;   // Report ID (global, tag 8)
        i += 1 + sz;
    }
    return 0;
}

// ---------------- connection state ----------------
static int conn = -1;                  // connection handle, -1 = none
static uint8_t peer[6], peer_type, own[6];
static int enc_done, enc_status;
static int want_scan_pair; static const char *name_filter;
static int found; static uint8_t found_addr[6], found_type; static char found_name[64];
static int conn_done, conn_status;

// L2CAP reassembly
static uint8_t l2buf[1100]; static int l2len, l2need;

// ATT synchronous request
static int att_wait, att_got; static uint8_t att_rsp[600]; static int att_rlen;
static uint16_t mtu = 23;

// SMP queue
static uint8_t smpq[8][80]; static int smpl[8]; static int smp_h, smp_t;
static int sec_req;

static void release_keys(void);

static void l2_send(uint16_t cid, const uint8_t *d, int n) {
    uint8_t b[600]; put16(b, n); put16(b + 2, cid); memcpy(b + 4, d, n);
    if (conn >= 0) hci_send_acl(conn, b, n + 4);
}

static int wait_flag(volatile int *flag, double secs) {
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + secs;
    while (!*flag && conn >= 0 && !hci_dead && CFAbsoluteTimeGetCurrent() < end) hci_run(0.02);
    return *flag;
}

static int att_req(const uint8_t *req, int n, double secs) {
    att_got = 0; att_wait = 1;
    l2_send(4, req, n);
    wait_flag(&att_got, secs);
    att_wait = 0;
    if (!att_got) { LOG("GATT request 0x%02x: no answer", req[0]); return -1; }
    return att_rsp[0];
}

// ---------------- key state -> local socket for the injector helpers ----------------
#define SOCK "/var/run/btkbd.sock"
static int lfd = -1; static int cl[8]; static int ncl;
static uint32_t held[MAXREP][32]; static int nheld[MAXREP];
static uint32_t prev[64]; static int nprev;
static int caps_on;
static void set_leds(void);

static void broadcast(const char *s) {
    for (int i = 0; i < ncl;) {
        if (send(cl[i], s, strlen(s), MSG_DONTWAIT) < 0 && errno != EAGAIN) { close(cl[i]); cl[i] = cl[--ncl]; continue; }
        i++;
    }
}

static void key_diff(void) {
    uint32_t now[64]; int nn = 0;
    for (int r = 0; r < MAXREP; r++)
        for (int k = 0; k < nheld[r] && nn < 64; k++) {
            int dup = 0; for (int j = 0; j < nn; j++) if (now[j] == held[r][k]) dup = 1;
            if (!dup) now[nn++] = held[r][k];
        }
    char line[40];
    for (int j = 0; j < nprev; j++) {               // released
        int still = 0; for (int k = 0; k < nn; k++) if (now[k] == prev[j]) still = 1;
        if (!still) { snprintf(line, sizeof line, "U %x %x\n", prev[j] >> 16, prev[j] & 0xffff); broadcast(line); }
    }
    for (int k = 0; k < nn; k++) {                  // pressed
        int was = 0; for (int j = 0; j < nprev; j++) if (prev[j] == now[k]) was = 1;
        if (!was) {
            snprintf(line, sizeof line, "D %x %x\n", now[k] >> 16, now[k] & 0xffff); broadcast(line);            if (now[k] == 0x70039) { caps_on ^= 1; set_leds(); }
        }
    }
    memcpy(prev, now, nn * sizeof(uint32_t)); nprev = nn;
}

static void on_accept(CFSocketRef s, CFSocketCallBackType t, CFDataRef a, const void *data, void *info) {
    int fd = *(const int *)data;
    uid_t uid; gid_t gid; struct stat st;
    if (getpeereid(fd, &uid, &gid) || (uid != 0 && (stat("/dev/console", &st) || st.st_uid != uid)) || ncl >= 8) { close(fd); return; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
    fcntl(fd, F_SETFL, O_NONBLOCK);
    cl[ncl++] = fd;
}

static int sock_listen(void) {
    unlink(SOCK);
    lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a = {0}; a.sun_family = AF_UNIX; strlcpy(a.sun_path, SOCK, sizeof a.sun_path);
    if (bind(lfd, (struct sockaddr *)&a, sizeof a) || listen(lfd, 8)) { perror("socket"); return -1; }
    chmod(SOCK, 0666);       // anyone may connect; on_accept only keeps root + the console user
    CFSocketRef s = CFSocketCreateWithNative(NULL, lfd, kCFSocketAcceptCallBack, on_accept, NULL);
    CFRunLoopSourceRef src = CFSocketCreateRunLoopSource(NULL, s, 0);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), src, kCFRunLoopDefaultMode);
    return 0;
}

static void on_notify(uint16_t h, const uint8_t *v, int n) {
    for (int i = 0; i < B.nrep; i++) {
        report_t *r = &B.rep[i];
        if (r->val != h || r->type != 1) continue;
        r->last_len = n;
        nheld[i] = hid_decode(B.uses_ids ? r->id : 0, v, n, held[i], 32);
        if (hci_verbose) { printf("key report id %u:", r->id); for (int j = 0; j < n; j++) printf(" %02x", v[j]); printf("\n"); }
        key_diff();
        return;
    }
}

static void att_in(const uint8_t *p, int n) {
    uint8_t op = p[0], r[32];
    switch (op) {
    case 0x1b: if (n >= 3) on_notify(le16(p + 1), p + 3, n - 3); return;            // notification
    case 0x1d: if (n >= 3) on_notify(le16(p + 1), p + 3, n - 3); r[0] = 0x1e; l2_send(4, r, 1); return;  // indication
    case 0x02: r[0] = 0x03; put16(r + 1, 23); l2_send(4, r, 3); return;               // their MTU request
    case 0x52: case 0xd2: return;                                                       // write command: ignore
    case 0x04: case 0x06: case 0x08: case 0x10:                                         // discovery on us: we have nothing
        r[0] = 0x01; r[1] = op; put16(r + 2, n >= 3 ? le16(p + 1) : 0); r[4] = 0x0a; l2_send(4, r, 5); return;
    case 0x0a: case 0x0c: case 0x12: case 0x16: case 0x18:
        r[0] = 0x01; r[1] = op; put16(r + 2, n >= 3 ? le16(p + 1) : 0); r[4] = 0x01; l2_send(4, r, 5); return;
    case 0x0e: case 0x20:
        r[0] = 0x01; r[1] = op; put16(r + 2, 0); r[4] = 0x06; l2_send(4, r, 5); return;
    }
    if (att_wait && !att_got) { att_rlen = n < 600 ? n : 600; memcpy(att_rsp, p, att_rlen); att_got = 1; }
}

static void sig_in(const uint8_t *p, int n) {
    if (n < 4) return;
    uint8_t code = p[0], id = p[1], r[16];
    if (code == 0x12 && n >= 12) {           // connection parameter update request: accept and apply
        uint16_t mn = le16(p + 4), mx = le16(p + 6), lat = le16(p + 8), to = le16(p + 10);
        r[0] = 0x13; r[1] = id; put16(r + 2, 2); put16(r + 4, 0); l2_send(5, r, 6);
        uint8_t c[14]; put16(c, conn); put16(c + 2, mn); put16(c + 4, mx); put16(c + 6, lat); put16(c + 8, to); put16(c + 10, 0); put16(c + 12, 0);
        hci_send_cmd(0x2013, c, 14);
        LOG("keyboard asked for interval %.1f-%.1f ms, latency %u: accepted", mn * 1.25, mx * 1.25, lat);
    } else if (code != 0x13 && code != 0x01) {
        r[0] = 0x01; r[1] = id; put16(r + 2, 2); put16(r + 4, 0); l2_send(5, r, 6);   // command reject
    }
}

static void smp_in(const uint8_t *p, int n) {
    if (p[0] == 0x0b) { sec_req = 1; return; }
    int nx = (smp_t + 1) & 7; if (nx == smp_h) return;
    memcpy(smpq[smp_t], p, n < 80 ? n : 80); smpl[smp_t] = n; smp_t = nx;
}
static int smp_get(uint8_t *out, double secs) {
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + secs;
    while (smp_h == smp_t && conn >= 0 && CFAbsoluteTimeGetCurrent() < end) hci_run(0.02);
    if (smp_h == smp_t) return -1;
    int n = smpl[smp_h]; memcpy(out, smpq[smp_h], n); smp_h = (smp_h + 1) & 7; return n;
}

static void on_acl(const uint8_t *pkt, int len) {
    uint16_t h = le16(pkt) & 0x0fff; int pb = (pkt[1] >> 4) & 3; int dl = le16(pkt + 2);
    if (h != conn || dl + 4 > len) return;
    const uint8_t *d = pkt + 4;
    if (pb != 1) {                    // start of an L2CAP frame
        if (dl < 4) return;
        l2need = le16(d) + 4; l2len = 0;
        if (l2need > (int)sizeof l2buf) { l2need = 0; return; }
    }
    if (!l2need || l2len + dl > l2need) { l2need = 0; return; }
    memcpy(l2buf + l2len, d, dl); l2len += dl;
    if (l2len < l2need) return;
    uint16_t cid = le16(l2buf + 2); int n = l2need - 4; l2need = 0;
    if (cid == 4) att_in(l2buf + 4, n);
    else if (cid == 5) sig_in(l2buf + 4, n);
    else if (cid == 6) smp_in(l2buf + 4, n);
}

static int adv_is_ours(const uint8_t *a, uint8_t type, int evt) {
    if (!B.valid) return 0;
    if (!memcmp(a, B.addr, 6) && type == B.type) return 1;
    if (B.has_irk && type == 1 && rpa_match(B.irk, a)) return 1;
    return 0;
}

static void adv_one(uint8_t evt, uint8_t type, const uint8_t *a, const uint8_t *d, int dl, int8_t rssi) {
    if (found) return;
    if (!want_scan_pair) {
        if (adv_is_ours(a, type, evt)) { memcpy(found_addr, a, 6); found_type = type; found = 1; }
        return;
    }
    if (evt == 1 || evt == 2 || evt == 3) return;      // directed / non-connectable / scannable-only: skip
    char name[64] = ""; int hid = 0, kbd = 0;
    for (int k = 0; k + 1 < dl;) {
        int l = d[k]; if (!l || k + l >= dl + 1) break;
        int t = d[k + 1];
        if ((t == 8 || t == 9) && l - 1 < 63) { memcpy(name, d + k + 2, l - 1); name[l - 1] = 0; }
        if (t == 2 || t == 3) for (int j = 2; j + 1 <= l; j += 2) if (le16(d + k + j) == 0x1812) hid = 1;
        if (t == 0x19 && l >= 3) { int ap = le16(d + k + 2); if (ap == 0x3c1 || ap == 0x3c0) kbd = 1; }
        k += l + 1;
    }
    int ok = name_filter ? (strcasestr(name, name_filter) != NULL) : (hid || kbd);
    if (name[0] || hid || kbd)
        LOG("  seen %s %s rssi %d%s%s", astr(a), name[0] ? name : "(no name)", rssi, hid ? " HID" : "", kbd ? " keyboard" : "");
    if (ok) { memcpy(found_addr, a, 6); found_type = type; strlcpy(found_name, name, 64); found = 1; }
}

static void on_event(uint8_t code, const uint8_t *p, int len) {
    if (code == 0x05 && len >= 4) {                       // disconnection complete
        if (le16(p + 1) == conn) {
            LOG("disconnected (reason 0x%02x)", p[3]);
            release_keys();
            conn = -1; l2need = 0;
        }
    } else if (code == 0x08 && len >= 4) {                // encryption change
        if (le16(p + 1) == conn) { enc_status = p[0] ? p[0] : (p[3] ? 0 : 0xff); enc_done = 1; }
    } else if (code == 0x30 && len >= 3) {                // key refresh complete
        if (le16(p + 1) == conn) { enc_status = p[0]; enc_done = 1; }
    } else if (code == 0x3e && len >= 1) {
        uint8_t sub = p[0];
        if (sub == 0x01 && len >= 19) {                   // connection complete
            conn_status = p[1]; conn_done = 1;
            if (!p[1]) { conn = le16(p + 2); peer_type = p[5]; memcpy(peer, p + 6, 6); }
        } else if (sub == 0x02 && len >= 2) {             // advertising reports (array layout)
            int num = p[1]; const uint8_t *q = p + 2;
            const uint8_t *evts = q, *types = q + num, *addrs = q + 2 * num, *lens = q + 8 * num;
            const uint8_t *data = q + 9 * num; int total = 0;
            for (int i = 0; i < num; i++) total += lens[i];
            if (2 + 10 * num + total > len) return;
            const uint8_t *rssis = data + total; int off = 0;
            for (int i = 0; i < num; i++) { adv_one(evts[i], types[i], addrs + 6 * i, data + off, lens[i], (int8_t)rssis[i]); off += lens[i]; }
        } else if (sub == 0x03 && len >= 10) {
            if (!p[1]) LOG("link: interval %.2f ms, latency %u, timeout %u ms", le16(p + 4) * 1.25, le16(p + 6), le16(p + 8) * 10);
        }
    }
}

static void release_keys(void) {
    memset(nheld, 0, sizeof nheld);
    key_diff();
}

static void set_leds(void) {                 // Caps Lock light on the keyboard
    if (conn < 0 || !enc_done || enc_status) return;
    for (int i = 0; i < B.nrep; i++) if (B.rep[i].type == 2) {
        uint8_t b[4]; b[0] = 0x52; put16(b + 1, B.rep[i].val); b[3] = caps_on ? 0x02 : 0x00;
        l2_send(4, b, 4);
        return;
    }
}

// ---------------- HCI setup / scanning / connecting ----------------
static int hci_setup(void) {
    uint8_t r[64]; int n;
    if (rtl_load_firmware(FW, CFG)) return -1;
    if (hci_cmd(0x0c03, NULL, 0, r, &n, 3)) { LOG("reset failed"); return -1; }
    uint8_t mask[8] = {0xff, 0xff, 0xfb, 0xff, 0x07, 0xf8, 0xbf, 0x3d};
    hci_cmd(0x0c01, mask, 8, r, &n, 2);
    uint8_t lemask[8] = {0x1f, 0, 0, 0, 0, 0, 0, 0};
    hci_cmd(0x2001, lemask, 8, r, &n, 2);
    if (hci_cmd(0x1009, NULL, 0, r, &n, 2) == 0) memcpy(own, r + 1, 6);
    LOG("dongle ready, address %s", astr(own));
    return 0;
}

static void scan(int on, int active) {
    uint8_t r[8]; int n;
    uint8_t en[2] = {0, 0};
    hci_cmd(0x200c, en, 2, r, &n, 2);
    if (!on) return;
    uint8_t sp[7] = {active, 0x30, 0x00, 0x30, 0x00, 0x00, 0x00};
    hci_cmd(0x200b, sp, 7, r, &n, 2);
    en[0] = 1;
    hci_cmd(0x200c, en, 2, r, &n, 2);
}

static int connect_to(const uint8_t *a, uint8_t type) {
    uint8_t c[25], r[8]; int n;
    put16(c, 0x0060); put16(c + 2, 0x0060); c[4] = 0; c[5] = type; memcpy(c + 6, a, 6); c[12] = 0;
    put16(c + 13, 0x0006); put16(c + 15, 0x000c); put16(c + 17, 0); put16(c + 19, 0x0190); put16(c + 21, 0); put16(c + 23, 0);
    conn_done = 0;
    int st = hci_cmd(0x200d, c, 25, r, &n, 2);
    if (st) { LOG("connect command failed (0x%02x)", st); return -1; }
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + 4;
    while (!conn_done && !hci_dead && CFAbsoluteTimeGetCurrent() < end) hci_run(0.02);
    if (!conn_done) {
        hci_cmd(0x200e, NULL, 0, r, &n, 2);                    // cancel
        CFAbsoluteTime e2 = CFAbsoluteTimeGetCurrent() + 1;
        while (!conn_done && CFAbsoluteTimeGetCurrent() < e2) hci_run(0.02);
        if (conn >= 0 && conn_status == 0) return 0;           // it connected right as we cancelled
        return -1;
    }
    if (conn_status) { LOG("connect failed (0x%02x)", conn_status); return -1; }
    l2need = 0; smp_h = smp_t = 0; sec_req = 0; enc_done = 0; mtu = 23;
    LOG("connected to %s", astr(peer));
    return 0;
}

static void disconnect(void) {
    if (conn < 0) return;
    uint8_t c[3], r[8]; int n; put16(c, conn); c[2] = 0x13;
    hci_cmd(0x0406, c, 3, r, &n, 2);
    CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + 2;
    while (conn >= 0 && CFAbsoluteTimeGetCurrent() < end) hci_run(0.02);
    conn = -1;
}

static int start_encryption(const uint8_t ltk[16], uint16_t ediv, const uint8_t rnd[8]) {
    uint8_t c[28], r[8]; int n;
    put16(c, conn); memcpy(c + 2, rnd, 8); put16(c + 10, ediv); memcpy(c + 12, ltk, 16);
    enc_done = 0; enc_status = 0;
    int st = hci_cmd(0x2019, c, 28, r, &n, 2);
    if (st) { LOG("start encryption failed (0x%02x)", st); return -1; }
    wait_flag(&enc_done, 8);
    if (!enc_done) { LOG("encryption: no answer"); return -1; }
    if (enc_status) { LOG("encryption failed (0x%02x)%s", enc_status, enc_status == 0x06 ? ": the keyboard forgot us, pair again" : ""); return -1; }
    return 0;
}

// ---------------- pairing (LE legacy, we display a passkey if the keyboard can type one) ----------------
static int do_pair(void) {
    uint8_t preq[7] = {0x01, 0x00 /*DisplayOnly*/, 0x00, 0x05 /*bond + MITM*/, 16, 0x00, 0x03};
    uint8_t pres[7], p[80];
    smp_h = smp_t = 0;
    l2_send(6, preq, 7);
    int n = smp_get(p, 30);
    if (n < 7 || p[0] != 0x02) { LOG("pairing refused (%s 0x%02x)", n > 0 && p[0] == 0x05 ? "reason" : "reply", n > 1 ? p[1] : 0); return -1; }
    memcpy(pres, p, 7);
    LOG("keyboard: io 0x%02x auth 0x%02x keysize %u keys 0x%02x/0x%02x", pres[1], pres[3], pres[4], pres[5], pres[6]);
    int keysize = pres[4] < 16 ? pres[4] : 16;
    uint8_t tk[16] = {0};
    int mitm = (preq[3] | pres[3]) & 0x04;
    if (mitm && (pres[1] == 0x02 || pres[1] == 0x04)) {
        uint32_t pk = arc4random_uniform(1000000);
        tk[0] = pk; tk[1] = pk >> 8; tk[2] = pk >> 16; tk[3] = pk >> 24;
        printf("\n   >>> On the Bluetooth keyboard, type  %06u  and press Enter <<<\n\n", pk);
        fflush(stdout);
    } else LOG("no passkey needed (Just Works)");
    uint8_t mrand[16], mconf[16];
    arc4random_buf(mrand, 16);
    c1(tk, mrand, preq, pres, 0, own, peer_type, peer, mconf);
    uint8_t m[17]; m[0] = 0x03; memcpy(m + 1, mconf, 16); l2_send(6, m, 17);
    n = smp_get(p, 60);
    if (n < 17 || p[0] != 0x03) { LOG("pairing failed at confirm (0x%02x 0x%02x)%s", p[0], n > 1 ? p[1] : 0, n > 1 && p[0] == 0x05 && p[1] == 0x01 ? ": wrong passkey" : ""); return -1; }
    uint8_t sconf[16]; memcpy(sconf, p + 1, 16);
    m[0] = 0x04; memcpy(m + 1, mrand, 16); l2_send(6, m, 17);
    n = smp_get(p, 30);
    if (n < 17 || p[0] != 0x04) { LOG("pairing failed at random (0x%02x 0x%02x)%s", p[0], n > 1 ? p[1] : 0, p[0] == 0x05 && p[1] == 0x04 ? ": confirm mismatch (wrong passkey?)" : ""); return -1; }
    uint8_t srand[16], chk[16]; memcpy(srand, p + 1, 16);
    c1(tk, srand, preq, pres, 0, own, peer_type, peer, chk);
    if (memcmp(chk, sconf, 16)) { uint8_t f[2] = {0x05, 0x04}; l2_send(6, f, 2); LOG("keyboard's confirm value is wrong"); return -1; }
    uint8_t stk[16]; s1(tk, srand, mrand, stk);
    if (keysize < 16) memset(stk + keysize, 0, 16 - keysize);
    uint8_t zero8[8] = {0};
    if (start_encryption(stk, 0, zero8)) return -1;
    LOG("link encrypted, receiving keys");
    memset(&B, 0, sizeof B);
    memcpy(B.addr, peer, 6); B.type = peer_type; B.keysize = keysize;
    int want = pres[6], got = 0;
    while ((got & want & 0x03) != (want & 0x03)) {
        n = smp_get(p, 10);
        if (n < 1) { LOG("keyboard didn't send its keys"); return -1; }
        if (p[0] == 0x06 && n >= 17) { memcpy(B.ltk, p + 1, 16); }
        else if (p[0] == 0x07 && n >= 11) { B.ediv = le16(p + 1); memcpy(B.rand, p + 3, 8); got |= 1; }
        else if (p[0] == 0x08 && n >= 17) { memcpy(B.irk, p + 1, 16); B.has_irk = 1; }
        else if (p[0] == 0x09 && n >= 8) { B.type = p[1]; memcpy(B.addr, p + 2, 6); got |= 2; }
        else if (p[0] == 0x05) { LOG("pairing failed (0x%02x)", n > 1 ? p[1] : 0); return -1; }
    }
    if (!(want & 1)) { memcpy(B.ltk, stk, 16); LOG("warning: keyboard sent no long-term key"); }
    LOG("paired. identity %s (%s)%s", astr(B.addr), B.type ? "random" : "public", B.has_irk ? ", has privacy key" : "");
    return 0;
}

// ---------------- GATT: find the HID service and its reports ----------------
typedef struct { uint16_t decl, val, end; uint8_t props; uint16_t uuid; } chr_t;

static int gatt_discover(void) {
    uint8_t q[32]; uint16_t hs = 0, he = 0, start = 1;
    for (;;) {                                            // primary services
        q[0] = 0x10; put16(q + 1, start); put16(q + 3, 0xffff); put16(q + 5, 0x2800);
        int op = att_req(q, 7, 10);
        if (op != 0x11) break;
        int L = att_rsp[1]; uint16_t last = 0;
        for (int i = 2; i + L <= att_rlen; i += L) {
            uint16_t s = le16(att_rsp + i), e = le16(att_rsp + i + 2);
            if (L == 6) {
                uint16_t u = le16(att_rsp + i + 4);
                if (u == 0x1812 && !hs) { hs = s; he = e; }
            }
            last = e;
        }
        if (last == 0xffff || last < start) break;
        start = last + 1;
    }
    if (!hs) { LOG("no HID service on this device"); return -1; }
    LOG("HID service at handles %u-%u", hs, he);
    chr_t ch[40]; int nc = 0; start = hs;
    while (start <= he && nc < 40) {                     // characteristics
        q[0] = 0x08; put16(q + 1, start); put16(q + 3, he); put16(q + 5, 0x2803);
        int op = att_req(q, 7, 10);
        if (op != 0x09) break;
        int L = att_rsp[1]; uint16_t last = start;
        for (int i = 2; i + L <= att_rlen && nc < 40; i += L) {
            chr_t *c = &ch[nc++];
            c->decl = le16(att_rsp + i); c->props = att_rsp[i + 2]; c->val = le16(att_rsp + i + 3);
            c->uuid = L == 7 ? le16(att_rsp + i + 5) : 0;
            last = c->decl;
        }
        if (last >= he) break;
        start = last + 1;
    }
    for (int i = 0; i < nc; i++) ch[i].end = i + 1 < nc ? ch[i + 1].decl - 1 : he;
    B.nrep = 0; B.maplen = 0; B.protomode = 0;
    for (int i = 0; i < nc; i++) {
        chr_t *c = &ch[i];
        if (c->uuid == 0x2a4e) B.protomode = c->val;
        if (c->uuid == 0x2a4b) {                         // report map (read + read blob)
            int off = 0;
            for (;;) {
                if (off == 0) { q[0] = 0x0a; put16(q + 1, c->val); if (att_req(q, 3, 10) != 0x0b) break; }
                else { q[0] = 0x0c; put16(q + 1, c->val); put16(q + 3, off); if (att_req(q, 5, 10) != 0x0d) break; }
                int got = att_rlen - 1;
                if (off + got > (int)sizeof B.map) break;
                memcpy(B.map + off, att_rsp + 1, got); off += got;
                if (got < mtu - 1) break;
            }
            B.maplen = off;
        }
        if (c->uuid == 0x2a4d && B.nrep < MAXREP) {      // report: find its reference + CCCD
            report_t *r = &B.rep[B.nrep]; memset(r, 0, sizeof *r); r->val = c->val;
            uint16_t refh = 0, s = c->val + 1;
            while (s <= c->end) {                        // list descriptors first
                q[0] = 0x04; put16(q + 1, s); put16(q + 3, c->end);
                if (att_req(q, 5, 10) != 0x05) break;
                int fmt = att_rsp[1], L = fmt == 1 ? 4 : 18; uint16_t last = s;
                for (int k = 2; k + L <= att_rlen; k += L) {
                    uint16_t h = le16(att_rsp + k), u = fmt == 1 ? le16(att_rsp + k + 2) : 0;
                    if (u == 0x2902) r->cccd = h;
                    if (u == 0x2908) refh = h;
                    last = h;
                }
                if (last >= c->end) break;
                s = last + 1;
            }
            if (refh) {                                  // then read the report reference
                q[0] = 0x0a; put16(q + 1, refh);
                if (att_req(q, 3, 10) == 0x0b && att_rlen >= 3) { r->id = att_rsp[1]; r->type = att_rsp[2]; }
            } else r->type = (c->props & 0x10) ? 1 : 3;
            LOG("  report id %u type %s handle %u%s", r->id, r->type == 1 ? "input" : r->type == 2 ? "output" : "feature",
                r->val, r->cccd ? " (notifies)" : "");
            B.nrep++;
        }
    }
    if (!B.maplen || !B.nrep) { LOG("couldn't read the keyboard's report layout"); return -1; }
    B.uses_ids = map_uses_ids(B.map, B.maplen);
    LOG("report map %d bytes, %s report IDs", B.maplen, B.uses_ids ? "with" : "without");
    return 0;
}

static int gatt_enable(void) {
    uint8_t q[8];
    if (B.protomode) { q[0] = 0x52; put16(q + 1, B.protomode); q[3] = 1; l2_send(4, q, 4); }   // report protocol
    int ok = 0;
    for (int i = 0; i < B.nrep; i++) {
        report_t *r = &B.rep[i];
        if (r->type != 1 || !r->cccd) continue;
        q[0] = 0x12; put16(q + 1, r->cccd); q[3] = 1; q[4] = 0;
        int op = att_req(q, 5, 10);
        if (op == 0x13) ok++;
        else LOG("  couldn't turn on report %u (0x%02x 0x%02x)", r->id, op, att_rlen > 4 ? att_rsp[4] : 0);
    }
    return ok ? 0 : -1;
}

static int make_virtual_keyboard(void) {
    int nf = hid_parse(B.map, B.maplen);
    LOG("key layout: %d input fields", nf);
    if (nf <= 0) return -1;
    return sock_listen();
}

static volatile sig_atomic_t stop;
static void on_sig(int s) { stop = 1; CFRunLoopStop(CFRunLoopGetMain()); }

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (getenv("BTKBD_DIR")) {
        static char b1[300], b2[300], b3[300]; const char *d = getenv("BTKBD_DIR");
        snprintf(b1, 300, "%s/bond.txt", d); snprintf(b2, 300, "%s/rtl8761bu_fw.bin", d); snprintf(b3, 300, "%s/rtl8761bu_config.bin", d);
        BOND = b1; FW = b2; CFG = b3;
    }
    if (getenv("V")) hci_verbose = 1;
    int pair = argc > 1 && !strcmp(argv[1], "pair");
    if (pair && argc > 2) name_filter = argv[2];
    signal(SIGINT, on_sig); signal(SIGTERM, on_sig);

    int opened = -2; unsigned uv, up;
    if (getenv("BTKBD_USB") && sscanf(getenv("BTKBD_USB"), "%x:%x", &uv, &up) == 2) opened = hci_open(uv, up);
    else for (unsigned i = 0; i < sizeof DONGLES / sizeof DONGLES[0] && opened == -2; i++) opened = hci_open(DONGLES[i][0], DONGLES[i][1]);
    if (opened == -2) LOG("no RTL8761B Bluetooth dongle found (plug it in, or set BTKBD_USB=vvvv:pppp)");
    if (opened) return 2;
    hci_set_callbacks(on_event, on_acl);
    if (hci_setup()) return 2;

    if (!pair) {
        if (bond_load()) { LOG("no paired keyboard yet: run 'btkbd pair' first"); return 1; }
        LOG("paired keyboard: %s %s", B.name, astr(B.addr));
        if (make_virtual_keyboard()) return 3;
    }

    if (pair) {
        LOG("looking for a keyboard in pairing mode%s%s ...", name_filter ? " named like " : "", name_filter ? name_filter : "");
        want_scan_pair = 1;
        for (int tries = 0; tries < 6 && !stop; tries++) {
            found = 0; scan(1, 1);
            CFAbsoluteTime end = CFAbsoluteTimeGetCurrent() + 60;
            while (!found && !stop && !hci_dead && CFAbsoluteTimeGetCurrent() < end) hci_run(0.05);
            scan(0, 0);
            if (!found) { LOG("no keyboard found. Is it in pairing mode?"); return 1; }
            char nm[64]; strlcpy(nm, found_name, 64);
            LOG("trying %s %s", astr(found_addr), nm);
            if (connect_to(found_addr, found_type)) continue;
            if (do_pair() || gatt_discover() || gatt_enable()) { disconnect(); LOG("will retry"); continue; }
            strlcpy(B.name, nm[0] ? nm : "Bluetooth LE Keyboard", sizeof B.name);
            B.valid = 1;
            if (bond_save()) { LOG("couldn't save the pairing"); return 1; }
            LOG("saved to %s. Done: start the service (or run btkbd) to use the keyboard.", BOND);
            disconnect();
            return 0;
        }
        return 1;
    }

    want_scan_pair = 0;
    while (!stop && !hci_dead) {
        found = 0; scan(1, 0);
        LOG("waiting for the keyboard (press a key to wake it)");
        while (!found && !stop && !hci_dead) hci_run(0.05);
        scan(0, 0);
        if (stop || hci_dead) break;
        if (connect_to(found_addr, found_type)) { hci_run(0.2); continue; }
        if (start_encryption(B.ltk, B.ediv, B.rand) || gatt_enable()) { disconnect(); hci_run(1); continue; }
        set_leds();
        LOG("keyboard ready");
        while (conn >= 0 && !stop && !hci_dead) hci_run(0.5);
    }
    release_keys();
    disconnect();
    scan(0, 0);
    unlink(SOCK);
    hci_close();
    LOG(hci_dead ? "dongle went away" : "stopped");
    return hci_dead ? 4 : 0;
}
