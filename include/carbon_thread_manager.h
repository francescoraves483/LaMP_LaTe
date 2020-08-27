#ifndef LATENCYTEST_CARBONTHREADMAN_H_INCLUDED
#define LATENCYTEST_CARBONTHREADMAN_H_INCLUDED

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "carbon_report_manager.h"
#include "options.h"

typedef struct _carbon_pthread_data {
	pthread_t tid;
	pthread_mutex_t mutex;

	// Pipe descriptors for a pipe used to unblock the thread when caling stopCarbonTimedThread(), in order to properly
	// terminate it
	int unlock_pd[2];
	struct flush_callback_args *args;
} carbon_pthread_data_t;

struct flush_callback_args {
	carbonReportStructure *reportPtr;
	struct options *opts;
	carbon_pthread_data_t *ctd;
};

#define carbon_pthread_mutex_lock(ctd) pthread_mutex_lock(&(ctd.mutex))
#define carbon_pthread_mutex_unlock(ctd) pthread_mutex_unlock(&(ctd.mutex))

int startCarbonTimedThread(carbon_pthread_data_t *ctd,carbonReportStructure *reportPtr,struct options *opts);
void stopCarbonTimedThread(carbon_pthread_data_t *ctd);

#endif