#include "timer_man.h"
#include <float.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <unistd.h>

#include <stdio.h>

/* This function creates a monotonic increasing timerfd timer and starts it, using as period the time_ms argument (specified in ms).
Return vale:
0: ok
-1: error when creating the timer
-2: error when starting the timer (the timer descriptor is automatically closed)
*/
int timerCreateAndSet(struct pollfd *timerMon,int *clockFd,uint64_t time_ms) {
	struct itimerspec new_value;
	time_t sec;
	long nanosec;

	// Create monotonic (increasing) timer
	*clockFd=timerfd_create(CLOCK_MONOTONIC,NO_FLAGS_TIMER);
	if(*clockFd==-1) {
		return -1;
	}

	// Convert time, in ms, to seconds and nanoseconds
	sec=(time_t) ((time_ms)/MILLISEC_TO_SEC);
	nanosec=MILLISEC_TO_NANOSEC*time_ms-sec*SEC_TO_NANOSEC;
	new_value.it_value.tv_nsec=nanosec;
	new_value.it_value.tv_sec=sec;
	new_value.it_interval.tv_nsec=nanosec;
	new_value.it_interval.tv_sec=sec;

	// Fill pollfd structure
	timerMon->fd=*clockFd;
	timerMon->revents=0;
	timerMon->events=POLLIN;

	// Start timer
	if(timerfd_settime(*clockFd,NO_FLAGS_TIMER,&new_value,NULL)==-1) {
		close(*clockFd);
		return -2;
	}

	return 0;
}

int timerRearmDouble(int clockFd,double time_ms_double) {
	struct itimerspec new_value;
	double time_ms_double_floor=floor(time_ms_double);
	time_t sec;
	long nanosec;

	sec=(time_t) (time_ms_double_floor/MILLISEC_TO_SEC);
	nanosec=(long) (MILLISEC_TO_NANOSEC*(time_ms_double-time_ms_double_floor)) + MILLISEC_TO_NANOSEC*time_ms_double_floor-sec*SEC_TO_NANOSEC;

	new_value.it_value.tv_nsec=nanosec;
	new_value.it_value.tv_sec=sec;
	new_value.it_interval.tv_nsec=nanosec;
	new_value.it_interval.tv_sec=sec;

	// Rearm timer with the new value
	if(timerfd_settime(clockFd,NO_FLAGS_TIMER,&new_value,NULL)==-1) {
		close(clockFd);
		return -2;
	}

	return 0;
}

int timerRearmRandom(int clockFd,struct options *opts) {
	double rand_val;

	if(opts->interval>=RAND_MAX || opts->interval>=INT_MAX || opts->rand_param<0) {
		return -2;
	}

	switch (opts->rand_type) {
		case RAND_PSEUDOUNIFORM:
			if(opts->rand_param>=opts->interval || opts->rand_param>=INT_MAX) {
				return -2;
			}

			rand_val=(double)rand_pseudouniform((int)opts->rand_param,(int)opts->interval);
			break;

		case RAND_UNIFORM:
			if(opts->rand_param>=opts->interval || opts->rand_param>=INT_MAX) {
				return -2;
			}

			rand_val=(double)rand_uniform((int)opts->rand_param,(int)opts->interval);
			break;

		case RAND_EXPONENTIAL:
			if(opts->rand_param<opts->interval || opts->interval>=DBL_MAX) {
				return -2;
			}

			rand_val=(double)opts->interval+rand_exponential(opts->rand_param-(double)opts->interval);
			break;

		case RAND_NORMAL:
			if(opts->interval>=DBL_MAX) {
				return -2;
			}

			do {
				rand_val=rand_gaussian((double)opts->interval,opts->rand_param);
			} while(rand_val<1 || rand_val>2*opts->interval-1);

			break;

		default:
			return -3;
	}

	if(opts->verboseFlag) {
		fprintf(stdout,"[INFO] New periodic interval: %.3f ms\n",rand_val);
	}

	return timerRearmDouble(clockFd,rand_val);
}

char * timerRandDistribCheckConsistency(uint64_t basic_interval,double param,rand_distribution_t rand_type) {
	if(basic_interval>=RAND_MAX) {
		return "the specified time interval (-t) exceeds the maximum obtainable random value";
	}

	if(basic_interval>=INT_MAX) {
		return "the specified time interval (-t) exceeds the maximum representable integer value";
	}

	if(param<0) {
		return "the specified random interval distrbution parameter is negative, i.e. it is invalid";
	}

	switch (rand_type) {
		case RAND_PSEUDOUNIFORM:
		case RAND_UNIFORM:
			if(param>=basic_interval) {
				return "lower interval limit cannot be greater or equal than the upper interval limit (i.e. -t value)";
			}

			if(param>=INT_MAX) {
				return "the lower interval limit is greater or equal with respect to the maximum representable integer number";
			}
			break;

		case RAND_EXPONENTIAL:
			if(param<basic_interval) {
				return "the mean value of the exponential distrbution cannot be less than its location parameter";
			}

			if(basic_interval>=DBL_MAX) {
				return "the location parameter is greater or equal with respect to the maximum representable double number";
			}
			break;

		case RAND_NORMAL:
			if(basic_interval>=DBL_MAX) {
				return "the mean is greater or equal with respect to the maximum representable double number";
			}
			break;

		default:
			return "unknown random distribution specified";
	}

	return NULL;
}