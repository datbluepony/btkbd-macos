// hidparse.c - minimal HID report-descriptor parser: report bytes -> set of pressed (page, usage)
#include "hidparse.h"
#include <string.h>

typedef struct {
    uint8_t id, var; uint16_t page; int bitoff, size, count, lmin;
    uint16_t umin, umax; int nus; uint32_t us[24];
} field_t;
static field_t F[96]; static int NF;

static uint32_t uval(const uint8_t *d, int sz) {
    uint32_t v = 0; for (int i = 0; i < sz; i++) v |= (uint32_t)d[i] << (8 * i); return v;
}
static int32_t sval(const uint8_t *d, int sz) {
    uint32_t v = uval(d, sz);
    if (sz == 1) return (int8_t)v; if (sz == 2) return (int16_t)v; return (int32_t)v;
}

int hid_parse(const uint8_t *d, int n) {
    NF = 0;
    struct { uint16_t page; int lmin, size, count; uint8_t id; } g = {0}, stk[4]; int sp = 0;
    uint32_t us[24]; int nus = 0; uint32_t umin = 0, umax = 0; int haverange = 0;
    int off[256]; memset(off, 0, sizeof off);
    for (int i = 0; i < n;) {
        uint8_t b = d[i];
        if (b == 0xfe) { if (i + 2 >= n) break; i += 3 + d[i + 1]; continue; }
        int sz = b & 3; if (sz == 3) sz = 4;
        if (i + 1 + sz > n) break;
        const uint8_t *v = d + i + 1; int type = (b >> 2) & 3, tag = b >> 4;
        if (type == 1) {                           // global
            switch (tag) {
            case 0: g.page = uval(v, sz); break;
            case 1: g.lmin = sval(v, sz); break;
            case 7: g.size = uval(v, sz); break;
            case 8: g.id = uval(v, sz); break;
            case 9: g.count = uval(v, sz); break;
            case 10: if (sp < 4) stk[sp++] = g; break;
            case 11: if (sp) g = stk[--sp]; break;
            }
        } else if (type == 2) {                    // local
            uint32_t u = uval(v, sz); if (sz < 4) u |= (uint32_t)g.page << 16;
            if (tag == 0 && nus < 24) us[nus++] = u;
            else if (tag == 1) { umin = u; haverange = 1; }
            else if (tag == 2) { umax = u; haverange = 1; }
        } else if (type == 0) {                    // main
            if (tag == 8) {                        // Input
                uint32_t fl = uval(v, sz);
                if (!(fl & 1) && NF < 96) {
                    field_t *f = &F[NF++]; memset(f, 0, sizeof *f);
                    f->id = g.id; f->var = (fl >> 1) & 1; f->bitoff = off[g.id];
                    f->size = g.size; f->count = g.count; f->lmin = g.lmin;
                    if (haverange) { f->page = umin >> 16; f->umin = umin & 0xffff; f->umax = umax & 0xffff; }
                    else if (nus) f->page = us[0] >> 16;
                    else f->page = g.page;
                    f->nus = nus; memcpy(f->us, us, nus * sizeof(uint32_t));
                }
                off[g.id] += g.size * g.count;
            } else if (tag == 9 || tag == 11) off[g.id] += 0 + (tag == 9 || tag == 11 ? 0 : 0);
            if (tag == 9) { /* output: not tracked for input offsets */ }
            if (tag == 11) { /* feature */ }
            nus = 0; haverange = 0; umin = umax = 0;
        }
        i += 1 + sz;
    }
    return NF;
}

static uint32_t bits(const uint8_t *r, int len, int off, int size) {
    uint32_t v = 0;
    for (int k = 0; k < size && k < 32; k++) {
        int b = off + k; if (b / 8 >= len) break;
        if (r[b / 8] & (1 << (b % 8))) v |= 1u << k;
    }
    return v;
}

int hid_decode(uint8_t id, const uint8_t *r, int len, uint32_t *out, int max) {
    int n = 0;
    for (int fi = 0; fi < NF; fi++) {
        field_t *f = &F[fi];
        if (f->id != id) continue;
        for (int i = 0; i < f->count && n < max; i++) {
            uint32_t v = bits(r, len, f->bitoff + i * f->size, f->size);
            uint32_t page = f->page, u = 0;
            if (f->var) {
                if (!v) continue;
                if (f->nus) { uint32_t x = f->us[i < f->nus ? i : f->nus - 1]; page = x >> 16; u = x & 0xffff; }
                else u = f->umin + i;
            } else {
                int idx = (int)v - f->lmin;
                if (idx < 0) continue;
                if (f->nus) { if (idx >= f->nus) continue; page = f->us[idx] >> 16; u = f->us[idx] & 0xffff; }
                else { if (f->umin + idx > f->umax) continue; u = f->umin + idx; }
                if (!u) continue;
                if (page == 7 && u <= 3) continue;          // rollover / error codes
            }
            out[n++] = (page << 16) | u;
        }
    }
    return n;
}
