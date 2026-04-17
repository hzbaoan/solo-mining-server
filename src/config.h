#ifndef SOLO_CONFIG_H
#define SOLO_CONFIG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "btc_address.h"
#include "solo_sds.h"

#define SOLO_EXTRA_NONCE1_SIZE 4
#define SOLO_MAX_ZMQ_BLOCK_URLS 8

struct process_settings {
    uint64_t            file_limit;
    uint64_t            core_limit;
};

struct log_settings {
    char                path[1024];
    char                flag[256];
};

struct stratum_settings {
    char                listen_addr[128];
    int                 listen_port;
    int                 max_pkg_size;
    int                 buf_limit;
};

struct bitcoind_settings {
    char                rpc_url[256];
    char                rpc_user[128];
    char                rpc_pass[128];
    char                rpc_cookie_file[1024];
    char                zmq_hashblock_url[256];
    char                zmq_block_urls[SOLO_MAX_ZMQ_BLOCK_URLS][256];
    size_t              zmq_block_url_count;
    char                p2p_fast_peer[256];
};

struct api_settings {
    char                listen_addr[128];
    int                 listen_port;
};

struct settings {
    struct process_settings  process;
    struct log_settings      log;
    struct stratum_settings  stratum;
    struct bitcoind_settings bitcoind;
    struct api_settings      api;

    char                network_name[32];
    enum bitcoin_network network;

    char                payout_address[256];
    sds                 payout_script;
    char                coinbase_message[64];
    bool                coinbase_worker_tag;
    bool                segwit_commitment_enabled;

    int                 diff_min;
    int                 diff_max;
    int                 diff_default;
    int                 target_time;
    int                 retarget_time;

    int                 client_max_idle_time;
    int                 extra_nonce2_size;
    int                 keep_old_jobs;
};

extern struct settings settings;

int load_config(const char *path);
void free_config(void);
int get_extra_nonce_size(void);

#endif
