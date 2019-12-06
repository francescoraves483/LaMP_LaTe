#include "timer_man.h"
#include <unistd.h>

/* This function creates a monotonic increasing timerfd timer and starts it, using as period the time_ms argument (specified in ms).
Return vale:
0: ok
-1: error when creating the timer
-2: error when starting the timer (the timer descriptor is automatically closed)
*/
int timerCreateAndSet(struct pollfd *timerMon, int *clockFd, uint64_t time_ms) {
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