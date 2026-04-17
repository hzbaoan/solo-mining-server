FROM debian:bookworm-slim AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    libcurl4-openssl-dev \
    libev-dev \
    libjansson-dev \
    libmicrohttpd-dev \
    libssl-dev \
    libzmq3-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

RUN make clean && make

FROM debian:bookworm-slim

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libcurl4 \
    libev4 \
    libjansson4 \
    libmicrohttpd12 \
    libssl3 \
    libzmq5 \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --create-home --home-dir /var/lib/solo-mining-server solo \
    && mkdir -p /etc/solo-mining-server /var/lib/solo-mining-server/log \
    && chown -R solo:solo /etc/solo-mining-server /var/lib/solo-mining-server

WORKDIR /var/lib/solo-mining-server

COPY --from=builder /src/solo_stratumd /usr/local/bin/solo_stratumd
COPY --from=builder /src/docker-entrypoint.sh /usr/local/bin/docker-entrypoint.sh

RUN chmod 0755 /usr/local/bin/docker-entrypoint.sh

USER solo

EXPOSE 3333 7152

ENTRYPOINT ["/usr/local/bin/docker-entrypoint.sh"]
