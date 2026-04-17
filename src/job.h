#ifndef SOLO_JOB_H
#define SOLO_JOB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <jansson.h>

#include "solo_sds.h"

#define SOLO_FOUND_BLOCKS_MAX 32

struct solo_job {
    int         refcount;
    char        job_id[9];
    uint32_t    version;
    uint32_t    curtime;
    uint32_t    mintime;
    uint32_t    nbits;
    uint32_t    height;
    uint64_t    coinbase_value;
    char        version_hex[9];
    char        curtime_hex[9];
    char        nbits_hex[9];
    char        prevhash_le[32];
    sds         prevhash_notify_hex;
    char        target[32];
    bool        has_witness_commitment;
    bool        has_witness_transactions;
    sds         coinbase_tx_prefix_bin;
    sds         coinbaseaux_bin;
    sds         coinbase2_bin;
    sds         coinbase2_hex;
    sds        *merkle_branch;
    size_t      merkle_branch_count;
    json_t     *merkle_json;
    sds         notify_prefix_json;
    sds         notify_suffix_clean_json;
    sds         notify_suffix_dirty_json;
    uint32_t    tx_count;
    sds         txs_hex;
    sds         submit_tail_hex;
    size_t      submit_tx_count_hex_len;
    sds         template_sig;
};

struct solo_found_block {
    uint32_t    height;
    char        hash_hex[65];
    char        result[32];
    uint64_t    timestamp_ms;
};

struct solo_job_status {
    bool        ready;
    bool        last_refresh_ok;
    uint64_t    last_refresh_ms;
    uint32_t    height;
    uint64_t    coinbase_value;
    uint32_t    tx_count;
    char        nbits_hex[9];
};

typedef void (*solo_job_update_cb)(const struct solo_job *job, bool clean_jobs);

enum solo_refresh_source {
    SOLO_REFRESH_STARTUP,
    SOLO_REFRESH_MANUAL,
    SOLO_REFRESH_LONGPOLL_TIMEOUT,
    SOLO_REFRESH_LONGPOLL_ERROR,
    SOLO_REFRESH_ZMQ_HASHBLOCK,
    SOLO_REFRESH_ZMQ_RAWBLOCK,
    SOLO_REFRESH_P2P_FAST_FOLLOWUP,
    SOLO_REFRESH_POST_SUBMIT
};

int solo_job_init(void);
void solo_job_shutdown(void);
struct solo_job *solo_job_current(void);
struct solo_job *solo_job_find(const char *job_id);
void solo_job_release(struct solo_job *job);
void solo_job_set_update_cb(solo_job_update_cb cb);
void solo_job_request_refresh(enum solo_refresh_source source);
bool solo_job_handle_fast_block_announcement(
    const char *prevhash_hex,
    const char *block_hash_hex,
    uint32_t block_time,
    uint32_t block_height,
    uint32_t block_nbits);
bool solo_job_is_ready(void);
void solo_job_get_status(struct solo_job_status *status);
sds solo_job_build_coinbase1(const struct solo_job *job, const char *worker_tag);
bool solo_job_share_meets_target(const char *hash_be, uint32_t difficulty);
bool solo_job_block_hash_valid(const struct solo_job *job, const char *hash_be);
bool solo_job_register_share(const char *hash_be);
int solo_job_submit_block(const struct solo_job *job, const char *block_head_hex, const void *coinbase_bin, size_t coinbase_len, const char *block_hash_hex);
void solo_job_get_found_blocks(struct solo_found_block *out, size_t max, size_t *count);
uint64_t solo_job_total_submitted(void);
uint64_t solo_job_total_accepted(void);

#endif
