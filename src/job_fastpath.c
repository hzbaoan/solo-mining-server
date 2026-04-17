#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <zmq.h>

#include "job_fastpath.h"
#include "config.h"
#include "job.h"
#include "solo_log.h"
#include "solo_utils.h"
#include "version.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

enum {
    ZMQ_BLOCK_DEBOUNCE_MS = 10,
    ZMQ_RECONNECT_DELAY_MS = 1000,
    ZMQ_CONNECT_RETRY_MS = 2000,
    P2P_PROTOCOL_VERSION = 70016,
    P2P_MAX_PAYLOAD_LEN = 4 * 1024 * 1024,
    P2P_CONNECT_TIMEOUT_MS = 5000,
    P2P_RECONNECT_DELAY_MS = 3000,
    P2P_MSG_BLOCK = 2,
    P2P_MSG_FILTERED_BLOCK = 3,
    P2P_MSG_CMPCT_BLOCK = 4,
    P2P_MSG_WITNESS_FLAG = 1U << 30,
};

struct p2p_message {
    char            command[13];
    unsigned char   *payload;
    size_t          payload_len;
};

struct block_announcement {
    char        prevhash_hex[65];
    char        block_hash_hex[65];
    uint32_t    block_time;
    uint32_t    block_nbits;
};

static pthread_t zmq_thread_id;
static pthread_t p2p_thread_id;
static volatile bool fastpath_running;
static bool zmq_started;
static bool p2p_started;
static volatile uint64_t last_zmq_trigger_ms;

static void sleep_interruptible(int total_ms)
{
    int remaining = total_ms;

    while (fastpath_running && remaining > 0) {
        int chunk = remaining > 200 ? 200 : remaining;
        usleep((useconds_t)chunk * 1000U);
        remaining -= chunk;
    }
}

static bool should_fire_zmq_trigger(void)
{
    uint64_t now = current_time_millis();
    uint64_t last = __atomic_load_n(&last_zmq_trigger_ms, __ATOMIC_RELAXED);

    if (now - last < ZMQ_BLOCK_DEBOUNCE_MS)
        return false;
    __atomic_store_n(&last_zmq_trigger_ms, now, __ATOMIC_RELAXED);
    return true;
}

static void trigger_zmq_refresh(const char *source)
{
    if (!should_fire_zmq_trigger())
        return;

    log_info("ZMQ block notification received source=%s; scheduling template refresh", source);
    if (strcmp(source, "hashblock") == 0)
        solo_job_request_refresh(SOLO_REFRESH_ZMQ_HASHBLOCK);
    else
        solo_job_request_refresh(SOLO_REFRESH_ZMQ_RAWBLOCK);
}

static void *zmq_block_main(void *unused)
{
    (void)unused;

    while (fastpath_running) {
        void *ctx = NULL;
        void *sock = NULL;
        int linger = 0;
        int connected = 0;
        int ipv6 = 1;

        if (settings.bitcoind.zmq_block_url_count == 0) {
            sleep_interruptible(1000);
            continue;
        }

        ctx = zmq_ctx_new();
        if (ctx == NULL) {
            log_warn("create ZMQ block context fail");
            sleep_interruptible(ZMQ_CONNECT_RETRY_MS);
            continue;
        }

        sock = zmq_socket(ctx, ZMQ_SUB);
        if (sock == NULL) {
            log_warn("create ZMQ block socket fail");
            zmq_ctx_term(ctx);
            sleep_interruptible(ZMQ_CONNECT_RETRY_MS);
            continue;
        }

        zmq_setsockopt(sock, ZMQ_LINGER, &linger, sizeof(linger));
        zmq_setsockopt(sock, ZMQ_IPV6, &ipv6, sizeof(ipv6));
        zmq_setsockopt(sock, ZMQ_SUBSCRIBE, "hashblock", 9);
        zmq_setsockopt(sock, ZMQ_SUBSCRIBE, "rawblock", 8);

        for (size_t i = 0; i < settings.bitcoind.zmq_block_url_count; ++i) {
            const char *url = settings.bitcoind.zmq_block_urls[i];
            if (url[0] == '\0')
                continue;
            if (zmq_connect(sock, url) == 0) {
                log_info("connected ZMQ block subscriber to %s", url);
                connected++;
            } else {
                log_warn("connect ZMQ block fail: %s", url);
            }
        }

        if (!connected) {
            zmq_close(sock);
            zmq_ctx_term(ctx);
            sleep_interruptible(ZMQ_CONNECT_RETRY_MS);
            continue;
        }

        while (fastpath_running) {
            char topic[32] = {0};
            unsigned char body[4096];
            int more = 0;
            size_t more_size = sizeof(more);
            const char *source = NULL;
            zmq_pollitem_t poll_item = { sock, 0, ZMQ_POLLIN, 0 };
            int rc;

            rc = zmq_poll(&poll_item, 1, 1000);
            if (rc < 0)
                break;
            if (rc == 0)
                continue;

            rc = zmq_recv(sock, topic, sizeof(topic) - 1, 0);
            if (rc < 0)
                break;
            topic[rc < (int)(sizeof(topic) - 1) ? rc : (int)(sizeof(topic) - 1)] = '\0';

            if (strcmp(topic, "hashblock") == 0)
                source = "hashblock";
            else if (strcmp(topic, "rawblock") == 0)
                source = "rawblock";

            if (zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &more_size) != 0 || !more) {
                if (source != NULL)
                    trigger_zmq_refresh(source);
                break;
            }

            rc = zmq_recv(sock, body, sizeof(body), 0);
            if (rc < 0) {
                if (source != NULL)
                    trigger_zmq_refresh(source);
                break;
            }
            if (rc > (int)sizeof(body))
                log_warn("ZMQ message truncated: received=%d buffer=%zu", rc, sizeof(body));

            if (zmq_getsockopt(sock, ZMQ_RCVMORE, &more, &more_size) == 0 && more) {
                unsigned char discard[64];
                (void)zmq_recv(sock, discard, sizeof(discard), 0);
            }

            if (source != NULL)
                trigger_zmq_refresh(source);
        }

        zmq_close(sock);
        zmq_ctx_term(ctx);
        if (fastpath_running) {
            log_warn("ZMQ block subscriber disconnected, retrying");
            sleep_interruptible(ZMQ_RECONNECT_DELAY_MS);
        }
    }

    return NULL;
}

static uint16_t default_p2p_port(void)
{
    switch (settings.network) {
    case BITCOIN_NETWORK_MAINNET:
        return 8333;
    case BITCOIN_NETWORK_TESTNET:
        return 18333;
    case BITCOIN_NETWORK_SIGNET:
        return 38333;
    case BITCOIN_NETWORK_REGTEST:
        return 18444;
    default:
        return 8333;
    }
}

static void network_magic(unsigned char out[4])
{
    switch (settings.network) {
    case BITCOIN_NETWORK_MAINNET:
        out[0] = 0xf9; out[1] = 0xbe; out[2] = 0xb4; out[3] = 0xd9;
        break;
    case BITCOIN_NETWORK_TESTNET:
        out[0] = 0x0b; out[1] = 0x11; out[2] = 0x09; out[3] = 0x07;
        break;
    case BITCOIN_NETWORK_SIGNET:
        out[0] = 0x0a; out[1] = 0x03; out[2] = 0xcf; out[3] = 0x40;
        break;
    case BITCOIN_NETWORK_REGTEST:
        out[0] = 0xfa; out[1] = 0xbf; out[2] = 0xb5; out[3] = 0xda;
        break;
    default:
        out[0] = 0xf9; out[1] = 0xbe; out[2] = 0xb4; out[3] = 0xd9;
        break;
    }
}

static int wait_fd(int fd, short events, int timeout_ms)
{
    struct pollfd pfd;
    int rc;

    pfd.fd = fd;
    pfd.events = events;
    pfd.revents = 0;

    rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0)
        return rc;
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        return -1;
    if ((pfd.revents & events) == 0)
        return 0;
    return 1;
}

static int read_full(int fd, void *buf, size_t len)
{
    size_t used = 0;

    while (fastpath_running && used < len) {
        int rc = wait_fd(fd, POLLIN, 1000);
        if (rc < 0)
            return -1;
        if (rc == 0)
            continue;

        rc = (int)recv(fd, (char *)buf + used, len - used, 0);
        if (rc == 0)
            return -1;
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        used += (size_t)rc;
    }

    return used == len ? 0 : -1;
}

static int write_full(int fd, const void *buf, size_t len)
{
    size_t used = 0;

    while (fastpath_running && used < len) {
        int rc = wait_fd(fd, POLLOUT, 1000);
        if (rc < 0)
            return -1;
        if (rc == 0)
            continue;

        rc = (int)send(fd, (const char *)buf + used, len - used, MSG_NOSIGNAL);
        if (rc < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                continue;
            return -1;
        }
        used += (size_t)rc;
    }

    return used == len ? 0 : -1;
}

static int split_peer_host_port(const char *peer, char *host, size_t host_size, char *port, size_t port_size)
{
    const char *port_str;
    char default_port[8];
    const char *colon;
    const char *first_colon;
    size_t host_len;

    if (peer == NULL || peer[0] == '\0' || host == NULL || port == NULL)
        return -1;

    snprintf(default_port, sizeof(default_port), "%u", (unsigned int)default_p2p_port());

    if (peer[0] == '[') {
        const char *end = strchr(peer, ']');
        if (end == NULL)
            return -1;
        host_len = (size_t)(end - (peer + 1));
        if (host_len == 0 || host_len >= host_size)
            return -1;
        memcpy(host, peer + 1, host_len);
        host[host_len] = '\0';
        if (end[1] == ':' && end[2] != '\0')
            port_str = end + 2;
        else
            port_str = default_port;
    } else {
        colon = strrchr(peer, ':');
        first_colon = strchr(peer, ':');
        if (colon != NULL && first_colon == colon) {
            host_len = (size_t)(colon - peer);
            if (host_len == 0 || host_len >= host_size)
                return -1;
            memcpy(host, peer, host_len);
            host[host_len] = '\0';
            port_str = colon + 1;
            if (port_str[0] == '\0')
                port_str = default_port;
        } else {
            if (snprintf(host, host_size, "%s", peer) >= (int)host_size)
                return -1;
            port_str = default_port;
        }
    }

    if (snprintf(port, port_size, "%s", port_str) >= (int)port_size)
        return -1;
    return 0;
}

static int connect_peer(const char *peer, int *fd_out, struct sockaddr_storage *addr_out, socklen_t *addr_len_out)
{
    struct addrinfo hints;
    struct addrinfo *result = NULL;
    struct addrinfo *rp;
    char host[256];
    char port[16];
    int fd = -1;
    int rc = -1;

    if (fd_out == NULL || addr_out == NULL || addr_len_out == NULL)
        return -1;
    if (split_peer_host_port(peer, host, sizeof(host), port, sizeof(port)) < 0)
        return -1;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    if (getaddrinfo(host, port, &hints, &result) != 0 || result == NULL)
        return -1;

    for (rp = result; rp != NULL && fastpath_running; rp = rp->ai_next) {
        int flags;
        int one = 1;
        struct pollfd pfd;
        int so_error = 0;
        socklen_t so_len = sizeof(so_error);

        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

        flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            close(fd);
            fd = -1;
            continue;
        }

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) {
            memcpy(addr_out, rp->ai_addr, rp->ai_addrlen);
            *addr_len_out = (socklen_t)rp->ai_addrlen;
            *fd_out = fd;
            rc = 0;
            break;
        }
        if (errno != EINPROGRESS) {
            close(fd);
            fd = -1;
            continue;
        }

        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        if (poll(&pfd, 1, P2P_CONNECT_TIMEOUT_MS) <= 0) {
            close(fd);
            fd = -1;
            continue;
        }
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) != 0 || so_error != 0) {
            close(fd);
            fd = -1;
            continue;
        }

        memcpy(addr_out, rp->ai_addr, rp->ai_addrlen);
        *addr_len_out = (socklen_t)rp->ai_addrlen;
        *fd_out = fd;
        rc = 0;
        break;
    }

    freeaddrinfo(result);
    return rc;
}

static int encode_network_address(unsigned char out[26], const struct sockaddr *sa)
{
    unsigned char port_bytes[2] = {0, 0};

    memset(out, 0, 26);
    if (sa != NULL && sa->sa_family == AF_INET) {
        const struct sockaddr_in *sin = (const struct sockaddr_in *)sa;
        out[18] = 0xff;
        out[19] = 0xff;
        memcpy(out + 20, &sin->sin_addr, 4);
        memcpy(port_bytes, &sin->sin_port, 2);
    } else if (sa != NULL && sa->sa_family == AF_INET6) {
        const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *)sa;
        memcpy(out + 8, &sin6->sin6_addr, 16);
        memcpy(port_bytes, &sin6->sin6_port, 2);
    }

    out[24] = port_bytes[0];
    out[25] = port_bytes[1];
    return 0;
}

static int build_version_payload(unsigned char *payload, size_t payload_size, size_t *payload_len, const struct sockaddr *remote_addr)
{
    struct solo_job *job = NULL;
    char user_agent[64];
    uint32_t start_height = 0;
    uint64_t nonce = 0;
    uint64_t now = 0;
    unsigned char remote_peer[26];
    unsigned char local_peer[26];
    unsigned char relay = 0;
    void *p = payload;
    size_t left = payload_size;

    if (payload == NULL || payload_len == NULL)
        return -1;

    job = solo_job_current();
    if (job != NULL) {
        start_height = job->height > 0 ? job->height - 1 : 0;
        solo_job_release(job);
    }

    now = (uint64_t)time(NULL);
    if (urandom(&nonce, sizeof(nonce)) < 0)
        nonce = current_time_millis();

    encode_network_address(remote_peer, remote_addr);
    encode_network_address(local_peer, NULL);
    snprintf(user_agent, sizeof(user_agent), "/solo-mining-server:%s/", SOLO_STRATUM_VERSION);

    if (pack_uint32_le(&p, &left, (uint32_t)P2P_PROTOCOL_VERSION) < 0 ||
        pack_uint64_le(&p, &left, 0) < 0 ||
        pack_uint64_le(&p, &left, now) < 0 ||
        pack_buf(&p, &left, remote_peer, sizeof(remote_peer)) < 0 ||
        pack_buf(&p, &left, local_peer, sizeof(local_peer)) < 0 ||
        pack_uint64_le(&p, &left, nonce) < 0 ||
        pack_varint_le(&p, &left, strlen(user_agent)) < 0 ||
        pack_buf(&p, &left, user_agent, strlen(user_agent)) < 0 ||
        pack_uint32_le(&p, &left, start_height) < 0 ||
        pack_buf(&p, &left, &relay, sizeof(relay)) < 0) {
        return -1;
    }

    *payload_len = payload_size - left;
    return 0;
}

static int build_sendcmpct_payload(unsigned char payload[9], uint64_t version)
{
    payload[0] = 1;
    for (size_t i = 0; i < 8; ++i)
        payload[1 + i] = (unsigned char)((version >> (i * 8)) & 0xff);
    return 9;
}

static int build_getheaders_payload(unsigned char *payload, size_t payload_size, size_t *payload_len, const unsigned char locator_hash_le[32])
{
    unsigned char stop_hash[32] = {0};
    void *p = payload;
    size_t left = payload_size;

    if (pack_uint32_le(&p, &left, (uint32_t)P2P_PROTOCOL_VERSION) < 0 ||
        pack_varint_le(&p, &left, 1) < 0 ||
        pack_buf(&p, &left, locator_hash_le, 32) < 0 ||
        pack_buf(&p, &left, stop_hash, sizeof(stop_hash)) < 0) {
        return -1;
    }

    *payload_len = payload_size - left;
    return 0;
}

static int send_p2p_message(int fd, const char *command, const unsigned char *payload, size_t payload_len)
{
    unsigned char header[24] = {0};
    unsigned char checksum_full[32];
    unsigned char magic[4];
    static const unsigned char empty_payload[1] = {0};

    if (payload == NULL)
        payload = empty_payload;

    if (strlen(command) > 12 || payload_len > P2P_MAX_PAYLOAD_LEN)
        return -1;

    network_magic(magic);
    memcpy(header, magic, sizeof(magic));
    memcpy(header + 4, command, strlen(command));
    header[16] = (unsigned char)(payload_len & 0xff);
    header[17] = (unsigned char)((payload_len >> 8) & 0xff);
    header[18] = (unsigned char)((payload_len >> 16) & 0xff);
    header[19] = (unsigned char)((payload_len >> 24) & 0xff);
    sha256d(payload, payload_len, checksum_full);
    memcpy(header + 20, checksum_full, 4);

    if (write_full(fd, header, sizeof(header)) < 0)
        return -1;
    if (payload_len > 0 && write_full(fd, payload, payload_len) < 0)
        return -1;
    return 0;
}

static void free_p2p_message(struct p2p_message *message)
{
    if (message == NULL)
        return;
    free(message->payload);
    memset(message, 0, sizeof(*message));
}

static int read_p2p_message(int fd, struct p2p_message *message)
{
    unsigned char header[24];
    unsigned char magic[4];
    unsigned char checksum_full[32];
    static const unsigned char empty_payload[1] = {0};
    uint32_t payload_len;

    if (message == NULL)
        return -1;
    memset(message, 0, sizeof(*message));

    if (read_full(fd, header, sizeof(header)) < 0)
        return -1;

    network_magic(magic);
    if (memcmp(header, magic, sizeof(magic)) != 0)
        return -1;

    memcpy(message->command, header + 4, 12);
    message->command[12] = '\0';

    payload_len = ((uint32_t)header[16]) |
        ((uint32_t)header[17] << 8) |
        ((uint32_t)header[18] << 16) |
        ((uint32_t)header[19] << 24);
    if (payload_len > P2P_MAX_PAYLOAD_LEN)
        return -1;

    if (payload_len > 0) {
        message->payload = malloc(payload_len);
        if (message->payload == NULL)
            return -1;
        if (read_full(fd, message->payload, payload_len) < 0) {
            free_p2p_message(message);
            return -1;
        }
    }
    message->payload_len = payload_len;

    sha256d(message->payload_len == 0 ? empty_payload : message->payload, message->payload_len, checksum_full);
    if (memcmp(header + 20, checksum_full, 4) != 0) {
        free_p2p_message(message);
        return -1;
    }

    return 0;
}

static int read_varint(const unsigned char *payload, size_t payload_len, size_t *pos, uint64_t *value)
{
    unsigned char prefix;

    if (payload == NULL || pos == NULL || value == NULL || *pos >= payload_len)
        return -1;

    prefix = payload[(*pos)++];
    if (prefix < 0xfd) {
        *value = prefix;
        return 0;
    }
    if (prefix == 0xfd) {
        if (*pos + 2 > payload_len)
            return -1;
        *value = (uint64_t)payload[*pos] | ((uint64_t)payload[*pos + 1] << 8);
        *pos += 2;
        return 0;
    }
    if (prefix == 0xfe) {
        if (*pos + 4 > payload_len)
            return -1;
        *value = (uint64_t)payload[*pos] |
            ((uint64_t)payload[*pos + 1] << 8) |
            ((uint64_t)payload[*pos + 2] << 16) |
            ((uint64_t)payload[*pos + 3] << 24);
        *pos += 4;
        return 0;
    }
    if (*pos + 8 > payload_len)
        return -1;
    *value = (uint64_t)payload[*pos] |
        ((uint64_t)payload[*pos + 1] << 8) |
        ((uint64_t)payload[*pos + 2] << 16) |
        ((uint64_t)payload[*pos + 3] << 24) |
        ((uint64_t)payload[*pos + 4] << 32) |
        ((uint64_t)payload[*pos + 5] << 40) |
        ((uint64_t)payload[*pos + 6] << 48) |
        ((uint64_t)payload[*pos + 7] << 56);
    *pos += 8;
    return 0;
}

static bool is_block_inventory_type(uint32_t inv_type)
{
    inv_type &= ~P2P_MSG_WITNESS_FLAG;
    return inv_type == P2P_MSG_BLOCK || inv_type == P2P_MSG_FILTERED_BLOCK || inv_type == P2P_MSG_CMPCT_BLOCK;
}

static int parse_inv_has_block(const unsigned char *payload, size_t payload_len, size_t *block_count)
{
    size_t pos = 0;
    uint64_t count = 0;
    size_t matches = 0;

    if (block_count == NULL)
        return -1;
    *block_count = 0;
    if (read_varint(payload, payload_len, &pos, &count) < 0)
        return -1;
    if (count > (payload_len - pos) / 36)
        return -1;

    for (uint64_t i = 0; i < count; ++i) {
        uint32_t inv_type;

        if (pos + 36 > payload_len)
            return -1;
        inv_type = ((uint32_t)payload[pos]) |
            ((uint32_t)payload[pos + 1] << 8) |
            ((uint32_t)payload[pos + 2] << 16) |
            ((uint32_t)payload[pos + 3] << 24);
        pos += 4 + 32;
        if (is_block_inventory_type(inv_type))
            matches++;
    }

    if (pos != payload_len)
        return -1;
    *block_count = matches;
    return 0;
}

static int parse_block_announcement_from_header(const unsigned char header[80], struct block_announcement *announcement)
{
    unsigned char prevhash_be[32];
    unsigned char block_hash[32];

    if (announcement == NULL)
        return -1;

    memcpy(prevhash_be, header + 4, 32);
    reverse_mem(prevhash_be, sizeof(prevhash_be));
    hash_to_hex32(prevhash_be, announcement->prevhash_hex);

    sha256d(header, 80, block_hash);
    reverse_mem(block_hash, sizeof(block_hash));
    hash_to_hex32(block_hash, announcement->block_hash_hex);

    announcement->block_time = ((uint32_t)header[68]) |
        ((uint32_t)header[69] << 8) |
        ((uint32_t)header[70] << 16) |
        ((uint32_t)header[71] << 24);
    announcement->block_nbits = ((uint32_t)header[72]) |
        ((uint32_t)header[73] << 8) |
        ((uint32_t)header[74] << 16) |
        ((uint32_t)header[75] << 24);
    return 0;
}

static int send_getheaders_for_current_tip(int fd)
{
    struct solo_job *job = solo_job_current();
    unsigned char payload[128];
    size_t payload_len = 0;
    int rc = -1;

    if (job == NULL)
        return -1;
    if (job->height == 0)
        goto out;

    if (build_getheaders_payload(payload, sizeof(payload), &payload_len, (const unsigned char *)job->prevhash_le) < 0)
        goto out;
    rc = send_p2p_message(fd, "getheaders", payload, payload_len);

out:
    solo_job_release(job);
    return rc;
}

static int send_fast_handshake_messages(int fd)
{
    unsigned char payload[9];
    int payload_len;

    if (send_p2p_message(fd, "sendheaders", NULL, 0) < 0)
        return -1;
    payload_len = build_sendcmpct_payload(payload, 2);
    if (send_p2p_message(fd, "sendcmpct", payload, (size_t)payload_len) < 0)
        return -1;
    payload_len = build_sendcmpct_payload(payload, 1);
    if (send_p2p_message(fd, "sendcmpct", payload, (size_t)payload_len) < 0)
        return -1;
    return 0;
}

static int handle_headers_message(const unsigned char *payload, size_t payload_len)
{
    struct solo_job *job = solo_job_current();
    size_t pos = 0;
    uint64_t count = 0;
    uint32_t base_height;

    if (job == NULL)
        return -1;
    base_height = job->height;
    solo_job_release(job);
    if (base_height == 0)
        return 0;

    if (read_varint(payload, payload_len, &pos, &count) < 0)
        return -1;
    if (count > (payload_len - pos) / 80)
        return -1;
    for (uint64_t i = 0; i < count; ++i) {
        struct block_announcement announcement;
        uint64_t txn_count = 0;

        if (pos + 80 > payload_len)
            return -1;
        if (parse_block_announcement_from_header(payload + pos, &announcement) < 0)
            return -1;
        pos += 80;
        if (read_varint(payload, payload_len, &pos, &txn_count) < 0)
            return -1;
        if (txn_count != 0)
            return -1;

        (void)solo_job_handle_fast_block_announcement(
            announcement.prevhash_hex,
            announcement.block_hash_hex,
            announcement.block_time,
            base_height + (uint32_t)i,
            announcement.block_nbits
        );
    }

    return pos == payload_len ? 0 : -1;
}

static int handle_cmpctblock_message(const unsigned char *payload, size_t payload_len)
{
    struct solo_job *job = solo_job_current();
    struct block_announcement announcement;
    uint32_t block_height = 0;

    if (payload_len < 80)
        return -1;
    if (job == NULL)
        return -1;
    block_height = job->height;
    solo_job_release(job);
    if (block_height == 0)
        return 0;

    if (parse_block_announcement_from_header(payload, &announcement) < 0)
        return -1;

    (void)solo_job_handle_fast_block_announcement(
        announcement.prevhash_hex,
        announcement.block_hash_hex,
        announcement.block_time,
        block_height,
        announcement.block_nbits
    );
    return 0;
}

static int run_p2p_session(const char *peer)
{
    int fd = -1;
    struct sockaddr_storage remote_addr;
    socklen_t remote_len = sizeof(remote_addr);
    unsigned char version_payload[256];
    size_t version_payload_len = 0;
    bool sent_verack = false;
    bool received_verack = false;
    bool sent_fast_handshake = false;
    char addr_buf[128] = {0};

    if (connect_peer(peer, &fd, &remote_addr, &remote_len) < 0)
        return -1;

    format_sockaddr((const struct sockaddr *)&remote_addr, remote_len, addr_buf, sizeof(addr_buf));
    log_info("p2p fast peer connected peer=%s resolved=%s", peer, addr_buf[0] ? addr_buf : "-");

    if (build_version_payload(version_payload, sizeof(version_payload), &version_payload_len, (const struct sockaddr *)&remote_addr) < 0 ||
        send_p2p_message(fd, "version", version_payload, version_payload_len) < 0) {
        close(fd);
        return -1;
    }

    while (fastpath_running) {
        struct p2p_message message;

        if (read_p2p_message(fd, &message) < 0) {
            close(fd);
            return -1;
        }

        if (strcmp(message.command, "version") == 0) {
            if (!sent_verack) {
                if (send_p2p_message(fd, "verack", NULL, 0) < 0) {
                    free_p2p_message(&message);
                    close(fd);
                    return -1;
                }
                sent_verack = true;
            }
            if (sent_verack && received_verack && !sent_fast_handshake) {
                if (send_fast_handshake_messages(fd) < 0) {
                    free_p2p_message(&message);
                    close(fd);
                    return -1;
                }
                sent_fast_handshake = true;
            }
        } else if (strcmp(message.command, "verack") == 0) {
            received_verack = true;
            if (sent_verack && !sent_fast_handshake) {
                if (send_fast_handshake_messages(fd) < 0) {
                    free_p2p_message(&message);
                    close(fd);
                    return -1;
                }
                sent_fast_handshake = true;
            }
        } else if (strcmp(message.command, "ping") == 0) {
            if (message.payload_len == 8 && send_p2p_message(fd, "pong", message.payload, message.payload_len) < 0) {
                free_p2p_message(&message);
                close(fd);
                return -1;
            }
        } else if (sent_fast_handshake && strcmp(message.command, "inv") == 0) {
            size_t block_count = 0;

            if (parse_inv_has_block(message.payload, message.payload_len, &block_count) == 0 && block_count > 0) {
                if (send_getheaders_for_current_tip(fd) < 0)
                    log_warn("p2p getheaders send failed peer=%s", addr_buf[0] ? addr_buf : peer);
                else
                    log_info("p2p fast inv fallback peer=%s count=%zu", addr_buf[0] ? addr_buf : peer, block_count);
            }
        } else if (sent_fast_handshake && strcmp(message.command, "headers") == 0) {
            if (handle_headers_message(message.payload, message.payload_len) < 0)
                log_warn("p2p headers parse failed peer=%s", addr_buf[0] ? addr_buf : peer);
        } else if (sent_fast_handshake && strcmp(message.command, "cmpctblock") == 0) {
            if (handle_cmpctblock_message(message.payload, message.payload_len) < 0)
                log_warn("p2p cmpctblock parse failed peer=%s", addr_buf[0] ? addr_buf : peer);
        }

        free_p2p_message(&message);
    }

    close(fd);
    return 0;
}

static void *p2p_main(void *unused)
{
    (void)unused;

    while (fastpath_running) {
        if (settings.bitcoind.p2p_fast_peer[0] == '\0') {
            sleep_interruptible(1000);
            continue;
        }

        if (run_p2p_session(settings.bitcoind.p2p_fast_peer) < 0 && fastpath_running)
            log_warn("p2p fast peer session failed peer=%s", settings.bitcoind.p2p_fast_peer);
        sleep_interruptible(P2P_RECONNECT_DELAY_MS);
    }

    return NULL;
}

int solo_job_fastpath_init(void)
{
    fastpath_running = true;
    last_zmq_trigger_ms = 0;
    zmq_started = false;
    p2p_started = false;

    if (settings.bitcoind.zmq_block_url_count > 0) {
        if (pthread_create(&zmq_thread_id, NULL, zmq_block_main, NULL) != 0) {
            fastpath_running = false;
            return -1;
        }
        zmq_started = true;
    }

    if (settings.bitcoind.p2p_fast_peer[0] != '\0') {
        if (pthread_create(&p2p_thread_id, NULL, p2p_main, NULL) != 0) {
            solo_job_fastpath_shutdown();
            return -1;
        }
        p2p_started = true;
    }

    return 0;
}

void solo_job_fastpath_shutdown(void)
{
    fastpath_running = false;

    if (zmq_started) {
        pthread_join(zmq_thread_id, NULL);
        zmq_started = false;
    }
    if (p2p_started) {
        pthread_join(p2p_thread_id, NULL);
        p2p_started = false;
    }
}
