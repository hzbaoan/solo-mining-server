#ifndef SOLO_STRATUM_H
#define SOLO_STRATUM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SOLO_RECENT_SHARES_MAX 10

struct solo_recent_share {
    char        user_agent[64];
    int         difficulty;
    double      actual_diff;
    uint64_t    timestamp_ms;
};

int solo_stratum_init(void);
void solo_stratum_run(void);
void solo_stratum_shutdown(void);
int solo_stratum_connection_count(void);
int solo_stratum_subscription_count(void);
double solo_stratum_est_total_th_s(void);
char *solo_stratum_get_workers_json(void);
void solo_stratum_get_recent_shares(struct solo_recent_share *out, size_t max, size_t *count);
bool solo_stratum_get_best_share(struct solo_recent_share *out);
uint64_t solo_stratum_total_shares_valid(void);
uint64_t solo_stratum_total_shares_error(void);

#endif
