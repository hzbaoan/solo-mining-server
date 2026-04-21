#ifndef SOLO_LOG_H
#define SOLO_LOG_H

#include <stdarg.h>

enum solo_log_level {
    SOLO_LOG_FATAL = 0,
    SOLO_LOG_ERROR,
    SOLO_LOG_WARN,
    SOLO_LOG_INFO,
    SOLO_LOG_DEBUG,
    SOLO_LOG_VIP,
    SOLO_LOG_LEVEL_MAX
};

int solo_log_init(const char *path, const char *flags);
void solo_log_close(void);
void solo_log_reopen(void);
void solo_log_stderr(const char *fmt, ...);
void solo_log_message(enum solo_log_level level, const char *func, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define log_fatal(...) solo_log_message(SOLO_LOG_FATAL, __func__, __VA_ARGS__)
#define log_error(...) solo_log_message(SOLO_LOG_ERROR, __func__, __VA_ARGS__)
#define log_warn(...) solo_log_message(SOLO_LOG_WARN, __func__, __VA_ARGS__)
#define log_info(...) solo_log_message(SOLO_LOG_INFO, __func__, __VA_ARGS__)
#define log_debug(...) solo_log_message(SOLO_LOG_DEBUG, __func__, __VA_ARGS__)
#define log_vip(...) solo_log_message(SOLO_LOG_VIP, __func__, __VA_ARGS__)

#endif
