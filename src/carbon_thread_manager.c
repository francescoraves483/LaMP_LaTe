#include "carbon_thread_manager.h"
#include "timer_man.h"
#include <poll.h>
#include <sys/timerfd.h>
#include <time.h>
#include <stdlib.h>
#include <unistd.h>

// Still to be performed:
//  - error management
static void *flush_loop (void *arg) {
	struct flush_callback_args *flush_loop_args=(struct flush_callback_args *) arg;
	struct timespec now;
	struct itimerspec new_value;
	int clockFd;
	struct pollfd timerMon[2];
	unsigned long long junk;

	// Create a new monotonic (increasing) timer
	clockFd=timerfd_create(CLOCK_MONOTONIC,NO_FLAGS_TIMER);
	if(clockFd==-1) {
		pthread_exit(NULL);
	}

	// Fill pollfd structure to monitor both the timer and the "unlock_pd" pipe in ctd, which
	// will be used to unlock the thread, which may be blocked inside poll(), when stopCarbonTimedThread() is called
	timerMon[0].fd=clockFd;
	timerMon[0].revents=0;
	timerMon[0].events=POLLIN;

	timerMon[1].fd=flush_loop_args->ctd->unlock_pd[0];
	timerMon[1].revents=0;
	timerMon[1].events=POLLIN;

	// Convert time, in ms, to seconds and nanoseconds, and set the timer interval
	new_value.it_interval.tv_nsec=0;
	new_value.it_interval.tv_sec=(time_t) (flush_loop_args->opts->carbon_interval);

	// Get the current time and set the initial expiration of the timer in such a way that it will expire
	// a little after an exact second, in CLOCK_REALTIME
	clock_gettime(CLOCK_REALTIME,&now);
	new_value.it_value.tv_nsec=now.tv_nsec==0 ? 0 : 1000000000-now.tv_nsec;
	new_value.it_value.tv_sec=0;

	// Start timer
	if(timerfd_settime(clockFd,NO_FLAGS_TIMER,&new_value,NULL)==-1) {
		close(clockFd);
		pthread_exit(NULL);
	}

	// Wait for the timer to expire the first time (we can pass 'ndfs'=1, as we are interested in monitoring only the first descriptor
	// now, i.e. clockFd, ignoring the pipe)
	if(poll(timerMon,1,INDEFINITE_BLOCK)>0) {
		// "Clear the event" by performing a read() on a junk variable
		if(read(clockFd,&junk,sizeof(junk))==-1) {
			pthread_exit(NULL);
		}
	}

	while(1) {
		if(poll(timerMon,2,INDEFINITE_BLOCK)>0) {
			// If poll was unlocked via pipe, terminate the loop (and the thread)
			if(timerMon[1].revents>0) {
				// Flush the metrics related to the last received packets
				carbonReportStructureFlush(flush_loop_args->reportPtr,flush_loop_args->opts,g_DECIMAL_DIGITS,1);
				break;
			}

			// If poll was unlocked via timer, clear the event (and terminate the loop in case the event could not be cleared)
			if(timerMon[0].revents>0 && read(clockFd,&junk,sizeof(junk))==-1) {
				break;
			}
		}

		pthread_mutex_lock(&(flush_loop_args->ctd->mutex));
		carbonReportStructureFlush(flush_loop_args->reportPtr,flush_loop_args->opts,g_DECIMAL_DIGITS,0);
		pthread_mutex_unlock(&(flush_loop_args->ctd->mutex));
	}

	pthread_exit(NULL);
}

int startCarbonTimedThread(carbon_pthread_data_t *ctd,carbonReportStructure *reportPtr,struct options *opts) {
	struct flush_callback_args *args;

	// Allocate memory for 'args'
	args=malloc(sizeof(struct flush_callback_args));

	if(!args) {
		fprintf(stderr,"Error: could not allocate memory for the arguments to be passed to the Graphite/Carbon flush thread.\n");
		return -1;
	}

	// Initialize the specified mutex
	if(pthread_mutex_init(&(ctd->mutex),NULL)!=0) {
		fprintf(stderr,"Error: could not allocate a mutex to synchronize the Graphite/Carbon flush thread.\n");
		return -1;
	}

	// Fill the flush_callback_args structure (arguments to be passed to the thread flush loop, which will periodically flush the metrics to Carbon)
	args->opts=opts;
	args->reportPtr=reportPtr;
	args->ctd=ctd;
	ctd->args=args;

	// Create the unlock_td pipe
	if(pipe(ctd->unlock_pd)<0) {
		fprintf(stderr,"Error: could not create the pipe for the graceful termination of the flush thread.\n");
		return -1;
	}

	// Create the thread, passing as argument a pointer to 'args'
	if(pthread_create(&(ctd->tid),NULL,&flush_loop,(void *) args)!=0) {
		pthread_mutex_destroy(&(ctd->mutex));
		close(ctd->unlock_pd[0]);
		close(ctd->unlock_pd[1]);

		fprintf(stderr,"Error: could not start the Graphite/Carbon flush thread.\n");
		return -1;
	}

	return 0;
}

void stopCarbonTimedThread(carbon_pthread_data_t *ctd) {
	// Write a single byte to the unlock_pd pipe to unlock the thread, to gracefully terminate it
	if(write(ctd->unlock_pd[1],"\0",1)<0) {
		fprintf(stderr,"Warning: could not gracefully terminate the Graphite/Carbon flush thread.\n"
			"Its termination will be forced.\n");
		pthread_cancel(ctd->tid);
	}

	// Close the unlock_pd pipe
	close(ctd->unlock_pd[0]);
	close(ctd->unlock_pd[1]);

	pthread_join(ctd->tid,NULL);

	if(ctd->args) {
		free(ctd->args);
	}

	pthread_mutex_destroy(&(ctd->mutex));
}