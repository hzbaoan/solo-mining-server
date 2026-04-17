#ifndef SOLO_RPC_H
#define SOLO_RPC_H

#include <stdbool.h>

#include <jansson.h>

struct solo_rpc_client {
    char url[256];
    char userpass[256];
    char cookie_file[1024];
    bool use_cookie;
    void *curl;
};

struct solo_rpc_error {
    long http_code;
    int rpc_code;
    int curl_code;
    bool timed_out;
    bool rpc_error;
    char rpc_message[256];
};

int solo_rpc_client_init(struct solo_rpc_client *client, const char *url, const char *user, const char *pass, const char *cookie_file);
void solo_rpc_client_free(struct solo_rpc_client *client);
json_t *solo_rpc_call_ex(struct solo_rpc_client *client,
                         const char *method,
                         json_t *params,
                         double timeout_seconds,
                         bool allow_null_result,
                         struct solo_rpc_error *error);

#endif
