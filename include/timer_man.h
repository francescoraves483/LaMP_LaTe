#ifndef TIMERMAN_H_INCLUDED
#define TIMERMAN_H_INCLUDED

#include "options.h"
#include <poll.h>
#include <inttypes.h>
#include <sys/timerfd.h>

#define NO_FLAGS_TIMER 0
#define INDEFINITE_BLOCK -1

// Time conversion constants
#define SEC_TO_NANOSEC 1000000000
#define MILLISEC_TO_NANOSEC 1000000
#define SEC_TO_MICROSEC 1000000
#define SEC_TO_MILLISEC 1000
#define MILLISEC_TO_SEC 1000
#define MILLISEC_TO_MICROSEC 1000
#define MICROSEC_TO_NANOSEC 1000
#define MICROSEC_TO_MILLISEC 1000

int timerCreateAndSet(struct pollfd *timerMon, int *clockFd, uint64_t time_ms);
int timerStop(int *clockFd);
int timerRearmDouble(int clockFd,double time_ms_double);
int timerRearmRandom(int clockFd,struct options *opts);
char * timerRandDistribCheckConsistency(uint64_t basic_interval,double param,rand_distribution_t rand_type);
#endif