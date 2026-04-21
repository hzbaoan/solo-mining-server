#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "btc_address.h"
#include "thirdparty_base58.h"
#include "thirdparty_segwit_addr.h"

static const char *network_hrp(enum bitcoin_network network)
{
    switch (network) {
    case BITCOIN_NETWORK_MAINNET:
        return "bc";
    case BITCOIN_NETWORK_TESTNET:
        return "tb";
    case BITCOIN_NETWORK_REGTEST:
        return "bcrt";
    default:
        return NULL;
    }
}

enum bitcoin_network bitcoin_network_parse(const char *name)
{
    if (name == NULL)
        return BITCOIN_NETWORK_UNKNOWN;
    if (strcasecmp(name, "mainnet") == 0)
        return BITCOIN_NETWORK_MAINNET;
    if (strcasecmp(name, "testnet") == 0)
        return BITCOIN_NETWORK_TESTNET;
    if (strcasecmp(name, "regtest") == 0)
        return BITCOIN_NETWORK_REGTEST;

    return BITCOIN_NETWORK_UNKNOWN;
}

const char *bitcoin_network_name(enum bitcoin_network network)
{
    switch (network) {
    case BITCOIN_NETWORK_MAINNET:
        return "mainnet";
    case BITCOIN_NETWORK_TESTNET:
        return "testnet";
    case BITCOIN_NETWORK_REGTEST:
        return "regtest";
    default:
        return "unknown";
    }
}

static bool is_segwit_address(const char *address)
{
    if (address == NULL)
        return false;

    return strncasecmp(address, "bc1", 3) == 0 ||
        strncasecmp(address, "tb1", 3) == 0 ||
        strncasecmp(address, "bcrt1", 5) == 0;
}

static bool network_accepts_legacy_version(enum bitcoin_network network, int version)
{
    switch (network) {
    case BITCOIN_NETWORK_MAINNET:
        return version == 0 || version == 5;
    case BITCOIN_NETWORK_TESTNET:
    case BITCOIN_NETWORK_REGTEST:
        return version == 111 || version == 196;
    default:
        return false;
    }
}

static int detect_legacy_version(const char *address)
{
    unsigned char decoded[25];
    size_t decoded_len = sizeof(decoded);

    if (!b58tobin(decoded, &decoded_len, address, 0))
        return -1;

    return b58check(decoded, sizeof(decoded), address, 0);
}

static sds script_from_legacy_address(const char *address, enum bitcoin_network network)
{
    uint8_t script[64];
    size_t script_len;
    int version = detect_legacy_version(address);
    if (version < 0 || !network_accepts_legacy_version(network, version))
        return NULL;

    script_len = blkmk_address_to_script(script, sizeof(script), address);
    if (script_len == 0)
        return NULL;

    return sdsnewlen(script, script_len);
}

static sds script_from_segwit_address(const char *address, enum bitcoin_network network)
{
    uint8_t witness_program[40];
    uint8_t script[42];
    size_t witness_len = 0;
    size_t script_len = 0;
    int witness_version = 0;
    const char *hrp = network_hrp(network);

    if (hrp == NULL)
        return NULL;
    if (!segwit_addr_decode(&witness_version, witness_program, &witness_len, hrp, address))
        return NULL;

    if (!(((witness_version == 0) && (witness_len == 20 || witness_len == 32)) ||
          ((witness_version == 1) && witness_len == 32))) {
        return NULL;
    }

    script[script_len++] = witness_version == 0 ? 0x00 : (uint8_t)(0x50 + witness_version);
    script[script_len++] = (uint8_t)witness_len;
    memcpy(script + script_len, witness_program, witness_len);
    script_len += witness_len;

    return sdsnewlen(script, script_len);
}

sds bitcoin_address_to_script(const char *address, enum bitcoin_network network)
{
    if (address == NULL || network == BITCOIN_NETWORK_UNKNOWN)
        return NULL;

    if (is_segwit_address(address))
        return script_from_segwit_address(address, network);

    return script_from_legacy_address(address, network);
}
