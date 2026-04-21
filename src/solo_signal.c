#include <signal.h>
#include <string.h>

#include "solo_signal.h"

volatile sig_atomic_t signal_reload;
volatile sig_atomic_t signal_exit;

static void signal_handler(int sig)
{
    if (sig == SIGHUP) {
        signal_reload = 1;
        return;
    }

    signal_exit = 1;
}

int init_signal(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGHUP, &sa, NULL) < 0)
        return -1;

    signal(SIGPIPE, SIG_IGN);
    return 0;
}
