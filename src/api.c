#include <microhttpd.h>
#include <stdint.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "api.h"
#include "config.h"
#include "job.h"
#include "solo_log.h"
#include "solo_utils.h"
#include "stratum.h"
#include "version.h"
#include "web.h"

static struct MHD_Daemon *api_daemon;
static struct sockaddr_storage api_sockaddr;
static bool api_sockaddr_ready;

static enum MHD_Result send_response(struct MHD_Connection *connection, unsigned int status, const char *data, size_t length, const char *content_type)
{
    struct MHD_Response *response = MHD_create_response_from_buffer(length, (void *)data, MHD_RESPMEM_MUST_COPY);
    enum MHD_Result ret;
    if (response == NULL)
        return MHD_NO;
    MHD_add_response_header(response, "Content-Type", content_type);
    MHD_add_response_header(response, "Cache-Control", "no-cache, no-store, must-revalidate");
    ret = MHD_queue_response(connection, status, response);
    MHD_destroy_response(response);
    return ret;
}

static char *generate_api_stats_json(void)
{
    struct solo_job_status status;
    struct solo_recent_share best_share;
    bool has_best_share;
    json_t *root = json_object();
    if (root == NULL)
        return NULL;
    solo_job_get_status(&status);
    has_best_share = solo_stratum_get_best_share(&best_share);

    json_object_set_new(root, "gateway_version", json_string("solo-mining-server " SOLO_STRATUM_VERSION));
    json_object_set_new(root, "uptime_seconds", json_integer((json_int_t)get_process_uptime_seconds()));
    json_object_set_new(root, "hashrate_th_s", json_real(solo_stratum_est_total_th_s()));
    json_object_set_new(root, "connections", json_integer(solo_stratum_connection_count()));
    json_object_set_new(root, "subscriptions", json_integer(solo_stratum_subscription_count()));
    json_object_set_new(root, "current_height", json_integer(status.height));
    json_object_set_new(root, "current_value_btc", json_real((double)status.coinbase_value / 100000000.0));
    json_object_set_new(root, "txn_count", json_integer(status.tx_count));
    json_object_set_new(root, "network_difficulty", json_real((double)calc_network_difficulty(status.nbits_hex)));
    json_object_set_new(root, "ready", status.ready && status.last_refresh_ok ? json_true() : json_false());
    json_object_set_new(root, "last_refresh_ok", status.last_refresh_ok ? json_true() : json_false());
    json_object_set_new(root, "last_refresh_ms", json_integer((json_int_t)status.last_refresh_ms));
    json_object_set_new(root, "shares_valid", json_integer((json_int_t)solo_stratum_total_shares_valid()));
    json_object_set_new(root, "shares_error", json_integer((json_int_t)solo_stratum_total_shares_error()));
    json_object_set_new(root, "blocks_submitted", json_integer((json_int_t)solo_job_total_submitted()));
    json_object_set_new(root, "blocks_accepted", json_integer((json_int_t)solo_job_total_accepted()));
    json_object_set_new(root, "best_share_available", has_best_share ? json_true() : json_false());
    json_object_set_new(root, "best_share_user_agent", has_best_share ? json_string(best_share.user_agent) : json_null());
    json_object_set_new(root, "best_share_difficulty", json_integer(has_best_share ? best_share.difficulty : 0));
    json_object_set_new(root, "best_share_actual_diff", json_real(has_best_share ? best_share.actual_diff : 0.0));
    json_object_set_new(root, "best_share_timestamp_ms", json_integer((json_int_t)(has_best_share ? best_share.timestamp_ms : 0)));
    {
        char *json_str = json_dumps(root, JSON_COMPACT);
        json_decref(root);
        return json_str;
    }
}

static char *generate_api_blocks_json(void)
{
    struct solo_found_block blocks[SOLO_FOUND_BLOCKS_MAX];
    size_t count = 0;
    json_t *root = json_array();
    if (root == NULL)
        return NULL;

    solo_job_get_found_blocks(blocks, SOLO_FOUND_BLOCKS_MAX, &count);
    for (size_t i = 0; i < count; ++i) {
        json_t *entry = json_object();
        if (entry == NULL)
            continue;
        json_object_set_new(entry, "height", json_integer(blocks[i].height));
        json_object_set_new(entry, "hash", json_string(blocks[i].hash_hex));
        json_object_set_new(entry, "result", json_string(blocks[i].result));
        json_object_set_new(entry, "timestamp_ms", json_integer((json_int_t)blocks[i].timestamp_ms));
        json_array_append_new(root, entry);
    }
    {
        char *json_str = json_dumps(root, JSON_COMPACT);
        json_decref(root);
        return json_str;
    }
}

static char *generate_api_shares_json(void)
{
    struct solo_recent_share shares[SOLO_RECENT_SHARES_MAX];
    size_t count = 0;
    json_t *root = json_array();
    if (root == NULL)
        return NULL;

    solo_stratum_get_recent_shares(shares, SOLO_RECENT_SHARES_MAX, &count);
    for (size_t i = 0; i < count; ++i) {
        json_t *entry = json_object();
        if (entry == NULL)
            continue;
        json_object_set_new(entry, "user_agent", json_string(shares[i].user_agent));
        json_object_set_new(entry, "difficulty", json_integer(shares[i].difficulty));
        json_object_set_new(entry, "actual_diff", json_real(shares[i].actual_diff));
        json_object_set_new(entry, "timestamp_ms", json_integer((json_int_t)shares[i].timestamp_ms));
        json_array_append_new(root, entry);
    }
    {
        char *json_str = json_dumps(root, JSON_COMPACT);
        json_decref(root);
        return json_str;
    }
}

static enum MHD_Result serve_json(struct MHD_Connection *connection, char *json_data, const char *fallback)
{
    if (json_data == NULL)
        return send_response(connection, MHD_HTTP_INTERNAL_SERVER_ERROR, fallback, strlen(fallback), "application/json");
    {
        enum MHD_Result ret = send_response(connection, MHD_HTTP_OK, json_data, strlen(json_data), "application/json");
        free(json_data);
        return ret;
    }
}

static enum MHD_Result api_answer(void *cls, struct MHD_Connection *connection, const char *url, const char *method,
    const char *version, const char *upload_data, size_t *upload_data_size, void **con_cls)
{
    (void)cls;
    (void)version;
    (void)upload_data;
    (void)con_cls;

    if (*upload_data_size != 0) {
        *upload_data_size = 0;
        return MHD_YES;
    }

    if (strcmp(method, "GET") != 0)
        return send_response(connection, MHD_HTTP_METHOD_NOT_ALLOWED, "method not allowed", 18, "text/plain");

    if (strcmp(url, "/api/stats") == 0)
        return serve_json(connection, generate_api_stats_json(), "{}");
    if (strcmp(url, "/api/workers") == 0)
        return serve_json(connection, solo_stratum_get_workers_json(), "[]");
    if (strcmp(url, "/api/blocks") == 0)
        return serve_json(connection, generate_api_blocks_json(), "[]");
    if (strcmp(url, "/api/shares") == 0)
        return serve_json(connection, generate_api_shares_json(), "[]");

    if (strcmp(url, "/healthz") == 0)
        return send_response(connection, MHD_HTTP_OK, "ok", 2, "text/plain");
    if (strcmp(url, "/readyz") == 0) {
        bool ready = solo_job_is_ready();
        return send_response(connection, ready ? MHD_HTTP_OK : MHD_HTTP_SERVICE_UNAVAILABLE, ready ? "ok" : "not-ready", ready ? 2 : 9, "text/plain");
    }
    if (strcmp(url, "/") == 0 || strcmp(url, "/index.html") == 0) {
        return send_response(connection, MHD_HTTP_OK, web_get_html_dashboard(), strlen(web_get_html_dashboard()), "text/html; charset=utf-8");
    }

    return send_response(connection, MHD_HTTP_NOT_FOUND, "not found", 9, "text/plain");
}

int solo_api_init(void)
{
    char port_str[16];
    if (settings.api.listen_port == 0)
        return 0;

    api_sockaddr_ready = false;
    if (settings.api.listen_addr[0] != '\0' && strcmp(settings.api.listen_addr, "0.0.0.0") != 0 && strcmp(settings.api.listen_addr, "::") != 0) {
        struct addrinfo hints;
        struct addrinfo *result = NULL;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        snprintf(port_str, sizeof(port_str), "%d", settings.api.listen_port);
        if (getaddrinfo(settings.api.listen_addr, port_str, &hints, &result) != 0 || result == NULL)
            return -1;
        memcpy(&api_sockaddr, result->ai_addr, result->ai_addrlen);
        api_sockaddr_ready = true;
        freeaddrinfo(result);
    }

    if (api_sockaddr_ready) {
        api_daemon = MHD_start_daemon(
            MHD_USE_AUTO | MHD_USE_INTERNAL_POLLING_THREAD,
            (uint16_t)settings.api.listen_port, NULL, NULL, &api_answer, NULL,
            MHD_OPTION_CONNECTION_LIMIT, 128,
            MHD_OPTION_LISTENING_ADDRESS_REUSE, 1U,
            MHD_OPTION_SOCK_ADDR, (struct sockaddr *)&api_sockaddr,
            MHD_OPTION_END);
    } else {
        api_daemon = MHD_start_daemon(
            MHD_USE_AUTO | MHD_USE_INTERNAL_POLLING_THREAD,
            (uint16_t)settings.api.listen_port, NULL, NULL, &api_answer, NULL,
            MHD_OPTION_CONNECTION_LIMIT, 128,
            MHD_OPTION_LISTENING_ADDRESS_REUSE, 1U,
            MHD_OPTION_END);
    }

    if (api_daemon == NULL)
        return -1;

    log_info("HTTP API dashboard running on %s:%d", settings.api.listen_addr, settings.api.listen_port);
    return 0;
}

void solo_api_shutdown(void)
{
    if (api_daemon) {
        MHD_stop_daemon(api_daemon);
        api_daemon = NULL;
    }
}
