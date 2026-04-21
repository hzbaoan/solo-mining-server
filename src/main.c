#include <errno.h>
#include <inttypes.h>
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "api.h"
#include "config.h"
#include "job.h"
#include "solo_log.h"
#include "solo_signal.h"
#include "solo_utils.h"
#include "stratum.h"
#include "version.h"

static int init_process_limits(void)
{
    if (settings.process.file_limit && set_file_limit(settings.process.file_limit) < 0) {
        solo_log_stderr(
            "warn: failed to apply file_limit=%" PRIu64 ": %s; continuing",
            settings.process.file_limit,
            strerror(errno)
        );
    }
    if (settings.process.core_limit && set_core_limit(settings.process.core_limit) < 0)
        return -1;
    return 0;
}

int main(int argc, char *argv[])
{
    int rc = EXIT_FAILURE;
    printf("process: %s version: %s, compile date: %s %s\n", "solo_stratumd", SOLO_STRATUM_VERSION, __DATE__, __TIME__);
    if (argc != 2) {
        printf("usage: %s config.json\n", argv[0]);
        return rc;
    }

    if (curl_global_init(CURL_GLOBAL_ALL) != CURLE_OK) {
        fprintf(stderr, "init curl fail\n");
        return rc;
    }
    solo_utils_init();

    if (load_config(argv[1]) < 0) {
        fprintf(stderr, "load config fail\n");
        goto out_config;
    }
    if (init_process_limits() < 0) {
        fprintf(stderr, "init process limits fail\n");
        goto out_config;
    }
    if (solo_log_init(settings.log.path, settings.log.flag) < 0) {
        fprintf(stderr, "init log fail\n");
        goto out_config;
    }
    if (init_signal() < 0) {
        fprintf(stderr, "init signal fail\n");
        goto out_log;
    }

    log_info("configured network: %s", bitcoin_network_name(settings.network));

    if (solo_job_init() < 0) {
        log_error("init job fail");
        goto out_log;
    }
    if (solo_stratum_init() < 0) {
        log_error("init stratum fail");
        goto out_job;
    }
    if (solo_api_init() < 0) {
        log_error("init api fail");
        goto out_stratum;
    }

    log_vip("server start");
    solo_log_stderr("server start");
    solo_stratum_run();
    log_vip("server stop");

    solo_api_shutdown();
    rc = EXIT_SUCCESS;

out_stratum:
    solo_stratum_shutdown();
out_job:
    solo_job_shutdown();
out_log:
    solo_log_close();
out_config:
    free_config();
    curl_global_cleanup();
    return rc;
}
