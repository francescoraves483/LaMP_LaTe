#ifndef LATENCYTEST_TIMEVALSUB_H_INCLUDED
#define LATENCYTEST_TIMEVALSUB_H_INCLUDED

#include <sys/time.h>

// "__attribute__((unused))" is added just to tell the clang compiler not to issue a warning
// for an unused 'static inline' (which is actually used in multiple modules)
static inline int timevalSub(struct timeval *in, struct timeval *out) __attribute__((unused));

// Calling the timeval structres in and out, as in iputils-ping code
// This inline function will perform out = out - in
// Adaptation from timeval_subtract, in GNU C Library documentation
static inline int timevalSub(struct timeval *in, struct timeval *out) {
	time_t original_out_tv_sec=out->tv_sec;

	/* Perform the carry for the later subtraction by updating @var{y}. */
	if (out->tv_usec < in->tv_usec) {
		int nsec = (in->tv_usec - out->tv_usec) / 1000000 + 1;
		in->tv_usec -= 1000000 * nsec;
		in->tv_sec += nsec;
	}

	if (out->tv_usec - in->tv_usec > 1000000) {
		int nsec = (out->tv_usec - in->tv_usec) / 1000000;
		in->tv_usec += 1000000 * nsec;
		in->tv_sec -= nsec;
	}

	/* Compute the time remaining to wait.
	@code{tv_usec} is certainly positive. */
	out->tv_sec-=in->tv_sec;
	out->tv_usec-=in->tv_usec;

	/* Return 1 if result is negative. */
	return original_out_tv_sec < in->tv_sec;
}

#endif