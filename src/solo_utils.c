#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <netdb.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "solo_utils.h"

static uint64_t process_start_seconds;

void solo_utils_init(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    process_start_seconds = (uint64_t)ts.tv_sec;
}

double current_timestamp(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (double)tv.tv_sec + ((double)tv.tv_usec / 1000000.0);
}

uint64_t current_time_millis(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return ((uint64_t)tv.tv_sec * 1000ULL) + ((uint64_t)tv.tv_usec / 1000ULL);
}

uint64_t get_process_uptime_seconds(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec - process_start_seconds;
}

static int set_limit(int resource, uint64_t limit)
{
    struct rlimit rl;
    rlim_t target;

    if (getrlimit(resource, &rl) < 0)
        return -1;

    target = (rl.rlim_max != RLIM_INFINITY && limit > (uint64_t)rl.rlim_max) ? rl.rlim_max : (rlim_t)limit;
    if (rl.rlim_cur == target)
        return 0;
    rl.rlim_cur = target;
    return setrlimit(resource, &rl);
}

int set_file_limit(uint64_t limit)
{
    return set_limit(RLIMIT_NOFILE, limit);
}

int set_core_limit(uint64_t limit)
{
    return set_limit(RLIMIT_CORE, limit);
}

int urandom(void *buf, size_t len)
{
    if (buf == NULL)
        return -1;
    return RAND_bytes(buf, (int)len) == 1 ? 0 : -1;
}

void sha256(const void *data, size_t len, void *out)
{
    SHA256(data, len, out);
}

void sha256d(const void *data, size_t len, void *out)
{
    unsigned char first[SHA256_DIGEST_LENGTH];
    sha256(data, len, first);
    sha256(first, sizeof(first), out);
}

void reverse_mem(void *data, size_t len)
{
    unsigned char *p = data;
    for (size_t i = 0; i < len / 2; ++i) {
        unsigned char tmp = p[i];
        p[i] = p[len - 1 - i];
        p[len - 1 - i] = tmp;
    }
}

static int hex_char_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return 10 + (c - 'a');
    if (c >= 'A' && c <= 'F')
        return 10 + (c - 'A');
    return -1;
}

int hex2bin_exact(const char *hex, void *out, size_t out_len)
{
    unsigned char *dst = out;

    if (hex == NULL || strlen(hex) != out_len * 2 || (out == NULL && out_len != 0))
        return -1;

    for (size_t i = 0; i < out_len; ++i) {
        int hi = hex_char_value(hex[i * 2]);
        int lo = hex_char_value(hex[(i * 2) + 1]);
        if (hi < 0 || lo < 0)
            return -1;
        dst[i] = (unsigned char)((hi << 4) | lo);
    }

    return 0;
}

sds hex2bin(const char *hex)
{
    size_t len;
    sds out;

    if (hex == NULL)
        return NULL;
    len = strlen(hex);
    if ((len & 1) != 0)
        return NULL;

    out = sdsnewlen(NULL, len / 2);
    if (out == NULL)
        return NULL;

    if (hex2bin_exact(hex, out, len / 2) < 0) {
        sdsfree(out);
        return NULL;
    }

    return out;
}

int bin2hex_into(const void *data, size_t len, char *out, size_t out_size)
{
    static const char *digits = "0123456789abcdef";
    const unsigned char *p = data;

    if (out == NULL || out_size < (len * 2) + 1 || (data == NULL && len != 0))
        return -1;

    for (size_t i = 0; i < len; ++i) {
        out[i * 2] = digits[p[i] >> 4];
        out[(i * 2) + 1] = digits[p[i] & 0x0f];
    }
    out[len * 2] = '\0';
    return 0;
}

sds bin2hex(const void *data, size_t len)
{
    sds out = sdsnewlen(NULL, len * 2);
    if (out == NULL)
        return NULL;

    if (bin2hex_into(data, len, out, (len * 2) + 1) < 0) {
        sdsfree(out);
        return NULL;
    }

    return out;
}

void hash_to_hex32(const unsigned char *bytes, char *hex)
{
    static const char *digits = "0123456789abcdef";
    for (size_t i = 0; i < 32; ++i) {
        hex[i * 2] = digits[bytes[i] >> 4];
        hex[(i * 2) + 1] = digits[bytes[i] & 0x0f];
    }
    hex[64] = '\0';
}

void strclearblank(char *s)
{
    size_t write_pos = 0;
    if (s == NULL)
        return;

    for (size_t i = 0; s[i] != '\0'; ++i) {
        if (!isspace((unsigned char)s[i]))
            s[write_pos++] = s[i];
    }
    s[write_pos] = '\0';
}

int pack_buf(void **p, size_t *left, const void *buf, size_t len)
{
    if (p == NULL || left == NULL || buf == NULL || *left < len)
        return -1;
    memcpy(*p, buf, len);
    *p = (char *)(*p) + len;
    *left -= len;
    return 0;
}

int pack_uint32_le(void **p, size_t *left, uint32_t value)
{
    unsigned char out[4];
    out[0] = (unsigned char)(value & 0xff);
    out[1] = (unsigned char)((value >> 8) & 0xff);
    out[2] = (unsigned char)((value >> 16) & 0xff);
    out[3] = (unsigned char)((value >> 24) & 0xff);
    return pack_buf(p, left, out, sizeof(out));
}

int pack_uint64_le(void **p, size_t *left, uint64_t value)
{
    unsigned char out[8];
    for (size_t i = 0; i < sizeof(out); ++i)
        out[i] = (unsigned char)((value >> (i * 8)) & 0xff);
    return pack_buf(p, left, out, sizeof(out));
}

int pack_varint_le(void **p, size_t *left, uint64_t value)
{
    unsigned char out[9];
    size_t len = 0;

    if (value < 0xfd) {
        out[len++] = (unsigned char)value;
    } else if (value <= 0xffff) {
        out[len++] = 0xfd;
        out[len++] = (unsigned char)(value & 0xff);
        out[len++] = (unsigned char)((value >> 8) & 0xff);
    } else if (value <= 0xffffffffULL) {
        out[len++] = 0xfe;
        out[len++] = (unsigned char)(value & 0xff);
        out[len++] = (unsigned char)((value >> 8) & 0xff);
        out[len++] = (unsigned char)((value >> 16) & 0xff);
        out[len++] = (unsigned char)((value >> 24) & 0xff);
    } else {
        out[len++] = 0xff;
        for (size_t i = 0; i < 8; ++i)
            out[len++] = (unsigned char)((value >> (i * 8)) & 0xff);
    }

    return pack_buf(p, left, out, len);
}

int pack_oppush(void **p, size_t *left, const void *buf, size_t len)
{
    unsigned char header[3];
    size_t header_len = 0;

    if (len <= 75) {
        header[header_len++] = (unsigned char)len;
    } else if (len <= 255) {
        header[header_len++] = 0x4c;
        header[header_len++] = (unsigned char)len;
    } else if (len <= 65535) {
        header[header_len++] = 0x4d;
        header[header_len++] = (unsigned char)(len & 0xff);
        header[header_len++] = (unsigned char)((len >> 8) & 0xff);
    } else {
        return -1;
    }

    if (pack_buf(p, left, header, header_len) < 0)
        return -1;
    return pack_buf(p, left, buf, len);
}

static size_t encode_scriptint(int64_t value, unsigned char out[9])
{
    uint64_t abs_value;
    size_t len = 0;
    bool negative = value < 0;

    if (value == 0)
        return 0;

    abs_value = negative ? (uint64_t)(-value) : (uint64_t)value;
    while (abs_value > 0) {
        out[len++] = (unsigned char)(abs_value & 0xff);
        abs_value >>= 8;
    }
    if ((out[len - 1] & 0x80) != 0) {
        out[len++] = negative ? 0x80 : 0x00;
    } else if (negative) {
        out[len - 1] |= 0x80;
    }

    return len;
}

int pack_oppushint_le(void **p, size_t *left, int64_t value)
{
    unsigned char encoded[9];
    size_t len = encode_scriptint(value, encoded);
    return pack_oppush(p, left, encoded, len);
}

static sds hash_concat32(const void *left, const void *right)
{
    unsigned char combined[64];
    sds out = sdsnewlen(NULL, 32);
    if (out == NULL)
        return NULL;

    memcpy(combined, left, 32);
    memcpy(combined + 32, right, 32);
    sha256d(combined, sizeof(combined), out);
    return out;
}

sds *get_merkle_branch(sds *nodes, size_t count, size_t *branch_count)
{
    sds *branch = NULL;
    sds *level = NULL;
    size_t level_count = count;
    size_t used = 0;

    if (branch_count == NULL)
        return NULL;
    *branch_count = 0;

    if (count == 0)
        return calloc(1, sizeof(sds *));

    branch = calloc(count, sizeof(*branch));
    level = calloc(count, sizeof(*level));
    if (branch == NULL || level == NULL)
        goto fail;

    for (size_t i = 0; i < count; ++i) {
        level[i] = sdsdup(nodes[i]);
        if (level[i] == NULL)
            goto fail;
    }

    while (level_count > 0) {
        sds *next_level = NULL;
        size_t next_count = level_count / 2;

        branch[used] = sdsdup(level[0]);
        if (branch[used] == NULL)
            goto fail;
        used++;

        if (next_count == 0)
            break;

        next_level = calloc(next_count, sizeof(*next_level));
        if (next_level == NULL)
            goto fail;

        for (size_t i = 0; i < next_count; ++i) {
            size_t src = 1 + (i * 2);
            const void *left = level[src];
            const void *right = (src + 1 < level_count) ? level[src + 1] : level[src];

            if (left == NULL || right == NULL) {
                free(next_level);
                goto fail;
            }

            next_level[i] = hash_concat32(left, right);
            if (next_level[i] == NULL) {
                free(next_level);
                goto fail;
            }
        }

        for (size_t i = 0; i < level_count; ++i)
            sdsfree(level[i]);
        free(level);
        level = next_level;
        level_count = next_count;
    }

    for (size_t i = 0; i < level_count; ++i)
        sdsfree(level[i]);
    free(level);
    *branch_count = used;
    return branch;

fail:
    if (level) {
        for (size_t i = 0; i < level_count; ++i)
            sdsfree(level[i]);
        free(level);
    }
    if (branch) {
        for (size_t i = 0; i < count; ++i)
            sdsfree(branch[i]);
        free(branch);
    }
    return NULL;
}

void get_merkle_root(char *root, sds *branch, size_t branch_count)
{
    unsigned char combined[64];
    unsigned char next[32];

    for (size_t i = 0; i < branch_count; ++i) {
        memcpy(combined, root, 32);
        memcpy(combined + 32, branch[i], 32);
        sha256d(combined, sizeof(combined), next);
        memcpy(root, next, sizeof(next));
    }
}

long double calc_network_difficulty(const char *bits_hex)
{
    char tpower[3] = {0};
    char tvalue[7] = {0};
    unsigned char tpower_val;
    unsigned long tvalue_val;
    char *ep = NULL;
    int i;
    long double d;
    signed short s;

    if (bits_hex == NULL || strlen(bits_hex) != 8)
        return 0.0;

    tpower[0] = bits_hex[0];
    tpower[1] = bits_hex[1];
    for (i = 0; i < 6; ++i)
        tvalue[i] = bits_hex[2 + i];

    tpower_val = (unsigned char)strtoul(tpower, &ep, 16);
    tvalue_val = strtoul(tvalue, &ep, 16);
    if (tvalue_val == 0)
        return 0.0;

    s = (signed short)29 - (signed short)tpower_val;
    d = powl(10.0, (double)s * (long double)2.4082399653118495617099111577959L + log10l(65535.0L / (long double)tvalue_val));
    return d;
}

double hash_to_difficulty(const unsigned char *hash_be)
{
    static const unsigned char diff1_target[32] = {
        0x00, 0x00, 0x00, 0x00, 0xff, 0xff, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    long double numerator = 0.0L;
    long double denominator = 0.0L;

    if (hash_be == NULL)
        return 0.0;

    for (size_t i = 0; i < 16; ++i) {
        numerator = (numerator * 256.0L) + diff1_target[i];
        denominator = (denominator * 256.0L) + hash_be[i];
    }

    if (denominator <= 0.0L)
        return 0.0;

    return (double)(numerator / denominator);
}

int mkdir_p(const char *path)
{
    char tmp[PATH_MAX];
    size_t len;

    if (path == NULL || path[0] == '\0')
        return 0;
    if (strlen(path) >= sizeof(tmp))
        return -1;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (len == 0)
        return 0;

    for (size_t i = 1; i < len; ++i) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char saved = tmp[i];
            tmp[i] = '\0';
            if (tmp[0] != '\0' && mkdir(tmp, 0755) < 0 && errno != EEXIST)
                return -1;
            tmp[i] = saved;
        }
    }

    if (mkdir(tmp, 0755) < 0 && errno != EEXIST)
        return -1;
    return 0;
}

int format_sockaddr(const struct sockaddr *sa, socklen_t salen, char *buf, size_t size)
{
    char host[NI_MAXHOST];
    char serv[NI_MAXSERV];
    int rc;

    if (sa == NULL || buf == NULL || size == 0)
        return -1;

    rc = getnameinfo(sa, salen, host, sizeof(host), serv, sizeof(serv), NI_NUMERICHOST | NI_NUMERICSERV);
    if (rc != 0)
        return -1;

    if (sa->sa_family == AF_INET6)
        return snprintf(buf, size, "[%s]:%s", host, serv) < (int)size ? 0 : -1;
    return snprintf(buf, size, "%s:%s", host, serv) < (int)size ? 0 : -1;
}

int sds_append_len_owned(sds *dst, const void *buf, size_t len)
{
    sds next;

    if (dst == NULL)
        return -1;
    if (len == 0)
        return 0;
    if (*dst == NULL)
        *dst = sdsempty();
    if (*dst == NULL)
        return -1;

    next = sdscatlen(*dst, buf, len);
    if (next == NULL) {
        sdsfree(*dst);
        *dst = NULL;
        return -1;
    }

    *dst = next;
    return 0;
}

int sds_append_sds_owned(sds *dst, const sds value)
{
    if (value == NULL)
        return -1;
    return sds_append_len_owned(dst, value, sdslen(value));
}

int sds_append_cstr_owned(sds *dst, const char *value)
{
    if (value == NULL)
        return -1;
    return sds_append_len_owned(dst, value, strlen(value));
}
