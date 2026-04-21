#ifndef SOLO_UTILS_H
#define SOLO_UTILS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

#include "solo_sds.h"

#define ERR_RET(expr)                  \
    do {                               \
        if ((expr) < 0)                \
            return -__LINE__;          \
    } while (0)

void solo_utils_init(void);
double current_timestamp(void);
uint64_t current_time_millis(void);
uint64_t get_process_uptime_seconds(void);

int set_file_limit(uint64_t limit);
int set_core_limit(uint64_t limit);
int urandom(void *buf, size_t len);

void sha256(const void *data, size_t len, void *out);
void sha256d(const void *data, size_t len, void *out);
void reverse_mem(void *data, size_t len);

int hex2bin_exact(const char *hex, void *out, size_t out_len);
sds hex2bin(const char *hex);
int bin2hex_into(const void *data, size_t len, char *out, size_t out_size);
sds bin2hex(const void *data, size_t len);
void hash_to_hex32(const unsigned char *bytes, char *hex);
void strclearblank(char *s);

int pack_buf(void **p, size_t *left, const void *buf, size_t len);
int pack_uint32_le(void **p, size_t *left, uint32_t value);
int pack_uint64_le(void **p, size_t *left, uint64_t value);
int pack_varint_le(void **p, size_t *left, uint64_t value);
int pack_oppush(void **p, size_t *left, const void *buf, size_t len);
int pack_oppushint_le(void **p, size_t *left, int64_t value);

sds *get_merkle_branch(sds *nodes, size_t count, size_t *branch_count);
void get_merkle_root(char *root, sds *branch, size_t branch_count);

long double calc_network_difficulty(const char *bits_hex);
double hash_to_difficulty(const unsigned char *hash_be);

int mkdir_p(const char *path);
int format_sockaddr(const struct sockaddr *sa, socklen_t salen, char *buf, size_t size);

int sds_append_len_owned(sds *dst, const void *buf, size_t len);
int sds_append_sds_owned(sds *dst, const sds value);
int sds_append_cstr_owned(sds *dst, const char *value);

#endif
