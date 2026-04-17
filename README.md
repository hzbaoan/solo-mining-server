# solo-mining-server

`solo-mining-server` is a self-contained Bitcoin solo Stratum server written in C.

It connects directly to `bitcoind` over JSON-RPC, builds solo mining jobs from `getblocktemplate`, accepts Stratum miner connections, submits solved blocks with `submitblock`, and exposes a built-in HTTP dashboard plus a read-only API.

## Highlights

- Native C implementation with local source only; no build-time `git clone` or external application checkout.
- Bitcoin solo mining flow based on `getblocktemplate` and `submitblock`.
- Stratum server with variable difficulty, worker tracking, duplicate share protection, and version-rolling support.
- Optional fast job refresh through ZeroMQ block notifications and an optional Bitcoin P2P fast peer.
- Embedded HTTP dashboard and JSON API.
- Docker image and compose examples included.
- Supports `mainnet`, `testnet`, `signet`, and `regtest`.

## Repository Layout

- `src/`: server source code
- `config.json`: example configuration
- `Dockerfile`: multi-stage container build
- `docker-compose.yml`: basic container deployment
- `docker-compose.umbrel.yml`: example for Umbrel external Docker networking

## Requirements

### Runtime

- A reachable `bitcoind` with JSON-RPC enabled
- A valid payout address for the selected Bitcoin network

### Optional fast-path integrations

- ZeroMQ block publishers from `bitcoind`
- A reachable Bitcoin P2P peer for earlier clean-job announcements

### Build dependencies

The native build links against:

- `libev`
- `libmicrohttpd`
- `libzmq`
- `libcurl`
- `jansson`
- `OpenSSL`
- `pthread`
- `libm`

On Debian or Ubuntu, the packages used by the container build are:

```sh
sudo apt-get update
sudo apt-get install -y \
  build-essential \
  ca-certificates \
  libcurl4-openssl-dev \
  libev-dev \
  libjansson-dev \
  libmicrohttpd-dev \
  libssl-dev \
  libzmq3-dev
```

## Build

```sh
make
```

The resulting binary is `./solo_stratumd`.

Clean build artifacts with:

```sh
make clean
make deps-clean
```

`deps-clean` is a compatibility target and currently does nothing.

## Quick Start

1. Review and edit [`config.json`](config.json).
2. Set your Bitcoin RPC credentials and payout address.
3. Optionally enable ZMQ and P2P fast-path settings.
4. Build the server with `make`.
5. Start it with:

```sh
./solo_stratumd config.json
```

The server listens on:

- Stratum: `0.0.0.0:3333` by default
- Dashboard/API: `0.0.0.0:7152` by default

## Configuration

The configuration file is JSON. The included [`config.json`](config.json) is a complete example.

### `process`

- `file_limit`: file descriptor soft limit to request at startup, best-effort within the OS hard limit
- `core_limit`: core dump size limit to request at startup

### `log`

- `path`: log file path
- `flag`: comma-separated log levels

### `stratum`

- `bind`: compatible array form such as `["tcp@0.0.0.0:3333"]`
- `listen_addr`: alternative host field if `bind` is not used
- `listen_port`: alternative port field if `bind` is not used
- `max_pkg_size`: maximum JSON request size
- `buf_limit`: per-client buffer limit

Note: when `bind` is present, the current implementation uses the first entry.

### `bitcoind`

RPC connection:

- `rpcurl`: full HTTP URL, for example `http://127.0.0.1:8332`
- Or `host` + `port`

RPC authentication:

- `rpcuser` + `rpcpassword`
- Or legacy `user` + `pass`
- Or `rpccookiefile`

Fast refresh options:

- `zmq_block_urls`: array of ZeroMQ publisher endpoints
- `zmq_hashblock_url`: legacy single-endpoint compatibility field
- `p2p_fast_peer`: optional Bitcoin peer in `host:port` form

The server requires either RPC user/password or a cookie file.

### `network`

- `name`: `mainnet`, `testnet`, `signet`, or `regtest`

### `coinbase`

- `address`: required payout address
- `message`: custom coinbase tag text
- `worker_tag`: append the worker label to the coinbase tag
- `segwit_commitment`: keep witness commitment output when the template requires it

### `difficulty`

- `min`: minimum share difficulty
- `max`: maximum share difficulty
- `default`: initial client difficulty
- `target_time`: target seconds per share
- `retarget_time`: retarget interval in seconds

### `runtime`

- `client_max_idle_time`: disconnect idle clients after this many seconds
- `extra_nonce2_size`: extranonce2 byte length
- `keep_old_jobs`: number of historical jobs kept for late submissions

### `api`

- `listen_addr`: HTTP bind address
- `listen_port`: HTTP port

Set `api.listen_port` to `0` to disable the dashboard and API.

## Stratum Support

Implemented request handling includes:

- `mining.configure`
- `mining.subscribe`
- `mining.authorize`
- `mining.submit`
- `mining.suggest_difficulty`
- `mining.get_transactions`
- `mining.extranonce.subscribe`

Current behavior:

- Variable difficulty is enabled by default unless the miner explicitly requests a fixed difficulty.
- Passwords in the form `d=<number>` are accepted as a difficulty hint during authorization.
- Version rolling is supported through `mining.configure` and `mining.set_version_mask`.
- Worker labels are derived from the username/account string and sanitized before use.

## RPC, Template Refresh, and Fast Paths

Core mining flow:

1. Run a startup direct `getblocktemplate`, then keep `longpoll` as the main template path
2. Build Stratum jobs for connected miners
3. Validate submitted shares
4. Submit full block candidates with `submitblock`

Fast refresh options:

- `zmq_block_urls` subscribes to `hashblock` and `rawblock` topics over ZeroMQ and schedules an immediate direct `getblocktemplate` refresh.
- `zmq_hashblock_url` is accepted for legacy single-endpoint setups and is mapped into the current ZMQ subscriber path.
- `p2p_fast_peer` enables an additional Bitcoin P2P listener for early clean-job updates plus a fast follow-up direct refresh.

Template refresh behavior:

- Startup performs a direct `getblocktemplate` fetch and requires it to succeed before the Stratum server is considered ready.
- `longpoll` is the steady-state template path.
- ZMQ and P2P signals do not replace `longpoll`; they schedule immediate direct refreshes so reconnecting miners can receive a fresh clean job without waiting for a periodic poll.

If no port is specified in `p2p_fast_peer`, the default port is chosen from the configured network:

- `mainnet`: `8333`
- `testnet`: `18333`
- `signet`: `38333`
- `regtest`: `18444`

`blocknotify` is not used by this server.

## HTTP Dashboard and API

When enabled, the HTTP server exposes:

- `GET /`: embedded dashboard
- `GET /index.html`: embedded dashboard
- `GET /api/stats`
- `GET /api/workers`
- `GET /api/blocks`
- `GET /api/shares`
- `GET /healthz`
- `GET /readyz`

### `/api/stats`

Returns a JSON object with:

- `gateway_version`
- `uptime_seconds`
- `hashrate_th_s`
- `connections`
- `subscriptions`
- `current_height`
- `current_value_btc`
- `txn_count`
- `network_difficulty`
- `ready`
- `last_refresh_ok`
- `last_refresh_ms`
- `shares_valid`
- `shares_error`
- `blocks_submitted`
- `blocks_accepted`

### `/api/workers`

Returns an array of connected subscribed workers with:

- `user_agent`
- `best_diff`
- `hashrate`

### `/api/blocks`

Returns recent found block submission records with:

- `height`
- `hash`
- `result`
- `timestamp_ms`

### `/api/shares`

Returns recent accepted shares with:

- `user_agent`
- `difficulty`
- `actual_diff`
- `timestamp_ms`

### Health Endpoints

- `/healthz` returns `200 OK` when the HTTP process is alive.
- `/readyz` returns `200 OK` only when the current mining job is ready and the last template refresh succeeded.

## Docker

Build the image:

```sh
docker build -t solo-mining-server .
```

Run it with the repository `config.json` mounted read-only:

```sh
docker run --rm \
  -p 3333:3333 \
  -p 7152:7152 \
  -v "$PWD/config.json:/etc/solo-mining-server/config.json:ro" \
  solo-mining-server
```

Container startup resolves the config path in this order:

1. `SOLO_CONFIG_PATH`
2. `/etc/solo-mining-server/config.json`

The default container image runs as an unprivileged `solo` user.

### Docker Compose

Start with:

```sh
docker compose up -d --build
```

### Umbrel Example

The included compose example attaches to the external Docker network `umbrel_main_network`:

```sh
docker compose -f docker-compose.umbrel.yml up -d --build
```

Before using it:

1. Update [`config.json`](config.json) with the correct RPC credentials and payout address.
2. Make sure the `bitcoind` hostname in the config matches a reachable container alias or IP on `umbrel_main_network`.
3. Confirm the Bitcoin service ports exposed by your Umbrel node match your configuration.

## Network and Address Support

Supported network names:

- `mainnet`
- `testnet`
- `signet`
- `regtest`

Accepted payout address formats depend on the selected network and include:

- Legacy P2PKH
- Legacy P2SH
- Native SegWit v0
- Taproot v1

Examples:

- Mainnet: `1...`, `3...`, `bc1q...`, `bc1p...`
- Testnet/Signet: `m...`, `n...`, `2...`, `tb1q...`, `tb1p...`
- Regtest: `m...`, `n...`, `2...`, `bcrt1q...`, `bcrt1p...`

## Notes and Limitations

- This project is intended for Bitcoin solo mining against a full node you control.
- The HTTP API is read-only and currently serves only built-in endpoints.
- The dashboard is embedded directly in the binary; there is no separate frontend build step.
- There is no automated test suite in this repository at the time of writing, so `make` is the main sanity check included here.
