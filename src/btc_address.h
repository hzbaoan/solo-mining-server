#ifndef BTC_ADDRESS_H
#define BTC_ADDRESS_H

#include "solo_sds.h"

enum bitcoin_network {
    BITCOIN_NETWORK_UNKNOWN = 0,
    BITCOIN_NETWORK_MAINNET,
    BITCOIN_NETWORK_TESTNET,
    BITCOIN_NETWORK_REGTEST,
};

enum bitcoin_network bitcoin_network_parse(const char *name);
const char *bitcoin_network_name(enum bitcoin_network network);
sds bitcoin_address_to_script(const char *address, enum bitcoin_network network);

#endif
