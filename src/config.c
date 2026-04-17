#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "config.h"

struct settings settings;

static void reset_settings(void)
{
    sdsfree(settings.payout_script);
    memset(&settings, 0, sizeof(settings));
}

static int copy_json_string(json_t *obj, const char *key, char *dst, size_t dst_size, const char *fallback, bool required)
{
    json_t *node;
    if (dst == NULL || dst_size == 0)
        return -1;
    if (obj == NULL || !json_is_object(obj)) {
        if (required && fallback == NULL)
            return -1;
        snprintf(dst, dst_size, "%s", fallback ? fallback : "");
        return 0;
    }

    node = json_object_get(obj, key);
    if (node == NULL || json_is_null(node)) {
        if (required && fallback == NULL)
            return -1;
        snprintf(dst, dst_size, "%s", fallback ? fallback : "");
        return 0;
    }
    if (!json_is_string(node))
        return -1;
    if ((size_t)snprintf(dst, dst_size, "%s", json_string_value(node)) >= dst_size)
        return -1;
    return 0;
}

static int copy_json_string_array(json_t *obj, const char *key, char dst[][256], size_t max_count, size_t *out_count)
{
    json_t *node;

    if (dst == NULL || max_count == 0 || out_count == NULL)
        return -1;

    *out_count = 0;
    if (obj == NULL || !json_is_object(obj))
        return 0;

    node = json_object_get(obj, key);
    if (node == NULL || json_is_null(node))
        return 0;
    if (!json_is_array(node))
        return -1;
    if (json_array_size(node) > max_count)
        return -1;

    for (size_t i = 0; i < json_array_size(node); ++i) {
        json_t *item = json_array_get(node, i);
        if (!json_is_string(item))
            return -1;
        if ((size_t)snprintf(dst[i], 256, "%s", json_string_value(item)) >= 256)
            return -1;
        (*out_count)++;
    }

    return 0;
}

static int copy_json_int(json_t *obj, const char *key, int *dst, int fallback, bool required)
{
    json_t *node;
    json_int_t value;

    if (dst == NULL)
        return -1;
    if (obj == NULL || !json_is_object(obj)) {
        if (required)
            return -1;
        *dst = fallback;
        return 0;
    }

    node = json_object_get(obj, key);
    if (node == NULL || json_is_null(node)) {
        if (required)
            return -1;
        *dst = fallback;
        return 0;
    }
    if (!json_is_integer(node))
        return -1;
    value = json_integer_value(node);
    if (value < INT_MIN || value > INT_MAX)
        return -1;
    *dst = (int)value;
    return 0;
}

static int copy_json_u64(json_t *obj, const char *key, uint64_t *dst, uint64_t fallback, bool required)
{
    json_t *node;
    json_int_t value;

    if (dst == NULL)
        return -1;
    if (obj == NULL || !json_is_object(obj)) {
        if (required)
            return -1;
        *dst = fallback;
        return 0;
    }

    node = json_object_get(obj, key);
    if (node == NULL || json_is_null(node)) {
        if (required)
            return -1;
        *dst = fallback;
        return 0;
    }
    if (!json_is_integer(node))
        return -1;
    value = json_integer_value(node);
    if (value < 0)
        return -1;
    *dst = (uint64_t)value;
    return 0;
}

static int copy_json_bool(json_t *obj, const char *key, bool *dst, bool fallback, bool required)
{
    json_t *node;
    if (dst == NULL)
        return -1;
    if (obj == NULL || !json_is_object(obj)) {
        if (required)
            return -1;
        *dst = fallback;
        return 0;
    }

    node = json_object_get(obj, key);
    if (node == NULL || json_is_null(node)) {
        if (required)
            return -1;
        *dst = fallback;
        return 0;
    }
    if (!json_is_boolean(node))
        return -1;
    *dst = json_boolean_value(node) ? true : false;
    return 0;
}

static json_t *require_object(json_t *root, const char *name, bool required)
{
    json_t *node = json_object_get(root, name);
    if (node == NULL || json_is_null(node)) {
        if (required)
            return NULL;
        return json_null();
    }
    if (!json_is_object(node))
        return NULL;
    return node;
}

static int parse_port_number(const char *value, int *port)
{
    char *end = NULL;
    long parsed;

    if (value == NULL || value[0] == '\0' || port == NULL)
        return -1;

    parsed = strtol(value, &end, 10);
    if (end == NULL || *end != '\0' || parsed <= 0 || parsed > 65535)
        return -1;

    *port = (int)parsed;
    return 0;
}

static int normalize_socket_host(char *host)
{
    size_t len;

    if (host == NULL)
        return -1;

    len = strlen(host);
    if (len >= 2 && host[0] == '[' && host[len - 1] == ']') {
        memmove(host, host + 1, len - 1);
        host[len - 2] = '\0';
    }

    return 0;
}

static int build_http_url(char *dst, size_t dst_size, const char *host, int port)
{
    bool needs_brackets;

    if (dst == NULL || dst_size == 0 || host == NULL || port <= 0)
        return -1;

    needs_brackets = strchr(host, ':') != NULL && host[0] != '[';
    if (snprintf(dst, dst_size, needs_brackets ? "http://[%s]:%d" : "http://%s:%d", host, port) >= (int)dst_size)
        return -1;

    return 0;
}

static int parse_process(json_t *root)
{
    json_t *node = require_object(root, "process", false);
    if (node == NULL)
        return -1;
    if (copy_json_u64(node, "file_limit", &settings.process.file_limit, 65535, false) < 0)
        return -1;
    if (copy_json_u64(node, "core_limit", &settings.process.core_limit, 0, false) < 0)
        return -1;
    return 0;
}

static int parse_log(json_t *root)
{
    json_t *node = require_object(root, "log", false);
    if (node == NULL)
        return -1;
    if (copy_json_string(node, "path", settings.log.path, sizeof(settings.log.path), "./log/solo_stratum.log", false) < 0)
        return -1;
    if (copy_json_string(node, "flag", settings.log.flag, sizeof(settings.log.flag), "fatal,error,warn,info,debug,vip", false) < 0)
        return -1;
    return 0;
}

static int parse_bind_string(const char *bind)
{
    const char *cursor;
    const char *port_sep;
    const char *end;
    size_t host_len;

    if (bind == NULL)
        return -1;
    cursor = strstr(bind, "tcp@");
    if (cursor == bind)
        cursor += 4;
    else
        cursor = bind;

    if (cursor[0] == '[') {
        end = strchr(cursor, ']');
        if (end == NULL || end[1] != ':' || end[2] == '\0')
            return -1;
        host_len = (size_t)(end - (cursor + 1));
        if (host_len == 0 || host_len >= sizeof(settings.stratum.listen_addr))
            return -1;

        memcpy(settings.stratum.listen_addr, cursor + 1, host_len);
        settings.stratum.listen_addr[host_len] = '\0';
        return parse_port_number(end + 2, &settings.stratum.listen_port);
    }

    port_sep = strrchr(cursor, ':');
    if (port_sep == NULL)
        return -1;
    host_len = (size_t)(port_sep - cursor);
    if (host_len == 0 || host_len >= sizeof(settings.stratum.listen_addr))
        return -1;

    memcpy(settings.stratum.listen_addr, cursor, host_len);
    settings.stratum.listen_addr[host_len] = '\0';
    return parse_port_number(port_sep + 1, &settings.stratum.listen_port);
}

static int parse_stratum(json_t *root)
{
    json_t *node = require_object(root, "stratum", false);
    json_t *bind;
    if (node == NULL)
        return -1;

    snprintf(settings.stratum.listen_addr, sizeof(settings.stratum.listen_addr), "%s", "0.0.0.0");
    settings.stratum.listen_port = 3333;
    settings.stratum.max_pkg_size = 10240;
    settings.stratum.buf_limit = 1048576;

    bind = json_object_get(node, "bind");
    if (bind && json_is_array(bind) && json_array_size(bind) > 0) {
        json_t *first = json_array_get(bind, 0);
        if (!json_is_string(first) || parse_bind_string(json_string_value(first)) < 0)
            return -1;
    } else {
        if (copy_json_string(node, "listen_addr", settings.stratum.listen_addr, sizeof(settings.stratum.listen_addr), "0.0.0.0", false) < 0)
            return -1;
        if (normalize_socket_host(settings.stratum.listen_addr) < 0)
            return -1;
        if (copy_json_int(node, "listen_port", &settings.stratum.listen_port, 3333, false) < 0)
            return -1;
    }

    if (copy_json_int(node, "max_pkg_size", &settings.stratum.max_pkg_size, 10240, false) < 0)
        return -1;
    if (copy_json_int(node, "buf_limit", &settings.stratum.buf_limit, 1048576, false) < 0)
        return -1;
    if (settings.stratum.listen_port <= 0)
        return -1;
    if (settings.stratum.max_pkg_size <= 0 || settings.stratum.buf_limit <= 0)
        return -1;
    if (settings.stratum.buf_limit < settings.stratum.max_pkg_size)
        return -1;
    return 0;
}

static int parse_bitcoind(json_t *root)
{
    json_t *node = require_object(root, "bitcoind", true);
    char host[128] = {0};
    int port = 8332;

    if (node == NULL)
        return -1;

    if (copy_json_string(node, "rpcurl", settings.bitcoind.rpc_url, sizeof(settings.bitcoind.rpc_url), "", false) < 0)
        return -1;
    if (copy_json_string(node, "rpcuser", settings.bitcoind.rpc_user, sizeof(settings.bitcoind.rpc_user), "", false) < 0)
        return -1;
    if (copy_json_string(node, "rpcpassword", settings.bitcoind.rpc_pass, sizeof(settings.bitcoind.rpc_pass), "", false) < 0)
        return -1;
    if (copy_json_string(node, "rpccookiefile", settings.bitcoind.rpc_cookie_file, sizeof(settings.bitcoind.rpc_cookie_file), "", false) < 0)
        return -1;
    if (copy_json_string(node, "zmq_hashblock_url", settings.bitcoind.zmq_hashblock_url, sizeof(settings.bitcoind.zmq_hashblock_url), "", false) < 0)
        return -1;
    if (copy_json_string_array(node, "zmq_block_urls", settings.bitcoind.zmq_block_urls, SOLO_MAX_ZMQ_BLOCK_URLS, &settings.bitcoind.zmq_block_url_count) < 0)
        return -1;
    if (copy_json_string(node, "p2p_fast_peer", settings.bitcoind.p2p_fast_peer, sizeof(settings.bitcoind.p2p_fast_peer), "", false) < 0)
        return -1;

    if (settings.bitcoind.zmq_block_url_count == 0 && settings.bitcoind.zmq_hashblock_url[0] != '\0') {
        snprintf(
            settings.bitcoind.zmq_block_urls[0],
            sizeof(settings.bitcoind.zmq_block_urls[0]),
            "%s",
            settings.bitcoind.zmq_hashblock_url
        );
        settings.bitcoind.zmq_block_url_count = 1;
    }

    if (settings.bitcoind.rpc_user[0] == '\0' &&
        copy_json_string(node, "user", settings.bitcoind.rpc_user, sizeof(settings.bitcoind.rpc_user), "", false) < 0) {
        return -1;
    }
    if (settings.bitcoind.rpc_pass[0] == '\0' &&
        copy_json_string(node, "pass", settings.bitcoind.rpc_pass, sizeof(settings.bitcoind.rpc_pass), "", false) < 0) {
        return -1;
    }

    if (settings.bitcoind.rpc_url[0] == '\0') {
        if (copy_json_string(node, "host", host, sizeof(host), "127.0.0.1", false) < 0)
            return -1;
        if (copy_json_int(node, "port", &port, 8332, false) < 0)
            return -1;
        if (build_http_url(settings.bitcoind.rpc_url, sizeof(settings.bitcoind.rpc_url), host, port) < 0)
            return -1;
    }

    if (settings.bitcoind.rpc_user[0] == '\0' && settings.bitcoind.rpc_cookie_file[0] == '\0') {
        printf("bitcoind.rpcuser/rpcpassword or bitcoind.rpccookiefile is required\n");
        return -1;
    }

    return 0;
}

static int parse_network(json_t *root)
{
    json_t *node = require_object(root, "network", false);
    if (node == NULL)
        return -1;
    if (copy_json_string(node, "name", settings.network_name, sizeof(settings.network_name), "mainnet", false) < 0)
        return -1;
    settings.network = bitcoin_network_parse(settings.network_name);
    if (settings.network == BITCOIN_NETWORK_UNKNOWN) {
        printf("invalid network.name: %s\n", settings.network_name);
        return -1;
    }
    return 0;
}

static int parse_coinbase(json_t *root)
{
    json_t *node = require_object(root, "coinbase", true);
    if (node == NULL)
        return -1;
    if (copy_json_string(node, "address", settings.payout_address, sizeof(settings.payout_address), NULL, true) < 0)
        return -1;
    if (copy_json_string(node, "message", settings.coinbase_message, sizeof(settings.coinbase_message), "solo", false) < 0)
        return -1;
    if (copy_json_bool(node, "worker_tag", &settings.coinbase_worker_tag, false, false) < 0)
        return -1;
    if (copy_json_bool(node, "segwit_commitment", &settings.segwit_commitment_enabled, true, false) < 0)
        return -1;

    settings.payout_script = bitcoin_address_to_script(settings.payout_address, settings.network);
    if (settings.payout_script == NULL) {
        printf("invalid payout address for network %s: %s\n", bitcoin_network_name(settings.network), settings.payout_address);
        return -1;
    }
    return 0;
}

static int parse_difficulty(json_t *root)
{
    json_t *node = require_object(root, "difficulty", false);
    if (node == NULL)
        return -1;
    if (copy_json_int(node, "min", &settings.diff_min, 1024, false) < 0)
        return -1;
    if (copy_json_int(node, "max", &settings.diff_max, 262144, false) < 0)
        return -1;
    if (copy_json_int(node, "default", &settings.diff_default, 8192, false) < 0)
        return -1;
    if (copy_json_int(node, "target_time", &settings.target_time, 5, false) < 0)
        return -1;
    if (copy_json_int(node, "retarget_time", &settings.retarget_time, 90, false) < 0)
        return -1;

    if (settings.diff_min <= 0 || settings.diff_max < settings.diff_min)
        return -1;
    if (settings.diff_default < settings.diff_min || settings.diff_default > settings.diff_max)
        return -1;
    if (settings.target_time <= 0 || settings.retarget_time <= 0)
        return -1;
    return 0;
}

static int parse_runtime(json_t *root)
{
    json_t *node = require_object(root, "runtime", false);
    if (node == NULL)
        return -1;
    if (copy_json_int(node, "client_max_idle_time", &settings.client_max_idle_time, 86400, false) < 0)
        return -1;
    if (copy_json_int(node, "extra_nonce2_size", &settings.extra_nonce2_size, 8, false) < 0)
        return -1;
    if (copy_json_int(node, "keep_old_jobs", &settings.keep_old_jobs, 16, false) < 0)
        return -1;

    if (settings.client_max_idle_time <= 0)
        return -1;
    if (settings.extra_nonce2_size <= 0 || settings.extra_nonce2_size > 16)
        return -1;
    if (settings.keep_old_jobs < 0)
        return -1;
    return 0;
}

static int parse_api(json_t *root)
{
    json_t *node = require_object(root, "api", false);
    if (node == NULL)
        return -1;
    if (copy_json_string(node, "listen_addr", settings.api.listen_addr, sizeof(settings.api.listen_addr), "0.0.0.0", false) < 0)
        return -1;
    if (normalize_socket_host(settings.api.listen_addr) < 0)
        return -1;
    if (copy_json_int(node, "listen_port", &settings.api.listen_port, 7152, false) < 0)
        return -1;
    if (settings.api.listen_port < 0 || settings.api.listen_port > 65535)
        return -1;
    return 0;
}

int load_config(const char *path)
{
    json_error_t error;
    json_t *root = json_load_file(path, 0, &error);

    reset_settings();
    if (root == NULL) {
        printf("json_load_file from: %s fail: %s in line: %d\n", path, error.text, error.line);
        return -1;
    }
    if (!json_is_object(root)) {
        json_decref(root);
        reset_settings();
        return -1;
    }

    if (parse_process(root) < 0 ||
        parse_log(root) < 0 ||
        parse_stratum(root) < 0 ||
        parse_bitcoind(root) < 0 ||
        parse_network(root) < 0 ||
        parse_coinbase(root) < 0 ||
        parse_difficulty(root) < 0 ||
        parse_runtime(root) < 0 ||
        parse_api(root) < 0) {
        json_decref(root);
        reset_settings();
        return -1;
    }

    json_decref(root);
    return 0;
}

void free_config(void)
{
    reset_settings();
}

int get_extra_nonce_size(void)
{
    return SOLO_EXTRA_NONCE1_SIZE + settings.extra_nonce2_size;
}
