#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

#include "solo_log.h"
#include "solo_utils.h"

static const char *level_names[SOLO_LOG_LEVEL_MAX] = {
    "FATAL",
    "ERROR",
    "WARN",
    "INFO",
    "DEBUG",
    "VIP",
};

static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
static bool enabled_levels[SOLO_LOG_LEVEL_MAX] = {true, true, true, true, true, true};
static FILE *log_file;
static char log_path[1024];

static void enable_default_levels(void)
{
    for (size_t i = 0; i < SOLO_LOG_LEVEL_MAX; ++i)
        enabled_levels[i] = true;
}

static void set_named_level(const char *name, bool enabled)
{
    if (strcasecmp(name, "fatal") == 0)
        enabled_levels[SOLO_LOG_FATAL] = enabled;
    else if (strcasecmp(name, "error") == 0)
        enabled_levels[SOLO_LOG_ERROR] = enabled;
    else if (strcasecmp(name, "warn") == 0 || strcasecmp(name, "warning") == 0)
        enabled_levels[SOLO_LOG_WARN] = enabled;
    else if (strcasecmp(name, "info") == 0)
        enabled_levels[SOLO_LOG_INFO] = enabled;
    else if (strcasecmp(name, "debug") == 0)
        enabled_levels[SOLO_LOG_DEBUG] = enabled;
    else if (strcasecmp(name, "vip") == 0)
        enabled_levels[SOLO_LOG_VIP] = enabled;
}

static void parse_levels(const char *flags)
{
    char copy[256];
    char *token;
    char *ctx = NULL;

    enable_default_levels();
    if (flags == NULL || flags[0] == '\0')
        return;

    for (size_t i = 0; i < SOLO_LOG_LEVEL_MAX; ++i)
        enabled_levels[i] = false;

    snprintf(copy, sizeof(copy), "%s", flags);
    token = strtok_r(copy, ",", &ctx);
    while (token != NULL) {
        while (*token != '\0' && isspace((unsigned char)*token))
            token++;
        set_named_level(token, true);
        token = strtok_r(NULL, ",", &ctx);
    }
}

static int ensure_parent_dir(const char *path)
{
    char copy[1024];
    char *last_sep;

    if (path == NULL || path[0] == '\0')
        return 0;
    snprintf(copy, sizeof(copy), "%s", path);
    last_sep = strrchr(copy, '/');
    if (last_sep == NULL)
        last_sep = strrchr(copy, '\\');
    if (last_sep == NULL)
        return 0;
    *last_sep = '\0';
    if (copy[0] == '\0')
        return 0;
    return mkdir_p(copy);
}

int solo_log_init(const char *path, const char *flags)
{
    pthread_mutex_lock(&log_lock);

    parse_levels(flags);
    if (path) {
        snprintf(log_path, sizeof(log_path), "%s", path);
        if (log_path[0] != '\0') {
            if (ensure_parent_dir(log_path) < 0) {
                pthread_mutex_unlock(&log_lock);
                return -1;
            }
            log_file = fopen(log_path, "a");
            if (log_file == NULL) {
                pthread_mutex_unlock(&log_lock);
                return -1;
            }
            setvbuf(log_file, NULL, _IOLBF, 0);
        }
    }

    pthread_mutex_unlock(&log_lock);
    return 0;
}

void solo_log_close(void)
{
    pthread_mutex_lock(&log_lock);
    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&log_lock);
}

void solo_log_reopen(void)
{
    pthread_mutex_lock(&log_lock);
    if (log_path[0] != '\0') {
        if (log_file)
            fclose(log_file);
        log_file = fopen(log_path, "a");
        if (log_file)
            setvbuf(log_file, NULL, _IOLBF, 0);
    }
    pthread_mutex_unlock(&log_lock);
}

void solo_log_stderr(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

void solo_log_message(enum solo_log_level level, const char *func, const char *fmt, ...)
{
    struct timeval tv;
    struct tm tm_info;
    char timebuf[32];
    char message[2048];
    va_list ap;

    if (level < 0 || level >= SOLO_LOG_LEVEL_MAX)
        return;
    if (!enabled_levels[level])
        return;

    gettimeofday(&tv, NULL);
    localtime_r(&tv.tv_sec, &tm_info);
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tm_info);

    va_start(ap, fmt);
    vsnprintf(message, sizeof(message), fmt, ap);
    va_end(ap);

    pthread_mutex_lock(&log_lock);
    fprintf(stderr, "%s.%03ld [%s] %s: %s\n", timebuf, tv.tv_usec / 1000L, func ? func : "-", level_names[level], message);
    if (log_file)
        fprintf(log_file, "%s.%03ld [%s] %s: %s\n", timebuf, tv.tv_usec / 1000L, func ? func : "-", level_names[level], message);
    pthread_mutex_unlock(&log_lock);
}
