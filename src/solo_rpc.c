#include <curl/curl.h>
#include <jansson.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "solo_log.h"
#include "solo_rpc.h"
#include "solo_utils.h"

struct rpc_buffer {
    char *data;
    size_t len;
};

#if LIBCURL_VERSION_NUM >= 0x072000
static int rpc_progress_cb(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow)
#else
static int rpc_progress_cb(void *clientp, double dltotal, double dlnow, double ultotal, double ulnow)
#endif
{
    const bool *cancel_flag = clientp;

    (void)dltotal;
    (void)dlnow;
    (void)ultotal;
    (void)ulnow;

    return cancel_flag != NULL && !__atomic_load_n(cancel_flag, __ATOMIC_ACQUIRE) ? 1 : 0;
}

static size_t rpc_write_cb(const void *ptr, size_t size, size_t nmemb, void *userdata)
{
    struct rpc_buffer *buf = userdata;
    size_t chunk = size * nmemb;
    char *new_data = realloc(buf->data, buf->len + chunk + 1);
    if (new_data == NULL)
        return 0;
    buf->data = new_data;
    memcpy(buf->data + buf->len, ptr, chunk);
    buf->len += chunk;
    buf->data[buf->len] = '\0';
    return chunk;
}

static bool refresh_cookie(struct solo_rpc_client *client)
{
    FILE *fp;
    if (client == NULL || !client->use_cookie)
        return false;

    fp = fopen(client->cookie_file, "r");
    if (fp == NULL) {
        log_error("open rpc cookie fail: %s", client->cookie_file);
        return false;
    }
    if (fgets(client->userpass, sizeof(client->userpass), fp) == NULL) {
        fclose(fp);
        log_error("read rpc cookie fail: %s", client->cookie_file);
        return false;
    }
    fclose(fp);
    client->userpass[strcspn(client->userpass, "\r\n")] = '\0';
    return true;
}

int solo_rpc_client_init(struct solo_rpc_client *client, const char *url, const char *user, const char *pass, const char *cookie_file)
{
    if (client == NULL || url == NULL)
        return -1;

    memset(client, 0, sizeof(*client));
    snprintf(client->url, sizeof(client->url), "%s", url);
    if (user && user[0]) {
        snprintf(client->userpass, sizeof(client->userpass), "%s:%s", user, pass ? pass : "");
    } else if (cookie_file && cookie_file[0]) {
        client->use_cookie = true;
        snprintf(client->cookie_file, sizeof(client->cookie_file), "%s", cookie_file);
        if (!refresh_cookie(client))
            return -1;
    }

    client->curl = curl_easy_init();
    return client->curl ? 0 : -1;
}

void solo_rpc_client_free(struct solo_rpc_client *client)
{
    if (client == NULL)
        return;
    if (client->curl)
        curl_easy_cleanup((CURL *)client->curl);
    memset(client, 0, sizeof(*client));
}

static void rpc_error_reset(struct solo_rpc_error *error)
{
    if (error == NULL)
        return;
    memset(error, 0, sizeof(*error));
}

static void rpc_error_set_message(struct solo_rpc_error *error, const char *message)
{
    if (error == NULL)
        return;
    snprintf(error->rpc_message, sizeof(error->rpc_message), "%s", message ? message : "");
}

static json_t *rpc_call_once(struct solo_rpc_client *client, const char *payload, double timeout_seconds, bool allow_null_result, struct solo_rpc_error *error)
{
    struct curl_slist *headers = NULL;
    struct rpc_buffer buffer = {0};
    CURL *curl = (CURL *)client->curl;
    json_t *root = NULL;
    json_t *result = NULL;
    json_t *error_obj = NULL;
    json_error_t jerr;
    long http_code = 0;
    CURLcode curl_rc;

    rpc_error_reset(error);

    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, client->url);
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)strlen(payload));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, rpc_write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buffer);
    curl_easy_setopt(curl, CURLOPT_FAILONERROR, 0L);
    curl_easy_setopt(curl, CURLOPT_TCP_NODELAY, 1L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, (long)(timeout_seconds * 1000.0));
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, (long)(timeout_seconds * 1000.0));
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (client->cancel_flag != NULL) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
#if LIBCURL_VERSION_NUM >= 0x072000
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, rpc_progress_cb);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, (void *)client->cancel_flag);
#else
        curl_easy_setopt(curl, CURLOPT_PROGRESSFUNCTION, rpc_progress_cb);
        curl_easy_setopt(curl, CURLOPT_PROGRESSDATA, (void *)client->cancel_flag);
#endif
    } else {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
    }
    if (client->userpass[0]) {
        curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
        curl_easy_setopt(curl, CURLOPT_USERPWD, client->userpass);
    }

    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);

    curl_rc = curl_easy_perform(curl);
    if (curl_rc != CURLE_OK) {
        if (error != NULL) {
            error->curl_code = (int)curl_rc;
            error->timed_out = curl_rc == CURLE_OPERATION_TIMEDOUT;
            rpc_error_set_message(error, curl_easy_strerror(curl_rc));
        }
        curl_slist_free_all(headers);
        free(buffer.data);
        return NULL;
    }

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    if (error != NULL)
        error->http_code = http_code;
    if (http_code < 200 || http_code >= 300) {
        rpc_error_set_message(error, "unexpected HTTP status");
        curl_slist_free_all(headers);
        free(buffer.data);
        return NULL;
    }

    root = json_loads(buffer.data ? buffer.data : "", 0, &jerr);
    free(buffer.data);
    curl_slist_free_all(headers);
    if (root == NULL) {
        rpc_error_set_message(error, jerr.text);
        return NULL;
    }

    error_obj = json_object_get(root, "error");
    result = json_object_get(root, "result");
    if (error_obj && !json_is_null(error_obj)) {
        if (error != NULL) {
            json_t *code_obj = json_object_get(error_obj, "code");
            json_t *message_obj = json_object_get(error_obj, "message");
            error->rpc_error = true;
            if (json_is_integer(code_obj))
                error->rpc_code = (int)json_integer_value(code_obj);
            if (json_is_string(message_obj))
                rpc_error_set_message(error, json_string_value(message_obj));
        }
        json_decref(root);
        return NULL;
    }
    if (result == NULL) {
        rpc_error_set_message(error, "missing result");
        json_decref(root);
        return NULL;
    }
    if (!allow_null_result && json_is_null(result)) {
        rpc_error_set_message(error, "null result");
        json_decref(root);
        return NULL;
    }

    json_incref(result);
    json_decref(root);
    return result;
}

json_t *solo_rpc_call_ex(struct solo_rpc_client *client,
                         const char *method,
                         json_t *params,
                         double timeout_seconds,
                         bool allow_null_result,
                         struct solo_rpc_error *error)
{
    json_t *request = NULL;
    json_t *params_local = NULL;
    json_t *result = NULL;
    char *payload = NULL;
    struct solo_rpc_error error_local;
    struct solo_rpc_error *error_out = error ? error : &error_local;

    if (client == NULL || method == NULL)
        return NULL;

    request = json_object();
    if (request == NULL)
        return NULL;
    if (params != NULL) {
        if (json_object_set(request, "params", params) < 0) {
            json_decref(request);
            return NULL;
        }
    } else {
        params_local = json_array();
        if (params_local == NULL) {
            json_decref(request);
            return NULL;
        }
        json_object_set_new(request, "params", params_local);
        params_local = NULL;
    }

    json_object_set_new(request, "jsonrpc", json_string("1.0"));
    json_object_set_new(request, "id", json_integer((json_int_t)current_time_millis()));
    json_object_set_new(request, "method", json_string(method));
    payload = json_dumps(request, JSON_COMPACT);
    json_decref(request);
    if (payload == NULL)
        return NULL;

    result = rpc_call_once(client, payload, timeout_seconds, allow_null_result, error_out);
    if (result == NULL && client->use_cookie && error_out->http_code == 401 && refresh_cookie(client))
        result = rpc_call_once(client, payload, timeout_seconds, allow_null_result, error_out);
    free(payload);
    return result;
}
