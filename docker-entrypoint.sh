#!/bin/sh
set -eu

if [ "$#" -gt 0 ]; then
    exec "$@"
fi

config_path="${SOLO_CONFIG_PATH:-}"
if [ -z "$config_path" ]; then
    config_path=/etc/solo-mining-server/config.json
fi
if [ ! -f "$config_path" ]; then
    echo "solo-mining-server: config file not found: $config_path" >&2
    exit 1
fi

exec /usr/local/bin/solo_stratumd "$config_path"
