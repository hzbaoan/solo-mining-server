#ifndef SOLO_SIGNAL_H
#define SOLO_SIGNAL_H

#include <signal.h>

extern volatile sig_atomic_t signal_reload;
extern volatile sig_atomic_t signal_exit;

int init_signal(void);

#endif
