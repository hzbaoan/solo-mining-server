#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include "job.h"
#include "job_fastpath.h"
#include "config.h"
#include "solo_log.h"
#include "solo_rpc.h"
#include "solo_utils.h"

enum {
    SHARE_SET_MIN_CAP = 1024,
    SHARE_SET_MAX_CAP = 4 * 1024 * 1024,
};

#define SOLO_LONGPOLL_TIMEOUT_SECONDS 120

static const int SOLO_SUBMIT_RETRY_DELAYS_MS[] = {0, 500, 1000, 2000, 4000};
static const int SOLO_CONFIRM_RETRY_DELAYS_MS[] = {300, 600, 1200, 2400, 4800};

struct share_entry {
    bool            used;
    unsigned char   hash[32];
};

struct share_set {
    struct share_entry  *entries;
    size_t              cap;
    size_t              count;
};

struct submit_request {
    sds                     block_hash;
    sds                     block_hex;
    uint32_t                job_height;
    struct submit_request   *next;
};

struct chain_tip_state {
    bool            ready;
    char            hash_hex[65];
    uint32_t        height;
    uint32_t        median_time_past;
    uint32_t        version;
    uint32_t        nbits;
    char            nbits_hex[9];
    unsigned char   target[32];
};

static pthread_rwlock_t job_lock = PTHREAD_RWLOCK_INITIALIZER;
static pthread_mutex_t submit_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t submit_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t share_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t status_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t refresh_thread_id;
static pthread_t longpoll_thread_id;
static pthread_t submit_thread_id;
static bool refresh_started;
static bool longpoll_started;
static bool submit_started;
static bool refresh_running;
static bool longpoll_running;
static bool submit_running;

static pthread_mutex_t refresh_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t refresh_cond = PTHREAD_COND_INITIALIZER;
static bool refresh_inflight;
static bool refresh_pending;
static enum solo_refresh_source refresh_pending_source;

static pthread_mutex_t longpoll_lock = PTHREAD_MUTEX_INITIALIZER;
static sds last_longpollid;
static struct solo_rpc_error longpoll_last_error;

static struct submit_request *submit_head;
static struct submit_request *submit_tail;
static struct solo_job **job_cache;
static size_t job_cache_count;
static size_t job_cache_limit;
static struct solo_job *current_job;
static solo_job_update_cb update_cb;
static uint32_t next_job_id;
static unsigned char diff1_target[32];
static struct share_set share_cache;
static bool last_refresh_ok;
static uint64_t last_refresh_ms;
static double last_network_hashrate_hps;
static struct chain_tip_state chain_tip;
static struct solo_found_block found_blocks[SOLO_FOUND_BLOCKS_MAX];
static size_t found_blocks_count;
static size_t found_blocks_pos;
static uint64_t blocks_submitted_total;
static uint64_t blocks_accepted_total;

static uint64_t share_hash_fn(const unsigned char hash[32])
{
    uint64_t value = 1469598103934665603ULL;
    for (size_t i = 0; i < 32; ++i) {
        value ^= hash[i];
        value *= 1099511628211ULL;
    }
    return value;
}

static int share_set_alloc(struct share_set *set, size_t cap)
{
    set->entries = calloc(cap, sizeof(*set->entries));
    if (set->entries == NULL)
        return -1;
    set->cap = cap;
    set->count = 0;
    return 0;
}

static int share_set_grow(struct share_set *set)
{
    struct share_entry *old_entries = set->entries;
    size_t old_cap = set->cap;
    size_t new_cap = old_cap ? old_cap << 1 : SHARE_SET_MIN_CAP;
    struct share_set tmp = {0};

    if (new_cap > SHARE_SET_MAX_CAP)
        return -1;
    if (share_set_alloc(&tmp, new_cap) < 0)
        return -1;

    for (size_t i = 0; i < old_cap; ++i) {
        size_t idx;
        if (!old_entries[i].used)
            continue;
        idx = share_hash_fn(old_entries[i].hash) & (tmp.cap - 1);
        while (tmp.entries[idx].used)
            idx = (idx + 1) & (tmp.cap - 1);
        tmp.entries[idx] = old_entries[i];
        tmp.count++;
    }

    free(set->entries);
    *set = tmp;
    return 0;
}

static int share_set_init(struct share_set *set)
{
    return share_set_alloc(set, SHARE_SET_MIN_CAP);
}

static void share_set_clear(struct share_set *set)
{
    if (set->entries == NULL)
        return;
    memset(set->entries, 0, sizeof(*set->entries) * set->cap);
    set->count = 0;
}

static bool share_set_add_unique(struct share_set *set, const unsigned char hash[32])
{
    size_t idx;
    if (set == NULL || set->entries == NULL)
        return false;

    if ((set->count + 1) * 10 >= set->cap * 7) {
        if (share_set_grow(set) < 0)
            return false;
    }

    idx = share_hash_fn(hash) & (set->cap - 1);
    while (set->entries[idx].used) {
        if (memcmp(set->entries[idx].hash, hash, 32) == 0)
            return false;
        idx = (idx + 1) & (set->cap - 1);
    }

    set->entries[idx].used = true;
    memcpy(set->entries[idx].hash, hash, 32);
    set->count++;
    return true;
}


static void submit_request_free(struct submit_request *req)
{
    if (req == NULL)
        return;
    sdsfree(req->block_hash);
    sdsfree(req->block_hex);
    free(req);
}

static struct submit_request *pop_submit_request(void)
{
    struct submit_request *req = submit_head;
    if (req == NULL)
        return NULL;
    submit_head = req->next;
    if (submit_head == NULL)
        submit_tail = NULL;
    req->next = NULL;
    return req;
}

static void clear_submit_queue(void)
{
    while (submit_head) {
        struct submit_request *req = pop_submit_request();
        submit_request_free(req);
    }
}

static void solo_job_free(struct solo_job *job)
{
    if (job == NULL)
        return;

    sdsfree(job->prevhash_notify_hex);
    sdsfree(job->coinbase_tx_prefix_bin);
    sdsfree(job->coinbaseaux_bin);
    sdsfree(job->coinbase2_bin);
    sdsfree(job->coinbase2_hex);
    if (job->merkle_branch) {
        for (size_t i = 0; i < job->merkle_branch_count; ++i)
            sdsfree(job->merkle_branch[i]);
        free(job->merkle_branch);
    }
    if (job->merkle_json)
        json_decref(job->merkle_json);
    sdsfree(job->notify_prefix_json);
    sdsfree(job->notify_suffix_clean_json);
    sdsfree(job->notify_suffix_dirty_json);
    sdsfree(job->txs_hex);
    sdsfree(job->submit_tail_hex);
    sdsfree(job->template_sig);
    free(job);
}

static void solo_job_acquire_ref(struct solo_job *job)
{
    if (job)
        __atomic_add_fetch(&job->refcount, 1, __ATOMIC_RELAXED);
}

void solo_job_release(struct solo_job *job)
{
    if (job == NULL)
        return;
    if (__atomic_sub_fetch(&job->refcount, 1, __ATOMIC_ACQ_REL) == 0)
        solo_job_free(job);
}

static void clear_job_cache(void)
{
    if (job_cache == NULL)
        return;

    for (size_t i = 0; i < job_cache_count; ++i) {
        solo_job_release(job_cache[i]);
        job_cache[i] = NULL;
    }
    job_cache_count = 0;
    current_job = NULL;
}

static int init_job_cache(void)
{
    job_cache_limit = (size_t)settings.keep_old_jobs + 1;
    if (job_cache_limit == 0)
        job_cache_limit = 1;
    job_cache = calloc(job_cache_limit, sizeof(*job_cache));
    return job_cache ? 0 : -1;
}

static int ensure_job_cache_capacity(size_t cap)
{
    struct solo_job **new_cache;
    size_t old_limit;

    if (cap == 0)
        return 0;

    pthread_rwlock_wrlock(&job_lock);
    if (job_cache_limit >= cap) {
        pthread_rwlock_unlock(&job_lock);
        return 0;
    }

    old_limit = job_cache_limit;
    new_cache = realloc(job_cache, cap * sizeof(*job_cache));
    if (new_cache == NULL) {
        pthread_rwlock_unlock(&job_lock);
        return -1;
    }

    memset(new_cache + old_limit, 0, (cap - old_limit) * sizeof(*new_cache));
    job_cache = new_cache;
    job_cache_limit = cap;
    pthread_rwlock_unlock(&job_lock);
    return 0;
}

static void cache_new_job(struct solo_job *job, bool clean_jobs, const struct chain_tip_state *tip)
{
    pthread_rwlock_wrlock(&job_lock);

    if (clean_jobs)
        clear_job_cache();

    while (job_cache_count >= job_cache_limit) {
        size_t tail = job_cache_count - 1;
        solo_job_release(job_cache[tail]);
        job_cache[tail] = NULL;
        job_cache_count--;
    }

    if (job_cache_count > 0)
        memmove(&job_cache[1], &job_cache[0], sizeof(*job_cache) * job_cache_count);

    job_cache[0] = job;
    job_cache_count++;
    current_job = job;
    if (tip)
        chain_tip = *tip;

    pthread_rwlock_unlock(&job_lock);
}

static uint64_t block_subsidy_sat(uint32_t height)
{
    uint32_t halvings = height / 210000U;
    if (halvings >= 64)
        return 0;
    return (50ULL * 100000000ULL) >> halvings;
}

static sds build_template_signature(json_t *tmpl)
{
    static const char *keys[] = {
        "previousblockhash",
        "bits",
        "target",
        "height",
        "version",
        "coinbasevalue",
        "coinbaseaux",
        "default_witness_commitment",
        "transactions",
        "curtime",
        "mintime",
    };
    json_t *sig = json_object();
    char *dump;
    sds result;

    if (sig == NULL)
        return NULL;

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        json_t *val = json_object_get(tmpl, keys[i]);
        if (val)
            json_object_set(sig, keys[i], val);
        else
            json_object_set_new(sig, keys[i], json_null());
    }

    dump = json_dumps(sig, JSON_SORT_KEYS | JSON_COMPACT);
    json_decref(sig);
    if (dump == NULL)
        return NULL;
    result = sdsnew(dump);
    free(dump);
    return result;
}

static bool gbt_forced_rule_supported(const char *rule)
{
    if (rule == NULL || rule[0] == '\0')
        return false;
    return strcmp(rule, "segwit") == 0;
}

static bool validate_template_forced_rules(json_t *tmpl)
{
    json_t *rules_obj;
    size_t index;
    json_t *rule_obj;

    if (tmpl == NULL)
        return false;

    rules_obj = json_object_get(tmpl, "rules");
    if (rules_obj == NULL)
        return true;
    if (!json_is_array(rules_obj)) {
        log_error("reject template: rules must be an array");
        return false;
    }

    json_array_foreach(rules_obj, index, rule_obj) {
        const char *rule_name = json_string_value(rule_obj);
        bool forced = false;

        if (!json_is_string(rule_obj) || rule_name == NULL || rule_name[0] == '\0') {
            log_error("reject template: invalid rule entry index=%zu", index);
            return false;
        }
        if (rule_name[0] == '!') {
            forced = true;
            rule_name++;
            if (rule_name[0] == '\0') {
                log_error("reject template: empty forced rule index=%zu", index);
                return false;
            }
        }
        if (forced && !gbt_forced_rule_supported(rule_name)) {
            log_error("reject template: unsupported forced rule=%s", rule_name);
            return false;
        }
    }

    return true;
}

static int build_prevhash_fields(struct solo_job *job, const char *prevhash_hex)
{
    unsigned char prevhash_be[32];
    unsigned char notify_bin[32];
    char notify_hex[65];

    if (hex2bin_exact(prevhash_hex, prevhash_be, sizeof(prevhash_be)) < 0)
        return -1;

    memcpy(job->prevhash_le, prevhash_be, sizeof(job->prevhash_le));
    reverse_mem(job->prevhash_le, sizeof(job->prevhash_le));

    memcpy(notify_bin, job->prevhash_le, sizeof(notify_bin));
    for (int i = 0; i < 8; ++i)
        reverse_mem(notify_bin + (i * 4), 4);
    if (bin2hex_into(notify_bin, sizeof(notify_bin), notify_hex, sizeof(notify_hex)) < 0)
        return -1;

    job->prevhash_notify_hex = sdsnew(notify_hex);
    return job->prevhash_notify_hex ? 0 : -1;
}

static int append_coinbase_aux(struct solo_job *job, json_t *tmpl)
{
    json_t *auxobj = json_object_get(tmpl, "coinbaseaux");
    const char *key;
    json_t *value;
    sds aux_hex = sdsempty();

    job->coinbaseaux_bin = sdsempty();
    if (job->coinbaseaux_bin == NULL || aux_hex == NULL)
        goto fail;
    if (!auxobj || !json_is_object(auxobj)) {
        sdsfree(aux_hex);
        return 0;
    }

    json_object_foreach(auxobj, key, value) {
        if (!json_is_string(value))
            goto fail;
        if (sds_append_cstr_owned(&aux_hex, json_string_value(value)) < 0)
            goto fail;
    }

    if (sdslen(aux_hex) == 0) {
        sdsfree(aux_hex);
        return 0;
    }

    sdsfree(job->coinbaseaux_bin);
    job->coinbaseaux_bin = hex2bin(aux_hex);
    sdsfree(aux_hex);
    return job->coinbaseaux_bin ? 0 : -1;

fail:
    sdsfree(aux_hex);
    return -1;
}

static int pack_output_transaction(void **p, size_t *left, sds script, uint64_t reward)
{
    ERR_RET(pack_uint64_le(p, left, reward));
    ERR_RET(pack_varint_le(p, left, sdslen(script)));
    ERR_RET(pack_buf(p, left, script, sdslen(script)));
    return 0;
}

static int build_coinbase_prefix(struct solo_job *job)
{
    unsigned char coinbase1[128];
    void *p = coinbase1;
    size_t left = sizeof(coinbase1);
    unsigned char txin_hash[32] = {0};

    ERR_RET(pack_uint32_le(&p, &left, 1));
    ERR_RET(pack_varint_le(&p, &left, 1));
    ERR_RET(pack_buf(&p, &left, txin_hash, sizeof(txin_hash)));
    ERR_RET(pack_uint32_le(&p, &left, 0xffffffffu));

    job->coinbase_tx_prefix_bin = sdsnewlen(coinbase1, sizeof(coinbase1) - left);
    return job->coinbase_tx_prefix_bin ? 0 : -1;
}

static int build_coinbase_suffix(struct solo_job *job, json_t *tmpl)
{
    unsigned char coinbase2[1024];
    void *p = coinbase2;
    size_t left = sizeof(coinbase2);
    int out_transaction_num = 1;
    const char *commitment_hex = tmpl ? json_string_value(json_object_get(tmpl, "default_witness_commitment")) : NULL;
    bool has_commitment = settings.segwit_commitment_enabled && commitment_hex != NULL;

    job->has_witness_commitment = has_commitment;

    ERR_RET(pack_uint32_le(&p, &left, 0xffffffffu));
    if (has_commitment)
        out_transaction_num += 1;

    ERR_RET(pack_varint_le(&p, &left, out_transaction_num));
    ERR_RET(pack_output_transaction(&p, &left, settings.payout_script, job->coinbase_value));

    if (has_commitment) {
        sds commitment = hex2bin(commitment_hex);
        if (commitment == NULL)
            return -1;
        if (pack_uint64_le(&p, &left, 0) < 0 ||
            pack_varint_le(&p, &left, sdslen(commitment)) < 0 ||
            pack_buf(&p, &left, commitment, sdslen(commitment)) < 0) {
            sdsfree(commitment);
            return -1;
        }
        sdsfree(commitment);
    }

    ERR_RET(pack_uint32_le(&p, &left, 0));

    job->coinbase2_bin = sdsnewlen(coinbase2, sizeof(coinbase2) - left);
    if (job->coinbase2_bin == NULL)
        return -1;
    job->coinbase2_hex = bin2hex(job->coinbase2_bin, sdslen(job->coinbase2_bin));
    return job->coinbase2_hex ? 0 : -1;
}

static int build_merkle_branch(struct solo_job *job, json_t *tmpl)
{
    json_t *txs = json_object_get(tmpl, "transactions");
    sds *nodes = NULL;

    if (!txs || !json_is_array(txs))
        return -1;

    job->tx_count = (uint32_t)json_array_size(txs);
    job->txs_hex = sdsempty();
    if (job->txs_hex == NULL)
        return -1;
    if (job->tx_count == 0) {
        job->merkle_json = json_array();
        return job->merkle_json ? 0 : -1;
    }

    nodes = calloc(job->tx_count, sizeof(*nodes));
    if (nodes == NULL)
        return -1;

    for (size_t i = 0; i < job->tx_count; ++i) {
        json_t *tx = json_array_get(txs, i);
        const char *data;
        const char *hash;
        const char *txid;

        if (!tx || !json_is_object(tx))
            goto fail;

        data = json_string_value(json_object_get(tx, "data"));
        hash = json_string_value(json_object_get(tx, "hash"));
        txid = json_string_value(json_object_get(tx, "txid"));
        if (data == NULL || hash == NULL)
            goto fail;
        if (txid != NULL && strcasecmp(txid, hash) != 0) {
            job->has_witness_transactions = true;
            if (!settings.segwit_commitment_enabled) {
                log_error(
                    "reject template height: %u tx_index: %zu witness tx present while coinbase.segwit_commitment=false",
                    job->height,
                    i
                );
                goto fail;
            }
        }
        if (txid == NULL)
            txid = hash;

        if (sds_append_cstr_owned(&job->txs_hex, data) < 0)
            goto fail;

        nodes[i] = hex2bin(txid);
        if (nodes[i] == NULL)
            goto fail;
        reverse_mem(nodes[i], sdslen(nodes[i]));
    }

    job->merkle_branch = get_merkle_branch(nodes, job->tx_count, &job->merkle_branch_count);
    if (job->merkle_branch == NULL)
        goto fail;

    job->merkle_json = json_array();
    if (job->merkle_json == NULL)
        goto fail;

    for (size_t i = 0; i < job->merkle_branch_count; ++i) {
        sds hex = bin2hex(job->merkle_branch[i], sdslen(job->merkle_branch[i]));
        if (hex == NULL)
            goto fail;
        json_array_append_new(job->merkle_json, json_string(hex));
        sdsfree(hex);
    }

    for (size_t i = 0; i < job->tx_count; ++i)
        sdsfree(nodes[i]);
    free(nodes);
    return 0;

fail:
    if (nodes) {
        for (size_t i = 0; i < job->tx_count; ++i)
            sdsfree(nodes[i]);
        free(nodes);
    }
    return -1;
}

static int append_json_quoted(sds *dst, const char *value)
{
    return (sds_append_cstr_owned(dst, "\"") < 0 ||
        sds_append_cstr_owned(dst, value) < 0 ||
        sds_append_cstr_owned(dst, "\"") < 0) ? -1 : 0;
}

static sds build_notify_suffix(const struct solo_job *job, const char *merkle_json, bool clean)
{
    sds dst = sdsempty();
    if (dst == NULL)
        return NULL;

    if (sds_append_cstr_owned(&dst, "\",") < 0 ||
        append_json_quoted(&dst, job->coinbase2_hex) < 0 ||
        sds_append_cstr_owned(&dst, ",") < 0 ||
        sds_append_cstr_owned(&dst, merkle_json) < 0 ||
        sds_append_cstr_owned(&dst, ",") < 0 ||
        append_json_quoted(&dst, job->version_hex) < 0 ||
        sds_append_cstr_owned(&dst, ",") < 0 ||
        append_json_quoted(&dst, job->nbits_hex) < 0 ||
        sds_append_cstr_owned(&dst, ",") < 0 ||
        append_json_quoted(&dst, job->curtime_hex) < 0 ||
        sds_append_cstr_owned(&dst, clean ? ",true]}" : ",false]}") < 0) {
        sdsfree(dst);
        return NULL;
    }

    return dst;
}

static int build_notify_templates(struct solo_job *job)
{
    char *merkle_json = NULL;

    if (job == NULL || job->merkle_json == NULL)
        return -1;

    merkle_json = json_dumps(job->merkle_json, JSON_COMPACT);
    if (merkle_json == NULL)
        goto fail;

    job->notify_prefix_json = sdsempty();
    if (job->notify_prefix_json == NULL)
        goto fail;

    if (sds_append_cstr_owned(&job->notify_prefix_json, "{\"id\":null,\"method\":\"mining.notify\",\"params\":[") < 0 ||
        append_json_quoted(&job->notify_prefix_json, job->job_id) < 0 ||
        sds_append_cstr_owned(&job->notify_prefix_json, ",") < 0 ||
        append_json_quoted(&job->notify_prefix_json, job->prevhash_notify_hex) < 0 ||
        sds_append_cstr_owned(&job->notify_prefix_json, ",\"") < 0) {
        goto fail;
    }

    job->notify_suffix_clean_json = build_notify_suffix(job, merkle_json, true);
    job->notify_suffix_dirty_json = build_notify_suffix(job, merkle_json, false);
    if (job->notify_suffix_clean_json == NULL || job->notify_suffix_dirty_json == NULL)
        goto fail;

    free(merkle_json);
    return 0;

fail:
    free(merkle_json);
    sdsfree(job->notify_prefix_json);
    sdsfree(job->notify_suffix_clean_json);
    sdsfree(job->notify_suffix_dirty_json);
    job->notify_prefix_json = NULL;
    job->notify_suffix_clean_json = NULL;
    job->notify_suffix_dirty_json = NULL;
    return -1;
}

static int build_submit_tail(struct solo_job *job)
{
    unsigned char tx_count_buf[10];
    char tx_count_hex[(sizeof(tx_count_buf) * 2) + 1];
    void *p = tx_count_buf;
    size_t left = sizeof(tx_count_buf);
    size_t tx_count_len;

    if (job == NULL)
        return -1;
    if (pack_varint_le(&p, &left, 1 + job->tx_count) < 0)
        return -1;

    tx_count_len = sizeof(tx_count_buf) - left;
    if (bin2hex_into(tx_count_buf, tx_count_len, tx_count_hex, sizeof(tx_count_hex)) < 0)
        return -1;

    job->submit_tail_hex = sdsnew(tx_count_hex);
    if (job->submit_tail_hex == NULL)
        return -1;
    job->submit_tx_count_hex_len = tx_count_len * 2;
    if (sds_append_sds_owned(&job->submit_tail_hex, job->txs_hex) < 0)
        return -1;

    return 0;
}

static struct solo_job *build_job_from_template(json_t *tmpl, sds template_sig)
{
    struct solo_job *job = calloc(1, sizeof(*job));
    json_t *version_obj;
    json_t *curtime_obj;
    json_t *mintime_obj;
    json_t *bits_obj;
    json_t *height_obj;
    json_t *prevhash_obj;
    json_t *target_obj;
    json_t *coinbasevalue_obj;
    sds target_bin;

    if (job == NULL || template_sig == NULL) {
        sdsfree(template_sig);
        return NULL;
    }

    job->refcount = 1;
    snprintf(job->job_id, sizeof(job->job_id), "%08x", __atomic_add_fetch(&next_job_id, 1, __ATOMIC_RELAXED));

    version_obj = json_object_get(tmpl, "version");
    curtime_obj = json_object_get(tmpl, "curtime");
    mintime_obj = json_object_get(tmpl, "mintime");
    bits_obj = json_object_get(tmpl, "bits");
    height_obj = json_object_get(tmpl, "height");
    prevhash_obj = json_object_get(tmpl, "previousblockhash");
    target_obj = json_object_get(tmpl, "target");
    coinbasevalue_obj = json_object_get(tmpl, "coinbasevalue");
    if (!json_is_integer(version_obj) || !json_is_integer(curtime_obj) || !json_is_string(bits_obj) ||
        !json_is_integer(height_obj) || !json_is_string(prevhash_obj) || !json_is_string(target_obj) ||
        !json_is_integer(coinbasevalue_obj)) {
        solo_job_free(job);
        sdsfree(template_sig);
        return NULL;
    }

    job->version = (uint32_t)json_integer_value(version_obj);
    {
        json_t *vbrequired_obj = json_object_get(tmpl, "vbrequired");
        job->vbrequired = json_is_integer(vbrequired_obj)
            ? (uint32_t)json_integer_value(vbrequired_obj)
            : 0;
    }
    job->curtime = (uint32_t)json_integer_value(curtime_obj);
    job->mintime = json_is_integer(mintime_obj) ? (uint32_t)json_integer_value(mintime_obj) : job->curtime;
    job->nbits = (uint32_t)strtoul(json_string_value(bits_obj), NULL, 16);
    job->height = (uint32_t)json_integer_value(height_obj);
    job->coinbase_value = (uint64_t)json_integer_value(coinbasevalue_obj);
    snprintf(job->version_hex, sizeof(job->version_hex), "%08x", job->version);
    snprintf(job->curtime_hex, sizeof(job->curtime_hex), "%08x", job->curtime);
    snprintf(job->nbits_hex, sizeof(job->nbits_hex), "%08x", job->nbits);

    target_bin = hex2bin(json_string_value(target_obj));
    if (target_bin == NULL || sdslen(target_bin) != sizeof(job->target)) {
        sdsfree(target_bin);
        solo_job_free(job);
        sdsfree(template_sig);
        return NULL;
    }
    memcpy(job->target, target_bin, sizeof(job->target));
    sdsfree(target_bin);

    if (build_prevhash_fields(job, json_string_value(prevhash_obj)) < 0 ||
        append_coinbase_aux(job, tmpl) < 0 ||
        build_merkle_branch(job, tmpl) < 0 ||
        build_coinbase_prefix(job) < 0 ||
        build_coinbase_suffix(job, tmpl) < 0 ||
        build_notify_templates(job) < 0 ||
        build_submit_tail(job) < 0) {
        solo_job_free(job);
        sdsfree(template_sig);
        return NULL;
    }

    job->template_sig = template_sig;
    return job;
}

static bool is_clean_job(const struct solo_job *old_job, const struct solo_job *new_job)
{
    if (old_job == NULL || new_job == NULL)
        return true;
    if (old_job->height != new_job->height)
        return true;
    return memcmp(old_job->prevhash_le, new_job->prevhash_le, sizeof(old_job->prevhash_le)) != 0;
}

static bool is_fast_job_confirmation(const struct solo_job *old_job, const struct solo_job *new_job)
{
    return old_job != NULL &&
        old_job->is_fast_job &&
        !is_clean_job(old_job, new_job);
}

static bool is_stale_template_after_fast_job(const struct solo_job *old_job, const struct solo_job *new_job)
{
    return old_job != NULL &&
        new_job != NULL &&
        old_job->is_fast_job &&
        new_job->height < old_job->height;
}

static bool derive_chain_tip_from_template(json_t *tmpl, struct chain_tip_state *tip)
{
    json_t *version_obj;
    json_t *bits_obj;
    json_t *height_obj;
    json_t *mintime_obj;
    json_t *prevhash_obj;
    json_t *target_obj;
    sds target_bin = NULL;

    if (tmpl == NULL || tip == NULL)
        return false;

    version_obj = json_object_get(tmpl, "version");
    bits_obj = json_object_get(tmpl, "bits");
    height_obj = json_object_get(tmpl, "height");
    mintime_obj = json_object_get(tmpl, "mintime");
    prevhash_obj = json_object_get(tmpl, "previousblockhash");
    target_obj = json_object_get(tmpl, "target");
    if (!json_is_integer(version_obj) || !json_is_string(bits_obj) || !json_is_integer(height_obj) ||
        !json_is_string(prevhash_obj) || !json_is_string(target_obj)) {
        return false;
    }

    memset(tip, 0, sizeof(*tip));
    snprintf(tip->hash_hex, sizeof(tip->hash_hex), "%s", json_string_value(prevhash_obj));
    tip->height = (uint32_t)json_integer_value(height_obj);
    if (tip->height > 0)
        tip->height -= 1;
    tip->median_time_past = json_is_integer(mintime_obj) ? (uint32_t)json_integer_value(mintime_obj) : 0;
    if (tip->median_time_past > 0)
        tip->median_time_past -= 1;
    tip->version = (uint32_t)json_integer_value(version_obj);
    tip->nbits = (uint32_t)strtoul(json_string_value(bits_obj), NULL, 16);
    snprintf(tip->nbits_hex, sizeof(tip->nbits_hex), "%08x", tip->nbits);

    target_bin = hex2bin(json_string_value(target_obj));
    if (target_bin == NULL || sdslen(target_bin) != sizeof(tip->target)) {
        sdsfree(target_bin);
        return false;
    }
    memcpy(tip->target, target_bin, sizeof(tip->target));
    tip->ready = true;
    sdsfree(target_bin);
    return true;
}

static bool decode_compact_target(uint32_t nbits, unsigned char target[32])
{
    uint32_t exponent = nbits >> 24;
    uint32_t mantissa = nbits & 0x007fffffU;

    if (target == NULL)
        return false;

    memset(target, 0, 32);
    if ((nbits & 0x00800000U) != 0 || mantissa == 0 || exponent == 0 || exponent > 32)
        return false;

    if (exponent <= 3U) {
        mantissa >>= 8U * (3U - exponent);
        for (uint32_t i = 0; i < exponent; ++i) {
            target[32U - exponent + i] =
                (unsigned char)((mantissa >> (8U * (exponent - 1U - i))) & 0xffU);
        }
        return true;
    }

    target[32U - exponent] = (unsigned char)((mantissa >> 16) & 0xffU);
    target[33U - exponent] = (unsigned char)((mantissa >> 8) & 0xffU);
    target[34U - exponent] = (unsigned char)(mantissa & 0xffU);
    return true;
}

static bool block_hash_meets_compact_target(const char *block_hash_hex, uint32_t block_nbits)
{
    unsigned char target[32];
    sds block_hash_bin = NULL;
    bool valid = false;

    if (block_hash_hex == NULL)
        return false;
    if (!decode_compact_target(block_nbits, target))
        return false;

    block_hash_bin = hex2bin(block_hash_hex);
    if (block_hash_bin == NULL || sdslen(block_hash_bin) != 32)
        goto out;

    valid = memcmp(block_hash_bin, target, 32) <= 0;

out:
    sdsfree(block_hash_bin);
    return valid;
}

static struct solo_job *build_fast_job(
    const struct chain_tip_state *tip,
    const char *block_hash_hex,
    uint32_t next_height,
    uint32_t estimated_mtp)
{
    struct solo_job *job = calloc(1, sizeof(*job));
    char signature[256];
    time_t now_secs;

    if (job == NULL || tip == NULL || block_hash_hex == NULL)
        return NULL;

    job->refcount = 1;
    job->is_fast_job = true;
    snprintf(job->job_id, sizeof(job->job_id), "%08x", __atomic_add_fetch(&next_job_id, 1, __ATOMIC_RELAXED));
    job->version = tip->version;
    job->mintime = estimated_mtp + 1;
    now_secs = time(NULL);
    if (now_secs < 0)
        now_secs = 0;
    job->curtime = (uint32_t)now_secs;
    if (job->curtime < job->mintime)
        job->curtime = job->mintime;
    job->nbits = tip->nbits;
    job->height = next_height;
    job->coinbase_value = block_subsidy_sat(next_height);
    snprintf(job->version_hex, sizeof(job->version_hex), "%08x", job->version);
    snprintf(job->curtime_hex, sizeof(job->curtime_hex), "%08x", job->curtime);
    snprintf(job->nbits_hex, sizeof(job->nbits_hex), "%08x", job->nbits);
    memcpy(job->target, tip->target, sizeof(job->target));

    job->coinbaseaux_bin = sdsempty();
    job->txs_hex = sdsempty();
    job->merkle_json = json_array();
    if (job->coinbaseaux_bin == NULL || job->txs_hex == NULL || job->merkle_json == NULL) {
        solo_job_free(job);
        return NULL;
    }

    if (build_prevhash_fields(job, block_hash_hex) < 0 ||
        build_coinbase_prefix(job) < 0 ||
        build_coinbase_suffix(job, NULL) < 0 ||
        build_notify_templates(job) < 0 ||
        build_submit_tail(job) < 0) {
        solo_job_free(job);
        return NULL;
    }

    snprintf(
        signature,
        sizeof(signature),
        "fast:%s:%u:%s:%08x",
        block_hash_hex,
        next_height,
        tip->nbits_hex,
        tip->version
    );
    job->template_sig = sdsnew(signature);
    if (job->template_sig == NULL) {
        solo_job_free(job);
        return NULL;
    }

    return job;
}

struct solo_job *solo_job_current(void)
{
    struct solo_job *job;
    pthread_rwlock_rdlock(&job_lock);
    job = current_job;
    solo_job_acquire_ref(job);
    pthread_rwlock_unlock(&job_lock);
    return job;
}

struct solo_job *solo_job_find(const char *job_id)
{
    struct solo_job *job = NULL;
    if (job_id == NULL)
        return NULL;

    pthread_rwlock_rdlock(&job_lock);
    for (size_t i = 0; i < job_cache_count; ++i) {
        if (strcmp(job_cache[i]->job_id, job_id) == 0) {
            job = job_cache[i];
            solo_job_acquire_ref(job);
            break;
        }
    }
    pthread_rwlock_unlock(&job_lock);
    return job;
}

void solo_job_get_status(struct solo_job_status *status)
{
    struct solo_job *job;
    if (status == NULL)
        return;

    memset(status, 0, sizeof(*status));
    pthread_mutex_lock(&status_lock);
    status->last_refresh_ok = last_refresh_ok;
    status->last_refresh_ms = last_refresh_ms;
    status->network_hashrate_hps = last_network_hashrate_hps;
    pthread_mutex_unlock(&status_lock);

    job = solo_job_current();
    if (job == NULL)
        return;

    status->ready = true;
    status->height = job->height;
    status->coinbase_value = job->coinbase_value;
    status->tx_count = job->tx_count;
    snprintf(status->nbits_hex, sizeof(status->nbits_hex), "%s", job->nbits_hex);
    solo_job_release(job);
}

bool solo_job_is_ready(void)
{
    struct solo_job_status status;
    solo_job_get_status(&status);
    return status.ready && status.last_refresh_ok;
}

void solo_job_set_update_cb(solo_job_update_cb cb)
{
    update_cb = cb;
}

static const char *refresh_source_name(enum solo_refresh_source source)
{
    switch (source) {
    case SOLO_REFRESH_STARTUP:
        return "startup";
    case SOLO_REFRESH_MANUAL:
        return "manual";
    case SOLO_REFRESH_LONGPOLL_SUCCESS:
        return "longpoll_success";
    case SOLO_REFRESH_LONGPOLL_TIMEOUT:
        return "longpoll_timeout";
    case SOLO_REFRESH_LONGPOLL_ERROR:
        return "longpoll_error";
    case SOLO_REFRESH_ZMQ_HASHBLOCK:
        return "zmq_hashblock";
    case SOLO_REFRESH_ZMQ_RAWBLOCK:
        return "zmq_rawblock";
    case SOLO_REFRESH_POST_SUBMIT:
        return "post_submit";
    default:
        return "unknown";
    }
}

static void update_refresh_status(bool ok);

static void log_p2p_fast_skip(
    const char *reason,
    const char *block_hash_hex,
    uint32_t block_height,
    uint32_t next_height,
    uint32_t current_vbrequired)
{
    log_info(
        "skip p2p fast job: reason=%s network=%s hash=%s block_height=%u next_height=%u current_vbrequired=%08x",
        reason ? reason : "unknown",
        bitcoin_network_name(settings.network),
        block_hash_hex ? block_hash_hex : "-",
        block_height,
        next_height,
        current_vbrequired
    );
}

static void request_refresh_locked(enum solo_refresh_source source)
{
    if (refresh_pending && refresh_pending_source != source) {
        log_info(
            "coalescing refresh request: pending=%s replaced_by=%s",
            refresh_source_name(refresh_pending_source),
            refresh_source_name(source)
        );
    }

    refresh_pending = true;
    refresh_pending_source = source;
    if (!refresh_inflight)
        pthread_cond_signal(&refresh_cond);
}

void solo_job_request_refresh(enum solo_refresh_source source)
{
    pthread_mutex_lock(&refresh_lock);
    request_refresh_locked(source);
    pthread_mutex_unlock(&refresh_lock);
}

bool solo_job_handle_fast_block_announcement(
    const char *prevhash_hex,
    const char *block_hash_hex,
    uint32_t block_time,
    uint32_t block_height,
    uint32_t block_nbits)
{
    struct chain_tip_state tip;
    struct chain_tip_state next_tip;
    struct solo_job *job = NULL;
    struct solo_job *current = NULL;
    sds next_prevhash_bin = NULL;
    uint32_t estimated_mtp;
    uint32_t next_height;
    uint32_t current_vbrequired = 0;
    bool applied = false;

    if (prevhash_hex == NULL || block_hash_hex == NULL || strlen(prevhash_hex) != 64 || strlen(block_hash_hex) != 64)
        return false;

    pthread_rwlock_rdlock(&job_lock);
    tip = chain_tip;
    pthread_rwlock_unlock(&job_lock);
    if (!tip.ready)
        return false;
    if (strcasecmp(tip.hash_hex, prevhash_hex) != 0)
        return false;
    if (block_height != tip.height + 1)
        return false;
    if (!block_hash_meets_compact_target(block_hash_hex, block_nbits)) {
        log_warn("reject p2p fast block: invalid pow hash=%s nbits=%08x", block_hash_hex, block_nbits);
        return false;
    }
    if (block_nbits != tip.nbits) {
        log_info(
            "skip p2p fast block: nbits mismatch hash=%s height=%u block_nbits=%08x expected=%08x",
            block_hash_hex,
            block_height,
            block_nbits,
            tip.nbits
        );
        return false;
    }

    next_height = block_height + 1;
    current = solo_job_current();
    if (current != NULL)
        current_vbrequired = current->vbrequired;

    if (settings.network != BITCOIN_NETWORK_MAINNET) {
        log_p2p_fast_skip("non-mainnet", block_hash_hex, block_height, next_height, current_vbrequired);
        solo_job_release(current);
        return false;
    }
    if (current == NULL) {
        log_p2p_fast_skip("missing-current-job", block_hash_hex, block_height, next_height, current_vbrequired);
        return false;
    }
    if (current->vbrequired != 0) {
        log_p2p_fast_skip("vbrequired", block_hash_hex, block_height, next_height, current_vbrequired);
        solo_job_release(current);
        return false;
    }
    if (next_height % 2016U == 0) {
        log_p2p_fast_skip("retarget-boundary", block_hash_hex, block_height, next_height, current_vbrequired);
        solo_job_release(current);
        return false;
    }

    next_prevhash_bin = hex2bin(block_hash_hex);
    if (next_prevhash_bin == NULL || sdslen(next_prevhash_bin) != 32) {
        sdsfree(next_prevhash_bin);
        solo_job_release(current);
        return false;
    }
    reverse_mem(next_prevhash_bin, sdslen(next_prevhash_bin));

    if ((current->height == next_height &&
         memcmp(current->prevhash_le, next_prevhash_bin, sizeof(current->prevhash_le)) == 0) ||
        current->height > next_height) {
        sdsfree(next_prevhash_bin);
        solo_job_release(current);
        return false;
    }
    solo_job_release(current);
    current = NULL;
    sdsfree(next_prevhash_bin);

    estimated_mtp = tip.median_time_past > block_time ? tip.median_time_past : block_time;
    job = build_fast_job(&tip, block_hash_hex, next_height, estimated_mtp);
    if (job == NULL)
        return false;

    memset(&next_tip, 0, sizeof(next_tip));
    next_tip.ready = true;
    snprintf(next_tip.hash_hex, sizeof(next_tip.hash_hex), "%s", block_hash_hex);
    next_tip.height = block_height;
    next_tip.median_time_past = estimated_mtp;
    next_tip.version = tip.version;
    next_tip.nbits = block_nbits;
    snprintf(next_tip.nbits_hex, sizeof(next_tip.nbits_hex), "%08x", next_tip.nbits);
    memcpy(next_tip.target, tip.target, sizeof(next_tip.target));

    pthread_mutex_lock(&share_lock);
    share_set_clear(&share_cache);
    pthread_mutex_unlock(&share_lock);

    cache_new_job(job, true, &next_tip);
    log_info("update fast job: %s height: %u clean: true source: p2p_fast", job->job_id, job->height);
    update_refresh_status(true);
    if (update_cb)
        update_cb(job, true);
    applied = true;
    return applied;
}

static int init_diff1_target(void)
{
    sds diff1 = hex2bin("00000000ffff0000000000000000000000000000000000000000000000000000");
    if (diff1 == NULL || sdslen(diff1) != sizeof(diff1_target)) {
        sdsfree(diff1);
        return -1;
    }
    memcpy(diff1_target, diff1, sizeof(diff1_target));
    sdsfree(diff1);
    return 0;
}

static sds build_coinbase_message(const char *worker_tag)
{
    char message[80];
    if (settings.coinbase_worker_tag && worker_tag && worker_tag[0])
        snprintf(message, sizeof(message), "/solo/%-.24s/%-.24s", settings.coinbase_message, worker_tag);
    else
        snprintf(message, sizeof(message), "/solo/%-.48s", settings.coinbase_message);
    return sdsnew(message);
}

sds solo_job_build_coinbase1(const struct solo_job *job, const char *worker_tag)
{
    sds message;
    unsigned char script[256];
    unsigned char coinbase1[512];
    void *p;
    size_t left;
    size_t script_prefix_len;
    size_t script_total_len;

    if (job == NULL)
        return NULL;

    message = build_coinbase_message(worker_tag);
    if (message == NULL)
        return NULL;

    p = script;
    left = sizeof(script);
    if (pack_oppushint_le(&p, &left, job->height) < 0) {
        sdsfree(message);
        return NULL;
    }
    if (sdslen(message) > 0 && pack_oppush(&p, &left, message, sdslen(message)) < 0) {
        sdsfree(message);
        return NULL;
    }
    sdsfree(message);
    if (sdslen(job->coinbaseaux_bin) > 0 && pack_buf(&p, &left, job->coinbaseaux_bin, sdslen(job->coinbaseaux_bin)) < 0)
        return NULL;

    script_prefix_len = sizeof(script) - left;
    script_total_len = script_prefix_len + get_extra_nonce_size();
    if (script_total_len > 100)
        return NULL;

    p = coinbase1;
    left = sizeof(coinbase1);
    if (pack_buf(&p, &left, job->coinbase_tx_prefix_bin, sdslen(job->coinbase_tx_prefix_bin)) < 0 ||
        pack_varint_le(&p, &left, script_total_len) < 0 ||
        pack_buf(&p, &left, script, script_prefix_len) < 0) {
        return NULL;
    }

    return sdsnewlen(coinbase1, sizeof(coinbase1) - left);
}

static void divide_u256(unsigned char value[32], uint32_t divisor)
{
    uint64_t remain = 0;
    for (int i = 0; i < 32; ++i) {
        uint64_t cur = (remain << 8) | value[i];
        value[i] = (unsigned char)(cur / divisor);
        remain = cur % divisor;
    }
}

bool solo_job_share_meets_target(const char *hash_be, uint32_t difficulty)
{
    unsigned char share_target[32];
    if (hash_be == NULL || difficulty == 0)
        return false;
    memcpy(share_target, diff1_target, sizeof(share_target));
    divide_u256(share_target, difficulty);
    return memcmp(hash_be, share_target, sizeof(share_target)) <= 0;
}

bool solo_job_block_hash_valid(const struct solo_job *job, const char *hash_be)
{
    if (job == NULL || hash_be == NULL)
        return false;
    return memcmp(hash_be, job->target, sizeof(job->target)) <= 0;
}

bool solo_job_register_share(const char *hash_be)
{
    bool accepted;
    pthread_mutex_lock(&share_lock);
    accepted = share_set_add_unique(&share_cache, (const unsigned char *)hash_be);
    pthread_mutex_unlock(&share_lock);
    return accepted;
}

static sds build_submit_coinbase(const struct solo_job *job, const void *coinbase_bin, size_t coinbase_len)
{
    const unsigned char *stripped = coinbase_bin;
    sds submit_coinbase;
    size_t offset = 0;

    if (job == NULL || coinbase_bin == NULL)
        return NULL;
    if (!job->has_witness_commitment)
        return sdsnewlen(coinbase_bin, coinbase_len);
    if (coinbase_len < 8)
        return NULL;

    /* Miners hash the stripped form for txid/merkle work; submitblock needs the fully serialized segwit coinbase. */
    submit_coinbase = sdsnewlen(NULL, coinbase_len + 36);
    if (submit_coinbase == NULL)
        return NULL;

    memcpy(submit_coinbase + offset, stripped, 4);
    offset += 4;
    submit_coinbase[offset++] = 0x00;
    submit_coinbase[offset++] = 0x01;
    memcpy(submit_coinbase + offset, stripped + 4, coinbase_len - 8);
    offset += coinbase_len - 8;
    submit_coinbase[offset++] = 0x01;
    submit_coinbase[offset++] = 0x20;
    memset(submit_coinbase + offset, 0, 32);
    offset += 32;
    memcpy(submit_coinbase + offset, stripped + coinbase_len - 4, 4);

    return submit_coinbase;
}

int solo_job_submit_block(const struct solo_job *job, const char *block_head_hex, const void *coinbase_bin, size_t coinbase_len, const char *block_hash_hex)
{
    sds submit_coinbase = NULL;
    sds submit_coinbase_hex = NULL;
    sds block_hex = NULL;
    struct submit_request *req = NULL;
    size_t block_head_hex_len;
    size_t submit_coinbase_hex_len;
    size_t submit_tail_hex_len;
    size_t total_len;
    size_t offset = 0;

    if (job == NULL || block_head_hex == NULL || coinbase_bin == NULL || block_hash_hex == NULL)
        return -1;
    if (job->has_witness_transactions && !job->has_witness_commitment) {
        log_error("refuse submitblock for candidate: %s witness txs require coinbase commitment", block_hash_hex);
        return -1;
    }
    if (job->submit_tail_hex == NULL || job->submit_tx_count_hex_len > sdslen(job->submit_tail_hex))
        return -1;
    submit_coinbase = build_submit_coinbase(job, coinbase_bin, coinbase_len);
    if (submit_coinbase == NULL)
        goto fail;
    submit_coinbase_hex = bin2hex(submit_coinbase, sdslen(submit_coinbase));
    if (submit_coinbase_hex == NULL)
        goto fail;

    block_head_hex_len = strlen(block_head_hex);
    submit_coinbase_hex_len = sdslen(submit_coinbase_hex);
    submit_tail_hex_len = sdslen(job->submit_tail_hex);
    total_len = block_head_hex_len + submit_coinbase_hex_len + submit_tail_hex_len;

    block_hex = sdsnewlen(NULL, total_len);
    if (block_hex == NULL)
        goto fail;

    memcpy(block_hex + offset, block_head_hex, block_head_hex_len);
    offset += block_head_hex_len;
    memcpy(block_hex + offset, job->submit_tail_hex, job->submit_tx_count_hex_len);
    offset += job->submit_tx_count_hex_len;
    memcpy(block_hex + offset, submit_coinbase_hex, submit_coinbase_hex_len);
    offset += submit_coinbase_hex_len;
    memcpy(block_hex + offset, job->submit_tail_hex + job->submit_tx_count_hex_len, submit_tail_hex_len - job->submit_tx_count_hex_len);
    offset += submit_tail_hex_len - job->submit_tx_count_hex_len;
    sdssetlen(block_hex, offset);

    req = calloc(1, sizeof(*req));
    if (req == NULL)
        goto fail;
    req->block_hash = sdsnew(block_hash_hex);
    req->block_hex = block_hex;
    req->job_height = job->height;
    block_hex = NULL;
    if (req->block_hash == NULL || req->block_hex == NULL)
        goto fail;

    pthread_mutex_lock(&submit_lock);
    if (!submit_running) {
        pthread_mutex_unlock(&submit_lock);
        log_error("submit queue unavailable for candidate: %s", block_hash_hex ? block_hash_hex : "(null)");
        goto fail;
    }
    if (submit_tail)
        submit_tail->next = req;
    else
        submit_head = req;
    submit_tail = req;
    pthread_cond_signal(&submit_cond);
    pthread_mutex_unlock(&submit_lock);

    log_vip("queue submitblock for candidate: %s", block_hash_hex);
    sdsfree(submit_coinbase);
    sdsfree(submit_coinbase_hex);
    return 0;

fail:
    log_error("queue submitblock failed for candidate: %s", block_hash_hex ? block_hash_hex : "(null)");
    sdsfree(submit_coinbase);
    sdsfree(submit_coinbase_hex);
    sdsfree(block_hex);
    submit_request_free(req);
    return -1;
}

static void update_refresh_status(bool ok)
{
    pthread_mutex_lock(&status_lock);
    last_refresh_ok = ok;
    last_refresh_ms = current_time_millis();
    pthread_mutex_unlock(&status_lock);
}

static void log_rpc_error(const char *context, const char *subject, const struct solo_rpc_error *error);

static void update_network_hashrate(double hps)
{
    pthread_mutex_lock(&status_lock);
    last_network_hashrate_hps = hps > 0.0 ? hps : 0.0;
    pthread_mutex_unlock(&status_lock);
}

static bool refresh_network_hashrate(struct solo_rpc_client *rpc)
{
    json_t *result;
    struct solo_rpc_error error;
    double value;

    if (rpc == NULL)
        return false;

    memset(&error, 0, sizeof(error));
    result = solo_rpc_call_ex(rpc, "getnetworkhashps", NULL, 5.0, false, &error);
    if (result == NULL) {
        log_rpc_error("getnetworkhashps failed", "network_hashrate", &error);
        return false;
    }
    if (!json_is_number(result)) {
        json_decref(result);
        log_warn("getnetworkhashps returned non-numeric result");
        return false;
    }

    value = json_number_value(result);
    json_decref(result);
    if (value <= 0.0)
        return false;

    update_network_hashrate(value);
    return true;
}

static void sleep_ms(int delay_ms)
{
    if (delay_ms > 0)
        usleep((useconds_t)delay_ms * 1000U);
}

static void log_rpc_error(const char *context, const char *subject, const struct solo_rpc_error *error)
{
    const char *target = subject && subject[0] ? subject : "-";

    if (error == NULL) {
        log_warn("%s subject=%s", context, target);
        return;
    }
    if (error->curl_code != 0) {
        log_warn(
            "%s subject=%s curl_code=%d timed_out=%s message=%s",
            context,
            target,
            error->curl_code,
            error->timed_out ? "true" : "false",
            error->rpc_message[0] ? error->rpc_message : "-"
        );
        return;
    }
    if (error->http_code != 0 && (error->http_code < 200 || error->http_code >= 300)) {
        log_warn("%s subject=%s http_code=%ld", context, target, error->http_code);
        return;
    }
    if (error->rpc_error) {
        log_warn(
            "%s subject=%s rpc_code=%d message=%s",
            context,
            target,
            error->rpc_code,
            error->rpc_message[0] ? error->rpc_message : "-"
        );
        return;
    }
    log_warn(
        "%s subject=%s http_code=%ld message=%s",
        context,
        target,
        error->http_code,
        error->rpc_message[0] ? error->rpc_message : "-"
    );
}

static json_t *build_gbt_request_params(const char *longpollid)
{
    json_t *params = NULL;
    json_t *record = NULL;
    json_t *rules = NULL;

    params = json_array();
    record = json_object();
    rules = json_array();
    if (!params || !record || !rules)
        goto fail;

    json_array_append_new(rules, json_string("segwit"));
    json_object_set_new(record, "rules", rules);
    rules = NULL;
    if (longpollid && longpollid[0] != '\0')
        json_object_set_new(record, "longpollid", json_string(longpollid));
    json_array_append_new(params, record);
    return params;

fail:
    if (params)
        json_decref(params);
    if (record)
        json_decref(record);
    if (rules)
        json_decref(rules);
    return NULL;
}

static void update_last_longpollid(json_t *result)
{
    json_t *longpollid_obj;
    const char *value = NULL;
    sds next = NULL;

    if (result == NULL)
        return;

    longpollid_obj = json_object_get(result, "longpollid");
    if (json_is_string(longpollid_obj))
        value = json_string_value(longpollid_obj);
    if (value != NULL)
        next = sdsnew(value);

    pthread_mutex_lock(&longpoll_lock);
    sdsfree(last_longpollid);
    last_longpollid = next;
    pthread_mutex_unlock(&longpoll_lock);
}

static sds copy_last_longpollid(void)
{
    sds copy = NULL;

    pthread_mutex_lock(&longpoll_lock);
    if (last_longpollid != NULL)
        copy = sdsdup(last_longpollid);
    pthread_mutex_unlock(&longpoll_lock);
    return copy;
}

static json_t *fetch_gbt_direct(struct solo_rpc_client *rpc)
{
    json_t *params;
    json_t *result;
    struct solo_rpc_error error;

    params = build_gbt_request_params(NULL);
    if (params == NULL)
        return NULL;

    result = solo_rpc_call_ex(rpc, "getblocktemplate", params, 10.0, false, &error);
    json_decref(params);
    if (result == NULL)
        log_rpc_error("getblocktemplate direct failed", "direct", &error);
    return result;
}

static json_t *fetch_gbt_longpoll(struct solo_rpc_client *rpc, const char *longpollid)
{
    json_t *params;
    json_t *result;

    memset(&longpoll_last_error, 0, sizeof(longpoll_last_error));

    params = build_gbt_request_params(longpollid);
    if (params == NULL)
        return NULL;

    result = solo_rpc_call_ex(
        rpc,
        "getblocktemplate",
        params,
        (double)SOLO_LONGPOLL_TIMEOUT_SECONDS,
        false,
        &longpoll_last_error
    );
    json_decref(params);
    return result;
}

static int apply_gbt_result(json_t *result, enum solo_refresh_source source)
{
    sds signature = NULL;
    struct solo_job *existing = NULL;
    struct solo_job *new_job = NULL;
    struct chain_tip_state new_tip;
    bool clean_jobs = true;
    bool fast_job_confirmation = false;

    if (result == NULL)
        goto fail;
    if (!validate_template_forced_rules(result))
        goto fail;

    update_last_longpollid(result);

    signature = build_template_signature(result);
    if (signature == NULL)
        goto fail;

    existing = solo_job_current();
    if (existing && existing->template_sig && sdscmp(existing->template_sig, signature) == 0) {
        log_info(
            "skip job update: unchanged template job: %s height: %u source: %s",
            existing->job_id,
            existing->height,
            refresh_source_name(source)
        );
        solo_job_release(existing);
        update_refresh_status(true);
        sdsfree(signature);
        return 0;
    }

    new_job = build_job_from_template(result, signature);
    signature = NULL;
    if (new_job == NULL)
        goto fail;
    if (!derive_chain_tip_from_template(result, &new_tip))
        goto fail;
    if (is_stale_template_after_fast_job(existing, new_job)) {
        log_info(
            "skip job update: stale template behind fast job existing: %s height: %u incoming: %s height: %u source: %s",
            existing->job_id,
            existing->height,
            new_job->job_id,
            new_job->height,
            refresh_source_name(source)
        );
        solo_job_release(existing);
        existing = NULL;
        solo_job_release(new_job);
        new_job = NULL;
        update_refresh_status(true);
        return 0;
    }

    fast_job_confirmation = is_fast_job_confirmation(existing, new_job);
    if (fast_job_confirmation && settings.keep_old_jobs == 0 && ensure_job_cache_capacity(2) < 0) {
        log_warn("preserve fast job confirmation cache fail source=%s", refresh_source_name(source));
        fast_job_confirmation = false;
    }

    clean_jobs = is_clean_job(existing, new_job);
    if (!clean_jobs && settings.keep_old_jobs == 0 && !fast_job_confirmation)
        clean_jobs = true;
    if (existing) {
        solo_job_release(existing);
        existing = NULL;
    }

    if (clean_jobs) {
        pthread_mutex_lock(&share_lock);
        share_set_clear(&share_cache);
        pthread_mutex_unlock(&share_lock);
    }

    cache_new_job(new_job, clean_jobs, &new_tip);
    update_refresh_status(true);
    log_info(
        "update job: %s height: %u clean: %s cache: %zu source: %s",
        new_job->job_id,
        new_job->height,
        clean_jobs ? "true" : "false",
        job_cache_count,
        refresh_source_name(source)
    );
    if (update_cb)
        update_cb(new_job, clean_jobs);
    return 0;

fail:
    if (existing)
        solo_job_release(existing);
    sdsfree(signature);
    if (new_job)
        solo_job_release(new_job);
    update_refresh_status(false);
    log_error("apply getblocktemplate result fail source=%s", refresh_source_name(source));
    return -1;
}

static void record_found_block(uint32_t height, const char *hash_hex, const char *result_str)
{
    struct solo_found_block *entry;

    pthread_mutex_lock(&status_lock);
    entry = &found_blocks[found_blocks_pos % SOLO_FOUND_BLOCKS_MAX];
    entry->height = height;
    snprintf(entry->hash_hex, sizeof(entry->hash_hex), "%s", hash_hex ? hash_hex : "");
    snprintf(entry->result, sizeof(entry->result), "%s", result_str ? result_str : "unknown");
    entry->timestamp_ms = current_time_millis();
    found_blocks_pos++;
    if (found_blocks_count < SOLO_FOUND_BLOCKS_MAX)
        found_blocks_count++;
    blocks_submitted_total++;
    if (result_str &&
        (strcmp(result_str, "submitted") == 0 || strcmp(result_str, "duplicate") == 0)) {
        blocks_accepted_total++;
    }
    pthread_mutex_unlock(&status_lock);
}

static int confirm_block_in_node(struct solo_rpc_client *rpc, const char *block_hash_hex)
{
    size_t attempt_count = sizeof(SOLO_CONFIRM_RETRY_DELAYS_MS) / sizeof(SOLO_CONFIRM_RETRY_DELAYS_MS[0]);

    for (size_t i = 0; i < attempt_count; ++i) {
        json_t *params;
        json_t *result;
        struct solo_rpc_error error;

        sleep_ms(SOLO_CONFIRM_RETRY_DELAYS_MS[i]);

        params = json_array();
        if (params == NULL)
            continue;
        json_array_append_new(params, json_string(block_hash_hex));
        json_array_append_new(params, json_false());
        result = solo_rpc_call_ex(rpc, "getblockheader", params, 5.0, false, &error);
        json_decref(params);

        if (result != NULL) {
            json_decref(result);
            return 0;
        }

        if (error.rpc_error) {
            if (error.rpc_code == -5 || strstr(error.rpc_message, "Block not found") != NULL)
                continue;
            log_rpc_error("block confirmation failed", block_hash_hex, &error);
            return -1;
        }
    }

    log_warn("block confirmation timed out; assuming submitted block=%s", block_hash_hex ? block_hash_hex : "-");
    return 1;
}

static void submit_block_reliable(struct solo_rpc_client *rpc, struct submit_request *req)
{
    const char *result_str = "submit_failed";
    size_t attempt_count = sizeof(SOLO_SUBMIT_RETRY_DELAYS_MS) / sizeof(SOLO_SUBMIT_RETRY_DELAYS_MS[0]);

    if (rpc == NULL || req == NULL)
        goto out;

    for (size_t i = 0; i < attempt_count; ++i) {
        json_t *params;
        json_t *result;
        struct solo_rpc_error error;

        sleep_ms(SOLO_SUBMIT_RETRY_DELAYS_MS[i]);

        params = json_array();
        if (params == NULL)
            continue;
        json_array_append_new(params, json_string(req->block_hex));
        result = solo_rpc_call_ex(rpc, "submitblock", params, 20.0, true, &error);
        json_decref(params);

        if (result == NULL) {
            log_rpc_error("submitblock attempt failed", req->block_hash, &error);
            continue;
        }

        if (json_is_null(result)) {
            int confirm_rc;
            json_decref(result);
            log_vip("submitblock accepted by node: %s", req->block_hash);
            confirm_rc = confirm_block_in_node(rpc, req->block_hash);
            if (confirm_rc < 0)
                result_str = "confirm_failed";
            else
                result_str = "submitted";
            goto out;
        }

        if (json_is_string(result)) {
            const char *reply = json_string_value(result);
            if (reply != NULL && strcmp(reply, "duplicate") == 0) {
                log_vip("submitblock duplicate treated as success: %s", req->block_hash);
                result_str = "duplicate";
            } else {
                log_error(
                    "submitblock rejected block=%s reason=%s",
                    req->block_hash ? req->block_hash : "-",
                    reply ? reply : "-"
                );
                result_str = "rejected";
            }
            json_decref(result);
            goto out;
        }

        {
            char *dump = json_dumps(result, JSON_COMPACT);
            log_error("submitblock unexpected reply block=%s payload=%s", req->block_hash, dump ? dump : "");
            free(dump);
        }
        json_decref(result);
        result_str = "rejected";
        goto out;
    }

    log_error("submitblock exhausted retries block=%s", req && req->block_hash ? req->block_hash : "-");

out:
    if (req != NULL)
        record_found_block(req->job_height, req->block_hash, result_str);
    solo_job_request_refresh(SOLO_REFRESH_POST_SUBMIT);
}

static void *submit_main(void *unused)
{
    struct solo_rpc_client rpc;
    (void)unused;

    if (solo_rpc_client_init(&rpc, settings.bitcoind.rpc_url, settings.bitcoind.rpc_user, settings.bitcoind.rpc_pass, settings.bitcoind.rpc_cookie_file) < 0) {
        log_error("init bitcoind submit rpc client fail");
        return NULL;
    }

    while (1) {
        struct submit_request *submit = NULL;

        pthread_mutex_lock(&submit_lock);
        while (submit_running && submit_head == NULL)
            pthread_cond_wait(&submit_cond, &submit_lock);
        if (!submit_running) {
            pthread_mutex_unlock(&submit_lock);
            break;
        }
        submit = pop_submit_request();
        pthread_mutex_unlock(&submit_lock);

        if (submit) {
            submit_block_reliable(&rpc, submit);
            submit_request_free(submit);
        }
    }

    solo_rpc_client_free(&rpc);
    return NULL;
}

static void *refresh_main(void *unused)
{
    struct solo_rpc_client rpc;
    (void)unused;

    if (solo_rpc_client_init(&rpc, settings.bitcoind.rpc_url, settings.bitcoind.rpc_user, settings.bitcoind.rpc_pass, settings.bitcoind.rpc_cookie_file) < 0) {
        log_error("init bitcoind refresh rpc client fail");
        return NULL;
    }

    pthread_mutex_lock(&refresh_lock);
    while (refresh_running) {
        enum solo_refresh_source source;
        json_t *result;

        while (refresh_running && !refresh_pending)
            pthread_cond_wait(&refresh_cond, &refresh_lock);
        if (!refresh_running)
            break;

        source = refresh_pending_source;
        refresh_pending = false;
        refresh_inflight = true;
        pthread_mutex_unlock(&refresh_lock);

        result = fetch_gbt_direct(&rpc);
        if (result != NULL) {
            if (apply_gbt_result(result, source) == 0)
                (void)refresh_network_hashrate(&rpc);
            json_decref(result);
        } else {
            update_refresh_status(false);
            log_error("direct refresh failed source=%s", refresh_source_name(source));
        }

        pthread_mutex_lock(&refresh_lock);
        refresh_inflight = false;
        if (refresh_pending)
            pthread_cond_signal(&refresh_cond);
    }
    pthread_mutex_unlock(&refresh_lock);

    solo_rpc_client_free(&rpc);
    return NULL;
}

static void *longpoll_main(void *unused)
{
    struct solo_rpc_client rpc;
    (void)unused;

    if (solo_rpc_client_init(&rpc, settings.bitcoind.rpc_url, settings.bitcoind.rpc_user, settings.bitcoind.rpc_pass, settings.bitcoind.rpc_cookie_file) < 0) {
        log_error("init bitcoind longpoll rpc client fail");
        return NULL;
    }
    rpc.cancel_flag = &longpoll_running;

    while (__atomic_load_n(&longpoll_running, __ATOMIC_ACQUIRE)) {
        sds longpollid = copy_last_longpollid();
        json_t *result = fetch_gbt_longpoll(&rpc, longpollid);

        sdsfree(longpollid);
        if (!__atomic_load_n(&longpoll_running, __ATOMIC_ACQUIRE)) {
            if (result)
                json_decref(result);
            break;
        }

        if (result != NULL) {
            if (apply_gbt_result(result, SOLO_REFRESH_LONGPOLL_SUCCESS) == 0)
                (void)refresh_network_hashrate(&rpc);
            json_decref(result);
            continue;
        }

        if (longpoll_last_error.timed_out) {
            log_info("getblocktemplate longpoll timed out; scheduling direct refresh");
            solo_job_request_refresh(SOLO_REFRESH_LONGPOLL_TIMEOUT);
        } else {
            log_rpc_error("getblocktemplate longpoll failed", "longpoll", &longpoll_last_error);
            solo_job_request_refresh(SOLO_REFRESH_LONGPOLL_ERROR);
        }
    }

    solo_rpc_client_free(&rpc);
    return NULL;
}

static void stop_submit_thread(void)
{
    pthread_mutex_lock(&submit_lock);
    submit_running = false;
    pthread_cond_broadcast(&submit_cond);
    pthread_mutex_unlock(&submit_lock);

    if (submit_started) {
        pthread_join(submit_thread_id, NULL);
        submit_started = false;
    }
}

static void stop_refresh_thread(void)
{
    pthread_mutex_lock(&refresh_lock);
    refresh_running = false;
    pthread_cond_broadcast(&refresh_cond);
    pthread_mutex_unlock(&refresh_lock);

    if (refresh_started) {
        pthread_join(refresh_thread_id, NULL);
        refresh_started = false;
    }
}

static void stop_longpoll_thread(void)
{
    __atomic_store_n(&longpoll_running, false, __ATOMIC_RELEASE);
    if (longpoll_started) {
        pthread_join(longpoll_thread_id, NULL);
        longpoll_started = false;
    }
}

static void reset_job_runtime_state(void)
{
    clear_submit_queue();

    pthread_rwlock_wrlock(&job_lock);
    clear_job_cache();
    memset(&chain_tip, 0, sizeof(chain_tip));
    pthread_rwlock_unlock(&job_lock);

    free(job_cache);
    job_cache = NULL;
    job_cache_count = 0;
    job_cache_limit = 0;
    current_job = NULL;

    free(share_cache.entries);
    memset(&share_cache, 0, sizeof(share_cache));

    pthread_mutex_lock(&status_lock);
    last_refresh_ok = false;
    last_refresh_ms = 0;
    last_network_hashrate_hps = 0.0;
    memset(found_blocks, 0, sizeof(found_blocks));
    found_blocks_count = 0;
    found_blocks_pos = 0;
    blocks_submitted_total = 0;
    blocks_accepted_total = 0;
    pthread_mutex_unlock(&status_lock);

    pthread_mutex_lock(&longpoll_lock);
    sdsfree(last_longpollid);
    last_longpollid = NULL;
    memset(&longpoll_last_error, 0, sizeof(longpoll_last_error));
    pthread_mutex_unlock(&longpoll_lock);

    submit_head = NULL;
    submit_tail = NULL;
    submit_running = false;
    refresh_running = false;
    __atomic_store_n(&longpoll_running, false, __ATOMIC_RELEASE);
    submit_started = false;
    refresh_started = false;
    longpoll_started = false;
    refresh_inflight = false;
    refresh_pending = false;
    refresh_pending_source = SOLO_REFRESH_MANUAL;
    update_cb = NULL;
}

int solo_job_init(void)
{
    struct solo_rpc_client startup_rpc = {0};
    json_t *startup_result = NULL;

    if (init_job_cache() < 0)
        return -1;
    if (share_set_init(&share_cache) < 0)
        goto fail;
    if (init_diff1_target() < 0)
        goto fail;

    if (urandom(&next_job_id, sizeof(next_job_id)) < 0)
        next_job_id = (uint32_t)current_time_millis();

    if (solo_rpc_client_init(&startup_rpc, settings.bitcoind.rpc_url, settings.bitcoind.rpc_user, settings.bitcoind.rpc_pass, settings.bitcoind.rpc_cookie_file) < 0)
        goto fail;

    startup_result = fetch_gbt_direct(&startup_rpc);
    if (startup_result == NULL)
        goto fail;
    if (apply_gbt_result(startup_result, SOLO_REFRESH_STARTUP) < 0)
        goto fail;
    (void)refresh_network_hashrate(&startup_rpc);
    json_decref(startup_result);
    startup_result = NULL;
    solo_rpc_client_free(&startup_rpc);

    submit_running = true;
    if (pthread_create(&submit_thread_id, NULL, submit_main, NULL) != 0)
        goto fail;
    submit_started = true;

    refresh_running = true;
    if (pthread_create(&refresh_thread_id, NULL, refresh_main, NULL) != 0)
        goto fail;
    refresh_started = true;

    __atomic_store_n(&longpoll_running, true, __ATOMIC_RELEASE);
    if (pthread_create(&longpoll_thread_id, NULL, longpoll_main, NULL) != 0)
        goto fail;
    longpoll_started = true;

    if (solo_job_fastpath_init() < 0)
        goto fail;

    return 0;

fail:
    if (startup_result)
        json_decref(startup_result);
    solo_rpc_client_free(&startup_rpc);
    solo_job_fastpath_shutdown();
    stop_longpoll_thread();
    stop_refresh_thread();
    stop_submit_thread();
    reset_job_runtime_state();
    return -1;
}

void solo_job_get_found_blocks(struct solo_found_block *out, size_t max, size_t *count)
{
    size_t n;
    if (out == NULL || count == NULL || max == 0) {
        if (count) *count = 0;
        return;
    }
    pthread_mutex_lock(&status_lock);
    n = found_blocks_count < max ? found_blocks_count : max;
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (found_blocks_pos - 1 - i) % SOLO_FOUND_BLOCKS_MAX;
        out[i] = found_blocks[idx];
    }
    *count = n;
    pthread_mutex_unlock(&status_lock);
}

uint64_t solo_job_total_submitted(void)
{
    uint64_t val;
    pthread_mutex_lock(&status_lock);
    val = blocks_submitted_total;
    pthread_mutex_unlock(&status_lock);
    return val;
}

uint64_t solo_job_total_accepted(void)
{
    uint64_t val;
    pthread_mutex_lock(&status_lock);
    val = blocks_accepted_total;
    pthread_mutex_unlock(&status_lock);
    return val;
}

void solo_job_shutdown(void)
{
    solo_job_fastpath_shutdown();
    stop_longpoll_thread();
    stop_refresh_thread();
    stop_submit_thread();
    reset_job_runtime_state();
}
