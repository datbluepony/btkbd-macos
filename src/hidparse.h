#pragma once
#include <stdint.h>
int hid_parse(const uint8_t *desc, int n);                                   // returns number of input fields
int hid_decode(uint8_t id, const uint8_t *r, int len, uint32_t *out, int max); // pressed (page<<16 | usage)
