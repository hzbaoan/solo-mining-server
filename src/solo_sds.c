#include <stdlib.h>
#include <string.h>

#include "solo_sds.h"

struct sds_hdr {
    size_t len;
    size_t cap;
    char buf[];
};

static struct sds_hdr *sds_hdr_ptr(const sds s)
{
    if (s == NULL)
        return NULL;
    return (struct sds_hdr *)(s - offsetof(struct sds_hdr, buf));
}

size_t sdslen(const sds s)
{
    struct sds_hdr *hdr = sds_hdr_ptr(s);
    return hdr ? hdr->len : 0;
}

static sds sds_alloc(size_t len)
{
    struct sds_hdr *hdr = calloc(1, sizeof(*hdr) + len + 1);
    if (hdr == NULL)
        return NULL;
    hdr->cap = len;
    hdr->buf[len] = '\0';
    return hdr->buf;
}

sds sdsnewlen(const void *init, size_t len)
{
    sds s = sds_alloc(len);
    struct sds_hdr *hdr = sds_hdr_ptr(s);
    if (s == NULL || hdr == NULL)
        return NULL;

    if (init && len > 0)
        memcpy(s, init, len);
    hdr->len = len;
    s[len] = '\0';
    return s;
}

sds sdsnew(const char *init)
{
    if (init == NULL)
        return sdsempty();
    return sdsnewlen(init, strlen(init));
}

sds sdsempty(void)
{
    return sdsnewlen("", 0);
}

sds sdsdup(const sds s)
{
    return sdsnewlen(s, sdslen(s));
}

void sdsfree(sds s)
{
    free(sds_hdr_ptr(s));
}

static sds sds_make_room_for(sds s, size_t addlen)
{
    struct sds_hdr *hdr = sds_hdr_ptr(s);
    size_t need;
    size_t newcap;

    if (hdr == NULL)
        return NULL;
    if (addlen <= (hdr->cap - hdr->len))
        return s;

    need = hdr->len + addlen;
    newcap = hdr->cap ? hdr->cap : 16;
    while (newcap < need) {
        if (newcap > ((size_t)-1 / 2)) {
            newcap = need;
            break;
        }
        newcap *= 2;
    }

    hdr = realloc(hdr, sizeof(*hdr) + newcap + 1);
    if (hdr == NULL)
        return NULL;

    hdr->cap = newcap;
    hdr->buf[hdr->len] = '\0';
    return hdr->buf;
}

sds sdscatlen(sds s, const void *t, size_t len)
{
    struct sds_hdr *hdr;
    if (len == 0)
        return s;

    s = sds_make_room_for(s, len);
    hdr = sds_hdr_ptr(s);
    if (s == NULL || hdr == NULL)
        return NULL;

    memcpy(s + hdr->len, t, len);
    hdr->len += len;
    s[hdr->len] = '\0';
    return s;
}

int sdscmp(const sds a, const sds b)
{
    size_t alen = sdslen(a);
    size_t blen = sdslen(b);
    size_t minlen = alen < blen ? alen : blen;
    int cmp;

    if (a == NULL && b == NULL)
        return 0;
    if (a == NULL)
        return -1;
    if (b == NULL)
        return 1;

    cmp = memcmp(a, b, minlen);
    if (cmp != 0)
        return cmp;
    if (alen == blen)
        return 0;
    return alen < blen ? -1 : 1;
}

void sdssetlen(sds s, size_t len)
{
    struct sds_hdr *hdr = sds_hdr_ptr(s);
    if (hdr == NULL)
        return;
    if (len > hdr->cap)
        len = hdr->cap;
    hdr->len = len;
    s[len] = '\0';
}
