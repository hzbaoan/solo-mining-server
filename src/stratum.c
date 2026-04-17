#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <ev.h>
#include <fcntl.h>
#include <inttypes.h>
#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "stratum.h"
#include "config.h"
#include "job.h"
#include "solo_log.h"
#include "solo_signal.h"
#include "solo_utils.h"

#define MAX_WORKER_NAME_LEN    64
#define MAX_USER_AGENT_LEN     64
#define VERSION_MASK_DEFAULT   536862720u
#define VARDIFF_RETARGET_SHARE 30
#define VARDIFF_BUF_SIZE       (VARDIFF_RETARGET_SHARE * 3)
#define HASHRATE_WINDOW_MS     180000ULL
#define MAX_COINBASE_BIN       2048

typedef struct vardiff_buf {
    int     use;
    int     pos;
    double  data[VARDIFF_BUF_SIZE];
} vardiff_buf;

struct client_info {
    double      connect_time;
    double      last_active_time;
    double      last_share_time;
    double      last_retarget_time;
    bool        subscribed;
    bool        authorized;
    bool        user_suggest_diff;
    char        worker[MAX_WORKER_NAME_LEN + 1];
    char        user_agent[MAX_USER_AGENT_LEN + 1];
    char        subscription_id[17];
    char        extra_nonce1[(SOLO_EXTRA_NONCE1_SIZE * 2) + 1];
    unsigned char extra_nonce1_bin[SOLO_EXTRA_NONCE1_SIZE];
    char        coinbase_job_id[9];
    sds         coinbase1_bin;
    sds         coinbase1_hex;
    sds         coinbase_stub_bin;
    int         difficulty;
    int         difficulty_last;
    vardiff_buf vardiff;
    uint64_t    last_retarget_share;
    uint64_t    share_valid;
    uint64_t    share_error;
    uint32_t    version_mask;
    uint32_t    version_mask_miner;
    double      best_share_diff;
    double      accepted_diff_window;
    uint64_t    accepted_window_start_ms;
    uint64_t    accepted_window_last_ms;
};

struct stratum_client {
    int                     fd;
    ev_io                   read_watcher;
    ev_io                   write_watcher;
    sds                     read_buf;
    sds                     write_buf;
    struct sockaddr_storage peer_addr;
    socklen_t               peer_addr_len;
    struct client_info      info;
    struct stratum_client   *prev;
    struct stratum_client   *next;
};

static struct ev_loop *stratum_loop;
static int listen_fd = -1;
static ev_io accept_watcher;
static ev_timer idle_timer;
static ev_timer signal_timer;
static ev_async job_async;
static pthread_rwlock_t clients_lock = PTHREAD_RWLOCK_INITIALIZER;
static struct stratum_client *clients_head;
static uint64_t subscription_counter;
static uint64_t extra_nonce1_counter;
static bool pending_clean_jobs;
static pthread_mutex_t shares_log_lock = PTHREAD_MUTEX_INITIALIZER;
static struct solo_recent_share recent_shares[SOLO_RECENT_SHARES_MAX];
static struct solo_recent_share best_share;
static bool best_share_ready;
static size_t recent_shares_count;
static size_t recent_shares_pos;
static uint64_t global_shares_valid;
static uint64_t global_shares_error;

static bool is_hex_string(const char *value, size_t expected_len)
{
    if (value == NULL || strlen(value) != expected_len)
        return false;

    for (size_t i = 0; i < expected_len; ++i) {
        if (!isxdigit((unsigned char)value[i]))
            return false;
    }

    return true;
}

static int parse_hex_u32_exact(const char *value, uint32_t *out)
{
    unsigned long parsed;
    char *end = NULL;

    if (out == NULL || !is_hex_string(value, 8))
        return -1;

    errno = 0;
    parsed = strtoul(value, &end, 16);
    if (errno != 0 || end == NULL || *end != '\0' || parsed > UINT32_MAX)
        return -1;

    *out = (uint32_t)parsed;
    return 0;
}

static int set_nonblocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int clamp_difficulty(int difficulty)
{
    if (difficulty < settings.diff_min)
        return settings.diff_min;
    if (difficulty > settings.diff_max)
        return settings.diff_max;
    return difficulty;
}

static void vardiff_append(vardiff_buf *vardiff, double time)
{
    vardiff->data[vardiff->pos++] = time;
    vardiff->pos %= VARDIFF_BUF_SIZE;
    if (vardiff->use < VARDIFF_BUF_SIZE)
        vardiff->use++;
}

static void vardiff_reset(vardiff_buf *vardiff)
{
    vardiff->use = 0;
    vardiff->pos = 0;
}

static double vardiff_avg(vardiff_buf *vardiff)
{
    int count = vardiff->use < VARDIFF_BUF_SIZE ? vardiff->use : VARDIFF_BUF_SIZE;
    double total = 0.0;
    if (count == 0)
        return 0.0;
    for (int i = 0; i < count; ++i)
        total += vardiff->data[i];
    return total / count;
}

static uint64_t now_ms(void)
{
    return current_time_millis();
}

static void set_pending_clean_jobs(bool clean_jobs)
{
    __atomic_store_n(&pending_clean_jobs, clean_jobs, __ATOMIC_RELEASE);
}

static bool get_pending_clean_jobs(void)
{
    return __atomic_load_n(&pending_clean_jobs, __ATOMIC_ACQUIRE);
}

static void clear_client_coinbase_cache(struct client_info *info)
{
    if (info == NULL)
        return;
    sdsfree(info->coinbase1_bin);
    sdsfree(info->coinbase1_hex);
    sdsfree(info->coinbase_stub_bin);
    info->coinbase1_bin = NULL;
    info->coinbase1_hex = NULL;
    info->coinbase_stub_bin = NULL;
    info->coinbase_job_id[0] = '\0';
}

static int set_client_extra_nonce1(struct client_info *info, uint32_t value)
{
    if (info == NULL)
        return -1;

    info->extra_nonce1_bin[0] = (unsigned char)(value >> 24);
    info->extra_nonce1_bin[1] = (unsigned char)(value >> 16);
    info->extra_nonce1_bin[2] = (unsigned char)(value >> 8);
    info->extra_nonce1_bin[3] = (unsigned char)value;
    return bin2hex_into(info->extra_nonce1_bin, sizeof(info->extra_nonce1_bin), info->extra_nonce1, sizeof(info->extra_nonce1));
}

static int ensure_client_coinbase_cache(struct client_info *info, const struct solo_job *job)
{
    if (info == NULL || job == NULL)
        return -1;

    if (strcmp(info->coinbase_job_id, job->job_id) == 0 &&
        info->coinbase1_bin != NULL &&
        info->coinbase1_hex != NULL &&
        info->coinbase_stub_bin != NULL) {
        return 0;
    }

    clear_client_coinbase_cache(info);

    info->coinbase1_bin = solo_job_build_coinbase1(job, info->worker);
    if (info->coinbase1_bin == NULL)
        goto fail;
    info->coinbase1_hex = bin2hex(info->coinbase1_bin, sdslen(info->coinbase1_bin));
    if (info->coinbase1_hex == NULL)
        goto fail;
    info->coinbase_stub_bin = sdsdup(info->coinbase1_bin);
    if (info->coinbase_stub_bin == NULL)
        goto fail;
    if (sds_append_len_owned(&info->coinbase_stub_bin, info->extra_nonce1_bin, sizeof(info->extra_nonce1_bin)) < 0)
        goto fail;

    snprintf(info->coinbase_job_id, sizeof(info->coinbase_job_id), "%s", job->job_id);
    return 0;

fail:
    clear_client_coinbase_cache(info);
    return -1;
}

static void record_share_stats(struct client_info *info, int accepted_diff, double actual_diff)
{
    uint64_t tsms = now_ms();
    if (info->accepted_window_start_ms == 0 || (tsms - info->accepted_window_start_ms) > HASHRATE_WINDOW_MS) {
        info->accepted_window_start_ms = tsms;
        info->accepted_diff_window = 0.0;
    }
    info->accepted_window_last_ms = tsms;
    info->accepted_diff_window += accepted_diff;
    if (actual_diff > info->best_share_diff)
        info->best_share_diff = actual_diff;
}

static double client_hashrate_ths(const struct client_info *info, uint64_t tsms)
{
    double elapsed;
    if (info->accepted_window_start_ms == 0 || info->accepted_window_last_ms == 0)
        return 0.0;
    if ((tsms - info->accepted_window_last_ms) > HASHRATE_WINDOW_MS)
        return 0.0;
    elapsed = (double)(tsms - info->accepted_window_start_ms) / 1000.0;
    if (elapsed <= 0.0)
        return 0.0;
    return (info->accepted_diff_window / elapsed) * 0.004294967296;
}

static void client_link_locked(struct stratum_client *client)
{
    client->next = clients_head;
    client->prev = NULL;
    if (clients_head)
        clients_head->prev = client;
    clients_head = client;
}

static void client_unlink_locked(struct stratum_client *client)
{
    if (client->prev)
        client->prev->next = client->next;
    else
        clients_head = client->next;
    if (client->next)
        client->next->prev = client->prev;
    client->prev = NULL;
    client->next = NULL;
}

static void client_free_locked(struct stratum_client *client)
{
    ev_io_stop(stratum_loop, &client->read_watcher);
    ev_io_stop(stratum_loop, &client->write_watcher);
    close(client->fd);
    sdsfree(client->read_buf);
    sdsfree(client->write_buf);
    clear_client_coinbase_cache(&client->info);
    client_unlink_locked(client);
    free(client);
}

static void client_close_locked(struct stratum_client *client, const char *reason)
{
    char addr[128] = {0};
    if (client == NULL)
        return;
    format_sockaddr((struct sockaddr *)&client->peer_addr, client->peer_addr_len, addr, sizeof(addr));
    log_info("connection close: fd=%d %s %s", client->fd, addr[0] ? addr : "-", reason ? reason : "");
    client_free_locked(client);
}

static ssize_t client_write_direct(int fd, const char *buf, size_t len)
{
#ifdef MSG_NOSIGNAL
    return send(fd, buf, len, MSG_NOSIGNAL);
#else
    return send(fd, buf, len, 0);
#endif
}


static int client_queue_send_locked(struct stratum_client *client, const char *buf, size_t len)
{
    ssize_t written;

    if (client == NULL || buf == NULL || len == 0)
        return 0;

    if (client->write_buf == NULL || sdslen(client->write_buf) == 0) {
        written = client_write_direct(client->fd, buf, len);
        if (written < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                written = 0;
            else
                return -1;
        }
        if ((size_t)written == len)
            return 0;
        buf += written;
        len -= (size_t)written;
    }

    if (sds_append_len_owned(&client->write_buf, buf, len) < 0)
        return -1;
    ev_io_start(stratum_loop, &client->write_watcher);
    return 0;
}

static int format_json_id(json_t *id, char *buf, size_t buf_size)
{
    int n;
    if (id == NULL || json_is_null(id)) {
        n = snprintf(buf, buf_size, "null");
    } else if (json_is_integer(id)) {
        n = snprintf(buf, buf_size, "%" JSON_INTEGER_FORMAT, json_integer_value(id));
    } else if (json_is_string(id)) {
        const char *s = json_string_value(id);
        size_t pos = 0;
        if (pos + 1 >= buf_size)
            return -1;
        buf[pos++] = '"';
        for (; *s; ++s) {
            if (*s == '"' || *s == '\\') {
                if (pos + 2 >= buf_size)
                    return -1;
                buf[pos++] = '\\';
            } else if (pos + 1 >= buf_size) {
                return -1;
            }
            buf[pos++] = *s;
        }
        if (pos + 2 > buf_size)
            return -1;
        buf[pos++] = '"';
        buf[pos] = '\0';
        return (int)pos;
    } else {
        char *dumped = json_dumps(id, JSON_ENCODE_ANY | JSON_COMPACT);
        if (dumped == NULL)
            return -1;
        n = snprintf(buf, buf_size, "%s", dumped);
        free(dumped);
    }
    return (n < 0 || (size_t)n >= buf_size) ? -1 : n;
}

static int send_notify_locked(struct stratum_client *client, const struct solo_job *job, const char *coinbase1_hex, bool clean_job)
{
    sds notify_suffix = clean_job ? job->notify_suffix_clean_json : job->notify_suffix_dirty_json;

    if (client == NULL || job == NULL || coinbase1_hex == NULL || job->notify_prefix_json == NULL || notify_suffix == NULL)
        return -1;
    if (client_queue_send_locked(client, job->notify_prefix_json, sdslen(job->notify_prefix_json)) < 0 ||
        client_queue_send_locked(client, coinbase1_hex, strlen(coinbase1_hex)) < 0 ||
        client_queue_send_locked(client, notify_suffix, sdslen(notify_suffix)) < 0) {
        return -1;
    }
    return client_queue_send_locked(client, "\n", 1);
}

static int send_error_locked(struct stratum_client *client, json_t *id, int error_code, const char *error_msg)
{
    char buf[512];
    char id_str[64];
    int n;

    if (format_json_id(id, id_str, sizeof(id_str)) < 0)
        return -1;
    n = snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"error\":[%d,\"%s\",null],\"result\":null}\n",
        id_str, error_code, error_msg);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;
    return client_queue_send_locked(client, buf, (size_t)n);
}

static int send_ok_locked(struct stratum_client *client, json_t *id)
{
    char buf[128];
    char id_str[64];
    int n;

    if (format_json_id(id, id_str, sizeof(id_str)) < 0)
        return -1;
    n = snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"result\":true,\"error\":null}\n",
        id_str);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;
    return client_queue_send_locked(client, buf, (size_t)n);
}

static int reject_submit_locked(struct stratum_client *client, json_t *id, const char *reason)
{
    if (client) {
        client->info.share_error++;
        pthread_mutex_lock(&shares_log_lock);
        global_shares_error++;
        pthread_mutex_unlock(&shares_log_lock);
    }
    return send_error_locked(client, id, 20, reason);
}

static int set_difficulty_locked(struct stratum_client *client, int difficulty)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "{\"id\":null,\"method\":\"mining.set_difficulty\",\"params\":[%d]}\n",
        difficulty);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;
    return client_queue_send_locked(client, buf, (size_t)n);
}

static int send_job_locked(struct stratum_client *client, const struct solo_job *job, bool clean_job)
{
    if (ensure_client_coinbase_cache(&client->info, job) < 0)
        return -1;
    return send_notify_locked(client, job, client->info.coinbase1_hex, clean_job);
}

static int send_current_job_locked(struct stratum_client *client)
{
    struct solo_job *job = solo_job_current();
    int ret = 0;
    if (job) {
        ret = send_job_locked(client, job, true);
        solo_job_release(job);
    }
    return ret;
}

static int retarget_on_new_share(struct client_info *info)
{
    double now = current_timestamp();
    double interval = now - info->last_share_time;
    int new_diff = info->difficulty;

    info->last_share_time = now;
    vardiff_append(&info->vardiff, interval);
    if (!((info->share_valid - info->last_retarget_share) >= VARDIFF_RETARGET_SHARE ||
          (now - info->last_retarget_time) >= settings.retarget_time)) {
        return 0;
    }

    info->last_retarget_time = now;
    info->last_retarget_share = info->share_valid;

    {
        double avg = vardiff_avg(&info->vardiff);
        if (avg <= 0)
            return 0;
        if (avg > settings.target_time) {
            if (avg / settings.target_time > 1.5)
                new_diff = info->difficulty / 2;
        } else {
            if (avg / settings.target_time < 0.7)
                new_diff = info->difficulty * 2;
        }
    }

    new_diff = clamp_difficulty(new_diff);
    if (new_diff != info->difficulty) {
        info->difficulty_last = info->difficulty;
        info->difficulty = new_diff;
        vardiff_reset(&info->vardiff);
        return 1;
    }
    return 0;
}

static int extract_worker_label(const char *account, char *worker, size_t size)
{
    char *copy = strdup(account);
    char *label;
    char *next;
    size_t pos = 0;

    if (copy == NULL)
        return -1;
    strclearblank(copy);
    if (strlen(copy) == 0) {
        free(copy);
        return -1;
    }

    label = strtok(copy, ".:-_");
    next = strtok(NULL, ".:-_");
    if (next)
        label = next;
    if (label == NULL || label[0] == '\0') {
        free(copy);
        return -1;
    }

    for (size_t i = 0; label[i] != '\0' && pos + 1 < size; ++i) {
        unsigned char c = (unsigned char)label[i];
        if (isalnum(c) || c == '-' || c == '_')
            worker[pos++] = (char)c;
        else if (!isspace(c))
            worker[pos++] = '_';
    }
    worker[pos] = '\0';
    free(copy);
    return pos == 0 ? -1 : 0;
}

static int handle_configure_locked(struct stratum_client *client, json_t *id, json_t *params)
{
    bool version_rolling = false;
    char mask_str[9] = {0};
    char id_str[64];
    char buf[256];
    int n;

    if (json_array_size(params) >= 1) {
        json_t *extensions = json_array_get(params, 0);
        if (extensions && json_is_array(extensions)) {
            for (size_t i = 0; i < json_array_size(extensions); ++i) {
                json_t *item = json_array_get(extensions, i);
                if (!json_is_string(item))
                    continue;
                if (strcmp(json_string_value(item), "version-rolling") == 0) {
                    uint32_t requested_mask = 0;
                    json_t *options = json_array_get(params, 1);
                    if (options && json_is_object(options)) {
                        json_t *mask_obj = json_object_get(options, "version-rolling.mask");
                        if (mask_obj && json_is_string(mask_obj))
                            (void)parse_hex_u32_exact(json_string_value(mask_obj), &requested_mask);
                    }
                    client->info.version_mask_miner = requested_mask;
                    client->info.version_mask = requested_mask & VERSION_MASK_DEFAULT;
                    snprintf(mask_str, sizeof(mask_str), "%08x", client->info.version_mask);
                    version_rolling = true;
                    break;
                }
            }
        }
    }

    if (format_json_id(id, id_str, sizeof(id_str)) < 0)
        return -1;

    if (version_rolling) {
        n = snprintf(buf, sizeof(buf),
            "{\"id\":%s,\"result\":{\"version-rolling\":true,\"version-rolling.mask\":\"%s\"},\"error\":null}\n",
            id_str, mask_str);
    } else {
        n = snprintf(buf, sizeof(buf),
            "{\"id\":%s,\"result\":{},\"error\":null}\n",
            id_str);
    }
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;
    if (client_queue_send_locked(client, buf, (size_t)n) < 0)
        return -1;

    if (version_rolling) {
        n = snprintf(buf, sizeof(buf),
            "{\"id\":null,\"method\":\"mining.set_version_mask\",\"params\":[\"%s\"]}\n",
            mask_str);
        if (n < 0 || (size_t)n >= sizeof(buf))
            return -1;
        return client_queue_send_locked(client, buf, (size_t)n);
    }

    return 0;
}

static int handle_subscribe_locked(struct stratum_client *client, json_t *id, json_t *params)
{
    char buf[512];
    char id_str[64];
    int n;

    if (json_array_size(params) > 0) {
        json_t *user_agent = json_array_get(params, 0);
        if (json_is_string(user_agent))
            snprintf(client->info.user_agent, sizeof(client->info.user_agent), "%s", json_string_value(user_agent));
    }

    if (format_json_id(id, id_str, sizeof(id_str)) < 0)
        return -1;

    n = snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"result\":["
        "[[\"mining.set_difficulty\",\"%s\"],[\"mining.notify\",\"%s\"]],"
        "\"%s\",%d"
        "],\"error\":null}\n",
        id_str,
        client->info.subscription_id,
        client->info.subscription_id,
        client->info.extra_nonce1,
        settings.extra_nonce2_size);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;
    if (client_queue_send_locked(client, buf, (size_t)n) < 0)
        return -1;

    client->info.subscribed = true;
    return 0;
}

static int handle_authorize_locked(struct stratum_client *client, json_t *id, json_t *params)
{
    int previous_diff = client->info.difficulty;
    const char *pass;
    json_t *account;
    json_t *password;
    char worker[MAX_WORKER_NAME_LEN + 1];

    if (!client->info.subscribed)
        return send_error_locked(client, id, 25, "Not subscribed");
    if (json_array_size(params) < 2)
        return -1;

    account = json_array_get(params, 0);
    password = json_array_get(params, 1);
    if (!json_is_string(account) || !json_is_string(password))
        return -1;
    if (extract_worker_label(json_string_value(account), worker, sizeof(worker)) < 0)
        return send_error_locked(client, id, 24, "Invalid worker name");
    if (strcmp(worker, client->info.worker) != 0) {
        snprintf(client->info.worker, sizeof(client->info.worker), "%s", worker);
        clear_client_coinbase_cache(&client->info);
    }

    if (send_ok_locked(client, id) < 0)
        return -1;

    pass = json_string_value(password);
    if (pass != NULL && strlen(pass) >= 3 && (pass[0] == 'd' || pass[0] == 'D') && pass[1] == '=' && isdigit((unsigned char)pass[2])) {
        int difficulty = clamp_difficulty(atoi(pass + 2));
        client->info.user_suggest_diff = true;
        client->info.difficulty = difficulty;
        client->info.difficulty_last = difficulty;
    }

    {
        bool first_auth = !client->info.authorized;
        client->info.authorized = true;
        if ((first_auth || client->info.difficulty != previous_diff) &&
            set_difficulty_locked(client, client->info.difficulty) < 0)
            return -1;
        if (send_current_job_locked(client) < 0)
            return -1;
    }

    return 0;
}

static int get_block_head(char *head, void *coinbase_buf, size_t coinbase_buf_size, size_t *coinbase_len,
    const struct solo_job *job, struct client_info *info,
    const char *extra_nonce2, uint32_t intime, uint32_t inonce, uint32_t version_mask)
{
    unsigned char extra_nonce2_bin[16];
    unsigned char merkle_root[32];
    uint32_t version;
    void *p = head;
    size_t left = 80;
    size_t stub_len;
    size_t en2_len;
    size_t cb2_len;

    if (!is_hex_string(extra_nonce2, (size_t)settings.extra_nonce2_size * 2))
        return -1;
    en2_len = (size_t)settings.extra_nonce2_size;
    if (hex2bin_exact(extra_nonce2, extra_nonce2_bin, en2_len) < 0)
        return -1;
    if (ensure_client_coinbase_cache(info, job) < 0)
        return -1;

    stub_len = sdslen(info->coinbase_stub_bin);
    cb2_len = sdslen(job->coinbase2_bin);
    if (stub_len + en2_len + cb2_len > coinbase_buf_size)
        return -1;

    memcpy(coinbase_buf, info->coinbase_stub_bin, stub_len);
    memcpy((unsigned char *)coinbase_buf + stub_len, extra_nonce2_bin, en2_len);
    memcpy((unsigned char *)coinbase_buf + stub_len + en2_len, job->coinbase2_bin, cb2_len);
    *coinbase_len = stub_len + en2_len + cb2_len;

    sha256d(coinbase_buf, *coinbase_len, merkle_root);
    get_merkle_root((char *)merkle_root, job->merkle_branch, job->merkle_branch_count);

    version = (job->version & ~(info->version_mask)) | (version_mask & info->version_mask);

    if (pack_uint32_le(&p, &left, version) < 0 ||
        pack_buf(&p, &left, job->prevhash_le, sizeof(job->prevhash_le)) < 0 ||
        pack_buf(&p, &left, merkle_root, sizeof(merkle_root)) < 0 ||
        pack_uint32_le(&p, &left, intime) < 0 ||
        pack_uint32_le(&p, &left, job->nbits) < 0 ||
        pack_uint32_le(&p, &left, inonce) < 0) {
        return -1;
    }

    return 0;
}

static int handle_submit_locked(struct stratum_client *client, json_t *id, json_t *params)
{
    json_t *account;
    json_t *job_id;
    json_t *extra_nonce2;
    json_t *ntime;
    json_t *nonce;
    uint32_t version_mask = 0;
    uint32_t submit_time;
    uint32_t submit_nonce;
    struct solo_job *job;
    unsigned char coinbase_buf[MAX_COINBASE_BIN];
    size_t coinbase_len = 0;
    char block_head[80];
    char block_hash[32];
    int accepted_diff;
    double actual_diff;
    char submit_worker[MAX_WORKER_NAME_LEN + 1];

    if (!client->info.subscribed)
        return send_error_locked(client, id, 25, "Not subscribed");
    if (!client->info.authorized)
        return send_error_locked(client, id, 24, "Unauthorized worker");
    if (json_array_size(params) < 5)
        return -1;

    account = json_array_get(params, 0);
    job_id = json_array_get(params, 1);
    extra_nonce2 = json_array_get(params, 2);
    ntime = json_array_get(params, 3);
    nonce = json_array_get(params, 4);
    if (!json_is_string(account) || !json_is_string(job_id) || !json_is_string(extra_nonce2) ||
        !json_is_string(ntime) || !json_is_string(nonce)) {
        return -1;
    }

    if (extract_worker_label(json_string_value(account), submit_worker, sizeof(submit_worker)) == 0 &&
        strcmp(submit_worker, client->info.worker) != 0) {
        log_info("connection worker changed from %s to %s", client->info.worker, submit_worker);
        snprintf(client->info.worker, sizeof(client->info.worker), "%s", submit_worker);
        clear_client_coinbase_cache(&client->info);
    }

    if (!is_hex_string(json_string_value(extra_nonce2), (size_t)settings.extra_nonce2_size * 2) ||
        parse_hex_u32_exact(json_string_value(ntime), &submit_time) < 0 ||
        parse_hex_u32_exact(json_string_value(nonce), &submit_nonce) < 0) {
        return reject_submit_locked(client, id, "Invalid submit params");
    }

    if (json_array_size(params) >= 6) {
        json_t *mask_obj = json_array_get(params, 5);
        if (!json_is_string(mask_obj))
            return -1;
        if (parse_hex_u32_exact(json_string_value(mask_obj), &version_mask) < 0)
            return reject_submit_locked(client, id, "Invalid version mask");
    }
    if (version_mask != 0 && ((~client->info.version_mask_miner) & version_mask) != 0)
        return reject_submit_locked(client, id, "Unauthorized version mask");

    job = solo_job_find(json_string_value(job_id));
    if (job == NULL)
        return send_error_locked(client, id, 21, "Job not found");

    if (submit_time < job->mintime) {
        solo_job_release(job);
        return reject_submit_locked(client, id, "ntime below mintime");
    }

    if (get_block_head(block_head, coinbase_buf, sizeof(coinbase_buf), &coinbase_len,
        job, &client->info,
        json_string_value(extra_nonce2), submit_time, submit_nonce, version_mask) < 0) {
        log_error("build block head failed worker=%s job=%s", client->info.worker, job->job_id);
        solo_job_release(job);
        return -1;
    }

    sha256d(block_head, sizeof(block_head), block_hash);
    reverse_mem(block_hash, sizeof(block_hash));

    if (!solo_job_register_share(block_hash)) {
        solo_job_release(job);
        client->info.share_error++;
        return send_error_locked(client, id, 22, "Duplicate share");
    }

    accepted_diff = client->info.difficulty;
    if (!solo_job_share_meets_target(block_hash, client->info.difficulty)) {
        if (!solo_job_share_meets_target(block_hash, client->info.difficulty_last)) {
            solo_job_release(job);
            client->info.share_error++;
            return send_error_locked(client, id, 23, "Low difficulty share");
        }
        accepted_diff = client->info.difficulty_last;
    }

    actual_diff = hash_to_difficulty((unsigned char *)block_hash);
    record_share_stats(&client->info, accepted_diff, actual_diff);

    if (solo_job_block_hash_valid(job, block_hash)) {
        sds block_head_hex = bin2hex(block_head, sizeof(block_head));
        sds block_hash_hex = bin2hex(block_hash, sizeof(block_hash));
        if (block_head_hex == NULL || block_hash_hex == NULL ||
            solo_job_submit_block(job, block_head_hex, coinbase_buf, coinbase_len, block_hash_hex) < 0) {
            sdsfree(block_head_hex);
            sdsfree(block_hash_hex);
            solo_job_release(job);
            return -1;
        }
        log_vip("block candidate accepted worker: %s hash: %s", client->info.worker, block_hash_hex);
        sdsfree(block_head_hex);
        sdsfree(block_hash_hex);
    }
    solo_job_release(job);

    if (send_ok_locked(client, id) < 0)
        return -1;

    client->info.share_valid++;
    {
        struct solo_recent_share *entry;
        pthread_mutex_lock(&shares_log_lock);
        global_shares_valid++;
        entry = &recent_shares[recent_shares_pos % SOLO_RECENT_SHARES_MAX];
        snprintf(entry->user_agent, sizeof(entry->user_agent), "%s",
            client->info.user_agent[0] ? client->info.user_agent : "Unknown");
        entry->difficulty = accepted_diff;
        entry->actual_diff = actual_diff;
        entry->timestamp_ms = current_time_millis();
        if (!best_share_ready || actual_diff > best_share.actual_diff) {
            best_share = *entry;
            best_share_ready = true;
        }
        recent_shares_pos++;
        if (recent_shares_count < SOLO_RECENT_SHARES_MAX)
            recent_shares_count++;
        pthread_mutex_unlock(&shares_log_lock);
    }
    if (!client->info.user_suggest_diff) {
        int ret = retarget_on_new_share(&client->info);
        if (ret > 0 && set_difficulty_locked(client, client->info.difficulty) < 0)
            return -1;
    }

    return 0;
}

static int handle_suggest_difficulty_locked(struct stratum_client *client, json_t *id, json_t *params)
{
    double request_diff;
    int difficulty;
    if (json_array_size(params) < 1 || !json_is_number(json_array_get(params, 0)))
        return -1;

    request_diff = json_number_value(json_array_get(params, 0));
    if (request_diff < 0)
        request_diff = -request_diff;
    difficulty = clamp_difficulty((int)request_diff);
    client->info.user_suggest_diff = true;
    client->info.difficulty = difficulty;
    client->info.difficulty_last = difficulty;

    if (send_ok_locked(client, id) < 0)
        return -1;
    if (client->info.authorized)
        return set_difficulty_locked(client, client->info.difficulty);
    return 0;
}

static int handle_get_transactions_locked(struct stratum_client *client, json_t *id)
{
    char buf[128];
    char id_str[64];
    int n;

    if (format_json_id(id, id_str, sizeof(id_str)) < 0)
        return -1;
    n = snprintf(buf, sizeof(buf),
        "{\"id\":%s,\"result\":[],\"error\":null}\n",
        id_str);
    if (n < 0 || (size_t)n >= sizeof(buf))
        return -1;
    return client_queue_send_locked(client, buf, (size_t)n);
}

static int process_line_locked(struct stratum_client *client, const char *line, size_t len)
{
    json_error_t err;
    json_t *request = json_loadb(line, len, 0, &err);
    json_t *id;
    json_t *method;
    json_t *params;
    int ret = 0;

    client->info.last_active_time = current_timestamp();

    if (request == NULL) {
        return -1;
    }

    id = json_object_get(request, "id");
    method = json_object_get(request, "method");
    params = json_object_get(request, "params");
    if (!id || !method || !json_is_string(method) || !params || !json_is_array(params)) {
        json_decref(request);
        return -1;
    }

    if (strcmp(json_string_value(method), "mining.configure") == 0)
        ret = handle_configure_locked(client, id, params);
    else if (strcmp(json_string_value(method), "mining.subscribe") == 0)
        ret = handle_subscribe_locked(client, id, params);
    else if (strcmp(json_string_value(method), "mining.authorize") == 0)
        ret = handle_authorize_locked(client, id, params);
    else if (strcmp(json_string_value(method), "mining.submit") == 0)
        ret = handle_submit_locked(client, id, params);
    else if (strcmp(json_string_value(method), "mining.suggest_difficulty") == 0)
        ret = handle_suggest_difficulty_locked(client, id, params);
    else if (strcmp(json_string_value(method), "mining.get_transactions") == 0)
        ret = handle_get_transactions_locked(client, id);
    else if (strcmp(json_string_value(method), "mining.extranonce.subscribe") == 0)
        ret = send_ok_locked(client, id);
    else
        ret = send_error_locked(client, id, 20, "Unknown method");

    json_decref(request);
    return ret < 0 ? -1 : 0;
}

static void on_client_write(struct ev_loop *loop, ev_io *watcher, int revents)
{
    struct stratum_client *client = watcher->data;
    ssize_t written;
    (void)loop;
    (void)revents;

    pthread_rwlock_wrlock(&clients_lock);
    if (client == NULL || client->write_buf == NULL || sdslen(client->write_buf) == 0) {
        ev_io_stop(stratum_loop, watcher);
        pthread_rwlock_unlock(&clients_lock);
        return;
    }

    written = client_write_direct(client->fd, client->write_buf, sdslen(client->write_buf));
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            pthread_rwlock_unlock(&clients_lock);
            return;
        }
        client_close_locked(client, "write error");
        pthread_rwlock_unlock(&clients_lock);
        return;
    }

    if ((size_t)written >= sdslen(client->write_buf)) {
        sdsfree(client->write_buf);
        client->write_buf = NULL;
        ev_io_stop(stratum_loop, watcher);
    } else {
        size_t remain = sdslen(client->write_buf) - (size_t)written;
        memmove(client->write_buf, client->write_buf + written, remain);
        sdssetlen(client->write_buf, remain);
    }
    pthread_rwlock_unlock(&clients_lock);
}

static void on_client_read(struct ev_loop *loop, ev_io *watcher, int revents)
{
    struct stratum_client *client = watcher->data;
    char buf[4096];
    ssize_t nread;
    (void)loop;
    (void)revents;

    pthread_rwlock_wrlock(&clients_lock);
    nread = recv(client->fd, buf, sizeof(buf), 0);
    if (nread <= 0) {
        client_close_locked(client, nread == 0 ? "eof" : "read error");
        pthread_rwlock_unlock(&clients_lock);
        return;
    }

    if (sds_append_len_owned(&client->read_buf, buf, (size_t)nread) < 0 ||
        client->read_buf == NULL ||
        sdslen(client->read_buf) > (size_t)settings.stratum.buf_limit) {
        client_close_locked(client, "buffer overflow");
        pthread_rwlock_unlock(&clients_lock);
        return;
    }

    while (client->read_buf) {
        char *newline = memchr(client->read_buf, '\n', sdslen(client->read_buf));
        size_t line_len;
        size_t remain;
        int rc;
        if (newline == NULL) {
            if (sdslen(client->read_buf) > (size_t)settings.stratum.max_pkg_size) {
                client_close_locked(client, "message too large");
                pthread_rwlock_unlock(&clients_lock);
                return;
            }
            break;
        }
        line_len = (size_t)(newline - client->read_buf);
        if (line_len > (size_t)settings.stratum.max_pkg_size) {
            client_close_locked(client, "message too large");
            pthread_rwlock_unlock(&clients_lock);
            return;
        }
        *newline = '\0';
        rc = process_line_locked(client, client->read_buf, line_len);
        remain = sdslen(client->read_buf) - line_len - 1;
        memmove(client->read_buf, newline + 1, remain);
        sdssetlen(client->read_buf, remain);
        if (rc < 0) {
            client_close_locked(client, "protocol error");
            pthread_rwlock_unlock(&clients_lock);
            return;
        }
    }
    pthread_rwlock_unlock(&clients_lock);
}

static void on_accept(struct ev_loop *loop, ev_io *watcher, int revents)
{
    struct sockaddr_storage peer_addr;
    socklen_t peer_len = sizeof(peer_addr);
    struct stratum_client *client;
    int fd;
    (void)loop;
    (void)watcher;
    (void)revents;

    fd = accept(listen_fd, (struct sockaddr *)&peer_addr, &peer_len);
    if (fd < 0)
        return;
    if (set_nonblocking(fd) < 0) {
        close(fd);
        return;
    }

    client = calloc(1, sizeof(*client));
    if (client == NULL) {
        close(fd);
        return;
    }
    client->fd = fd;
    client->peer_addr = peer_addr;
    client->peer_addr_len = peer_len;
    client->read_watcher.data = client;
    client->write_watcher.data = client;
    client->info.connect_time = current_timestamp();
    client->info.last_active_time = client->info.connect_time;
    client->info.last_share_time = client->info.connect_time;
    client->info.last_retarget_time = client->info.connect_time;
    client->info.difficulty = settings.diff_default;
    client->info.difficulty_last = settings.diff_default;
    snprintf(client->info.worker, sizeof(client->info.worker), "%s", "worker");
    snprintf(client->info.subscription_id, sizeof(client->info.subscription_id), "%016" PRIx64, ++subscription_counter);
    if (set_client_extra_nonce1(&client->info, (uint32_t)(++extra_nonce1_counter)) < 0) {
        close(fd);
        free(client);
        return;
    }

    ev_io_init(&client->read_watcher, on_client_read, fd, EV_READ);
    ev_io_init(&client->write_watcher, on_client_write, fd, EV_WRITE);

    pthread_rwlock_wrlock(&clients_lock);
    client_link_locked(client);
    ev_io_start(stratum_loop, &client->read_watcher);
    pthread_rwlock_unlock(&clients_lock);
}

static void on_idle_timer(struct ev_loop *loop, ev_timer *watcher, int revents)
{
    struct stratum_client *client;
    struct stratum_client *next;
    double now = current_timestamp();
    (void)loop;
    (void)watcher;
    (void)revents;

    pthread_rwlock_wrlock(&clients_lock);
    client = clients_head;
    while (client) {
        next = client->next;
        if ((now - client->info.last_active_time) > settings.client_max_idle_time)
            client_close_locked(client, "idle timeout");
        client = next;
    }
    pthread_rwlock_unlock(&clients_lock);
}

static void on_signal_timer(struct ev_loop *loop, ev_timer *watcher, int revents)
{
    (void)watcher;
    (void)revents;
    if (signal_reload) {
        signal_reload = 0;
        solo_log_reopen();
        solo_job_request_refresh(SOLO_REFRESH_MANUAL);
    }
    if (signal_exit) {
        signal_exit = 0;
        ev_break(loop, EVBREAK_ALL);
    }
}

static void on_job_async(struct ev_loop *loop, ev_async *watcher, int revents)
{
    struct solo_job *job;
    struct stratum_client *client;
    struct stratum_client *next;
    bool clean_jobs = get_pending_clean_jobs();
    (void)loop;
    (void)watcher;
    (void)revents;

    job = solo_job_current();
    if (job == NULL)
        return;

    pthread_rwlock_wrlock(&clients_lock);
    client = clients_head;
    while (client) {
        next = client->next;
        if (client->info.authorized && send_job_locked(client, job, clean_jobs) < 0)
            client_close_locked(client, "broadcast error");
        client = next;
    }
    pthread_rwlock_unlock(&clients_lock);
    solo_job_release(job);
}

static void on_job_update(const struct solo_job *job, bool clean_jobs)
{
    (void)job;
    set_pending_clean_jobs(clean_jobs);
    if (stratum_loop)
        ev_async_send(stratum_loop, &job_async);
}

static int create_listen_socket(void)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    char port[16];
    int fd = -1;
    int yes = 1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;
    snprintf(port, sizeof(port), "%d", settings.stratum.listen_port);

    if (getaddrinfo(settings.stratum.listen_addr[0] ? settings.stratum.listen_addr : NULL, port, &hints, &result) != 0)
        return -1;

    for (rp = result; rp != NULL; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
        if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0 && listen(fd, SOMAXCONN) == 0) {
            if (set_nonblocking(fd) == 0)
                break;
        }
        close(fd);
        fd = -1;
    }

    freeaddrinfo(result);
    return fd;
}

int solo_stratum_connection_count(void)
{
    int count = 0;
    struct stratum_client *client;
    pthread_rwlock_rdlock(&clients_lock);
    for (client = clients_head; client != NULL; client = client->next)
        count++;
    pthread_rwlock_unlock(&clients_lock);
    return count;
}

int solo_stratum_subscription_count(void)
{
    int count = 0;
    struct stratum_client *client;
    pthread_rwlock_rdlock(&clients_lock);
    for (client = clients_head; client != NULL; client = client->next) {
        if (client->info.subscribed)
            count++;
    }
    pthread_rwlock_unlock(&clients_lock);
    return count;
}

double solo_stratum_est_total_th_s(void)
{
    double total = 0.0;
    uint64_t tsms = now_ms();
    struct stratum_client *client;
    pthread_rwlock_rdlock(&clients_lock);
    for (client = clients_head; client != NULL; client = client->next) {
        if (client->info.subscribed)
            total += client_hashrate_ths(&client->info, tsms);
    }
    pthread_rwlock_unlock(&clients_lock);
    return total;
}

char *solo_stratum_get_workers_json(void)
{
    json_t *root = json_array();
    struct stratum_client *client;
    uint64_t tsms = now_ms();
    if (root == NULL)
        return NULL;
    pthread_rwlock_rdlock(&clients_lock);
    for (client = clients_head; client != NULL; client = client->next) {
        if (client->info.subscribed) {
            json_t *worker = json_object();
            json_object_set_new(worker, "user_agent", json_string(client->info.user_agent[0] ? client->info.user_agent : "Unknown"));
            json_object_set_new(worker, "best_diff", json_real(client->info.best_share_diff));
            json_object_set_new(worker, "hashrate", json_real(client_hashrate_ths(&client->info, tsms)));
            json_array_append_new(root, worker);
        }
    }
    pthread_rwlock_unlock(&clients_lock);
    {
        char *json_str = json_dumps(root, JSON_COMPACT);
        json_decref(root);
        return json_str;
    }
}

void solo_stratum_get_recent_shares(struct solo_recent_share *out, size_t max, size_t *count)
{
    size_t n;
    if (out == NULL || count == NULL || max == 0) {
        if (count) *count = 0;
        return;
    }
    pthread_mutex_lock(&shares_log_lock);
    n = recent_shares_count < max ? recent_shares_count : max;
    for (size_t i = 0; i < n; ++i) {
        size_t idx = (recent_shares_pos - 1 - i) % SOLO_RECENT_SHARES_MAX;
        out[i] = recent_shares[idx];
    }
    *count = n;
    pthread_mutex_unlock(&shares_log_lock);
}

bool solo_stratum_get_best_share(struct solo_recent_share *out)
{
    bool ready;

    pthread_mutex_lock(&shares_log_lock);
    ready = best_share_ready;
    if (out) {
        if (ready)
            *out = best_share;
        else
            memset(out, 0, sizeof(*out));
    }
    pthread_mutex_unlock(&shares_log_lock);
    return ready;
}

uint64_t solo_stratum_total_shares_valid(void)
{
    uint64_t val;
    pthread_mutex_lock(&shares_log_lock);
    val = global_shares_valid;
    pthread_mutex_unlock(&shares_log_lock);
    return val;
}

uint64_t solo_stratum_total_shares_error(void)
{
    uint64_t val;
    pthread_mutex_lock(&shares_log_lock);
    val = global_shares_error;
    pthread_mutex_unlock(&shares_log_lock);
    return val;
}

int solo_stratum_init(void)
{
    listen_fd = create_listen_socket();
    if (listen_fd < 0)
        return -1;

    if (urandom(&subscription_counter, sizeof(subscription_counter)) < 0)
        subscription_counter = now_ms();
    if (urandom(&extra_nonce1_counter, sizeof(extra_nonce1_counter)) < 0)
        extra_nonce1_counter = now_ms();

    stratum_loop = ev_default_loop(0);
    if (stratum_loop == NULL) {
        close(listen_fd);
        listen_fd = -1;
        return -1;
    }

    ev_io_init(&accept_watcher, on_accept, listen_fd, EV_READ);
    ev_io_start(stratum_loop, &accept_watcher);

    ev_timer_init(&idle_timer, on_idle_timer, 60.0, 60.0);
    ev_timer_start(stratum_loop, &idle_timer);

    ev_timer_init(&signal_timer, on_signal_timer, 0.5, 0.5);
    ev_timer_start(stratum_loop, &signal_timer);

    ev_async_init(&job_async, on_job_async);
    ev_async_start(stratum_loop, &job_async);

    set_pending_clean_jobs(false);
    solo_job_set_update_cb(on_job_update);
    return 0;
}

void solo_stratum_run(void)
{
    ev_run(stratum_loop, 0);
}

void solo_stratum_shutdown(void)
{
    struct stratum_client *client;
    struct stratum_client *next;

    solo_job_set_update_cb(NULL);

    if (stratum_loop) {
        ev_io_stop(stratum_loop, &accept_watcher);
        ev_timer_stop(stratum_loop, &idle_timer);
        ev_timer_stop(stratum_loop, &signal_timer);
        ev_async_stop(stratum_loop, &job_async);
    }

    pthread_rwlock_wrlock(&clients_lock);
    client = clients_head;
    while (client) {
        next = client->next;
        client_free_locked(client);
        client = next;
    }
    pthread_rwlock_unlock(&clients_lock);

    if (listen_fd >= 0) {
        close(listen_fd);
        listen_fd = -1;
    }

    set_pending_clean_jobs(false);
    stratum_loop = NULL;
}
