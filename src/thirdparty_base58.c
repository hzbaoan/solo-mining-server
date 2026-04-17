/*
 * Copyright 2012-2014 Luke Dashjr
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the standard MIT license.
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/*
    Combined and lightly adapted from Luke Dashjr's libblkmaker and libbase58.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "thirdparty_base58.h"
#include "solo_utils.h"

static bool solo_sha256_impl(void *digest, const void *buffer, size_t length)
{
    sha256(buffer, length, digest);
    return true;
}

#define b58_sha256_impl solo_sha256_impl

size_t blkmk_address_to_script(void *out, size_t outsz, const char *addr)
{
    unsigned char addrbin[25];
    unsigned char *cout = out;
    const size_t b58sz = strlen(addr);
    int addrver;
    size_t rv;

    rv = sizeof(addrbin);
    if (!b58tobin(addrbin, &rv, addr, b58sz))
        return 0;
    addrver = b58check(addrbin, sizeof(addrbin), addr, b58sz);
    switch (addrver) {
    case 0:
    case 111:
        if (outsz < (rv = 25))
            return rv;
        cout[0] = 0x76;
        cout[1] = 0xa9;
        cout[2] = 0x14;
        memcpy(&cout[3], &addrbin[1], 20);
        cout[23] = 0x88;
        cout[24] = 0xac;
        return rv;
    case 5:
    case 196:
        if (outsz < (rv = 23))
            return rv;
        cout[0] = 0xa9;
        cout[1] = 0x14;
        memcpy(&cout[2], &addrbin[1], 20);
        cout[22] = 0x87;
        return rv;
    default:
        return 0;
    }
}

static const int8_t b58digits_map[] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, -1, -1, -1, -1, -1, -1,
    -1, 9, 10, 11, 12, 13, 14, 15, 16, -1, 17, 18, 19, 20, 21, -1,
    22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, -1, -1, -1, -1, -1,
    -1, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, -1, 44, 45, 46,
    47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, -1, -1, -1, -1, -1,
};

typedef uint64_t b58_maxint_t;
typedef uint32_t b58_almostmaxint_t;
#define b58_almostmaxint_bits (sizeof(b58_almostmaxint_t) * 8)
static const b58_almostmaxint_t b58_almostmaxint_mask = ((((b58_maxint_t)1) << b58_almostmaxint_bits) - 1);

bool b58tobin(void *bin, size_t *binszp, const char *b58, size_t b58sz)
{
    size_t binsz = *binszp;
    const unsigned char *b58u = (void *)b58;
    unsigned char *binu = bin;
    size_t outisz = (binsz + sizeof(b58_almostmaxint_t) - 1) / sizeof(b58_almostmaxint_t);
    b58_almostmaxint_t outi[outisz];
    b58_maxint_t t;
    b58_almostmaxint_t c;
    size_t i;
    size_t j;
    uint8_t bytesleft = binsz % sizeof(b58_almostmaxint_t);
    b58_almostmaxint_t zeromask = bytesleft ? (b58_almostmaxint_mask << (bytesleft * 8)) : 0;
    unsigned zerocount = 0;

    if (!b58sz)
        b58sz = strlen(b58);

    for (i = 0; i < outisz; ++i)
        outi[i] = 0;

    for (i = 0; i < b58sz && b58u[i] == '1'; ++i)
        ++zerocount;

    for (; i < b58sz; ++i) {
        if (b58u[i] & 0x80)
            return false;
        if (b58digits_map[b58u[i]] == -1)
            return false;
        c = (unsigned)b58digits_map[b58u[i]];
        for (j = outisz; j--;) {
            t = ((b58_maxint_t)outi[j]) * 58 + c;
            c = t >> b58_almostmaxint_bits;
            outi[j] = t & b58_almostmaxint_mask;
        }
        if (c)
            return false;
        if (outi[0] & zeromask)
            return false;
    }

    j = 0;
    if (bytesleft) {
        for (i = bytesleft; i > 0; --i)
            *(binu++) = (outi[0] >> (8 * (i - 1))) & 0xff;
        ++j;
    }

    for (; j < outisz; ++j) {
        for (i = sizeof(*outi); i > 0; --i)
            *(binu++) = (outi[j] >> (8 * (i - 1))) & 0xff;
    }

    binu = bin;
    for (i = 0; i < binsz; ++i) {
        if (binu[i])
            break;
        --*binszp;
    }
    *binszp += zerocount;

    return true;
}

static bool my_dblsha256(void *hash, const void *data, size_t datasz)
{
    uint8_t buf[0x20];
    return b58_sha256_impl(buf, data, datasz) && b58_sha256_impl(hash, buf, sizeof(buf));
}

int b58check(const void *bin, size_t binsz, const char *base58str, size_t b58sz)
{
    unsigned char buf[32];
    const uint8_t *binc = bin;
    unsigned i;

    (void)b58sz;
    if (binsz < 4)
        return -4;
    if (!my_dblsha256(buf, bin, binsz - 4))
        return -2;
    if (memcmp(&binc[binsz - 4], buf, 4))
        return -1;

    for (i = 0; binc[i] == '\0' && base58str[i] == '1'; ++i) {
    }
    if (binc[i] == '\0' || base58str[i] == '1')
        return -3;

    return binc[0];
}
